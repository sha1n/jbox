// drift_sampler.cpp

#include "drift_sampler.hpp"

#include <chrono>

namespace jbox::control {

namespace {
// Operating point — the ring-fill level the PI controller drives
// toward. The value is populated per-route at attemptStart (see
// route_manager.cpp) based on the selected latency preset:
//   Safe / Low latency → ring/2 (symmetric over/underrun headroom)
//   Performance        → ring/4 (asymmetric: short drain headroom in
//                                 exchange for ~2–5 ms of residency
//                                 savings; drum-monitoring preset).
// Falls back to ring/2 only if the cached value is unexpectedly zero,
// which should never happen on a running route.
double ringTargetFill(const RouteRecord& r) {
    if (!r.ring) return 0.0;
    const std::uint32_t cached = r.latency_components.ring_target_fill_frames;
    if (cached > 0) return static_cast<double>(cached);
    return static_cast<double>(r.ring->usableCapacityFrames()) * 0.5;
}
}  // namespace

DriftSampler::DriftSampler(RouteManager& rm) noexcept : rm_(rm) {}

DriftSampler::~DriftSampler() { stop(); }

void DriftSampler::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&DriftSampler::threadLoop, this);
}

void DriftSampler::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void DriftSampler::threadLoop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    constexpr auto period = milliseconds(10);
    while (running_.load(std::memory_order_relaxed)) {
        next += period;
        const auto now = steady_clock::now();
        if (next < now) next = now;  // don't burst-catch-up after a stall
        std::this_thread::sleep_until(next);
        tickAll(0.010);
    }
}

void DriftSampler::tickAll(double dt_seconds) {
    auto running = rm_.runningRoutes();
    for (auto* r : running) {
        if (!r || !r->ring || !r->converter) continue;
        const double fill   = static_cast<double>(r->ring->framesAvailableForRead());
        const double target = ringTargetFill(*r);
        const double error  = fill - target;  // positive = too full
        const double ppm    = r->tracker.update(error, dt_seconds);
        // Sign convention: when the ring is too full (error > 0 -> ppm > 0)
        // we tell the converter the input is arriving FASTER than nominal.
        // The converter then consumes more input frames per output frame,
        // draining the ring back toward target.
        //   eff_src = nominal_src * (1 + ppm * 1e-6)
        const double eff_src = r->nominal_src_rate * (1.0 + ppm * 1e-6);
        r->target_input_rate.store(eff_src, std::memory_order_relaxed);
    }
}

}  // namespace jbox::control
