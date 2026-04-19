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
#include "route_manager.hpp"
#include "jbox_engine.h"

#include <memory>
#include <vector>

namespace jbox::control {

class Engine {
public:
    explicit Engine(std::unique_ptr<IDeviceBackend> backend);
    ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Refreshes the backend's device list and returns a snapshot.
    const std::vector<BackendDeviceInfo>& enumerateDevices();

    // Forwards to RouteManager.
    jbox_route_id_t  addRoute(const RouteManager::RouteConfig& cfg,
                              jbox_error_t* err);
    jbox_error_code_t removeRoute(jbox_route_id_t id);
    jbox_error_code_t startRoute(jbox_route_id_t id);
    jbox_error_code_t stopRoute(jbox_route_id_t id);
    jbox_error_code_t pollStatus(jbox_route_id_t id,
                                 jbox_route_status_t* out) const;

    DeviceManager&   deviceManager() { return dm_; }
    RouteManager&    routeManager()  { return rm_; }

private:
    DeviceManager dm_;
    RouteManager  rm_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_ENGINE_HPP
