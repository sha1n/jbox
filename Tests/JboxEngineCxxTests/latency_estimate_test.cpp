// latency_estimate_test.cpp — unit tests for the per-route latency
// estimator (spec.md § 2.12).
//
// The estimator is a pure function: it takes the component frame counts
// + sample rates and returns a single end-to-end latency in
// microseconds. The sum is split at the SRC/DST-rate boundary so a
// route that bridges 48 kHz ↔ 44.1 kHz composes correctly; a same-rate
// route degenerates to the single-rate formula printed in the spec.

#include "latency_estimate.hpp"

#include <catch_amalgamated.hpp>

using jbox::control::LatencyComponents;
using jbox::control::estimateLatencyMicroseconds;

namespace {
// Round to the nearest microsecond for tolerant comparisons against
// hand-computed expected values — integer division in the estimator can
// truncate by up to 1 us.
constexpr std::uint64_t kTolUs = 2;
}

TEST_CASE("[latency_estimate] zero components yield zero microseconds",
          "[latency_estimate]") {
    LatencyComponents c{};
    c.src_sample_rate_hz = 48000.0;
    c.dst_sample_rate_hz = 48000.0;
    REQUIRE(estimateLatencyMicroseconds(c) == 0);
}

TEST_CASE("[latency_estimate] same-rate sum matches the spec formula",
          "[latency_estimate]") {
    // 48 kHz both sides. Every frame is 1000000/48000 = 20.833... µs.
    // Compose a realistic route:
    //   src_hal=24, src_safety=16, src_buffer=256, ring_target=2047,
    //   converter_prime=24, dst_buffer=256, dst_safety=16, dst_hal=24
    // total_frames = 2663; expected us ≈ 2663 * 20.8333 ≈ 55479 us.
    LatencyComponents c{};
    c.src_hal_latency_frames       = 24;
    c.src_safety_offset_frames     = 16;
    c.src_buffer_frames            = 256;
    c.ring_target_fill_frames      = 2047;
    c.converter_prime_frames       = 24;
    c.dst_buffer_frames            = 256;
    c.dst_safety_offset_frames     = 16;
    c.dst_hal_latency_frames       = 24;
    c.src_sample_rate_hz           = 48000.0;
    c.dst_sample_rate_hz           = 48000.0;

    const auto us = estimateLatencyMicroseconds(c);
    const std::uint64_t expected =
        static_cast<std::uint64_t>(2663.0 * 1'000'000.0 / 48000.0);
    REQUIRE(us >= expected - kTolUs);
    REQUIRE(us <= expected + kTolUs);
}

TEST_CASE("[latency_estimate] different src/dst rates compose side-by-side",
          "[latency_estimate]") {
    // src=48000, dst=44100. src-side totals 2343 frames, dst-side 320.
    // Expected us = 2343*1e6/48000 + 320*1e6/44100
    //             = 48812.5 + 7256.2 = 56068 us (≈).
    LatencyComponents c{};
    c.src_hal_latency_frames   = 24;
    c.src_safety_offset_frames = 16;
    c.src_buffer_frames        = 256;
    c.ring_target_fill_frames  = 2047;
    c.converter_prime_frames   = 24;
    c.dst_buffer_frames        = 256;
    c.dst_safety_offset_frames = 16;
    c.dst_hal_latency_frames   = 24;
    c.src_sample_rate_hz       = 48000.0;
    c.dst_sample_rate_hz       = 44100.0;

    const auto us = estimateLatencyMicroseconds(c);
    const double expected_f =
        2343.0 * 1'000'000.0 / 48000.0
      + 320.0  * 1'000'000.0 / 44100.0;
    const auto expected = static_cast<std::uint64_t>(expected_f);
    REQUIRE(us >= expected - kTolUs);
    REQUIRE(us <= expected + kTolUs);
}

TEST_CASE("[latency_estimate] missing src rate → 0 (unknown)",
          "[latency_estimate]") {
    LatencyComponents c{};
    c.src_buffer_frames = 256;
    c.dst_buffer_frames = 256;
    c.src_sample_rate_hz = 0.0;
    c.dst_sample_rate_hz = 48000.0;
    REQUIRE(estimateLatencyMicroseconds(c) == 0);
}

TEST_CASE("[latency_estimate] missing dst rate → 0 (unknown)",
          "[latency_estimate]") {
    LatencyComponents c{};
    c.src_buffer_frames = 256;
    c.dst_buffer_frames = 256;
    c.src_sample_rate_hz = 48000.0;
    c.dst_sample_rate_hz = 0.0;
    REQUIRE(estimateLatencyMicroseconds(c) == 0);
}

TEST_CASE("[latency_estimate] negative rate → 0 (defensive)",
          "[latency_estimate]") {
    LatencyComponents c{};
    c.src_buffer_frames = 256;
    c.dst_buffer_frames = 256;
    c.src_sample_rate_hz = -48000.0;
    c.dst_sample_rate_hz = 48000.0;
    REQUIRE(estimateLatencyMicroseconds(c) == 0);
}

TEST_CASE("[latency_estimate] ring target fill dominates when large",
          "[latency_estimate]") {
    // Exercise the Phase 6 production sizing: 4096-floor ring means
    // target_fill = 2047 frames. Nothing else set. At 48 kHz that's
    // ~42 651 µs — the "why is my latency 40 ms" power-user number.
    LatencyComponents c{};
    c.ring_target_fill_frames = 2047;
    c.src_sample_rate_hz = 48000.0;
    c.dst_sample_rate_hz = 48000.0;
    const auto us = estimateLatencyMicroseconds(c);
    const auto expected =
        static_cast<std::uint64_t>(2047.0 * 1'000'000.0 / 48000.0);
    REQUIRE(us >= expected - kTolUs);
    REQUIRE(us <= expected + kTolUs);
}

TEST_CASE("[latency_estimate] is monotonic — adding frames never decreases us",
          "[latency_estimate]") {
    LatencyComponents base{};
    base.src_buffer_frames = 128;
    base.dst_buffer_frames = 128;
    base.src_sample_rate_hz = 48000.0;
    base.dst_sample_rate_hz = 48000.0;
    const auto baseline = estimateLatencyMicroseconds(base);

    LatencyComponents more = base;
    more.src_hal_latency_frames = 32;
    REQUIRE(estimateLatencyMicroseconds(more) >= baseline);

    LatencyComponents even_more = more;
    even_more.dst_hal_latency_frames = 32;
    REQUIRE(estimateLatencyMicroseconds(even_more) >= estimateLatencyMicroseconds(more));
}
