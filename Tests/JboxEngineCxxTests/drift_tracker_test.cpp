// drift_tracker_test.cpp — unit tests for the DriftTracker PI controller.
//
// These tests exercise the controller in isolation — there is no plant
// model here. Closed-loop convergence against a real or simulated
// ring buffer / AudioConverter is a Phase 4 concern.

#include <catch_amalgamated.hpp>

#include "drift_tracker.hpp"

#include <cmath>

using jbox::control::DriftTracker;

TEST_CASE("DriftTracker: fresh tracker reports zero state", "[drift_tracker]") {
    DriftTracker tracker(1.0, 0.1, 10.0);
    REQUIRE(tracker.lastOutput() == 0.0);
    REQUIRE(tracker.lastError() == 0.0);
    REQUIRE(tracker.currentIntegral() == 0.0);
}

TEST_CASE("DriftTracker: configuration is reported back unchanged", "[drift_tracker]") {
    DriftTracker tracker(2.5, 0.05, 42.0);
    REQUIRE(tracker.kp() == 2.5);
    REQUIRE(tracker.ki() == 0.05);
    REQUIRE(tracker.maxOutput() == 42.0);
}

TEST_CASE("DriftTracker: zero error yields zero output", "[drift_tracker]") {
    DriftTracker tracker(1.0, 0.1, 10.0);
    REQUIRE(tracker.update(0.0, 0.01) == 0.0);
    REQUIRE(tracker.currentIntegral() == 0.0);
}

TEST_CASE("DriftTracker: positive error yields positive output, clamped to max", "[drift_tracker]") {
    DriftTracker tracker(1.0, 0.0, 10.0);  // P-only, to isolate from integral
    // Small error: purely proportional.
    REQUIRE(tracker.update(5.0, 0.01) == 5.0);
    REQUIRE(tracker.lastError() == 5.0);
    REQUIRE(tracker.lastOutput() == 5.0);
    // Large error: clamped.
    REQUIRE(tracker.update(1000.0, 0.01) == 10.0);
}

TEST_CASE("DriftTracker: negative error yields negative output, clamped to -max", "[drift_tracker]") {
    DriftTracker tracker(1.0, 0.0, 10.0);
    REQUIRE(tracker.update(-3.0, 0.01) == -3.0);
    REQUIRE(tracker.update(-1000.0, 0.01) == -10.0);
}

TEST_CASE("DriftTracker: integral accumulates over time with constant error", "[drift_tracker]") {
    // P=0, I=1: output is just `integral = error × total_time`.
    DriftTracker tracker(0.0, 1.0, 1000.0);  // big clamp so we don't saturate

    // Five 1-second ticks with error=2.0 → integral = 10, output = 10.
    for (int i = 0; i < 5; ++i) {
        tracker.update(2.0, 1.0);
    }
    REQUIRE(tracker.currentIntegral() == Catch::Approx(10.0));
    REQUIRE(tracker.lastOutput() == Catch::Approx(10.0));
}

TEST_CASE("DriftTracker: output combines P and I contributions", "[drift_tracker]") {
    DriftTracker tracker(1.0, 2.0, 1000.0);
    // First tick: error=3, dt=0.5
    //   integral tentative = 0 + 3*0.5 = 1.5
    //   raw = 1*3 + 2*1.5 = 6.0
    REQUIRE(tracker.update(3.0, 0.5) == Catch::Approx(6.0));
    REQUIRE(tracker.currentIntegral() == Catch::Approx(1.5));
}

TEST_CASE("DriftTracker: saturating output triggers anti-windup", "[drift_tracker]") {
    // Large constant error; output saturates on the first tick. The
    // integrator must NOT accumulate while saturated, so that when
    // the error returns to zero, the controller can recover quickly.
    DriftTracker tracker(1.0, 1.0, 5.0);

    // First tick: error=100, dt=1
    //   tentative integral = 0 + 100*1 = 100
    //   raw = 1*100 + 1*100 = 200 → clamped to 5.0 → saturated
    //   anti-windup: integral stays at 0.
    tracker.update(100.0, 1.0);
    REQUIRE(tracker.lastOutput() == 5.0);
    REQUIRE(tracker.currentIntegral() == 0.0);

    // Second tick: error=100, dt=1. Same outcome; integral still 0.
    tracker.update(100.0, 1.0);
    REQUIRE(tracker.currentIntegral() == 0.0);

    // Now error returns to zero; output should return to zero immediately.
    REQUIRE(tracker.update(0.0, 1.0) == 0.0);
}

TEST_CASE("DriftTracker: integral DOES accumulate when output is within limits",
          "[drift_tracker]") {
    // Error is small enough that output never saturates; integrator
    // should track the error history faithfully.
    DriftTracker tracker(0.0, 1.0, 100.0);  // I-only, large clamp

    tracker.update(2.0, 0.5);   // integral += 1.0
    REQUIRE(tracker.currentIntegral() == Catch::Approx(1.0));
    tracker.update(2.0, 0.5);   // integral += 1.0
    REQUIRE(tracker.currentIntegral() == Catch::Approx(2.0));
    tracker.update(-1.0, 0.5);  // integral -= 0.5
    REQUIRE(tracker.currentIntegral() == Catch::Approx(1.5));
}

TEST_CASE("DriftTracker: reset clears all state", "[drift_tracker]") {
    DriftTracker tracker(1.0, 1.0, 100.0);
    tracker.update(5.0, 1.0);
    tracker.update(-3.0, 1.0);
    REQUIRE(tracker.currentIntegral() != 0.0);
    REQUIRE(tracker.lastOutput() != 0.0);

    tracker.reset();
    REQUIRE(tracker.currentIntegral() == 0.0);
    REQUIRE(tracker.lastOutput() == 0.0);
    REQUIRE(tracker.lastError() == 0.0);
}

TEST_CASE("DriftTracker: dt=0 re-evaluates proportional with no integral change",
          "[drift_tracker]") {
    DriftTracker tracker(1.0, 1.0, 100.0);
    tracker.update(2.0, 1.0);
    const double integral_after_first = tracker.currentIntegral();

    const double out = tracker.update(3.0, 0.0);
    // integral unchanged; output = kp*error + ki*integral
    REQUIRE(tracker.currentIntegral() == Catch::Approx(integral_after_first));
    REQUIRE(out == Catch::Approx(1.0 * 3.0 + 1.0 * integral_after_first));
}

TEST_CASE("DriftTracker: negative dt is treated as zero (no time advance)",
          "[drift_tracker]") {
    DriftTracker tracker(1.0, 1.0, 100.0);
    tracker.update(5.0, 1.0);
    const double integral_after_first = tracker.currentIntegral();

    // Negative dt should not be honored; treat like dt=0.
    tracker.update(7.0, -1.0);
    REQUIRE(tracker.currentIntegral() == Catch::Approx(integral_after_first));
}

TEST_CASE("DriftTracker: realistic ppm-style configuration behaves sensibly",
          "[drift_tracker]") {
    // Default Phase 4 starting gains per docs/spec.md § 2.6.
    //   Kp = 1e-6, Ki = 1e-8, max output 100 ppm.
    // Feed a constant error of 256 frames (typical half-buffer at 1ms)
    // for many control ticks (10ms each → ~100 Hz).
    DriftTracker tracker(1e-6, 1e-8, 100.0);

    constexpr int kTicks = 2000;  // 20 seconds of 10ms ticks
    constexpr double kDt = 0.01;
    constexpr double kError = 256.0;

    double out = 0.0;
    for (int i = 0; i < kTicks; ++i) {
        out = tracker.update(kError, kDt);
    }

    // After 20 s, integral ≈ error × total_time = 256 × 20 = 5120.
    // Output ≈ 1e-6*256 + 1e-8*5120 = 2.56e-4 + 5.12e-5 ≈ 3.07e-4 ppm.
    // Sanity check: within expected order of magnitude, far below clamp.
    REQUIRE(out > 0.0);
    REQUIRE(out < tracker.maxOutput());
    REQUIRE(out == Catch::Approx(3.072e-4).epsilon(0.001));
}

TEST_CASE("DriftTracker: sign of output follows sign of error", "[drift_tracker]") {
    DriftTracker tracker(1.0, 0.5, 100.0);

    // Positive error → positive output.
    REQUIRE(tracker.update(1.0, 0.01) > 0.0);
    tracker.reset();

    // Negative error → negative output.
    REQUIRE(tracker.update(-1.0, 0.01) < 0.0);
}

TEST_CASE("DriftTracker: reset allows reuse for a new route",
          "[drift_tracker]") {
    // Simulate one "route" accumulating windup, then resetting and
    // starting fresh; the new trajectory should start from zero.
    DriftTracker tracker(1.0, 1.0, 5.0);

    for (int i = 0; i < 50; ++i) {
        tracker.update(100.0, 1.0);  // saturates, but let's also
                                     // build some integral between resets
    }
    tracker.reset();

    // Fresh start; single small-error tick behaves like first-tick math.
    REQUIRE(tracker.update(1.0, 0.5) == Catch::Approx(1.0 * 1.0 + 1.0 * 0.5));
}
