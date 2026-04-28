import Foundation

// MARK: - Codable conformance on existing engine-facing value types
//
// `ChannelEdge` and `DeviceReference` carry the conformance on their
// primary declarations (Swift's auto-synthesis for structs requires
// it); the raw-value enums below accept `Codable` from an extension.

extension LatencyMode:             Codable {}
extension Engine.ResamplerQuality: Codable {}
extension AppearanceMode:          Codable {}

// MARK: - StoredPreferences

/// User preferences persisted to `state.json` (docs/spec.md § 3.1.4).
/// Distinct from the engine-facing value types above: those describe
/// an individual setting's shape; this bundles the full preferences
/// document that rides inside `StoredAppState`. All fields are
/// optional on decode so additive schema changes don't break older
/// files — the defaults match spec § 3.1.4.
public struct StoredPreferences: Codable, Equatable, Sendable {
    public var launchAtLogin: Bool
    public var resamplerQuality: Engine.ResamplerQuality
    public var appearance: AppearanceMode
    public var showMetersInMenuBar: Bool
    /// Advanced-tab diagnostics toggle (Phase 6 refinement #4). Added
    /// to `StoredPreferences` as an additive extension of spec § 3.1.4
    /// so the user's choice survives relaunch instead of living only
    /// in `UserDefaults` alongside the other preferences.
    public var showDiagnostics: Bool

    public init(launchAtLogin: Bool = false,
                resamplerQuality: Engine.ResamplerQuality = .mastering,
                appearance: AppearanceMode = .system,
                showMetersInMenuBar: Bool = false,
                showDiagnostics: Bool = false) {
        self.launchAtLogin         = launchAtLogin
        self.resamplerQuality      = resamplerQuality
        self.appearance            = appearance
        self.showMetersInMenuBar   = showMetersInMenuBar
        self.showDiagnostics       = showDiagnostics
    }

    // Missing keys fall back to the struct's defaults rather than
    // failing decode — additive fields don't break existing files.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.launchAtLogin         = try c.decodeIfPresent(Bool.self, forKey: .launchAtLogin) ?? false
        self.resamplerQuality      = try c.decodeIfPresent(Engine.ResamplerQuality.self, forKey: .resamplerQuality) ?? .mastering
        self.appearance            = try c.decodeIfPresent(AppearanceMode.self, forKey: .appearance) ?? .system
        self.showMetersInMenuBar   = try c.decodeIfPresent(Bool.self, forKey: .showMetersInMenuBar) ?? false
        self.showDiagnostics       = try c.decodeIfPresent(Bool.self, forKey: .showDiagnostics) ?? false
    }
}

// MARK: - StoredRoute

/// Durable representation of a user-configured route (spec § 3.1.3).
/// The runtime `Route` in `EngineStore.swift` keys on engine-assigned
/// `UInt32` ids that do not survive a process restart — this type
/// keeps a `UUID` so the route has a stable identity across launches.
///
/// `latencyMode` extends the spec's v1 field list — added in Phase 6
/// post-Slice-B so the user's tier choice survives relaunch. It is
/// optional on decode (default `.off`) so pre-Phase-7 state files
/// still load.
public struct StoredRoute: Codable, Equatable, Identifiable, Sendable {
    public let id: UUID
    public var name: String
    public var isAutoName: Bool
    public var sourceDevice: DeviceReference
    public var destDevice: DeviceReference
    public var mapping: [ChannelEdge]
    public let createdAt: Date
    public var modifiedAt: Date
    public var latencyMode: LatencyMode
    public var bufferFrames: UInt32?
    /// ABI v14 — master VCA-style fader, in dB. Default 0 (unity).
    /// Optional on decode so pre-v14 state.json files load unchanged.
    public var masterGainDb: Float
    /// ABI v14 — per-channel trim, in dB. Empty means "no trim" ≡ all 0 dB.
    public var trimDbs: [Float]
    /// ABI v14 — mute toggle, independent of fader state.
    public var muted: Bool

    public init(id: UUID,
                name: String,
                isAutoName: Bool,
                sourceDevice: DeviceReference,
                destDevice: DeviceReference,
                mapping: [ChannelEdge],
                createdAt: Date,
                modifiedAt: Date,
                latencyMode: LatencyMode = .off,
                bufferFrames: UInt32? = nil,
                masterGainDb: Float = 0.0,
                trimDbs: [Float] = [],
                muted: Bool = false) {
        self.id           = id
        self.name         = name
        self.isAutoName   = isAutoName
        self.sourceDevice = sourceDevice
        self.destDevice   = destDevice
        self.mapping      = mapping
        self.createdAt    = createdAt
        self.modifiedAt   = modifiedAt
        self.latencyMode  = latencyMode
        self.bufferFrames = bufferFrames
        self.masterGainDb = masterGainDb
        self.trimDbs      = trimDbs
        self.muted        = muted
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.id           = try c.decode(UUID.self, forKey: .id)
        self.name         = try c.decode(String.self, forKey: .name)
        self.isAutoName   = try c.decode(Bool.self, forKey: .isAutoName)
        self.sourceDevice = try c.decode(DeviceReference.self, forKey: .sourceDevice)
        self.destDevice   = try c.decode(DeviceReference.self, forKey: .destDevice)
        self.mapping      = try c.decode([ChannelEdge].self, forKey: .mapping)
        self.createdAt    = try c.decode(Date.self, forKey: .createdAt)
        self.modifiedAt   = try c.decode(Date.self, forKey: .modifiedAt)
        self.latencyMode  = try c.decodeIfPresent(LatencyMode.self, forKey: .latencyMode) ?? .off
        self.bufferFrames = try c.decodeIfPresent(UInt32.self, forKey: .bufferFrames)
        self.masterGainDb = try c.decodeIfPresent(Float.self, forKey: .masterGainDb) ?? 0.0
        self.trimDbs      = try c.decodeIfPresent([Float].self, forKey: .trimDbs) ?? []
        self.muted        = try c.decodeIfPresent(Bool.self, forKey: .muted) ?? false
    }
}

// MARK: - StoredAppState (root document)

/// The root `state.json` document (docs/spec.md § 3.1.5). Carries the
/// schema version tag, the user's persisted entities, and a
/// `lastQuittedAt` timestamp for future UX (e.g. "welcome back"
/// banners after a crash). Empty state is the legitimate first-launch
/// shape.
public struct StoredAppState: Codable, Equatable, Sendable {
    /// Bump when the on-disk schema changes incompatibly; add a
    /// matching entry to `StateStore`'s migration ladder. Staying at
    /// `1` through Phase 7 because every field added so far is
    /// additive (missing keys decode to their defaults).
    public static let currentSchemaVersion: Int = 1

    public var schemaVersion: Int
    public var routes: [StoredRoute]
    public var preferences: StoredPreferences
    public var lastQuittedAt: Date?

    public init(schemaVersion: Int = StoredAppState.currentSchemaVersion,
                routes: [StoredRoute] = [],
                preferences: StoredPreferences = StoredPreferences(),
                lastQuittedAt: Date? = nil) {
        self.schemaVersion = schemaVersion
        self.routes        = routes
        self.preferences   = preferences
        self.lastQuittedAt = lastQuittedAt
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.schemaVersion = try c.decode(Int.self, forKey: .schemaVersion)
        self.routes        = try c.decodeIfPresent([StoredRoute].self, forKey: .routes) ?? []
        self.preferences   = try c.decodeIfPresent(StoredPreferences.self, forKey: .preferences) ?? StoredPreferences()
        self.lastQuittedAt = try c.decodeIfPresent(Date.self, forKey: .lastQuittedAt)
    }
}
