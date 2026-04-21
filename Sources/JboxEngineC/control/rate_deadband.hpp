// rate_deadband.hpp — decide whether a proposed AudioConverter input
// rate differs from the last-applied rate by more than a "meaningful"
// threshold. Used on the RT thread to avoid re-setting
// kAudioConverterCurrentInputStreamDescription for sub-audible PI noise
// (~1e-7 ppm per tick). Every setInputRate call flushes the Apple
// converter's polyphase filter state, which on real hardware manifests
// as audible click artifacts and extra ring consumption (see
// docs/spec.md § 2.6 drift-tracker notes and drift_tracker.cpp header).
//
// Pure, header-only: trivially RT-safe and trivially unit-testable.
// Threshold is in ppm of nominal (1.0 ppm = 48 mHz at 48 kHz) — well
// below the audible rate-error threshold (~10 ppm) while still gating
// the 2e-7 ppm per-tick PI noise.

#ifndef JBOX_CONTROL_RATE_DEADBAND_HPP
#define JBOX_CONTROL_RATE_DEADBAND_HPP

#include <cmath>

namespace jbox::control {

inline constexpr double kRateDeadbandPpm = 1.0;

inline bool shouldApplyRate(double proposed,
                            double last_applied,
                            double nominal) noexcept {
    if (proposed <= 0.0) return false;
    if (nominal <= 0.0)  return false;
    const double threshold = nominal * (kRateDeadbandPpm * 1e-6);
    return std::fabs(proposed - last_applied) > threshold;
}

}  // namespace jbox::control

#endif  // JBOX_CONTROL_RATE_DEADBAND_HPP
