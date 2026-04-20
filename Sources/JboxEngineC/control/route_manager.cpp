// route_manager.cpp — route lifecycle and IOProc wiring.

#include "route_manager.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace jbox::control {

// -----------------------------------------------------------------------------
// RT trampolines
// -----------------------------------------------------------------------------

namespace {

// Configuration that keeps the RT path allocation-free:
//   ring capacity (frames) = 4 * max(source_buf, dest_buf)
// Per docs/spec.md § 2.3; yields ~5–10 ms headroom at 48k/64-frame buffers.
constexpr std::uint32_t kRingCapacityBase = 256;
constexpr std::uint32_t kRtScratchMaxFrames = 8192;

// Floor on the RCU grace period applied by DeviceIOMux after each
// atomic dispatch-list swap. Derived from the larger of the two
// devices' buffer periods, but never shorter than this.
constexpr double kGracePeriodFloorSeconds = 0.002;

double computeGracePeriod(const BackendDeviceInfo& info) {
    const double rate = info.nominal_sample_rate > 0.0
                            ? info.nominal_sample_rate
                            : 48000.0;
    const double frames = info.buffer_frame_size > 0
                              ? static_cast<double>(info.buffer_frame_size)
                              : 64.0;
    const double buffer_period = frames / rate;
    return std::max(kGracePeriodFloorSeconds, 1.5 * buffer_period);
}

void inputIOProcCallback(const float* samples,
                         std::uint32_t frame_count,
                         std::uint32_t channel_count,
                         void* user_data) {
    auto* r = static_cast<RouteRecord*>(user_data);
    if (r->ring == nullptr) return;
    if (channel_count != r->source_total_channels) return;  // unexpected

    const std::uint32_t out_channels = r->channels_count;
    float* scratch = r->input_scratch.data();

    // Extract selected source channels into interleaved scratch.
    for (std::uint32_t f = 0; f < frame_count; ++f) {
        const float* src_frame = samples + f * channel_count;
        float*       dst_frame = scratch + f * out_channels;
        for (std::uint32_t i = 0; i < out_channels; ++i) {
            dst_frame[i] = src_frame[r->mapping[i].src];
        }
    }

    const std::size_t written = r->ring->writeFrames(scratch, frame_count);
    if (written < frame_count) {
        r->overrun_count.fetch_add(1, std::memory_order_relaxed);
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
    if (channel_count != r->dest_total_channels) return;

    // Apply any pending rate update from the control thread. RT-thread-
    // local `last_applied_rate` suppresses redundant setProperty calls.
    const double target = r->target_input_rate.load(std::memory_order_relaxed);
    if (target > 0.0 && target != r->last_applied_rate) {
        r->converter->setInputRate(target);
        r->last_applied_rate = target;
    }

    const std::uint32_t in_channels = r->channels_count;
    float* scratch = r->output_scratch.data();

    const std::size_t produced = r->converter->convert(
        scratch, frame_count, &ringPullCallback, r);
    if (produced < frame_count) {
        r->underrun_count.fetch_add(1, std::memory_order_relaxed);
        std::memset(scratch + produced * in_channels, 0,
                    (frame_count - produced) * in_channels * sizeof(float));
    }

    for (std::uint32_t f = 0; f < frame_count; ++f) {
        const float* src_frame = scratch + f * in_channels;
        float*       dst_frame = samples + f * channel_count;
        for (std::uint32_t i = 0; i < in_channels; ++i) {
            dst_frame[r->mapping[i].dst] = src_frame[i];
        }
    }

    r->frames_consumed.fetch_add(produced, std::memory_order_relaxed);
}

}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

RouteManager::RouteManager(DeviceManager& dm) : dm_(dm) {}

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
    rec->channels_count = static_cast<std::uint32_t>(cfg.mapping.size());
    rec->state          = JBOX_ROUTE_STATE_STOPPED;

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
    out->state           = r.state;
    out->last_error      = r.last_error;
    out->frames_produced = r.frames_produced.load(std::memory_order_relaxed);
    out->frames_consumed = r.frames_consumed.load(std::memory_order_relaxed);
    out->underrun_count  = r.underrun_count.load(std::memory_order_relaxed);
    out->overrun_count   = r.overrun_count.load(std::memory_order_relaxed);
    return JBOX_OK;
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
    // Resolve devices.
    const BackendDeviceInfo* src = dm_.findByUid(r.source_uid);
    const BackendDeviceInfo* dst = dm_.findByUid(r.dest_uid);
    if (src == nullptr || dst == nullptr) {
        r.state = JBOX_ROUTE_STATE_WAITING;
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

    // Size the ring buffer: 4x the larger device buffer, capped at a
    // reasonable maximum. If either device reports 0 buffer size
    // (e.g., simulated devices that don't set it), fall back to base.
    const std::uint32_t max_buffer =
        std::max(src->buffer_frame_size, dst->buffer_frame_size);
    const std::uint32_t capacity_frames =
        std::max<std::uint32_t>(kRingCapacityBase,
                                max_buffer > 0 ? max_buffer * 4 : kRingCapacityBase);

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

    // Attach to the source device's mux (input side). Creates the mux
    // on demand; subsequent routes using this device will share it.
    DeviceIOMux& src_mux = getOrCreateMux(
        r.source_uid,
        src->input_channel_count,
        src->output_channel_count,
        computeGracePeriod(*src));
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
        dst->output_channel_count,
        computeGracePeriod(*dst));
    if (!dst_mux.attachOutput(&r, &outputIOProcCallback, &r)) {
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
    // r.state and r.last_error are intentionally not touched here;
    // callers set them to the appropriate value after this call.
    // Counters are preserved across stop/start cycles for visibility.
}

DeviceIOMux& RouteManager::getOrCreateMux(const std::string& uid,
                                          std::uint32_t input_channel_count,
                                          std::uint32_t output_channel_count,
                                          double grace_period_seconds) {
    auto it = muxes_.find(uid);
    if (it != muxes_.end()) return *it->second;
    auto mux = std::make_unique<DeviceIOMux>(
        dm_.backend(), uid, input_channel_count, output_channel_count,
        grace_period_seconds);
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
    releaseRouteResources(r);
    r.state      = JBOX_ROUTE_STATE_STOPPED;
    r.last_error = JBOX_OK;
}

}  // namespace jbox::control
