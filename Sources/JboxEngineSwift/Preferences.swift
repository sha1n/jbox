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
    /// defaults in spec § 3.1.4.
    public static let `default`: AppearanceMode = .system

    /// Recover a stored raw value defensively — unknown strings (a
    /// preferences file edited by hand, a forward-migrated schema)
    /// fall back to the default rather than crashing.
    public init(rawValueOrDefault raw: String) {
        self = AppearanceMode(rawValue: raw) ?? Self.default
    }
}
