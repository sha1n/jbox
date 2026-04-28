// engine.cpp — Engine facade.

#include "engine.hpp"

#include <chrono>
#include <utility>

namespace jbox::control {

Engine::Engine(std::unique_ptr<IDeviceBackend> backend,
               bool spawn_sampler_thread,
               bool spawn_log_drainer)
    : drainer_(spawn_log_drainer ? std::make_unique<LogDrainer>() : nullptr),
      dm_(std::move(backend)),
      // Watcher must be constructed AFTER dm_ — the backend reference
      // it captures is dm_.backend(), and dm_ owns that backend.
      watcher_(dm_.backend()),
      rm_(dm_, drainer_ ? drainer_->queue() : nullptr),
      sampler_(rm_) {
    dm_.refresh();
    if (spawn_sampler_thread) {
        sampler_.start();
        // 7.6.4 hot-plug consumer. Same gating flag as the drift
        // sampler: tests that opt out (`spawn_sampler_thread=false`)
        // drive `tickHotPlug()` synchronously instead.
        hotplug_running_.store(true, std::memory_order_release);
        hotplug_thread_ = std::thread(&Engine::hotPlugThreadLoop, this);
    }
}

Engine::~Engine() {
    sampler_.stop();
    if (hotplug_running_.exchange(false, std::memory_order_acq_rel)) {
        if (hotplug_thread_.joinable()) hotplug_thread_.join();
    }
    // Stop the drainer explicitly before the member destructors run,
    // so any control-thread events queued during rm_ teardown are
    // drained to the sink.
    if (drainer_) drainer_->stop();
}

const std::vector<BackendDeviceInfo>& Engine::enumerateDevices() {
    return dm_.refresh();
}

void Engine::tickHotPlug() {
    auto events = watcher_.drain();
    if (events.empty()) return;
    rm_.handleDeviceChanges(events);
}

void Engine::hotPlugThreadLoop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    constexpr auto period = milliseconds(100);  // 10 Hz
    while (hotplug_running_.load(std::memory_order_relaxed)) {
        next += period;
        const auto now = steady_clock::now();
        if (next < now) next = now;
        std::this_thread::sleep_until(next);
        tickHotPlug();
    }
}

std::vector<std::string> Engine::channelNames(const std::string& uid,
                                              std::uint32_t direction) {
    return dm_.backend().channelNames(uid, direction);
}

jbox_route_id_t Engine::addRoute(const RouteManager::RouteConfig& cfg,
                                 jbox_error_t* err) {
    return rm_.addRoute(cfg, err);
}

jbox_error_code_t Engine::removeRoute(jbox_route_id_t id) {
    return rm_.removeRoute(id);
}

jbox_error_code_t Engine::renameRoute(jbox_route_id_t id,
                                      const std::string& new_name) {
    return rm_.renameRoute(id, new_name);
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

jbox_error_code_t Engine::pollLatencyComponents(
    jbox_route_id_t id,
    jbox_route_latency_components_t* out) const {
    return rm_.pollLatencyComponents(id, out);
}

std::size_t Engine::pollMeters(jbox_route_id_t id,
                               jbox_meter_side_t side,
                               float* out_peaks,
                               std::size_t max_channels) {
    return rm_.pollMeters(id, side, out_peaks, max_channels);
}

}  // namespace jbox::control
