// engine.cpp — Engine facade.

#include "engine.hpp"

#include <utility>

namespace jbox::control {

Engine::Engine(std::unique_ptr<IDeviceBackend> backend,
               bool spawn_sampler_thread)
    : dm_(std::move(backend)),
      rm_(dm_),
      sampler_(rm_) {
    dm_.refresh();
    if (spawn_sampler_thread) sampler_.start();
}

Engine::~Engine() { sampler_.stop(); }

const std::vector<BackendDeviceInfo>& Engine::enumerateDevices() {
    return dm_.refresh();
}

jbox_route_id_t Engine::addRoute(const RouteManager::RouteConfig& cfg,
                                 jbox_error_t* err) {
    return rm_.addRoute(cfg, err);
}

jbox_error_code_t Engine::removeRoute(jbox_route_id_t id) {
    return rm_.removeRoute(id);
}

jbox_error_code_t Engine::startRoute(jbox_route_id_t id) {
    return rm_.startRoute(id);
}

jbox_error_code_t Engine::stopRoute(jbox_route_id_t id) {
    return rm_.stopRoute(id);
}

jbox_error_code_t Engine::pollStatus(jbox_route_id_t id,
                                     jbox_route_status_t* out) const {
    return rm_.pollStatus(id, out);
}

}  // namespace jbox::control
