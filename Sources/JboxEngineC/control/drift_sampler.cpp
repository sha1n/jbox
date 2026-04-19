// drift_sampler.cpp

#include "drift_sampler.hpp"

#include <chrono>

namespace jbox::control {

namespace {
// Operating point: target half-fill of the ring, so over/underrun
// headroom is symmetric. Stated in frames; matches ring capacity
// scaling in RouteManager.
double ringTargetFill(const RouteRecord& r) {
    if (!r.ring) return 0.0;
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
