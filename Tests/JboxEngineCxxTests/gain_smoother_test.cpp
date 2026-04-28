// gain_smoother_test.cpp — unit tests for the RT-thread-local block-rate
// gain smoother (rt/gain_smoother.hpp). The smoother converges current
// toward target via a one-pole IIR; alpha is computed once at start
// from sample rate and a 10 ms time constant. See docs/2026-04-28-
// route-gain-mixer-strip-design.md § 5.2.

#include "gain_smoother.hpp"

#include <cmath>
#include <limits>

#include <catch_amalgamated.hpp>

using jbox::rt::GainSmoother;

namespace {
// Fire `block_count` blocks of `block_frames` each at the given sample
// rate, returning the smoother's final `current` value.
float runBlocks(GainSmoother& s,
                float target,
                std::uint32_t block_frames,
                std::uint32_t block_count) {
    for (std::uint32_t i = 0; i < block_count; ++i) {
        s.step(target, block_frames);
    }
    return s.current;
}
}  // namespace

TEST_CASE("[gain_smoother] step from unity to unity is a no-op",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    REQUIRE(s.current == 1.0f);
    s.step(1.0f, 64);
    REQUIRE(s.current == 1.0f);
}

TEST_CASE("[gain_smoother] reaches 95% of step within ~30 ms at 48 kHz",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    // 30 ms at 48 kHz = 1440 frames. Run as 22 blocks of 64.
    const float final_value = runBlocks(s, /*target=*/0.5f,
                                         /*block_frames=*/64,
                                         /*block_count=*/22);
    // 95% of step from 1.0 to 0.5 is 0.525.
    REQUIRE(final_value <= 0.525f + 0.005f);
    REQUIRE(final_value >= 0.475f);
}

TEST_CASE("[gain_smoother] reaches 95% of step within ~30 ms at 96 kHz",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(96000.0, 0.010);
    // 30 ms at 96 kHz = 2880 frames. 45 blocks of 64.
    const float final_value = runBlocks(s, 0.5f, 64, 45);
    REQUIRE(final_value <= 0.525f + 0.005f);
    REQUIRE(final_value >= 0.475f);
}

TEST_CASE("[gain_smoother] no overshoot on a step",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 1.0f;
    float prev = s.current;
    for (int i = 0; i < 100; ++i) {
        s.step(0.0f, 64);
        REQUIRE(s.current <= prev + 1e-6f);   // monotonic non-increasing
        REQUIRE(s.current >= 0.0f);
        prev = s.current;
    }
}

TEST_CASE("[gain_smoother] mute target decays exponentially toward zero",
          "[gain_smoother]") {
    // The original plan text required `< 1e-6` after 50 ms with
    // tau=10ms. That's mathematically impossible: 50 ms is ~5 tau, so
    // exp(-5) ~= 6.7e-3 is the floor for an ideal one-pole. The intent
    // of the test is to confirm a mute target genuinely drives the
    // smoother audibly inaudible — not absolute silence — so we assert
    // a -40 dB threshold (linear < 1e-2) at 50 ms, matching what a
    // listener perceives as silent, plus a stricter 200 ms ceiling for
    // hard-mute completion (~20 tau, well below FP underflow).
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 1.0f;
    // 50 ms at 48 kHz = 2400 frames. 38 blocks of 64 covers it.
    const float at_50ms = runBlocks(s, 0.0f, 64, 38);
    REQUIRE(at_50ms < 1e-2f);                   // -40 dB or better
    REQUIRE(at_50ms < std::exp(-4.5f));         // within ~10% of ideal exp(-5)

    // 200 ms is ~20 tau; well below FP single-precision underflow
    // for the recurrence so we can require true near-zero.
    runBlocks(s, 0.0f, 64, 113);                // additional ~150 ms
    REQUIRE(s.current < 1e-6f);
    REQUIRE(s.current >= 0.0f);
}

TEST_CASE("[gain_smoother] block_frames=0 is a no-op",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 0.7f;
    s.step(0.0f, 0);
    REQUIRE(s.current == 0.7f);
}

// ---------------------------------------------------------------------------
// Edge-case coverage (extends the plan-mandated tests above).
// ---------------------------------------------------------------------------

TEST_CASE("[gain_smoother] NaN target leaves current unchanged",
          "[gain_smoother]") {
    // Documented policy (see gain_smoother.hpp): step() rejects a
    // non-finite target and leaves current untouched. Rationale:
    // accepting NaN would propagate through the IIR and corrupt the
    // smoother permanently — far worse than dropping a single update.
    // The atomic_set_route_gain control-thread setter already clamps
    // its inputs, so this guard exists as a defence-in-depth invariant
    // for the RT thread.
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 0.42f;
    s.step(std::numeric_limits<float>::quiet_NaN(), 64);
    REQUIRE(s.current == 0.42f);
    REQUIRE(std::isfinite(s.current));

    // +inf is also rejected.
    s.step(std::numeric_limits<float>::infinity(), 64);
    REQUIRE(s.current == 0.42f);

    // -inf is also rejected.
    s.step(-std::numeric_limits<float>::infinity(), 64);
    REQUIRE(s.current == 0.42f);
}

TEST_CASE("[gain_smoother] non-positive sample rate disables smoothing",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(0.0, 0.010);
    REQUIRE(s.pole_per_frame == 0.0f);
    // step() with no smoothing configured snaps directly to target.
    s.step(0.25f, 64);
    REQUIRE(s.current == 0.25f);
}

TEST_CASE("[gain_smoother] negative sample rate disables smoothing",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(-48000.0, 0.010);
    REQUIRE(s.pole_per_frame == 0.0f);
    s.step(0.3f, 64);
    REQUIRE(s.current == 0.3f);
}

TEST_CASE("[gain_smoother] non-positive tau disables smoothing",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.0);
    REQUIRE(s.pole_per_frame == 0.0f);
    s.step(0.6f, 64);
    REQUIRE(s.current == 0.6f);
}

TEST_CASE("[gain_smoother] negative tau disables smoothing",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, -0.010);
    REQUIRE(s.pole_per_frame == 0.0f);
    s.step(0.6f, 64);
    REQUIRE(s.current == 0.6f);
}

TEST_CASE("[gain_smoother] very large block size converges within 1e-6",
          "[gain_smoother]") {
    // A 1M-frame block at 48 kHz / tau=10ms is ~21 seconds of audio,
    // ~2100 time constants. pole^frames underflows to 0 in float,
    // collapsing the recurrence to current = target. Assert that the
    // result is finite and within 1e-6 of the target.
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 1.0f;
    s.step(0.5f, 1u << 20);
    REQUIRE(std::isfinite(s.current));
    REQUIRE(std::fabs(s.current - 0.5f) < 1e-6f);
}

TEST_CASE("[gain_smoother] symmetric step: down then up both reach 95%",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);

    // 30 ms at 48 kHz = 1440 frames; run 22 blocks of 64.
    // Down from 1.0 -> 0.5: 95% point is 0.525.
    float v = runBlocks(s, 0.5f, 64, 22);
    REQUIRE(v <= 0.525f + 0.005f);
    REQUIRE(v >= 0.475f);

    // Drive to a clean 0.5 baseline so the second leg starts at the
    // same place regardless of the residual approximation above.
    s.current = 0.5f;

    // Up from 0.5 -> 1.0: 95% point is 0.975.
    v = runBlocks(s, 1.0f, 64, 22);
    REQUIRE(v >= 0.975f - 0.005f);
    REQUIRE(v <= 1.0f + 1e-6f);
}

TEST_CASE("[gain_smoother] two instances at different rates are independent",
          "[gain_smoother]") {
    GainSmoother a;
    GainSmoother b;
    a.setTimeConstant(48000.0, 0.010);
    b.setTimeConstant(96000.0, 0.010);
    REQUIRE(a.pole_per_frame != b.pole_per_frame);

    // Drive A to 0.0; B should still be at unity until stepped.
    a.step(0.0f, 1u << 16);
    REQUIRE(a.current < 1e-6f);
    REQUIRE(b.current == 1.0f);

    // Now drive B; A's state is unaffected.
    b.step(0.25f, 1u << 16);
    REQUIRE(std::fabs(b.current - 0.25f) < 1e-6f);
    REQUIRE(a.current < 1e-6f);
}

TEST_CASE("[gain_smoother] default-constructed current is unity",
          "[gain_smoother]") {
    // Pre-condition for the "fresh smoother passes audio unchanged
    // until first step" guarantee documented in the header.
    GainSmoother s;
    REQUIRE(s.current == 1.0f);
    REQUIRE(s.pole_per_frame == 0.0f);
}

TEST_CASE("[gain_smoother] pre-loaded current is preserved by setTimeConstant",
          "[gain_smoother]") {
    // Callers (RouteManager::attemptStart) may want to seed the
    // smoother to the current target gain before configuring its time
    // constant, so that route start-up doesn't pop from unity.
    // setTimeConstant must not touch `current`.
    GainSmoother s;
    s.current = 0.5f;
    s.setTimeConstant(48000.0, 0.010);
    REQUIRE(s.current == 0.5f);
    // And the seeded value persists across an initial unity-target step
    // that converges back toward 1.0 — i.e. the seed isn't snapped.
    s.step(1.0f, 1);
    REQUIRE(s.current >= 0.5f);
    REQUIRE(s.current < 1.0f);
}
