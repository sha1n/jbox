import Foundation

// MARK: - Codable conformance on existing engine-facing value types
//
// `ChannelEdge` and `DeviceReference` carry the conformance on their
// primary declarations (Swift's auto-synthesis for structs requires
// it); the raw-value enums below accept `Codable` from an extension.

extension LatencyMode:             Codable {}
extension Engine.ResamplerQuality: Codable {}
extension AppearanceMode:          Codable {}

// `BufferSizePolicy` has an associated-value case, so the auto-synthesis
// would produce a tagged shape. We already maintain a `storedRaw: UInt32`
// form for `@AppStorage` — encode that same single integer on disk so
// state.json round-trips against the pre-v1 UserDefaults representation
// and remains diff-readable (0 = useDeviceSetting; N = frames).
extension BufferSizePolicy: Codable {
    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        let raw = try container.decode(UInt32.self)
        self.init(storedRaw: raw)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(storedRaw)
    }
}

// MARK: - StoredPreferences

/// User preferences persisted to `state.json` (docs/spec.md § 3.1.5).
/// Distinct from the engine-facing value types above: those describe
/// an individual setting's shape; this bundles the full preferences
/// document that rides inside `StoredAppState`. All fields are
/// optional on decode so additive schema changes don't break older
/// files — the defaults match spec § 3.1.5.
public struct StoredPreferences: Codable, Equatable, Sendable {
    public var launchAtLogin: Bool
    public var bufferSizePolicy: BufferSizePolicy
    public var resamplerQuality: Engine.ResamplerQuality
    public var appearance: AppearanceMode
    public var showMetersInMenuBar: Bool
    /// Advanced-tab diagnostics toggle (Phase 6 refinement #4). Added
    /// to `StoredPreferences` as an additive extension of spec § 3.1.5
    /// so the user's choice survives relaunch instead of living only
    /// in `UserDefaults` alongside the other preferences.
    public var showDiagnostics: Bool

    public init(launchAtLogin: Bool = false,
                bufferSizePolicy: BufferSizePolicy = .useDeviceSetting,
                resamplerQuality: Engine.ResamplerQuality = .mastering,
                appearance: AppearanceMode = .system,
                showMetersInMenuBar: Bool = false,
                showDiagnostics: Bool = false) {
        self.launchAtLogin       = launchAtLogin
        self.bufferSizePolicy    = bufferSizePolicy
        self.resamplerQuality    = resamplerQuality
        self.appearance          = appearance
        self.showMetersInMenuBar = showMetersInMenuBar
        self.showDiagnostics     = showDiagnostics
    }

    // Missing keys fall back to the struct's defaults rather than
    // failing decode — additive fields don't break existing files.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.launchAtLogin       = try c.decodeIfPresent(Bool.self, forKey: .launchAtLogin) ?? false
        self.bufferSizePolicy    = try c.decodeIfPresent(BufferSizePolicy.self, forKey: .bufferSizePolicy) ?? .useDeviceSetting
        self.resamplerQuality    = try c.decodeIfPresent(Engine.ResamplerQuality.self, forKey: .resamplerQuality) ?? .mastering
        self.appearance          = try c.decodeIfPresent(AppearanceMode.self, forKey: .appearance) ?? .system
        self.showMetersInMenuBar = try c.decodeIfPresent(Bool.self, forKey: .showMetersInMenuBar) ?? false
        self.showDiagnostics     = try c.decodeIfPresent(Bool.self, forKey: .showDiagnostics) ?? false
    }
}

// MARK: - StoredRoute

/// Durable representation of a user-configured route (spec § 3.1.3).
/// The runtime `Route` in `EngineStore.swift` keys on engine-assigned
/// `UInt32` ids that do not survive a process restart — this type
/// keeps a `UUID` so persisted scenes can reference specific routes
/// across launches.
///
/// `latencyMode` and `bufferFrames` extend the spec's field list. They
/// were added in Phase 6 post-Slice-B and are required on disk so a
/// user's Performance-tier choice survives relaunch; both are
/// optional on decode (defaults: `.off`, `nil`) so pre-Phase-7
/// state files still load.
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

    public init(id: UUID,
                name: String,
                isAutoName: Bool,
                sourceDevice: DeviceReference,
                destDevice: DeviceReference,
                mapping: [ChannelEdge],
                createdAt: Date,
                modifiedAt: Date,
                latencyMode: LatencyMode = .off,
                bufferFrames: UInt32? = nil) {
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
    }
}

// MARK: - StoredScene

/// Named set of routes to activate together (spec § 3.1.4). Scene
/// activation logic is out of scope for the persistence slice; the
/// type exists so a future scene editor can round-trip through
/// `state.json` without a schema bump.
public enum StoredSceneActivationMode: String, Codable, Equatable, Sendable, CaseIterable {
    case exclusive
    case additive

    public static let `default`: StoredSceneActivationMode = .exclusive
}

public struct StoredScene: Codable, Equatable, Identifiable, Sendable {
    public let id: UUID
    public var name: String
    public var routeIds: [UUID]
    public var activationMode: StoredSceneActivationMode

    public init(id: UUID,
                name: String,
                routeIds: [UUID],
                activationMode: StoredSceneActivationMode = .default) {
        self.id             = id
        self.name           = name
        self.routeIds       = routeIds
        self.activationMode = activationMode
    }
}

// MARK: - StoredAppState (root document)

/// The root `state.json` document (docs/spec.md § 3.1.6). Carries the
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
    public var scenes: [StoredScene]
    public var preferences: StoredPreferences
    public var lastQuittedAt: Date?

    public init(schemaVersion: Int = StoredAppState.currentSchemaVersion,
                routes: [StoredRoute] = [],
                scenes: [StoredScene] = [],
                preferences: StoredPreferences = StoredPreferences(),
                lastQuittedAt: Date? = nil) {
        self.schemaVersion = schemaVersion
        self.routes        = routes
        self.scenes        = scenes
        self.preferences   = preferences
        self.lastQuittedAt = lastQuittedAt
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.schemaVersion = try c.decode(Int.self, forKey: .schemaVersion)
        self.routes        = try c.decodeIfPresent([StoredRoute].self, forKey: .routes) ?? []
        self.scenes        = try c.decodeIfPresent([StoredScene].self, forKey: .scenes) ?? []
        self.preferences   = try c.decodeIfPresent(StoredPreferences.self, forKey: .preferences) ?? StoredPreferences()
        self.lastQuittedAt = try c.decodeIfPresent(Date.self, forKey: .lastQuittedAt)
    }
}
