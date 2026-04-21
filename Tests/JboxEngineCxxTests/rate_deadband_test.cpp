// rate_deadband_test.cpp — unit tests for the drift-rate deadband used
// on the RT thread to decide when to call AudioConverter::setInputRate.
//
// Context
// -------
// The drift sampler ticks the PI controller at 100 Hz and stores a new
// target_input_rate atomic every tick. In steady state the per-tick PI
// output adjusts the proposed rate by ~1e-7 ppm — several orders of
// magnitude below the human-audible rate-error threshold (~10 ppm).
// The naive `target != last_applied` comparison used before this
// deadband fired on every tick, causing AudioConverter to flush its
// polyphase filter state on every RT callback (see drift_tracker.cpp
// header comment and docs/spec.md § 2.6). On real hardware that flush
// manifests as click artifacts and extra ring consumption.
//
// These tests exercise the pure decision function in isolation.

#include "rate_deadband.hpp"

#include <catch_amalgamated.hpp>

using jbox::control::shouldApplyRate;
using jbox::control::kRateDeadbandPpm;

TEST_CASE("[rate_deadband] applies first non-zero proposal from cold start",
          "[rate_deadband]") {
    REQUIRE(shouldApplyRate(48000.0, 0.0, 48000.0));
    REQUIRE(shouldApplyRate(44100.0, 0.0, 44100.0));
}

TEST_CASE("[rate_deadband] gates sub-threshold changes",
          "[rate_deadband]") {
    // A 0.1 ppm change at 48 kHz = 4.8 mHz — 10× below the 1 ppm threshold.
    const double proposed = 48000.0 + 48000.0 * 0.1e-6;
    REQUIRE_FALSE(shouldApplyRate(proposed, 48000.0, 48000.0));
}

TEST_CASE("[rate_deadband] accepts above-threshold changes",
          "[rate_deadband]") {
    // A 2 ppm change at 48 kHz = 96 mHz — 2× above the 1 ppm threshold.
    const double proposed = 48000.0 + 48000.0 * 2e-6;
    REQUIRE(shouldApplyRate(proposed, 48000.0, 48000.0));
}

TEST_CASE("[rate_deadband] accepts symmetric recovery downward",
          "[rate_deadband]") {
    // Applied +5 ppm earlier; now PI is telling us the drift has stopped.
    const double applied = 48000.0 + 48000.0 * 5e-6;
    REQUIRE(shouldApplyRate(48000.0, applied, 48000.0));
}

TEST_CASE("[rate_deadband] gates a realistic PI-noise tick sequence",
          "[rate_deadband]") {
    // Simulate 100 PI ticks of the regime that previously caused
    // per-tick flushes: each tick walks target_input_rate by ~1e-7 ppm
    // around nominal — well inside the deadband. A well-tuned deadband
    // must leave last_applied_rate untouched for the entire run.
    double last_applied = 48000.0;
    int applied_count = 0;
    for (int i = 0; i < 100; ++i) {
        const double ppm_noise =
            (i % 2 == 0 ? 1.0 : -1.0) * (1e-7 * static_cast<double>(i + 1));
        const double proposed = 48000.0 * (1.0 + ppm_noise * 1e-6);
        if (shouldApplyRate(proposed, last_applied, 48000.0)) {
            last_applied = proposed;
            ++applied_count;
        }
    }
    REQUIRE(applied_count == 0);
    REQUIRE(last_applied == 48000.0);
}

TEST_CASE("[rate_deadband] tracks a real 5 ppm ramp with a handful of applies",
          "[rate_deadband]") {
    // Simulate the PI controller slowly acquiring a real +5 ppm drift
    // over 10 s of 100 Hz ticks. The deadband must let enough rate
    // updates through to track the drift, but should not fire on every
    // sub-ppm intermediate step.
    double last_applied = 48000.0;
    int applied_count = 0;
    for (int tick = 0; tick < 1000; ++tick) {
        const double fraction = std::min(1.0, tick / 500.0);
        const double ppm_now = 5.0 * fraction;
        const double proposed = 48000.0 * (1.0 + ppm_now * 1e-6);
        if (shouldApplyRate(proposed, last_applied, 48000.0)) {
            last_applied = proposed;
            ++applied_count;
        }
    }
    // 5 ppm traversed ÷ 1 ppm deadband = ~5 steps. Allow 3..8 for
    // float rounding slack.
    REQUIRE(applied_count >= 3);
    REQUIRE(applied_count <= 8);
}

TEST_CASE("[rate_deadband] rejects zero and negative proposals",
          "[rate_deadband]") {
    REQUIRE_FALSE(shouldApplyRate(0.0, 48000.0, 48000.0));
    REQUIRE_FALSE(shouldApplyRate(-48000.0, 48000.0, 48000.0));
}

TEST_CASE("[rate_deadband] rejects zero and negative nominal",
          "[rate_deadband]") {
    // Defensive: a malformed RouteRecord should never cause a flush.
    REQUIRE_FALSE(shouldApplyRate(48000.0, 0.0, 0.0));
    REQUIRE_FALSE(shouldApplyRate(48000.0, 0.0, -48000.0));
}

TEST_CASE("[rate_deadband] exposes the threshold constant for documentation",
          "[rate_deadband]") {
    // The threshold is part of the contract. Pin it so a careless tweak
    // to the header is caught here.
    REQUIRE(kRateDeadbandPpm == 1.0);
}
