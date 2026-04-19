// drift_tracker.hpp — pure proportional-integral controller used to
// correct for clock drift between two independent audio devices.
//
// Usage
// -----
// The engine computes an "error" each control-thread tick — typically
// `fill_level - target`, where fill_level is the current RingBuffer
// depth and target is the midpoint of the buffer. It passes that
// error plus the elapsed `dt` in seconds to `update(error, dt)`; the
// returned value is the controller output, clamped to ±max_output.
// Higher layers translate that into the AudioConverter sample-rate
// adjustment (typically interpreting the output as ppm).
//
// DriftTracker is deliberately agnostic about what the error and
// output represent. It is only a PI controller with anti-windup.
// See docs/spec.md §§ 2.5, 2.6.
//
// Non-RT
// ------
// DriftTracker runs on the control thread at ~100 Hz. It lives in
// control/ and uses plain doubles; it is not reachable from the
// audio IOProc callbacks.

#ifndef JBOX_CONTROL_DRIFT_TRACKER_HPP
#define JBOX_CONTROL_DRIFT_TRACKER_HPP

namespace jbox::control {

class DriftTracker {
public:
    // Construct with PI gains and an absolute output clamp.
    //   kp         : proportional gain (output per unit of error)
    //   ki         : integral gain     (output per unit of error per second)
    //   max_output : |output| is clamped to this value after PI math
    //
    // Empirical starting values for ring-buffer drift control in ppm:
    //   kp ≈ 1e-6, ki ≈ 1e-8, max_output = 100.0 (ppm)
    // These will be tuned during Phase 4 against real hardware; see
    // docs/plan.md § Phase 4.
    DriftTracker(double kp, double ki, double max_output) noexcept
        : kp_(kp), ki_(ki), max_output_(max_output) {}

    // Observe the current error and advance the controller by dt
    // seconds. Returns the new (clamped) output.
    //
    // Anti-windup: if the un-clamped output would saturate, the
    // integral contribution from this tick is rolled back so the
    // integrator does not "wind up" against the clamp.
    //
    // dt < 0 is treated as 0 (no-op tick). dt == 0 means only the
    // proportional term is re-evaluated with no new integral
    // accumulation.
    double update(double error, double dt_seconds) noexcept {
        if (dt_seconds < 0.0) {
            dt_seconds = 0.0;
        }

        const double tentative_integral = integral_ + error * dt_seconds;
        const double raw = kp_ * error + ki_ * tentative_integral;

        double output = raw;
        if (output > max_output_) output = max_output_;
        else if (output < -max_output_) output = -max_output_;

        // Anti-windup: commit the integral only if the output did not
        // saturate. If it saturated, leave the integrator where it was
        // so it doesn't accumulate further against the clamp.
        if (output == raw) {
            integral_ = tentative_integral;
        }

        last_error_ = error;
        last_output_ = output;
        return output;
    }

    // Clear internal state. Useful on route start or after a large
    // disturbance (e.g., buffer reset).
    void reset() noexcept {
        integral_ = 0.0;
        last_error_ = 0.0;
        last_output_ = 0.0;
    }

    // Observers (for telemetry and tests).
    double lastOutput()      const noexcept { return last_output_; }
    double lastError()       const noexcept { return last_error_; }
    double currentIntegral() const noexcept { return integral_; }

    double kp()        const noexcept { return kp_; }
    double ki()        const noexcept { return ki_; }
    double maxOutput() const noexcept { return max_output_; }

private:
    double kp_;
    double ki_;
    double max_output_;
    double integral_{0.0};
    double last_error_{0.0};
    double last_output_{0.0};
};

// Defined in drift_tracker.cpp. Source of truth for the Phase 4
// production PI gains; tests may override per-route via the
// DriftTracker ctor to exercise edge cases.
double phase4Kp()        noexcept;
double phase4Ki()        noexcept;
double phase4MaxOutput() noexcept;

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DRIFT_TRACKER_HPP
