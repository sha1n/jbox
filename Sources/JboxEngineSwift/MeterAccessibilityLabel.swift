import Foundation

/// Builds a VoiceOver-friendly summary of per-channel peak levels for
/// the expanded `MeterPanel`. spec § 4.8 (accessibility) /
/// docs/plan.md Phase 6 Slice B follow-up.
///
/// Lives in the Swift engine module — not in `JboxApp` — so the pure
/// label-formatting logic is unit-testable without pulling in SwiftUI,
/// matching the convention `MeterLevel` already established.
public enum MeterAccessibilityLabel {

    /// Composite label for `.accessibilityLabel(...)` on the panel.
    /// Linear peak samples are converted to dBFS; values at or below
    /// `MeterLevel.floorDb` are reported as `silent` so listeners get
    /// a clean token instead of "minus infinity dBFS".
    public static func summary(source: [Float], destination: [Float]) -> String {
        "Source \(side(source)). Destination \(side(destination))."
    }

    private static func side(_ peaks: [Float]) -> String {
        guard !peaks.isEmpty else { return "no channels" }
        return peaks.enumerated()
            .map { i, v in "channel \(i + 1) \(level(v))" }
            .joined(separator: ", ")
    }

    private static func level(_ peak: Float) -> String {
        guard peak > 0 else { return "silent" }
        let db = 20 * log10f(peak)
        if db <= MeterLevel.floorDb { return "silent" }
        return "\(Int(db.rounded())) dBFS"
    }
}
