// route_manager.hpp — per-route lifecycle: devices, IOProcs, ring buffer.
//
// RouteManager owns the routes the engine knows about and drives each
// through its lifecycle state machine:
//
//   stopped → starting → running                       (both devices present)
//   stopped → waiting  → starting → running            (start before device present)
//   running → error                                    (future: disconnect handling)
//
// For Phase 3, auto-recovery from WAITING / ERROR when a device
// re-appears is deferred (see docs/plan.md § Phase 5 for the multi-
// route / shared-device work). For now: a route that can't find its
// devices at start time stays in WAITING; the user calls start again
// (typically after refreshing the DeviceManager) to retry.
//
// Phase 3 device-sharing constraint: a device can be used as source
// by at most one route and as destination by at most one route
// simultaneously. Attempting a second concurrent registration fails
// with JBOX_ERR_DEVICE_BUSY. Phase 5 relaxes this via multi-route
// dispatch behind a single shared IOProc.
//
// See docs/spec.md §§ 2.3, 2.7.

#ifndef JBOX_CONTROL_ROUTE_MANAGER_HPP
#define JBOX_CONTROL_ROUTE_MANAGER_HPP

#include "audio_converter_wrapper.hpp"
#include "channel_mapper.hpp"
#include "device_manager.hpp"
#include "drift_tracker.hpp"
#include "jbox_engine.h"
#include "ring_buffer.hpp"

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

    // Lifecycle.
    jbox_route_state_t state      = JBOX_ROUTE_STATE_STOPPED;
    jbox_error_code_t  last_error = JBOX_OK;

    // Runtime resources (populated on successful startRoute, cleared on stop).
    std::vector<float>                               ring_storage;
    std::unique_ptr<jbox::rt::RingBuffer>            ring;
    std::unique_ptr<jbox::rt::AudioConverterWrapper> converter;
    // Placeholder gains; Task 8 (docs/phase4-plan.md) replaces these with
    // the tuned phase4Kp() / phase4Ki() / phase4MaxOutput() constants.
    DriftTracker                                     tracker{1e-6, 1e-8, 100.0};
    IOProcId input_ioproc  = kInvalidIOProcId;
    IOProcId output_ioproc = kInvalidIOProcId;

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
    // Takes a reference to a DeviceManager that outlives this RouteManager.
    explicit RouteManager(DeviceManager& dm);
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
    std::unordered_map<jbox_route_id_t, std::unique_ptr<RouteRecord>> routes_;
    jbox_route_id_t next_id_ = 1;

    // Track which devices are currently in use as source or destination
    // by running routes; Phase 3 enforces one-per-direction.
    std::unordered_map<std::string, jbox_route_id_t> source_in_use_;
    std::unordered_map<std::string, jbox_route_id_t> dest_in_use_;

    // Internal helpers.
    jbox_error_code_t attemptStart(RouteRecord& r);
    void              teardown(RouteRecord& r);
    // Release all runtime allocations and backend registrations on r without
    // touching r.state or r.last_error. Called by teardown (which then sets
    // state = STOPPED / last_error = JBOX_OK) and by the ERROR-return paths
    // in attemptStart (which set their own state / error afterward).
    void              releaseRouteResources(RouteRecord& r);
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_ROUTE_MANAGER_HPP
