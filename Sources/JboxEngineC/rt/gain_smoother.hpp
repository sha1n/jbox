// gain_smoother.hpp — RT-safe block-rate one-pole IIR for fader smoothing.
//
// Used per-route, per-channel from the output / duplex IOProc to converge
// `current` toward an atomic `target` value (linear amplitude) over a
// 10 ms time constant. Smoothing is applied once per IOProc block, not
// per sample — at sub-millisecond block sizes the audible difference is
// nil and per-sample smoothing would cost N MACs per channel.
//
// Pure value type. RT-safe by construction:
//   - no heap allocation
//   - no locks, no syscalls
//   - bounded execution time (a constant amount of arithmetic per step)
//
// `pole_per_frame` is computed once at attemptStart via
// setTimeConstant(rate, tau). Per-block recurrence:
//
//   pole_block = pole_per_frame ^ frames
//   current    = pole_block * current + (1 - pole_block) * target
//
// Mathematically equivalent to running `frames` per-sample updates with
// the same target held constant, at one pow() per block. The pole is
// sized so the 1/e time constant is `tau` regardless of block size.
//
// Non-finite-target policy
// ------------------------
// step() rejects a non-finite target (NaN / +-inf) and leaves `current`
// untouched. Accepting NaN would propagate through the recurrence and
// permanently corrupt the smoother; dropping a single update is the
// strictly better failure mode. The control-thread setter that publishes
// the atomic target already clamps its inputs, so this is a defence-in-
// depth invariant for the RT thread.

#ifndef JBOX_RT_GAIN_SMOOTHER_HPP
#define JBOX_RT_GAIN_SMOOTHER_HPP

#include <cmath>
#include <cstdint>

namespace jbox::rt {

struct GainSmoother {
    // Linear amplitude. Defaults to unity so a freshly-constructed
    // smoother passes audio through unchanged before its first step().
    float current = 1.0f;

    // Per-frame pole coefficient. 0 means "no smoothing configured" —
    // step() then snaps current to target. Real configurations always
    // populate this in setTimeConstant().
    float pole_per_frame = 0.0f;

    // Configure the smoother's time constant in seconds at the given
    // sample rate. tau_seconds is the 1/e settling time; 95% settling
    // is reached at ~3*tau (so 30 ms for tau = 10 ms).
    //
    // Does NOT touch `current`. Callers may seed `current` before or
    // after this call — useful at route start-up to avoid popping from
    // the default unity to whatever target the user has configured.
    void setTimeConstant(double sample_rate, double tau_seconds) noexcept {
        if (sample_rate <= 0.0 || tau_seconds <= 0.0) {
            pole_per_frame = 0.0f;
            return;
        }
        // Per-frame pole p such that y[n+1] = p*y[n] + (1-p)*target
        // gives the right time constant: p = exp(-1 / (tau * fs)).
        pole_per_frame =
            static_cast<float>(std::exp(-1.0 / (tau_seconds * sample_rate)));
    }

    // Advance the smoother by `frames` frames toward `target`.
    // Block-rate update: pole_per_block = pole_per_frame ^ frames.
    // Mathematically equivalent to running `frames` per-sample updates
    // with the same target held constant, but at one pow() per block.
    //
    // Non-finite `target` (NaN / +-inf) is rejected: `current` is left
    // unchanged. See "Non-finite-target policy" in the file header for
    // rationale. std::isfinite is constexpr-friendly and does not
    // allocate, so it's permitted in RT-safe code.
    void step(float target, std::uint32_t frames) noexcept {
        if (frames == 0) return;
        if (!std::isfinite(target)) return;
        if (pole_per_frame <= 0.0f) {
            current = target;
            return;
        }
        const float pole_block =
            std::pow(pole_per_frame, static_cast<float>(frames));
        current = pole_block * current + (1.0f - pole_block) * target;
    }
};

}  // namespace jbox::rt

#endif  // JBOX_RT_GAIN_SMOOTHER_HPP
