import CoreGraphics
import Foundation

/// Pure dB-to-fraction math and color-zone classification for the
/// per-channel meter bars. Lives in the Swift engine module so it can
/// be unit-tested without pulling in SwiftUI; spec § 4.5 defines the
/// thresholds.
public enum MeterLevel {

    /// Display noise floor — peaks at or below this level render as
    /// empty (no fill).
    public static let floorDb: Float = -60

    /// Upper bound of the green zone (exclusive); anything above is the
    /// yellow "near clip" zone.
    public static let nearDb: Float = -6

    /// Lower bound of the red "clipped" zone (inclusive).
    public static let clipDb: Float = -3

    public enum Zone: Equatable, Sendable {
        case silent
        case normal
        case near
        case clip
    }

    /// Map a linear peak sample (0…≥1) to a 0…1 bar-fill fraction.
    public static func fractionFor(peak: Float) -> Float {
        guard peak > 0 else { return 0 }
        return fractionFor(dB: 20 * log10f(peak))
    }

    /// Map a dBFS value directly to a 0…1 bar-fill fraction.
    public static func fractionFor(dB: Float) -> Float {
        let clamped = max(floorDb, min(0, dB))
        return (clamped - floorDb) / -floorDb
    }

    /// Classify a linear peak sample into one of the four zones.
    public static func zone(for peak: Float) -> Zone {
        guard peak > 0 else { return .silent }
        let dB = 20 * log10f(peak)
        if dB <= floorDb { return .silent }
        if dB >= clipDb  { return .clip }
        if dB >= nearDb  { return .near }
        return .normal
    }

    /// Standard dBFS marks for the DAW-style meter scale (descending,
    /// inclusive of 0 and `floorDb`). Used by the SwiftUI `DbScale`
    /// view to draw gridlines + labels along the bar zone.
    /// See `docs/spec.md` § 4.5.
    /// Map a dBFS mark to the y coordinate of its tick line inside a
    /// `DbScale` Canvas of total height `canvasHeight`. The bar zone
    /// occupies the inner subrange `[labelOverflow, canvasHeight -
    /// labelOverflow]`; the extra `labelOverflow` of headroom above
    /// and below is reserved for the "0" and "-60" labels so they
    /// don't get clipped by the Canvas's own bounds. The tick line
    /// for 0 dBFS lands exactly on the bar zone's top edge regardless
    /// of how much headroom is reserved, and likewise for −60 dBFS at
    /// the bottom edge.
    public static func dbScaleTickY(forDb dB: Float,
                                    canvasHeight: CGFloat,
                                    labelOverflow: CGFloat) -> CGFloat {
        let inner = max(0, canvasHeight - 2 * labelOverflow)
        let frac = fractionFor(dB: dB)
        return labelOverflow + inner * CGFloat(1 - frac)
    }

    public static let dawScaleMarks: [(dB: Float, label: String)] = [
        (0,   "0"),
        (-3,  "-3"),
        (-6,  "-6"),
        (-12, "-12"),
        (-18, "-18"),
        (-24, "-24"),
        (-36, "-36"),
        (-48, "-48"),
        (-60, "-60"),
    ]
}
