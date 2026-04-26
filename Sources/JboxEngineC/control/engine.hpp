// engine.hpp — the top-level engine facade.
//
// Engine owns the DeviceManager + RouteManager pair and is what the
// public jbox_engine_t opaque handle refers to under the hood. Most
// calls here are thin forwarders; the real logic lives in the
// component classes.
//
// Thread model: single-threaded from the engine control thread.
//
// See docs/spec.md § 1.1 (layer 4).

#ifndef JBOX_CONTROL_ENGINE_HPP
#define JBOX_CONTROL_ENGINE_HPP

#include "device_backend.hpp"
#include "device_manager.hpp"
#include "drift_sampler.hpp"
#include "log_drainer.hpp"
#include "route_manager.hpp"
#include "jbox_engine.h"

#include <memory>
#include <vector>

namespace jbox::control {

class Engine {
public:
    Engine(std::unique_ptr<IDeviceBackend> backend,
           bool spawn_sampler_thread,
           bool spawn_log_drainer = true);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Refreshes the backend's device list and returns a snapshot.
    const std::vector<BackendDeviceInfo>& enumerateDevices();

    // Per-channel names for the given device + direction. Direction
    // must be exactly one of kBackendDirectionInput or
    // kBackendDirectionOutput (a bitmask with both bits set would
    // be ambiguous). Returns an empty vector on unknown device or
    // invalid direction.
    std::vector<std::string> channelNames(const std::string& uid,
                                          std::uint32_t direction);

    // Forwards to RouteManager.
    jbox_route_id_t  addRoute(const RouteManager::RouteConfig& cfg,
                              jbox_error_t* err);
    jbox_error_code_t removeRoute(jbox_route_id_t id);
    jbox_error_code_t renameRoute(jbox_route_id_t id, const std::string& new_name);
    jbox_error_code_t startRoute(jbox_route_id_t id);
    jbox_error_code_t stopRoute(jbox_route_id_t id);
    jbox_error_code_t pollStatus(jbox_route_id_t id,
                                 jbox_route_status_t* out) const;
    jbox_error_code_t pollLatencyComponents(
        jbox_route_id_t id,
        jbox_route_latency_components_t* out) const;
    std::size_t       pollMeters(jbox_route_id_t id,
                                 jbox_meter_side_t side,
                                 float* out_peaks,
                                 std::size_t max_channels);

    // Engine-wide resampler quality preset (docs/spec.md § 4.6). Thin
    // forwarder onto RouteManager's atomic — changing it here affects
    // subsequently-started routes only, since each converter is built
    // once at route start. The Swift layer pushes preference changes
    // here; routes already running keep their original preset until
    // stopped and started again.
    void              setResamplerQuality(jbox::rt::ResamplerQuality q) {
        rm_.setResamplerQuality(q);
    }
    jbox::rt::ResamplerQuality resamplerQuality() const {
        return rm_.resamplerQuality();
    }

    DeviceManager&   deviceManager() { return dm_; }
    RouteManager&    routeManager()  { return rm_; }
    DriftSampler&    driftSampler()  { return sampler_; }
    LogDrainer*      logDrainer()    { return drainer_.get(); }

private:
    // The drainer is owned here so both the queue pointer (handed to
    // RouteManager) and the consumer thread share one lifetime.
    std::unique_ptr<LogDrainer> drainer_;
    DeviceManager dm_;
    RouteManager  rm_;
    DriftSampler  sampler_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_ENGINE_HPP
