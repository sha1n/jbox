import Foundation

// MARK: - User-facing preference value types
//
// Typed enums that back the three-tab Preferences window (docs/spec.md
// § 4.6). Living here — not in the app target — so the raw-value
// mappings and defaults can be exercised by Swift Testing without
// pulling SwiftUI or AppKit into the test target.

/// Appearance preference. Applied to the main `WindowGroup` (and the
/// menu bar popover) via `.preferredColorScheme(...)`. `.system`
/// returns `nil` so SwiftUI falls back to the OS-level appearance.
public enum AppearanceMode: String, Sendable, Equatable, Hashable, CaseIterable {
    case system = "system"
    case light  = "light"
    case dark   = "dark"

    /// Default — follow the OS appearance. Matches `Preferences`
    /// defaults in spec § 3.1.5.
    public static let `default`: AppearanceMode = .system

    /// Recover a stored raw value defensively — unknown strings (a
    /// preferences file edited by hand, a forward-migrated schema)
    /// fall back to the default rather than crashing.
    public init(rawValueOrDefault raw: String) {
        self = AppearanceMode(rawValue: raw) ?? Self.default
    }
}

/// Buffer-size policy. When set to `.useDeviceSetting` (default), Jbox
/// leaves each device's HAL buffer at whatever size it currently
/// reports. When set to `.explicitOverride`, `AddRouteSheet` defaults
/// new Performance-mode routes to `frames`; the user can still change
/// the value per-route on the sheet before confirming. Existing routes
/// are not retroactively reconfigured.
public enum BufferSizePolicy: Sendable, Equatable, Hashable {
    case useDeviceSetting
    case explicitOverride(frames: UInt32)

    public static let `default`: BufferSizePolicy = .useDeviceSetting

    /// Canonical frame options surfaced in the picker. Matches the
    /// AddRouteSheet per-route choices so the two presets feel
    /// consistent and we avoid showing values the HAL will never
    /// actually honour.
    public static let frameChoices: [UInt32] = [32, 64, 128, 256, 512, 1024]

    /// When `.explicitOverride(N)`, the frame count; otherwise `nil`.
    public var frames: UInt32? {
        switch self {
        case .useDeviceSetting:             return nil
        case .explicitOverride(let frames): return frames
        }
    }

    /// Stored raw representation — 0 means "use device setting",
    /// anything else means explicit-override with that frame count.
    /// `@AppStorage` accepts `Int`, so we encode through a single
    /// `UInt32` value and the app layer bridges to the key.
    public var storedRaw: UInt32 {
        switch self {
        case .useDeviceSetting:             return 0
        case .explicitOverride(let frames): return frames
        }
    }

    /// Build from the stored raw representation. Unknown frame
    /// counts (not in `frameChoices`) are accepted as-is on the
    /// Swift side — the HAL range clamp in the engine is the true
    /// gate on validity.
    public init(storedRaw raw: UInt32) {
        self = (raw == 0)
            ? .useDeviceSetting
            : .explicitOverride(frames: raw)
    }
}
