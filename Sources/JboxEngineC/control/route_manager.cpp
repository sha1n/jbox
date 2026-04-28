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
// preset selected per-route when the user picks "Low latency". It
// halves the multiplier and drops the floor to 512 frames (~10.6 ms
// at 48 k). Risk: USB-burst sources will underrun; the UI copy makes
// that explicit and the user can step back.
//
// A later pass added a third tier, `kRingPerformance` (2× / 256 floor
// ≈ 5.3 ms at 48 k) paired with a drift-setpoint change from
// usableCapacity/2 → usableCapacity/4. That second knob is the real
// lever: halving the steady-state ring residency shaves off another
// ~2–5 ms without shrinking the burst-protection capacity. Drum
// monitoring on cross-device V31 → Apollo rigs was the motivating
// scenario; users must opt in and accept a visibly higher underrun
// probability.
struct RingSizing {
    std::uint32_t multiplier;
    std::uint32_t floor;
    // Fraction of `usableCapacityFrames` the drift sampler aims to
    // maintain in the ring at steady state. 0.5 is the symmetric
    // (safe) operating point; 0.25 gives aggressive drain headroom at
    // the cost of symmetric underrun margin.
    double        target_fill_fraction;
};
constexpr RingSizing kRingSafe        { /*mult*/ 8, /*floor*/ 4096, 0.5  };
constexpr RingSizing kRingLowLatency  { /*mult*/ 3, /*floor*/  512, 0.5  };
constexpr RingSizing kRingPerformance { /*mult*/ 2, /*floor*/  256, 0.25 };

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

// Direct-monitor fast-path callback. Fires once per device IOProc
// tick on an aggregate / duplex device with both input and output
// buffers populated. Copies the mapped source channels straight into
// the matching destination channels with no ring, no SRC, no drift
// correction — latency = HAL + one buffer period. RT-safe.
void duplexIOProcCallback(const float* input_samples,
                          std::uint32_t input_frame_count,
                          std::uint32_t input_channel_count,
                          float*        output_samples,
                          std::uint32_t output_frame_count,
                          std::uint32_t /*output_channel_count_hint*/,
                          void* user_data) {
    auto* r = static_cast<RouteRecord*>(user_data);
    if (input_samples == nullptr || output_samples == nullptr) return;
    if (input_channel_count != r->source_total_channels ||
        /*output_channel_count_hint*/ r->dest_total_channels == 0) {
        if (!r->reported_channel_mismatch.exchange(true, std::memory_order_relaxed)) {
            tryPushLog(r->log_queue, jbox::rt::kLogChannelMismatch, r->id,
                       static_cast<std::uint64_t>(r->source_total_channels),
                       static_cast<std::uint64_t>(input_channel_count));
        }
        return;
    }

    const std::uint32_t out_ch_count = r->dest_total_channels;
    const std::uint32_t frames =
        input_frame_count < output_frame_count ? input_frame_count
                                               : output_frame_count;

    for (std::uint32_t i = 0; i < r->channels_count; ++i) {
        const std::uint32_t src_ch = r->mapping[i].src;
        const std::uint32_t dst_ch = r->mapping[i].dst;
        float peak_in  = 0.0f;
        float peak_out = 0.0f;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float s = input_samples[f * input_channel_count + src_ch];
            output_samples[f * out_ch_count + dst_ch] = s;
            const float a = std::fabs(s);
            if (a > peak_in)  peak_in  = a;
            if (a > peak_out) peak_out = a;
        }
        r->source_meter.updateMax(i, peak_in);
        r->dest_meter.updateMax(i, peak_out);
    }

    r->frames_produced.fetch_add(frames, std::memory_order_relaxed);
    r->frames_consumed.fetch_add(frames, std::memory_order_relaxed);
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
    rec->latency_mode   = cfg.latency_mode;
    rec->buffer_frames_override = cfg.buffer_frames;
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
    // 7.6.3 dispose retry: if the duplex teardown left a residual
    // IOProc handle, take one more best-effort attempt before erasing.
    // The route record is going away; this is the route's last chance
    // to clean up its kernel-side resource. A persistent refusal here
    // is a logged leak — 7.6.4's HAL listeners will surface it the
    // next time the device topology changes.
    if (it->second->duplex_ioproc_id != kInvalidIOProcId) {
        const IOProcId stuck = it->second->duplex_ioproc_id;
        if (!dm_.backend().closeCallback(stuck)) {
            tryPushLog(log_queue_, jbox::rt::kLogTeardownFailure, id,
                       0, static_cast<std::uint64_t>(stuck));
        }
    }
    routes_.erase(it);
    return JBOX_OK;
}

jbox_error_code_t RouteManager::renameRoute(jbox_route_id_t id,
                                            const std::string& new_name) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    // `name` is only read on the control thread (RT callbacks never
    // touch it), so a plain assign is race-free here.
    it->second->name = new_name;
    return JBOX_OK;
}

std::string RouteManager::routeName(jbox_route_id_t id) const {
    auto it = routes_.find(id);
    if (it == routes_.end()) return {};
    return it->second->name;
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

jbox_error_code_t RouteManager::pollLatencyComponents(
    jbox_route_id_t id,
    jbox_route_latency_components_t* out) const {
    if (out == nullptr) return JBOX_ERR_INVALID_ARGUMENT;
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    const RouteRecord& r = *it->second;
    const LatencyComponents& c = r.latency_components;
    out->src_hal_latency_frames   = c.src_hal_latency_frames;
    out->src_safety_offset_frames = c.src_safety_offset_frames;
    out->src_buffer_frames        = c.src_buffer_frames;
    out->ring_target_fill_frames  = c.ring_target_fill_frames;
    out->converter_prime_frames   = c.converter_prime_frames;
    out->dst_buffer_frames        = c.dst_buffer_frames;
    out->dst_safety_offset_frames = c.dst_safety_offset_frames;
    out->dst_hal_latency_frames   = c.dst_hal_latency_frames;
    out->src_sample_rate_hz       = c.src_sample_rate_hz;
    out->dst_sample_rate_hz       = c.dst_sample_rate_hz;
    out->total_us                 = r.estimated_latency_us;
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

namespace {
// 7.6.5 wake-recovery schedule. Linear backoff: attempt N waits
// (N-1) * kWakeBackoffBaseMs after the prior attempt fired. With
// base=200ms and budget=3, attempts fire at +0ms, +200ms, +600ms
// cumulative from the wake event. After the last attempt the
// budget is exhausted and the route stays WAITING+SYSTEM_SUSPENDED;
// 7.6.4's device-change watcher remains the long-tail recovery
// mechanism for devices that come back later still.
constexpr std::uint32_t kWakeBackoffBaseMs = 200;
constexpr std::uint8_t  kWakeRetryBudget   = 3;
}  // namespace

void RouteManager::prepareForSleep() {
    // Synchronous teardown of every running route. Called from the
    // PowerStateWatcher's sleep handler before it acks macOS, so
    // resources are released and IOProcs unregistered before the
    // system actually powers down. teardown() sets state=STOPPED +
    // last_error=JBOX_OK; we override both immediately afterward
    // to mark the route as suspended-pending-recovery.
    for (auto& [id, rec] : routes_) {
        auto& r = *rec;
        if (r.state != JBOX_ROUTE_STATE_RUNNING) continue;
        teardown(r);
        r.state      = JBOX_ROUTE_STATE_WAITING;
        r.last_error = JBOX_ERR_SYSTEM_SUSPENDED;
        // wake_retries_remaining stays 0 here — recoverFromWake will
        // prime it once the system reports kPoweredOn.
        tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting, id, 0u, 0u);
    }
}

void RouteManager::recoverFromWake() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, rec] : routes_) {
        (void)id;
        auto& r = *rec;
        if (r.state != JBOX_ROUTE_STATE_WAITING) continue;
        if (r.last_error != JBOX_ERR_SYSTEM_SUSPENDED) continue;
        // Prime the retry budget for an immediate first attempt;
        // subsequent attempts get linear-backoff schedules computed
        // by tickWakeRetries.
        r.wake_retries_remaining = kWakeRetryBudget;
        r.wake_next_retry_at     = now;
    }
}

void RouteManager::tickWakeRetries(
    std::chrono::steady_clock::time_point now) {
    bool refreshed = false;
    for (auto& [id, rec] : routes_) {
        (void)id;
        auto& r = *rec;
        if (r.wake_retries_remaining == 0) continue;
        if (now < r.wake_next_retry_at) continue;

        if (!refreshed) {
            // Refresh once per pass — many routes may share devices,
            // so a single refresh covers them all. Also picks up any
            // device that finished re-enumerating since the last tick.
            dm_.refresh();
            refreshed = true;
        }

        // attemptStart sets state to RUNNING on success; to WAITING
        // (with last_error untouched) when devices are still missing;
        // or to ERROR on attach-side failures. We preserve SYSTEM_
        // SUSPENDED for the missing-device case because attemptStart
        // doesn't set last_error on its WAITING path; the ERROR path
        // overrides last_error with the relevant code, which is
        // appropriate (the route's truly stuck for non-device
        // reasons).
        (void)attemptStart(r);

        if (r.state == JBOX_ROUTE_STATE_RUNNING) {
            r.wake_retries_remaining = 0;
            continue;
        }

        --r.wake_retries_remaining;
        if (r.wake_retries_remaining > 0) {
            // Number of attempts already fired (1, 2). The wait until
            // the next attempt is `fired_attempt * kWakeBackoffBaseMs`,
            // so total cumulative time from priming is
            //   t0 + sum_{i=1..N-1}(i * baseMs)
            // For budget=3 + base=200 that lands on +0ms, +200ms,
            // +600ms — the backoff schedule the test cases pin.
            const std::uint32_t fired_attempt =
                static_cast<std::uint32_t>(kWakeRetryBudget) -
                static_cast<std::uint32_t>(r.wake_retries_remaining);
            r.wake_next_retry_at = now + std::chrono::milliseconds(
                fired_attempt * kWakeBackoffBaseMs);
        }
    }
}

void RouteManager::handleDeviceChanges(
    const std::vector<DeviceChangeEvent>& events) {
    // 7.6.4: react to backend-emitted topology changes.
    //
    // Two kinds of work to do per drained event batch:
    //
    //   (1) Force-stop running routes whose source / destination
    //       UID matches a kDeviceIsNotAlive event. The route
    //       transitions to WAITING with last_error = DEVICE_GONE
    //       (vs JBOX_OK on initial-WAITING from startRoute) so the
    //       UI can differentiate "waiting on first plug-in" from
    //       "yanked, recovering". Idempotent: an already-WAITING
    //       route on the same UID is left alone.
    //
    //   (2) Refresh the device manager + retry every WAITING route
    //       on any kDeviceListChanged or kAggregateMembersChanged
    //       event. attemptStart already handles "still missing →
    //       stay WAITING" gracefully, so a burst of events during
    //       a sample-rate cascade collapses naturally.
    //
    // Coalescing is implicit: two passes — losses first, then
    // reappearance + refresh — so a single batch carrying both
    // kinds (e.g. a device flap) leaves the manager in a coherent
    // state at the end. No timer-based debounce inside this method;
    // see `device_change_watcher.hpp` for the rationale.
    if (events.empty()) return;

    bool any_list_change = false;
    for (const auto& ev : events) {
        switch (ev.kind) {
            case DeviceChangeEvent::kDeviceIsNotAlive: {
                if (ev.uid.empty()) continue;
                for (auto& [id, rec] : routes_) {
                    auto& r = *rec;
                    if (r.state != JBOX_ROUTE_STATE_RUNNING) continue;
                    if (r.source_uid != ev.uid && r.dest_uid != ev.uid) continue;
                    // Tear down — releases ring / converter / scratch /
                    // mux attachments — then transition to WAITING with
                    // the device-loss origin code. teardown() always
                    // sets state = STOPPED + last_error = JBOX_OK; we
                    // override both immediately afterward.
                    teardown(r);
                    r.state      = JBOX_ROUTE_STATE_WAITING;
                    r.last_error = JBOX_ERR_DEVICE_GONE;
                    tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting,
                               id,
                               r.source_uid == ev.uid ? 1u : 0u,
                               r.dest_uid   == ev.uid ? 1u : 0u);
                }
                break;
            }
            case DeviceChangeEvent::kDeviceListChanged:
            case DeviceChangeEvent::kAggregateMembersChanged: {
                any_list_change = true;
                break;
            }
        }
    }

    // (2) — collapse all list-changed / members-changed events into a
    // single refresh + retry pass. Refresh updates the cached
    // enumeration so attemptStart's `dm_.findByUid` lookups see the
    // new topology.
    if (any_list_change) {
        dm_.refresh();
        for (auto& [id, rec] : routes_) {
            auto& r = *rec;
            if (r.state != JBOX_ROUTE_STATE_WAITING) continue;
            (void)attemptStart(r);
        }
    }
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

    // Direct-monitor fast path. When Performance mode is selected and
    // source and destination are the same device UID (typically an
    // aggregate device wrapping both physical interfaces), we bypass
    // the ring buffer and AudioConverter entirely: a single duplex
    // IOProc copies input samples to output in one callback. Latency
    // collapses to HAL + one buffer period. Exclusive: the backend
    // refuses the duplex attach if any IOProc already targets the
    // device, so a pre-existing mux on this UID is a hard block.
    if (r.latency_mode == 2 && r.source_uid == r.dest_uid) {
        // Direct-monitor fast path: bypass ring + converter and copy
        // input→output in one duplex IOProc. The HAL buffer size is
        // whatever the device is at — Phase 7.6 dropped Jbox's hog
        // claim and buffer-shrink negotiation; users dial the buffer
        // in their interface software (UA Console, RME TotalMix,
        // Audio MIDI Setup, etc.) and Jbox respects it.

        // Exclusivity: no other route on this device.
        if (muxes_.find(r.source_uid) != muxes_.end()) {
            r.state = JBOX_ROUTE_STATE_ERROR;
            r.last_error = JBOX_ERR_DEVICE_BUSY;
            return JBOX_ERR_DEVICE_BUSY;
        }

        // 7.6.3 retry hook: a previous teardown's destroy may have
        // refused, leaving r.duplex_ioproc_id populated. Retry the
        // close before opening fresh — otherwise openDuplexCallback
        // would refuse on the device's still-registered slot and the
        // route would be stuck in ERROR until the user removes and
        // recreates it. Persistent refusal (the retry itself refuses)
        // returns DEVICE_BUSY with a fresh failure log; the next start
        // tries again.
        if (r.duplex_ioproc_id != kInvalidIOProcId) {
            if (dm_.backend().closeCallback(r.duplex_ioproc_id)) {
                r.duplex_ioproc_id = kInvalidIOProcId;
            } else {
                tryPushLog(log_queue_, jbox::rt::kLogTeardownFailure, r.id,
                           0, static_cast<std::uint64_t>(r.duplex_ioproc_id));
                r.state = JBOX_ROUTE_STATE_ERROR;
                r.last_error = JBOX_ERR_DEVICE_BUSY;
                return JBOX_ERR_DEVICE_BUSY;
            }
        }

        r.nominal_src_rate = src->nominal_sample_rate > 0.0
                                 ? src->nominal_sample_rate
                                 : 48000.0;
        r.nominal_dst_rate = r.nominal_src_rate;

        // Latency components for the fast path: no ring, no converter.
        // The device buffer contributes exactly once (one callback
        // carries both directions), so we drop dst_buffer_frames.
        LatencyComponents lc{};
        lc.src_hal_latency_frames   = src->input_device_latency_frames;
        lc.src_safety_offset_frames = src->input_safety_offset_frames;
        lc.src_buffer_frames        = src->buffer_frame_size;
        lc.ring_target_fill_frames  = 0;
        lc.converter_prime_frames   = 0;
        lc.dst_buffer_frames        = 0;
        lc.dst_safety_offset_frames = dst->output_safety_offset_frames;
        lc.dst_hal_latency_frames   = dst->output_device_latency_frames;
        lc.src_sample_rate_hz       = r.nominal_src_rate;
        lc.dst_sample_rate_hz       = r.nominal_dst_rate;

        // Express the user's per-route HAL buffer-frame-size
        // preference, if any (Superior-Drummer-style: a single
        // property write, no hog claim, no eviction). On a duplex
        // self-route src == dst, so the call covers both sides.
        // macOS resolves the actual buffer with `max-across-clients`
        // — co-resident apps with a bigger ask will keep the device
        // at their value until they stop asking.
        if (r.buffer_frames_override > 0) {
            dm_.backend().setBufferFrameSize(
                r.source_uid, r.buffer_frames_override);
        }

        // Read the device's current buffer frame size for the latency
        // pill. Reflects the post-write value when an override
        // landed; otherwise reflects whatever the device is at.
        const std::uint32_t current_buffer =
            dm_.backend().currentBufferFrameSize(r.source_uid);
        if (current_buffer > 0) {
            lc.src_buffer_frames = current_buffer;
        }
        r.latency_components   = lc;
        r.estimated_latency_us = estimateLatencyMicroseconds(lc);

        const IOProcId id = dm_.backend().openDuplexCallback(
            r.source_uid, &duplexIOProcCallback, &r);
        if (id == kInvalidIOProcId) {
            r.state = JBOX_ROUTE_STATE_ERROR;
            r.last_error = JBOX_ERR_DEVICE_BUSY;
            return JBOX_ERR_DEVICE_BUSY;
        }
        if (!dm_.backend().startDevice(r.source_uid)) {
            // startDevice returns false if already started; that's
            // fine — the IOProc will still receive callbacks.
        }

        r.duplex_ioproc_id = id;
        r.duplex_mode      = true;
        r.state            = JBOX_ROUTE_STATE_RUNNING;
        r.last_error       = JBOX_OK;
        tryPushLog(log_queue_, jbox::rt::kLogRouteStarted, r.id,
                   static_cast<std::uint64_t>(r.source_total_channels),
                   static_cast<std::uint64_t>(r.dest_total_channels));
        return JBOX_OK;
    }

    // Size the ring buffer. See the ring-capacity note near the top of
    // this file for the three presets and their trade-offs. The tier
    // is chosen by r.latency_mode (0=safe, 1=low, 2=performance).
    // Note: the ring is sized from the pre-shrink cached buffer
    // values because the ring is allocated here, before the mux
    // attach runs the shrink. Re-sizing post-shrink is not worth it:
    // ring *capacity* only affects burst-overflow headroom, and a
    // larger-than-strictly-necessary ring gives us *more* protection
    // against source bursts, not less. What actually determines
    // steady-state latency is the drift-sampler setpoint
    // (`ring_target_fill_frames`), which we compute from the
    // post-shrink buffer sizes after the attach below.
    const RingSizing sizing = [&]() {
        switch (r.latency_mode) {
            case 2: return kRingPerformance;
            case 1: return kRingLowLatency;
            default: return kRingSafe;
        }
    }();
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
            r.nominal_src_rate, r.nominal_dst_rate, r.channels_count,
            resamplerQuality());
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
    // once here, on the control thread. The `src_buffer_frames` /
    // `dst_buffer_frames` / `ring_target_fill_frames` fields are
    // refreshed below, after the mux attach runs the buffer shrink.
    LatencyComponents lc{};
    lc.src_hal_latency_frames   = src->input_device_latency_frames;
    lc.src_safety_offset_frames = src->input_safety_offset_frames;
    lc.src_buffer_frames        = src->buffer_frame_size;
    lc.ring_target_fill_frames  = static_cast<std::uint32_t>(
        static_cast<double>(r.ring->usableCapacityFrames()) *
        sizing.target_fill_fraction);
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
    if (!src_mux.attachInput(&r, &inputIOProcCallback, &r)) {
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
    if (!dst_mux.attachOutput(&r, &outputIOProcCallback, &r)) {
        // releaseRouteResources detaches the already-attached input
        // side and cleans up any mux we created.
        releaseRouteResources(r);
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
    }
    r.attached_dst_mux = &dst_mux;

    // Re-read each device's post-attach buffer frame size and fold
    // both the buffers and the drift-sampler setpoint into the
    // pill. The mux's buffer-shrink request (driven by
    // `mux_buffer_target`) runs inside attachInput / attachOutput
    // above, so the pre-attach pill is stale; without this refresh
    // the pill would report the pre-shrink buffer size AND the
    // drift setpoint would still be derived from the pre-shrink
    // ring sizing, both of which inflate the reported latency
    // (and the second one actually inflates the *real* steady-state
    // residency the sampler aims for — see drift_sampler.cpp).
    //
    // The ring *capacity* itself stays pre-shrink-sized; that just
    // means more burst-overflow headroom than strictly necessary,
    // which we happily keep. Only the setpoint changes.
    // Cross-device path: express the user's HAL buffer-frame-size
    // preference (if any) on both sides before the latency-pill
    // re-read. Same SD-style semantics as the duplex fast path —
    // single property write per device, no hog claim.
    if (r.buffer_frames_override > 0) {
        dm_.backend().setBufferFrameSize(
            r.source_uid, r.buffer_frames_override);
        if (r.dest_uid != r.source_uid) {
            dm_.backend().setBufferFrameSize(
                r.dest_uid, r.buffer_frames_override);
        }
    }

    // Refresh the latency pill from each device's current buffer
    // frame size — reflects the post-write value when an override
    // landed; otherwise reflects whatever the devices are at. The
    // ring's drift setpoint is recomputed from these values below
    // so the steady-state residency tracks the actual buffer.
    {
        const std::uint32_t applied_src =
            dm_.backend().currentBufferFrameSize(r.source_uid);
        if (applied_src > 0) lc.src_buffer_frames = applied_src;
        const std::uint32_t applied_dst =
            dm_.backend().currentBufferFrameSize(r.dest_uid);
        if (applied_dst > 0) lc.dst_buffer_frames = applied_dst;
        const std::uint32_t applied_max =
            std::max(lc.src_buffer_frames, lc.dst_buffer_frames);
        const std::uint32_t effective_capacity =
            std::max<std::uint32_t>(sizing.floor,
                                    applied_max > 0
                                        ? applied_max * sizing.multiplier
                                        : sizing.floor);
        lc.ring_target_fill_frames = static_cast<std::uint32_t>(
            static_cast<double>(effective_capacity) *
            sizing.target_fill_fraction);
        r.latency_components   = lc;
        r.estimated_latency_us = estimateLatencyMicroseconds(lc);
    }

    r.state      = JBOX_ROUTE_STATE_RUNNING;
    r.last_error = JBOX_OK;
    tryPushLog(log_queue_, jbox::rt::kLogRouteStarted, r.id,
               static_cast<std::uint64_t>(r.source_total_channels),
               static_cast<std::uint64_t>(r.dest_total_channels));
    return JBOX_OK;
}

void RouteManager::releaseRouteResources(RouteRecord& r) {
    // Duplex fast-path teardown runs first and is mutually exclusive
    // with the mux-based path (the fast path never registers with a
    // mux). closeCallback is synchronous on Core Audio — it stops the
    // IOProc and blocks until the last in-flight callback returns,
    // so any RT reference to `r` is gone before we release resources.
    //
    // 7.6.3 contract: when the destroy refuses (HAL returned non-noErr
    // / SimulatedBackend's failure-injection seam fired) we DO NOT
    // null `r.duplex_ioproc_id`. The next attemptStart's duplex path
    // sees the residual handle and retries the close before opening
    // fresh; removeRoute makes one final best-effort attempt. The
    // failure itself is logged immediately so an operator can see
    // exactly which route + IOProcId refused to tear down.
    //
    // The rest of releaseRouteResources still runs unconditionally —
    // ring / converter / scratch / mux attachments are released on
    // every call, success or failure. "State fully drained except
    // for the IOProc handle" is the explicit 7.6.3 contract.
    if (r.duplex_mode) {
        if (r.duplex_ioproc_id != kInvalidIOProcId) {
            if (dm_.backend().closeCallback(r.duplex_ioproc_id)) {
                r.duplex_ioproc_id = kInvalidIOProcId;
            } else {
                tryPushLog(log_queue_, jbox::rt::kLogTeardownFailure, r.id,
                           0, static_cast<std::uint64_t>(r.duplex_ioproc_id));
            }
        }
        dm_.backend().stopDevice(r.source_uid);
        r.duplex_mode = false;
    }

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
        dm_.backend(), uid, input_channel_count, output_channel_count,
        log_queue_);
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
