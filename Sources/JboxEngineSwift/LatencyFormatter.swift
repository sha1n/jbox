import Foundation

/// Compact "`~NN ms`" pill formatter for per-route latency. Engine
/// reports microseconds (see `RouteStatus.estimatedLatencyUs`); this
/// pure function produces the short display string shown next to the
/// route's counters in the list. The engine already approximates the
/// number (spec.md § 2.12 is explicit about this), so rendering rounds
/// to the coarsest unit that still communicates the order of magnitude.
///
/// Buckets:
///  - 0 µs                 → `nil` (UI hides the pill)
///  - 1 ..< 1 000 µs       → `"<1 ms"`
///  - 1 000 ..< 10 000 µs  → `"~N.N ms"` (one decimal, rounded)
///  - 10 000 ..< 1 000 000 → `"~N ms"` (integer, rounded half-up)
///  - ≥ 1 000 000 µs       → `"~N.N s"` (one decimal, rounded)
public enum LatencyFormatter {
    /// Short text for a single `LatencyComponents` row in the
    /// diagnostics breakdown (Phase 6 refinement #4). Converts the
    /// given frame count at the given sample rate and renders one of
    /// three bands:
    ///   - 0 frames or 0 rate           → `"—"`
    ///   - < 1 ms                       → `"%.2f ms"`
    ///   - < 10 ms                      → `"%.1f ms"`
    ///   - ≥ 10 ms                      → `"%.0f ms"`
    /// The thresholds mirror `pillText` so adjacent rows look
    /// consistent; per-row values are component contributions, not
    /// the end-to-end total, so sub-millisecond precision is useful.
    public static func breakdownLabel(frames: UInt32, rate: Double) -> String {
        guard rate > 0, frames > 0 else { return "—" }
        let ms = Double(frames) * 1000.0 / rate
        if ms < 1.0  { return String(format: "%.2f ms", ms) }
        if ms < 10.0 { return String(format: "%.1f ms", ms) }
        return String(format: "%.0f ms", ms)
    }

    public static func pillText(microseconds: UInt64) -> String? {
        guard microseconds > 0 else { return nil }
        if microseconds < 1_000 {
            return "<1 ms"
        }
        if microseconds < 10_000 {
            let tenths = (microseconds + 50) / 100   // round to nearest tenth-ms
            let whole = tenths / 10
            let frac  = tenths % 10
            return "~\(whole).\(frac) ms"
        }
        if microseconds < 1_000_000 {
            let ms = (microseconds + 500) / 1_000    // round half-up
            return "~\(ms) ms"
        }
        let tenthsOfSec = (microseconds + 50_000) / 100_000
        let whole = tenthsOfSec / 10
        let frac  = tenthsOfSec % 10
        return "~\(whole).\(frac) s"
    }
}
