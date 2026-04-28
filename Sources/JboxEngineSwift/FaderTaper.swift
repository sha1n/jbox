import Foundation

/// Pure dB ↔ slider-position math for the route fader UI.
///
/// Two scales coexist:
///   * **Position** — `Float` in `0...1`, what the SwiftUI fader binds to.
///   * **dB** — `Float`, what the engine ABI takes; finite within
///     `[minFiniteDb, maxDb]`, with `-Float.infinity` representing mute
///     (linear amplitude `0`).
///
/// The taper is piecewise linear in dB:
///   * `[0, muteThresholdPosition)` → `-infinity` (snap to silence).
///   * `[muteThresholdPosition, unityPosition)` → linear in dB from
///     `-60 dB` (at the threshold) to `0 dB` (just below unity).
///   * `[unityPosition, 1]` → linear in dB from `0 dB` to `+12 dB`.
///
/// Lives in `JboxEngineSwift` (not `JboxApp`) so it is unit-testable
/// without SwiftUI. See
/// `docs/2026-04-28-route-gain-mixer-strip-design.md §§ 4.4, 7.3`.
///
/// ## Non-finite-input policy
///
/// The fader is talked to by both UI gestures and persistence reload, so
/// every entry point has to give a sane answer for non-finite inputs
/// rather than propagating NaN / ±infinity into the engine ABI.
///
/// * **NaN** — clamps to the bottom of the legal range:
///   `dbForPosition(.nan) → -infinity`,
///   `positionFor(db: .nan) → 0`,
///   `amplitudeFor(db: .nan) → 0`.
///   Treating NaN as "silence" matches what the user would want from a
///   corrupted preferences file (no surprise gain blast).
/// * **+infinity** — clamps to the top of the documented range, never
///   unbounded:
///   `dbForPosition(.infinity) → maxDb` (position clamped to `1`),
///   `positionFor(db: .infinity) → 1.0`,
///   `amplitudeFor(db: .infinity) → amplitudeFor(db: maxDb)` (the linear
///   amplitude at `+12 dB`, ≈ `3.9811`). This keeps the engine away from
///   ±infinity gain on the audio path even if a caller fumbles a
///   computation upstream.
/// * **-infinity** — represents mute, by construction:
///   `positionFor(db: -.infinity) → 0`,
///   `amplitudeFor(db: -.infinity) → 0`.
public enum FaderTaper {

    public static let maxDb: Float = 12.0
    public static let unityDb: Float = 0.0
    public static let minFiniteDb: Float = -60.0
    public static let unityPosition: Float = 0.75
    public static let muteThresholdPosition: Float = 0.04

    /// Slider position (`0...1`) → dB.
    ///
    /// See "Non-finite-input policy" in the type doc for NaN / ±infinity
    /// handling. NaN snaps to `-infinity`; out-of-range positions clamp
    /// to `[0, 1]` first.
    public static func dbForPosition(_ pos: Float) -> Float {
        if pos.isNaN { return -.infinity }
        let p = max(0, min(1, pos))
        if p < muteThresholdPosition { return -.infinity }
        if p >= unityPosition {
            // Top segment: linear in dB from 0 to +12.
            let t = (p - unityPosition) / (1.0 - unityPosition)
            return unityDb + t * (maxDb - unityDb)
        }
        // Lower segment: linear in dB from -60 (at muteThresholdPosition)
        // up to 0 (at unityPosition).
        let t = (p - muteThresholdPosition) / (unityPosition - muteThresholdPosition)
        return minFiniteDb + t * (unityDb - minFiniteDb)
    }

    /// dB → slider position (`0...1`).
    ///
    /// `-infinity` and `NaN` map to `0`; `+infinity` and any value
    /// `>= maxDb` map to `1`.
    public static func positionFor(db: Float) -> Float {
        if db.isNaN { return 0 }
        if db == -.infinity { return 0 }
        if db >= maxDb { return 1 }
        if db >= unityDb {
            let t = (db - unityDb) / (maxDb - unityDb)
            return unityPosition + t * (1.0 - unityPosition)
        }
        if db <= minFiniteDb { return muteThresholdPosition }
        let t = (db - minFiniteDb) / (unityDb - minFiniteDb)
        return muteThresholdPosition + t * (unityPosition - muteThresholdPosition)
    }

    /// dB → linear amplitude.
    ///
    /// `-infinity`, `NaN`, and any finite value at-or-below `-120 dB`
    /// (denormal guard) map to `0`. `+infinity` clamps to
    /// `amplitudeFor(db: maxDb)` — we refuse to amplify beyond the
    /// documented `[-60, +12] dB` range even when a caller hands us a
    /// non-finite gain.
    public static func amplitudeFor(db: Float) -> Float {
        if db.isNaN { return 0 }
        if db == -.infinity { return 0 }
        if db == .infinity { return powf(10.0, maxDb / 20.0) }
        if db <= -120 { return 0 }
        return powf(10.0, db / 20.0)
    }
}
