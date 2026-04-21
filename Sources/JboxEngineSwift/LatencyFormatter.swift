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
