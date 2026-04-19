// route_manager.cpp — route lifecycle and IOProc wiring.

#include "route_manager.hpp"

#include "ring_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace jbox::control {

// -----------------------------------------------------------------------------
// RouteRecord — internal per-route state
// -----------------------------------------------------------------------------

struct RouteManager::RouteRecord {
    // Immutable configuration.
    jbox_route_id_t          id = JBOX_INVALID_ROUTE_ID;
    std::string              name;
    std::string              source_uid;
    std::string              dest_uid;
    std::vector<ChannelEdge> mapping;

    // Lifecycle.
    jbox_route_state_t state      = JBOX_ROUTE_STATE_STOPPED;
    jbox_error_code_t  last_error = JBOX_OK;

    // Runtime resources (populated on successful startRoute, cleared on stop).
    std::vector<float>                ring_storage;
    std::unique_ptr<jbox::rt::RingBuffer> ring;
    IOProcId input_ioproc  = kInvalidIOProcId;
    IOProcId output_ioproc = kInvalidIOProcId;
    // Scratch buffers for interleaving selected channels into the
    // ring format. Allocated at route start; sized conservatively.
    std::vector<float> input_scratch;
    std::vector<float> output_scratch;

    std::uint32_t channels_count        = 0;  // mapping.size()
    std::uint32_t source_total_channels = 0;  // device's input channel count
    std::uint32_t dest_total_channels   = 0;  // device's output channel count

    // Atomic counters updated from RT threads.
    std::atomic<std::uint64_t> frames_produced{0};
    std::atomic<std::uint64_t> frames_consumed{0};
    std::atomic<std::uint64_t> underrun_count{0};
    std::atomic<std::uint64_t> overrun_count{0};
};

// -----------------------------------------------------------------------------
// RT trampolines
// -----------------------------------------------------------------------------

namespace {

// Configuration that keeps the RT path allocation-free:
//   ring capacity (frames) = 4 * max(source_buf, dest_buf)
// Per docs/spec.md § 2.3; yields ~5–10 ms headroom at 48k/64-frame buffers.
constexpr std::uint32_t kRingCapacityBase = 256;
constexpr std::uint32_t kRtScratchMaxFrames = 8192;

void inputIOProcCallback(const float* samples,
                         std::uint32_t frame_count,
                         std::uint32_t channel_count,
                         void* user_data) {
    auto* r = static_cast<RouteManager::RouteRecord*>(user_data);
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

void outputIOProcCallback(float* samples,
                          std::uint32_t frame_count,
                          std::uint32_t channel_count,
                          void* user_data) {
    auto* r = static_cast<RouteManager::RouteRecord*>(user_data);
    if (r->ring == nullptr) return;
    if (channel_count != r->dest_total_channels) return;

    const std::uint32_t in_channels = r->channels_count;
    float* scratch = r->output_scratch.data();

    const std::size_t read = r->ring->readFrames(scratch, frame_count);
    if (read < frame_count) {
        r->underrun_count.fetch_add(1, std::memory_order_relaxed);
        // Zero-fill the remainder.
        std::memset(scratch + read * in_channels,
                    0,
                    (frame_count - read) * in_channels * sizeof(float));
    }

    // The output buffer is already zero-filled by the backend; we
    // only write the destination channels selected in the mapping.
    for (std::uint32_t f = 0; f < frame_count; ++f) {
        const float* src_frame = scratch + f * in_channels;
        float*       dst_frame = samples + f * channel_count;
        for (std::uint32_t i = 0; i < in_channels; ++i) {
            dst_frame[r->mapping[i].dst] = src_frame[i];
        }
    }

    r->frames_consumed.fetch_add(read, std::memory_order_relaxed);
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

    // Phase 3: device-sharing constraint. No two routes can register
    // the same direction on the same device.
    if (source_in_use_.count(r.source_uid) && source_in_use_[r.source_uid] != r.id) {
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
    }
    if (dest_in_use_.count(r.dest_uid) && dest_in_use_[r.dest_uid] != r.id) {
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
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

    // Register IOProcs.
    IDeviceBackend& be = dm_.backend();
    r.input_ioproc = be.openInputCallback(r.source_uid, &inputIOProcCallback, &r);
    if (r.input_ioproc == kInvalidIOProcId) {
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
    }
    r.output_ioproc = be.openOutputCallback(r.dest_uid, &outputIOProcCallback, &r);
    if (r.output_ioproc == kInvalidIOProcId) {
        be.closeCallback(r.input_ioproc);
        r.input_ioproc = kInvalidIOProcId;
        r.state = JBOX_ROUTE_STATE_ERROR;
        r.last_error = JBOX_ERR_DEVICE_BUSY;
        return JBOX_ERR_DEVICE_BUSY;
    }

    // Mark in-use. Must happen before startDevice so device accounting
    // is consistent if teardown is needed mid-start.
    source_in_use_[r.source_uid] = r.id;
    dest_in_use_[r.dest_uid]     = r.id;

    // Start devices.
    if (!be.startDevice(r.source_uid)) {
        // startDevice returns false if already started; tolerate that
        // (another route may have started it under Phase 5, or a test
        // may have done it). Treat as success unless an open failed.
    }
    if (!be.startDevice(r.dest_uid)) {
        // same tolerance
    }

    r.state      = JBOX_ROUTE_STATE_RUNNING;
    r.last_error = JBOX_OK;
    return JBOX_OK;
}

void RouteManager::teardown(RouteRecord& r) {
    IDeviceBackend& be = dm_.backend();

    // Stop devices (if this route holds them).
    if (source_in_use_.count(r.source_uid) && source_in_use_[r.source_uid] == r.id) {
        be.stopDevice(r.source_uid);
        source_in_use_.erase(r.source_uid);
    }
    if (dest_in_use_.count(r.dest_uid) && dest_in_use_[r.dest_uid] == r.id) {
        be.stopDevice(r.dest_uid);
        dest_in_use_.erase(r.dest_uid);
    }

    // Close IOProcs.
    if (r.input_ioproc != kInvalidIOProcId) {
        be.closeCallback(r.input_ioproc);
        r.input_ioproc = kInvalidIOProcId;
    }
    if (r.output_ioproc != kInvalidIOProcId) {
        be.closeCallback(r.output_ioproc);
        r.output_ioproc = kInvalidIOProcId;
    }

    // Release runtime resources. AudioDeviceStop is synchronous on
    // Core Audio, so by the time closeCallback returns no IOProc
    // callback is in flight for this record.
    r.ring.reset();
    r.ring_storage.clear();
    r.input_scratch.clear();
    r.output_scratch.clear();

    r.state      = JBOX_ROUTE_STATE_STOPPED;
    r.last_error = JBOX_OK;
    // Counters are preserved across stop/start cycles for visibility.
}

}  // namespace jbox::control
