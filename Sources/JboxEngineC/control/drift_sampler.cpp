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
        // Drift correction convention: if we're too full, slow the
        // effective input rate down so the converter consumes at the
        // nominal-dst rate but the source produces less -> fill falls.
        // That means ppm with the same sign as error should *reduce*
        // input rate (negate here).
        const double eff_src = r->nominal_src_rate * (1.0 - ppm * 1e-6);
        r->target_input_rate.store(eff_src, std::memory_order_relaxed);
    }
}

}  // namespace jbox::control
