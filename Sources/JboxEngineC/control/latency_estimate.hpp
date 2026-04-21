// latency_estimate.hpp — pure-logic helper for end-to-end route latency.
//
// Implements the estimate described in docs/spec.md § 2.12: sum of
// HAL latency + safety offsets + buffer sizes + steady-state ring fill
// + SRC polyphase prime frames, reported in microseconds.
//
// The sum is split at the SRC/DST rate boundary so a route that bridges
// different device rates composes correctly (pre-converter frames use
// src_sample_rate_hz; post-converter frames use dst_sample_rate_hz).
// A same-rate route degenerates to the single-rate formula.
//
// Pure function: no I/O, no allocation, no platform calls. The engine
// computes the components once at startRoute (from HAL properties +
// AudioConverter prime info + ring sizing) and calls this to produce
// the `estimated_latency_us` reported through jbox_route_status_t.

#ifndef JBOX_CONTROL_LATENCY_ESTIMATE_HPP
#define JBOX_CONTROL_LATENCY_ESTIMATE_HPP

#include <cstdint>

namespace jbox::control {

struct LatencyComponents {
    // Source side — contributions measured in frames at src_sample_rate_hz.
    std::uint32_t src_hal_latency_frames   = 0;  // kAudioDevicePropertyLatency, input scope
    std::uint32_t src_safety_offset_frames = 0;  // kAudioDevicePropertySafetyOffset, input scope
    std::uint32_t src_buffer_frames        = 0;  // kAudioDevicePropertyBufferFrameSize, input
    std::uint32_t ring_target_fill_frames  = 0;  // drift-sampler setpoint, ≈ ring/2

    // Destination side — contributions measured in frames at dst_sample_rate_hz.
    std::uint32_t converter_prime_frames   = 0;  // AudioConverter kAudioConverterPrimeInfo
    std::uint32_t dst_buffer_frames        = 0;  // kAudioDevicePropertyBufferFrameSize, output
    std::uint32_t dst_safety_offset_frames = 0;  // kAudioDevicePropertySafetyOffset, output scope
    std::uint32_t dst_hal_latency_frames   = 0;  // kAudioDevicePropertyLatency, output scope

    double src_sample_rate_hz = 0.0;
    double dst_sample_rate_hz = 0.0;
};

// Estimate total route latency, in microseconds. Returns 0 when either
// sample rate is missing (≤ 0) — the UI treats 0 as "unknown" and hides
// the pill.
std::uint64_t estimateLatencyMicroseconds(const LatencyComponents& c);

}  // namespace jbox::control

#endif  // JBOX_CONTROL_LATENCY_ESTIMATE_HPP
