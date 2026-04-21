// latency_estimate.cpp — compute end-to-end route latency in µs.
//
// See latency_estimate.hpp and docs/spec.md § 2.12 for the formula and
// per-component semantics.

#include "latency_estimate.hpp"

namespace jbox::control {

std::uint64_t estimateLatencyMicroseconds(const LatencyComponents& c) {
    if (!(c.src_sample_rate_hz > 0.0) || !(c.dst_sample_rate_hz > 0.0)) {
        return 0;
    }

    const double src_side_frames =
        static_cast<double>(c.src_hal_latency_frames)
      + static_cast<double>(c.src_safety_offset_frames)
      + static_cast<double>(c.src_buffer_frames)
      + static_cast<double>(c.ring_target_fill_frames);

    const double dst_side_frames =
        static_cast<double>(c.converter_prime_frames)
      + static_cast<double>(c.dst_buffer_frames)
      + static_cast<double>(c.dst_safety_offset_frames)
      + static_cast<double>(c.dst_hal_latency_frames);

    const double us =
        src_side_frames * 1'000'000.0 / c.src_sample_rate_hz
      + dst_side_frames * 1'000'000.0 / c.dst_sample_rate_hz;

    if (!(us > 0.0)) return 0;
    return static_cast<std::uint64_t>(us);
}

}  // namespace jbox::control
