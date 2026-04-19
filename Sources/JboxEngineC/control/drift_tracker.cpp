// drift_tracker.cpp — Phase 4 PI gains (named constants).
//
// These are the Phase 4 production gains used by RouteRecord's per-
// route DriftTracker. They come from two sources:
//   (1) the empirical starting values in docs/spec.md § 2.6, and
//   (2) the tuning harness in
//       Tests/JboxEngineCxxTests/drift_tuning_harness.cpp.
//
// Selection rationale
// -------------------
// The tuning harness sweeps (kp, ki) across a 5x4 grid at +/-50 ppm
// simulated drift. Under the current simulated-backend test shape
// (SimulatedBackend + real Apple AudioConverter), ring fill is bounded
// primarily by AudioConverter's internal buffer flush that fires on
// every setInputRate() call — not by the PI controller itself. All
// grid points therefore converge at roughly the same time with similar
// steady-state error; the harness output does not distinguish them
// meaningfully. Real-hardware tuning is part of the Phase 4 soak run
// per docs/plan.md Phase 4 exit criteria (deferred to the owner).
//
// Conclusion: pick the conservative starting values (kp = 1e-6,
// ki = 1e-8, max = 100 ppm). These match both docs and the drift
// tracker's own header comment. Higher kp risks oscillation when
// real-hardware tuning eventually happens on top of a real converter
// without the setInputRate-flush side-effect; lower kp is slower to
// respond. 1e-6 is the middle of the grid and the safest starting
// point for the hardware pass.
//
// Units
// -----
//   kp          : ppm per frame (output ppm per unit of fill-error)
//   ki          : ppm per frame-second (integral accumulation rate)
//   max_output  : ppm absolute clamp

#include "drift_tracker.hpp"

namespace jbox::control {

constexpr double kDriftKp        = 1e-6;
constexpr double kDriftKi        = 1e-8;
constexpr double kDriftMaxOutput = 100.0;

double phase4Kp()        noexcept { return kDriftKp; }
double phase4Ki()        noexcept { return kDriftKi; }
double phase4MaxOutput() noexcept { return kDriftMaxOutput; }

}  // namespace jbox::control
