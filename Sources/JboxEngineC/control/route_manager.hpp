// route_manager.hpp — per-route lifecycle: devices, IOProcs, ring buffer.
//
// RouteManager owns the routes the engine knows about and drives each
// through its lifecycle state machine:
//
//   stopped → starting → running                       (both devices present)
//   stopped → waiting  → starting → running            (start before device present)
//   running → error                                    (future: disconnect handling)
//
// Auto-recovery from WAITING / ERROR when a device re-appears is still
// deferred (pairs with the hot-plug listener). For now: a route that
// can't find its devices at start time stays in WAITING; the user
// calls start again (typically after refreshing the DeviceManager) to
// retry.
//
// Phase 5: multiple routes may share a source device or a destination
// device. Each physical device backs a single DeviceIOMux which owns
// one backend input IOProc and one output IOProc, multiplexing per-
// route work behind an atomic dispatch list (see device_io_mux.hpp and
// docs/spec.md § 2.7).
//
// See docs/spec.md §§ 2.3, 2.7.

#ifndef JBOX_CONTROL_ROUTE_MANAGER_HPP
#define JBOX_CONTROL_ROUTE_MANAGER_HPP

#include "audio_converter_wrapper.hpp"
#include "channel_mapper.hpp"
#include "device_io_mux.hpp"
#include "device_manager.hpp"
#include "drift_tracker.hpp"
#include "jbox_engine.h"
#include "ring_buffer.hpp"
#include "rt_log_queue.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jbox::control {

// Internal per-route record. Lives in the header (not the .cpp) so the
// drift sampler (control/drift_sampler.*) can iterate running routes
// without being a friend of RouteManager. Not part of the public C API.
struct RouteRecord {
    // Immutable configuration.
    jbox_route_id_t          id = JBOX_INVALID_ROUTE_ID;
    std::string              name;
    std::string              source_uid;
    std::string              dest_uid;
    std::vector<ChannelEdge> mapping;

    // Borrowed pointer to the engine's RT log queue (may be null when
    // the engine is configured without a drainer, e.g. in some tests).
    // Lifetime is tied to the Engine that owns both this record and
    // the LogDrainer holding the queue.
    jbox::rt::DefaultRtLogQueue* log_queue = nullptr;

    // Edge-triggered flags for RT logging. RT callbacks set these to
    // `true` on the first qualifying event after a (re)start so the
    // queue doesn't get spammed during sustained trouble. Reset on
    // startRoute / stopRoute via the control thread.
    std::atomic<bool> reported_underrun{false};
    std::atomic<bool> reported_overrun{false};
    std::atomic<bool> reported_channel_mismatch{false};

    // Lifecycle.
    jbox_route_state_t state      = JBOX_ROUTE_STATE_STOPPED;
    jbox_error_code_t  last_error = JBOX_OK;

    // Runtime resources (populated on successful startRoute, cleared on stop).
    std::vector<float>                               ring_storage;
    std::unique_ptr<jbox::rt::RingBuffer>            ring;
    std::unique_ptr<jbox::rt::AudioConverterWrapper> converter;
    // Phase 4 production gains; definitions in drift_tracker.cpp.
    DriftTracker                                     tracker{phase4Kp(), phase4Ki(), phase4MaxOutput()};

    // Muxes we are currently attached to. Non-null means the route has
    // a live per-route dispatch entry on that mux and will keep doing
    // RT work there until detach*. The pointers are borrowed from
    // RouteManager::muxes_ and remain valid until this route detaches.
    DeviceIOMux* attached_src_mux = nullptr;
    DeviceIOMux* attached_dst_mux = nullptr;

    std::vector<float> input_scratch;
    std::vector<float> output_scratch;

    std::uint32_t channels_count        = 0;
    std::uint32_t source_total_channels = 0;
    std::uint32_t dest_total_channels   = 0;

    double nominal_src_rate = 0.0;
    double nominal_dst_rate = 0.0;

    // Control thread writes; RT thread reads and applies via
    // converter.setInputRate(). 0.0 means "unset; use nominal".
    std::atomic<double> target_input_rate{0.0};

    // RT-thread-local: last rate we pushed to the converter.
    double last_applied_rate = 0.0;

    // Counters.
    std::atomic<std::uint64_t> frames_produced{0};
    std::atomic<std::uint64_t> frames_consumed{0};
    std::atomic<std::uint64_t> underrun_count{0};
    std::atomic<std::uint64_t> overrun_count{0};
};

class RouteManager {
public:
    // Takes a reference to a DeviceManager that outlives this RouteManager,
    // plus an optional RT log queue pointer (borrowed from the Engine's
    // LogDrainer). A null queue disables logging for this manager — RT
    // producers check it before calling `tryPush`.
    explicit RouteManager(DeviceManager& dm,
                          jbox::rt::DefaultRtLogQueue* log_queue = nullptr);
    ~RouteManager();

    RouteManager(const RouteManager&) = delete;
    RouteManager& operator=(const RouteManager&) = delete;

    struct RouteConfig {
        std::string                      source_uid;
        std::string                      dest_uid;
        std::vector<jbox::control::ChannelEdge> mapping;
        std::string                      name;  // may be empty
    };

    // Add a route in state STOPPED. Returns the new id on success, or
    // JBOX_INVALID_ROUTE_ID with *err set on failure.
    jbox_route_id_t addRoute(const RouteConfig& cfg, jbox_error_t* err);

    // Remove a route. Stops it first if running.
    jbox_error_code_t removeRoute(jbox_route_id_t id);

    // Request start. Transitions:
    //   STOPPED → WAITING     (device(s) missing at start time)
    //   STOPPED → RUNNING     (happy path)
    //   STOPPED → ERROR       (IOProc / start failure)
    //   WAITING → RUNNING     (on retry once devices are present)
    //   already RUNNING       → no-op
    jbox_error_code_t startRoute(jbox_route_id_t id);

    // Transition to STOPPED from any other state.
    jbox_error_code_t stopRoute(jbox_route_id_t id);

    // Fill in the status snapshot. Thread-safe insofar as counters
    // are atomics; state is mutated only on the control thread.
    jbox_error_code_t pollStatus(jbox_route_id_t id, jbox_route_status_t* out) const;

    // Number of routes currently known to the manager (in any state).
    std::size_t routeCount() const { return routes_.size(); }

    // Access to the underlying device manager.
    DeviceManager& deviceManager() { return dm_; }

    // Snapshot of currently-running routes, for the drift sampler.
    // Control-thread only. Returns raw pointers into the owning map;
    // the pointers are valid until the next add/remove/stop call.
    std::vector<RouteRecord*> runningRoutes();

private:

    DeviceManager& dm_;
    jbox::rt::DefaultRtLogQueue* log_queue_ = nullptr;
    std::unordered_map<jbox_route_id_t, std::unique_ptr<RouteRecord>> routes_;
    jbox_route_id_t next_id_ = 1;

    // One mux per physical device UID currently referenced by at least
    // one running route. Lazily created on first attach, destroyed once
    // both directions go idle (the destructor closes any remaining
    // backend IOProc IDs and stops the device).
    std::unordered_map<std::string, std::unique_ptr<DeviceIOMux>> muxes_;

    // Internal helpers.
    jbox_error_code_t attemptStart(RouteRecord& r);
    void              teardown(RouteRecord& r);
    // Release all runtime allocations and mux attachments on r without
    // touching r.state or r.last_error. Called by teardown (which then sets
    // state = STOPPED / last_error = JBOX_OK) and by the ERROR-return paths
    // in attemptStart (which set their own state / error afterward).
    void              releaseRouteResources(RouteRecord& r);
    DeviceIOMux&      getOrCreateMux(const std::string& uid,
                                     std::uint32_t input_channel_count,
                                     std::uint32_t output_channel_count);
    void              destroyMuxIfUnused(const std::string& uid);
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_ROUTE_MANAGER_HPP
