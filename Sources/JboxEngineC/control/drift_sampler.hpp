// drift_sampler.hpp — engine-owned ~100 Hz control-thread ticker that
// reads each running route's ring fill, advances its DriftTracker, and
// publishes a new effective input rate into the route's
// target_input_rate atomic. The RT thread picks up and applies the new
// rate on its next IOProc callback.
//
// Two modes:
//  - Production: start() spawns a std::thread that calls tickAll()
//    every ~10 ms (100 Hz). stop() joins.
//  - Test:       callers skip start() and invoke tickAll(dt) directly.
//
// The sampler does not hold a lock on RouteManager; tickAll() asks
// RouteManager for its current running-route snapshot each tick.
// RouteManager mutations happen on the control thread (same thread as
// the sampler); tests drive both serially, and production spawns the
// sampler from the control thread so mutations and ticks interleave
// only on that thread.
//
// See docs/phase4-design.md §§ 3, 5.

#ifndef JBOX_CONTROL_DRIFT_SAMPLER_HPP
#define JBOX_CONTROL_DRIFT_SAMPLER_HPP

#include "route_manager.hpp"

#include <atomic>
#include <thread>

namespace jbox::control {

class DriftSampler {
public:
    explicit DriftSampler(RouteManager& rm) noexcept;
    ~DriftSampler();

    DriftSampler(const DriftSampler&) = delete;
    DriftSampler& operator=(const DriftSampler&) = delete;

    // Production: spawn the ticker thread. Safe to call at most once
    // per DriftSampler instance.
    void start();

    // Stop the ticker thread (if running) and join. Idempotent.
    void stop();

    // Advance all running routes by dt_seconds. Thread-safe only under
    // the control-thread-only contract above. Tests call this directly.
    void tickAll(double dt_seconds);

private:
    void threadLoop();

    RouteManager&   rm_;
    std::atomic<bool> running_{false};
    std::thread     thread_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DRIFT_SAMPLER_HPP
