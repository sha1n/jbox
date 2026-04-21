// route_manager.cpp — route lifecycle and IOProc wiring.

#include "route_manager.hpp"

#include "rate_deadband.hpp"
#include "rt_log_codes.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace jbox::control {

// Monotonic counter we stamp into RtLogEvent::timestamp. A pure counter
// is cheaper than reading a clock and is enough for the drainer to
// order events as observed.
namespace {
std::atomic<std::uint64_t> g_log_seq{0};

inline std::uint64_t nextLogSeq() {
    return g_log_seq.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline void tryPushLog(jbox::rt::DefaultRtLogQueue* q,
                       jbox::rt::RtLogCode code,
                       std::uint32_t route_id,
                       std::uint64_t a = 0,
                       std::uint64_t b = 0) {
    if (q == nullptr) return;
    jbox::rt::RtLogEvent ev{};
    ev.timestamp = nextLogSeq();
    ev.code = code;
    ev.route_id = route_id;
    ev.value_a = a;
    ev.value_b = b;
    (void)q->tryPush(ev);  // drop on full; drainer is best-effort.
}
}  // namespace

// -----------------------------------------------------------------------------
// RT trampolines
// -----------------------------------------------------------------------------

namespace {

// Ring-capacity tuning (per docs/spec.md § 2.3).
//
// The ring has to absorb short-term imbalance between the source and
// destination IOProcs. The dominant source of imbalance on real
// hardware is burst-delivery jitter from USB class-compliant devices
// (e.g. a Roland V31), which frequently deliver source samples in
// small bursts with gaps between them rather than one buffer per tick.
// (Clock drift is handled separately by the drift tracker + the
// AudioConverter ratio; the ring still has to be large enough for the
// PI controller to react before an under/overrun.)
//
// Phase 4's original sizing (`4 × max_buffer`, floor 256 frames) was
// tuned against the simulated backend, which delivers synchronously
// and never bursts. Real-hardware Phase-6 testing (V31 → Apollo, 48 k,
// 64-frame device buffers) produced u1000+ underruns in under a minute
// because 4 × 64 = 256 frames ≈ 5 ms is smaller than a typical USB
// delivery gap. `kRingSafe` (8 × max_buffer, floor 4096 frames ≈ 85 ms
// at 48 k) is the default — generous for a routing tool that is not
// used for live monitoring, but well above any burst gap we have
// measured.
//
// Phase 6 refinement #6 adds `kRingLowLatency` — an opt-in tighter
// preset selected per-route when the user flips "Low latency" on. It
// halves the multiplier and drops the floor to 512 frames (~10.6 ms
// at 48 k). Risk: USB-burst sources will underrun; the UI copy makes
// that explicit and the user can flip back if they hear clicks.
struct RingSizing {
    std::uint32_t multiplier;
    std::uint32_t floor;
};
constexpr RingSizing kRingSafe       { /*mult*/ 8, /*floor*/ 4096 };
constexpr RingSizing kRingLowLatency { /*mult*/ 3, /*floor*/  512 };

constexpr std::uint32_t kRtScratchMaxFrames = 8192;


void inputIOProcCallback(const float* samples,
                         std::uint32_t frame_count,
                         std::uint32_t channel_count,
                         void* user_data) {
    auto* r = static_cast<RouteRecord*>(user_data);
    if (r->ring == nullptr) return;
    if (channel_count != r->source_total_channels) {
        if (!r->reported_channel_mismatch.exchange(true, std::memory_order_relaxed)) {
            tryPushLog(r->log_queue, jbox::rt::kLogChannelMismatch, r->id,
                       static_cast<std::uint64_t>(r->source_total_channels),
                       static_cast<std::uint64_t>(channel_count));
        }
        return;
    }

    const std::uint32_t out_channels = r->channels_count;
    float* scratch = r->input_scratch.data();

    // Extract selected source channels into interleaved scratch, and
    // track per-channel peak in one pass. Per-channel outer loop gives
    // us O(channels_count) atomic meter updates rather than
    // O(frames × channels), at the cost of a strided read of `samples`
    // — acceptable given typical block sizes (32–256 frames) and small
    // channel counts keep both buffers in L1.
    for (std::uint32_t i = 0; i < out_channels; ++i) {
        const std::uint32_t src_ch = r->mapping[i].src;
        float peak = 0.0f;
        for (std::uint32_t f = 0; f < frame_count; ++f) {
            const float s = samples[f * channel_count + src_ch];
            scratch[f * out_channels + i] = s;
            const float a = std::fabs(s);
            if (a > peak) peak = a;
        }
        r->source_meter.updateMax(i, peak);
    }

    const std::size_t written = r->ring->writeFrames(scratch, frame_count);
    if (written < frame_count) {
        const auto total = r->overrun_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!r->reported_overrun.exchange(true, std::memory_order_relaxed)) {
            tryPushLog(r->log_queue, jbox::rt::kLogOverrun, r->id,
                       static_cast<std::uint64_t>(total));
        }
    }
    r->frames_produced.fetch_add(written, std::memory_order_relaxed);
}

// Pull callback handed to AudioConverterWrapper::convert. Reads from
// the route's ring buffer into the converter's scratch.
std::size_t ringPullCallback(float* dst, std::size_t frames, void* user) {
    auto* r = static_cast<RouteRecord*>(user);
    if (r == nullptr || r->ring == nullptr) return 0;
    return r->ring->readFrames(dst, frames);
}

void outputIOProcCallback(float* samples,
                          std::uint32_t frame_count,
                          std::uint32_t channel_count,
                          void* user_data) {
    auto* r = static_cast<RouteRecord*>(user_data);
    if (r->ring == nullptr || r->converter == nullptr) return;
    if (channel_count != r->dest_total_channels) {
        if (!r->reported_channel_mismatch.exchange(true, std::memory_order_relaxed)) {
            tryPushLog(r->log_queue, jbox::rt::kLogChannelMismatch, r->id,
                       static_cast<std::uint64_t>(r->dest_total_channels),
                       static_cast<std::uint64_t>(channel_count));
        }
        return;
    }

    // Apply any pending rate update from the control thread. RT-thread-
    // local `last_applied_rate` short-circuits repeated proposals.
    //
    // Deadband (see rate_deadband.hpp): only push the rate through to
    // the Apple AudioConverter when the proposed change crosses ~1 ppm
    // of nominal. Every setInputRate() call flushes the converter's
    // polyphase filter state, consuming ~16 extra input frames as it
    // re-primes (characterized in audio_converter_wrapper_test.cpp,
    // [hypothesis] case). At 100 Hz PI-tick rate that was 1600 extra
    // frames/sec drained from the ring — the real-hardware origin of
    // the click artifacts and slow-growing underrun counter we saw on
    // the V31 → Apollo route. Gating sub-ppm updates eliminates the
    // flush storm without meaningfully reducing drift-correction
    // authority (1 ppm ≪ the ~10 ppm audible threshold).
    const double target = r->target_input_rate.load(std::memory_order_relaxed);
    if (shouldApplyRate(target, r->last_applied_rate, r->nominal_src_rate)) {
        r->converter->setInputRate(target);
        r->last_applied_rate = target;
    }

    const std::uint32_t in_channels = r->channels_count;
    float* scratch = r->output_scratch.data();

    const std::size_t produced = r->converter->convert(
        scratch, frame_count, &ringPullCallback, r);
    if (produced < frame_count) {
        const auto total = r->underrun_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!r->reported_underrun.exchange(true, std::memory_order_relaxed)) {
            tryPushLog(r->log_queue, jbox::rt::kLogUnderrun, r->id,
                       static_cast<std::uint64_t>(total));
        }
        std::memset(scratch + produced * in_channels, 0,
                    (frame_count - produced) * in_channels * sizeof(float));
    }

    // Place converted samples on the destination device's selected
    // output channels and track per-channel peak in one pass. Same
    // channel-outer pattern as the input side to keep the atomic cost
    // at O(channels_count) instead of O(frames × channels).
    for (std::uint32_t i = 0; i < in_channels; ++i) {
        const std::uint32_t dst_ch = r->mapping[i].dst;
        float peak = 0.0f;
        for (std::uint32_t f = 0; f < frame_count; ++f) {
            const float s = scratch[f * in_channels + i];
            samples[f * channel_count + dst_ch] = s;
            const float a = std::fabs(s);
            if (a > peak) peak = a;
        }
        r->dest_meter.updateMax(i, peak);
    }

    r->frames_consumed.fetch_add(produced, std::memory_order_relaxed);
}

}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

RouteManager::RouteManager(DeviceManager& dm,
                           jbox::rt::DefaultRtLogQueue* log_queue)
    : dm_(dm), log_queue_(log_queue) {}

RouteManager::~RouteManager() {
    for (auto& [id, rec] : routes_) {
        if (rec->state == JBOX_ROUTE_STATE_RUNNING) {
            teardown(*rec);
        }
    }
}

static void setError(jbox_error_t* err, jbox_error_code_t code, const char* message) {
    if (err != nullptr) {
        err->code = code;
        err->message = message;
    }
}

jbox_route_id_t RouteManager::addRoute(const RouteConfig& cfg, jbox_error_t* err) {
    if (cfg.source_uid.empty() || cfg.dest_uid.empty()) {
        setError(err, JBOX_ERR_INVALID_ARGUMENT, "source_uid and dest_uid are required");
        return JBOX_INVALID_ROUTE_ID;
    }
    const auto mapping_err = validate({cfg.mapping.data(), cfg.mapping.size()});
    if (mapping_err != ChannelMapperError::kOk) {
        setError(err, JBOX_ERR_MAPPING_INVALID, channelMapperErrorName(mapping_err));
        return JBOX_INVALID_ROUTE_ID;
    }

    auto rec = std::make_unique<RouteRecord>();
    rec->id             = next_id_++;
    rec->name           = cfg.name;
    rec->source_uid     = cfg.source_uid;
    rec->dest_uid       = cfg.dest_uid;
    rec->mapping        = cfg.mapping;
    rec->low_latency    = cfg.low_latency;
    rec->channels_count = static_cast<std::uint32_t>(cfg.mapping.size());
    rec->state          = JBOX_ROUTE_STATE_STOPPED;
    rec->log_queue      = log_queue_;

    const jbox_route_id_t id = rec->id;
    routes_[id] = std::move(rec);
    return id;
}

jbox_error_code_t RouteManager::removeRoute(jbox_route_id_t id) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    if (it->second->state == JBOX_ROUTE_STATE_RUNNING ||
        it->second->state == JBOX_ROUTE_STATE_WAITING) {
        teardown(*it->second);
    }
    routes_.erase(it);
    return JBOX_OK;
}

jbox_error_code_t RouteManager::startRoute(jbox_route_id_t id) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    RouteRecord& r = *it->second;
    if (r.state == JBOX_ROUTE_STATE_RUNNING) return JBOX_OK;
    return attemptStart(r);
}

jbox_error_code_t RouteManager::stopRoute(jbox_route_id_t id) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    RouteRecord& r = *it->second;
    if (r.state == JBOX_ROUTE_STATE_STOPPED) return JBOX_OK;
    teardown(r);
    return JBOX_OK;
}

jbox_error_code_t RouteManager::pollStatus(jbox_route_id_t id,
                                           jbox_route_status_t* out) const {
    if (out == nullptr) return JBOX_ERR_INVALID_ARGUMENT;
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    const RouteRecord& r = *it->second;
    out->state                = r.state;
    out->last_error           = r.last_error;
    out->frames_produced      = r.frames_produced.load(std::memory_order_relaxed);
    out->frames_consumed      = r.frames_consumed.load(std::memory_order_relaxed);
    out->underrun_count       = r.underrun_count.load(std::memory_order_relaxed);
    out->overrun_count        = r.overrun_count.load(std::memory_order_relaxed);
    out->estimated_latency_us = r.estimated_latency_us;
    return JBOX_OK;
}

std::size_t RouteManager::pollMeters(jbox_route_id_t   id,
                                     jbox_meter_side_t side,
                                     float*            out_peaks,
                                     std::size_t       max_channels) {
    if (out_peaks == nullptr || max_channels == 0) return 0;
    if (side != JBOX_METER_SIDE_SOURCE && side != JBOX_METER_SIDE_DEST) return 0;
    auto it = routes_.find(id);
    if (it == routes_.end()) return 0;
    RouteRecord& r = *it->second;
    if (r.state != JBOX_ROUTE_STATE_RUNNING) return 0;

    auto& meter = (side == JBOX_METER_SIDE_SOURCE) ? r.source_meter : r.dest_meter;
    const std::size_t n = std::min<std::size_t>(r.channels_count, max_channels);
    for (std::size_t i = 0; i < n; ++i) {
        out_peaks[i] = meter.readAndReset(i);
    }
    return n;
}

std::vector<RouteRecord*> RouteManager::runningRoutes() {
    std::vector<RouteRecord*> out;
    out.reserve(routes_.size());
    for (auto& [id, rec] : routes_) {
        if (rec->state == JBOX_ROUTE_STATE_RUNNING) out.push_back(rec.get());
    }
    return out;
}

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

jbox_error_code_t RouteManager::attemptStart(RouteRecord& r) {
    // Fresh (re)start — reset edge-triggered RT log flags so any
    // first-of-kind event after this start gets reported again.
    r.reported_underrun.store(false, std::memory_order_relaxed);
    r.reported_overrun.store(false, std::memory_order_relaxed);
    r.reported_channel_mismatch.store(false, std::memory_order_relaxed);

    // Clear any stale meter peaks from a previous run. readAndReset
    // both reads (discarded) and atomically zeroes the stored peak.
    for (std::size_t i = 0; i < jbox::rt::kAtomicMeterMaxChannels; ++i) {
        (void)r.source_meter.readAndReset(i);
        (void)r.dest_meter.readAndReset(i);
    }

    // Resolve devices.
    const BackendDeviceInfo* src = dm_.findByUid(r.source_uid);
    const BackendDeviceInfo* dst = dm_.findByUid(r.dest_uid);
    if (src == nullptr || dst == nullptr) {
        r.state = JBOX_ROUTE_STATE_WAITING;
        tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting, r.id,
                   src == nullptr ? 1u : 0u,
                   dst == nullptr ? 1u : 0u);
        return JBOX_OK;  // not an error; user will retry once devices appear
    }

    r.state = JBOX_ROUTE_STATE_STARTING;

    r.source_total_channels = src->input_channel_count;
    r.dest_total_channels   = dst->output_channel_count;

    // Validate channel indices against devices.
    for (const auto& edge : r.mapping) {
        if (static_cast<std::uint32_t>(edge.src) >= r.source_total_channels ||
            static_cast<std::uint32_t>(edge.dst) >= r.dest_total_channels) {
            r.state = JBOX_ROUTE_STATE_ERROR;
            r.last_error = JBOX_ERR_MAPPING_INVALID;
            return JBOX_ERR_MAPPING_INVALID;
        }
    }

    // Size the ring buffer. See the ring-capacity note near the top of
    // this file for the rationale. Safe preset: 8 × device-buffer /
    // 4096 floor (absorbs USB burst-delivery jitter). Low-latency
    // preset: 3 × / 512 floor (opt-in per route).
    const RingSizing sizing = r.low_latency ? kRingLowLatency : kRingSafe;
    const std::uint32_t max_buffer =
        std::max(src->buffer_frame_size, dst->buffer_frame_size);
    const std::uint32_t capacity_frames =
        std::max<std::uint32_t>(sizing.floor,
                                max_buffer > 0 ? max_buffer * sizing.multiplier
                                               : sizing.floor);

    r.ring_storage.assign(static_cast<std::size_t>(capacity_frames) * r.channels_count, 0.0f);
    r.ring = std::make_unique<jbox::rt::RingBuffer>(
        r.ring_storage.data(), capacity_frames, r.channels_count);

    r.input_scratch.assign(
        static_cast<std::size_t>(kRtScratchMaxFrames) * r.channels_count, 0.0f);
    r.output_scratch.assign(
        static_cast<std::size_t>(kRtScratchMaxFrames) * r.channels_count, 0.0f);

    r.nominal_src_rate = src->nominal_sample_rate > 0.0
                             ? src->nominal_sample_rate
                             : 48000.0;
    r.nominal_dst_rate = dst->nominal_sample_rate > 0.0
                             ? dst->nominal_sample_rate
                             : r.nominal_src_rate;

    try {
        r.converter = std::make_unique<jbox::rt::AudioConverterWrapper>(
            r.nominal_src_rate, r.nominal_dst_rate, r.channels_count);
    } catch (const std::exception&) {
        releaseRouteResources(r);
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_INTERNAL;
        return JBOX_ERR_INTERNAL;
    }

    r.target_input_rate.store(r.nominal_src_rate, std::memory_order_relaxed);
    r.last_applied_rate = r.nominal_src_rate;
    r.tracker.reset();

    // Per-route latency estimate — see docs/spec.md § 2.12. Computed
    // once here, on the control thread, from HAL-reported latency +
    // safety offset + buffer sizes + the drift-sampler ring setpoint
    // + the converter's leading prime frames. Not recomputed while
    // running; stop → start refreshes it if any component changes.
    LatencyComponents lc{};
    lc.src_hal_latency_frames   = src->input_device_latency_frames;
    lc.src_safety_offset_frames = src->input_safety_offset_frames;
    lc.src_buffer_frames        = src->buffer_frame_size;
    lc.ring_target_fill_frames  =
        static_cast<std::uint32_t>(r.ring->usableCapacityFrames() / 2);
    lc.converter_prime_frames   = r.converter->primeLeadingFrames();
    lc.dst_buffer_frames        = dst->buffer_frame_size;
    lc.dst_safety_offset_frames = dst->output_safety_offset_frames;
    lc.dst_hal_latency_frames   = dst->output_device_latency_frames;
    lc.src_sample_rate_hz       = r.nominal_src_rate;
    lc.dst_sample_rate_hz       = r.nominal_dst_rate;
    r.latency_components   = lc;
    r.estimated_latency_us = estimateLatencyMicroseconds(lc);

    // Attach to the source device's mux (input side). Creates the mux
    // on demand; subsequent routes using this device will share it.
    DeviceIOMux& src_mux = getOrCreateMux(
        r.source_uid,
        src->input_channel_count,
        src->output_channel_count);
    if (!src_mux.attachInput(&r, &inputIOProcCallback, &r, r.low_latency)) {
        destroyMuxIfUnused(r.source_uid);
        releaseRouteResources(r);
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
    }
    r.attached_src_mux = &src_mux;

    // Attach to the destination device's mux (output side). If the
    // destination UID happens to equal the source UID, this is the
    // same mux — attaching the output direction is still valid.
    DeviceIOMux& dst_mux = getOrCreateMux(
        r.dest_uid,
        dst->input_channel_count,
        dst->output_channel_count);
    if (!dst_mux.attachOutput(&r, &outputIOProcCallback, &r, r.low_latency)) {
        // releaseRouteResources detaches the already-attached input
        // side and cleans up any mux we created.
        releaseRouteResources(r);
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
    }
    r.attached_dst_mux = &dst_mux;

    r.state      = JBOX_ROUTE_STATE_RUNNING;
    r.last_error = JBOX_OK;
    tryPushLog(log_queue_, jbox::rt::kLogRouteStarted, r.id,
               static_cast<std::uint64_t>(r.source_total_channels),
               static_cast<std::uint64_t>(r.dest_total_channels));
    return JBOX_OK;
}

void RouteManager::releaseRouteResources(RouteRecord& r) {
    // Detach from muxes first. Each detach blocks for one grace period
    // after the atomic swap, after which no RT callback can still be
    // referencing r's RT resources and it is safe to release them.
    if (r.attached_src_mux != nullptr) {
        r.attached_src_mux->detachInput(&r);
        r.attached_src_mux = nullptr;
        destroyMuxIfUnused(r.source_uid);
    }
    if (r.attached_dst_mux != nullptr) {
        r.attached_dst_mux->detachOutput(&r);
        r.attached_dst_mux = nullptr;
        destroyMuxIfUnused(r.dest_uid);
    }

    if (r.converter) {
        r.converter->reset();
        r.converter.reset();
    }
    r.target_input_rate.store(0.0, std::memory_order_relaxed);
    r.last_applied_rate = 0.0;
    r.tracker.reset();

    r.ring.reset();
    r.ring_storage.clear();
    r.input_scratch.clear();
    r.output_scratch.clear();
    r.latency_components   = {};
    r.estimated_latency_us = 0;
    // r.state and r.last_error are intentionally not touched here;
    // callers set them to the appropriate value after this call.
    // Counters are preserved across stop/start cycles for visibility.
}

DeviceIOMux& RouteManager::getOrCreateMux(const std::string& uid,
                                          std::uint32_t input_channel_count,
                                          std::uint32_t output_channel_count) {
    auto it = muxes_.find(uid);
    if (it != muxes_.end()) return *it->second;
    auto mux = std::make_unique<DeviceIOMux>(
        dm_.backend(), uid, input_channel_count, output_channel_count);
    DeviceIOMux& ref = *mux;
    muxes_.emplace(uid, std::move(mux));
    return ref;
}

void RouteManager::destroyMuxIfUnused(const std::string& uid) {
    auto it = muxes_.find(uid);
    if (it == muxes_.end()) return;
    if (!it->second->hasAnyInput() && !it->second->hasAnyOutput()) {
        muxes_.erase(it);
    }
}

void RouteManager::teardown(RouteRecord& r) {
    const bool was_active = (r.state == JBOX_ROUTE_STATE_RUNNING ||
                             r.state == JBOX_ROUTE_STATE_STARTING ||
                             r.state == JBOX_ROUTE_STATE_WAITING);
    releaseRouteResources(r);
    r.state      = JBOX_ROUTE_STATE_STOPPED;
    r.last_error = JBOX_OK;
    if (was_active) {
        tryPushLog(log_queue_, jbox::rt::kLogRouteStopped, r.id);
    }
}

}  // namespace jbox::control
