import Foundation
import Testing
@testable import JboxEngineSwift

// Unit tests for the Codable persistence layer (docs/spec.md § 3.1).
// These exercise JSON round-trips and defaults without touching the
// filesystem — `StateStoreTests.swift` covers the on-disk writer.
//
// Structured so each entity's shape is pinned by at least one
// round-trip case, plus schema-version / forward-compat guards on the
// root `StoredAppState`.

// MARK: - Helpers

private enum JSON {
    static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        e.dateEncodingStrategy = .iso8601
        return e
    }()

    static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()

    static func roundTrip<T: Codable>(_ value: T) throws -> T {
        let data = try encoder.encode(value)
        return try decoder.decode(T.self, from: data)
    }
}

// MARK: - BufferSizePolicy Codable

@Suite("BufferSizePolicy Codable")
struct BufferSizePolicyCodableTests {
    @Test(".useDeviceSetting encodes as 0")
    func useDeviceSettingEncodesAsZero() throws {
        let data = try JSON.encoder.encode(BufferSizePolicy.useDeviceSetting)
        let text = String(decoding: data, as: UTF8.self)
        #expect(text == "0")
    }

    @Test(".explicitOverride encodes as the frame count")
    func explicitOverrideEncodesAsFrames() throws {
        let data = try JSON.encoder.encode(BufferSizePolicy.explicitOverride(frames: 256))
        let text = String(decoding: data, as: UTF8.self)
        #expect(text == "256")
    }

    @Test("decoding 0 yields .useDeviceSetting")
    func decodeZeroIsUseDeviceSetting() throws {
        let data = Data("0".utf8)
        let policy = try JSON.decoder.decode(BufferSizePolicy.self, from: data)
        #expect(policy == .useDeviceSetting)
    }

    @Test("decoding N yields .explicitOverride(N)")
    func decodeNonZeroIsOverride() throws {
        let data = Data("512".utf8)
        let policy = try JSON.decoder.decode(BufferSizePolicy.self, from: data)
        #expect(policy == .explicitOverride(frames: 512))
    }

    @Test("round-trip preserves every frame choice")
    func roundTripFrameChoices() throws {
        for frames in BufferSizePolicy.frameChoices {
            let value = BufferSizePolicy.explicitOverride(frames: frames)
            #expect(try JSON.roundTrip(value) == value)
        }
    }
}

// MARK: - StoredPreferences

@Suite("StoredPreferences Codable")
struct StoredPreferencesCodableTests {
    @Test("defaults match spec § 3.1.5 (+ showDiagnostics / shareDevicesByDefault extensions)")
    func defaultsMatchSpec() {
        let p = StoredPreferences()
        #expect(p.launchAtLogin == false)
        #expect(p.bufferSizePolicy == .useDeviceSetting)
        #expect(p.resamplerQuality == .mastering)
        #expect(p.appearance == .system)
        #expect(p.showMetersInMenuBar == false)
        #expect(p.showDiagnostics == false)
        #expect(p.shareDevicesByDefault == false)
    }

    @Test("shareDevicesByDefault round-trips and defaults to false on missing key")
    func shareDevicesByDefaultRoundTrip() throws {
        let on = StoredPreferences(shareDevicesByDefault: true)
        #expect(try JSON.roundTrip(on).shareDevicesByDefault == true)
        // Missing key on a pre-Phase-7.5 state.json decodes to the
        // safe default (preserves today's exclusive behaviour).
        let data = Data("{}".utf8)
        let decoded = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(decoded.shareDevicesByDefault == false)
    }

    @Test("showDiagnostics round-trips and defaults to false on missing key")
    func showDiagnosticsRoundTrip() throws {
        let on = StoredPreferences(showDiagnostics: true)
        #expect(try JSON.roundTrip(on).showDiagnostics == true)
        let data = Data(#"{"showDiagnostics": false}"#.utf8)
        let decoded = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(decoded.showDiagnostics == false)
    }

    @Test("default-initialised preferences round-trip")
    func defaultsRoundTrip() throws {
        let p = StoredPreferences()
        #expect(try JSON.roundTrip(p) == p)
    }

    @Test("non-default preferences round-trip")
    func nonDefaultRoundTrip() throws {
        let p = StoredPreferences(
            launchAtLogin: true,
            bufferSizePolicy: .explicitOverride(frames: 128),
            resamplerQuality: .highQuality,
            appearance: .dark,
            showMetersInMenuBar: true)
        #expect(try JSON.roundTrip(p) == p)
    }

    @Test("decoding JSON with missing keys falls back to defaults")
    func missingKeysUseDefaults() throws {
        // A previous schema version (or a hand-edited file) may drop keys
        // that were added later. Tolerate that rather than refusing to
        // load — additive fields should never break backward-compat.
        let data = Data("{}".utf8)
        let p = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(p == StoredPreferences())
    }

    @Test("decoding JSON with extra keys ignores them")
    func extraKeysIgnored() throws {
        let data = Data(#"{"launchAtLogin": true, "futureToggle": "value"}"#.utf8)
        let p = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(p.launchAtLogin == true)
    }
}

// MARK: - StoredRoute

@Suite("StoredRoute Codable")
struct StoredRouteCodableTests {
    private func sampleEdges() -> [ChannelEdge] {
        [ChannelEdge(src: 0, dst: 0), ChannelEdge(src: 1, dst: 1)]
    }

    @Test("default-initialised stored route round-trips")
    func defaultRoundTrip() throws {
        let r = StoredRoute(
            id: UUID(uuidString: "3F2504E0-4F89-11D3-9A0C-0305E82C3301")!,
            name: "V31 → Apollo",
            isAutoName: false,
            sourceDevice: DeviceReference(uid: "src-uid", lastKnownName: "V31"),
            destDevice:   DeviceReference(uid: "dst-uid", lastKnownName: "Apollo"),
            mapping: sampleEdges(),
            createdAt: Date(timeIntervalSince1970: 1_700_000_000),
            modifiedAt: Date(timeIntervalSince1970: 1_700_000_100))
        #expect(try JSON.roundTrip(r) == r)
    }

    @Test("latencyMode and bufferFrames survive the round-trip")
    func latencyFieldsRoundTrip() throws {
        // Extension to spec § 3.1.3: without persisting these, users lose
        // their tier / per-route buffer-size override on every relaunch,
        // which defeats the point of per-route Performance mode.
        let r = StoredRoute(
            id: UUID(),
            name: "custom", isAutoName: false,
            sourceDevice: DeviceReference(uid: "a", lastKnownName: "A"),
            destDevice:   DeviceReference(uid: "b", lastKnownName: "B"),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            createdAt: Date(), modifiedAt: Date(),
            latencyMode: .performance,
            bufferFrames: 64)
        let copy = try JSON.roundTrip(r)
        #expect(copy.latencyMode == .performance)
        #expect(copy.bufferFrames == 64)
    }

    @Test("fan-out mapping (1:N) round-trips intact")
    func fanOutMappingRoundTrip() throws {
        // Phase 6 added fan-out support (one src → multiple dsts); the
        // persistence layer must preserve duplicate-src edges in order,
        // otherwise a saved route "Mic → L+R" would silently lose a
        // speaker on relaunch.
        let fanout: [ChannelEdge] = [
            ChannelEdge(src: 0, dst: 0),
            ChannelEdge(src: 0, dst: 1),
            ChannelEdge(src: 0, dst: 2),
        ]
        let r = StoredRoute(
            id: UUID(), name: "fan-out", isAutoName: false,
            sourceDevice: DeviceReference(uid: "s", lastKnownName: "S"),
            destDevice:   DeviceReference(uid: "d", lastKnownName: "D"),
            mapping: fanout,
            createdAt: Date(), modifiedAt: Date())
        let copy = try JSON.roundTrip(r)
        #expect(copy.mapping == fanout)
        #expect(copy.mapping.count == 3)
    }

    @Test("shareDevices round-trips as Bool? (nil inherits global default)")
    func shareDevicesRoundTrip() throws {
        // nil means "inherit the global default" — the resolution
        // rule is exercised by AppStateShareDeviceResolutionTests
        // below. Round-trip here pins the on-disk shape.
        let withShare = StoredRoute(
            id: UUID(), name: "share", isAutoName: false,
            sourceDevice: DeviceReference(uid: "s", lastKnownName: "S"),
            destDevice:   DeviceReference(uid: "d", lastKnownName: "D"),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            createdAt: Date(), modifiedAt: Date(),
            shareDevices: true)
        #expect(try JSON.roundTrip(withShare).shareDevices == true)

        let withoutShare = StoredRoute(
            id: UUID(), name: "explicit-false", isAutoName: false,
            sourceDevice: DeviceReference(uid: "s", lastKnownName: "S"),
            destDevice:   DeviceReference(uid: "d", lastKnownName: "D"),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            createdAt: Date(), modifiedAt: Date(),
            shareDevices: false)
        #expect(try JSON.roundTrip(withoutShare).shareDevices == false)
    }

    @Test("missing shareDevices key decodes to nil (pre-Phase-7.5 JSON)")
    func shareDevicesMissingIsNil() throws {
        let json = """
        {
          "id": "\(UUID().uuidString)",
          "name": "legacy", "isAutoName": true,
          "sourceDevice": {"uid": "s", "lastKnownName": "S"},
          "destDevice":   {"uid": "d", "lastKnownName": "D"},
          "mapping": [{"src": 0, "dst": 0}],
          "createdAt":  "2026-01-01T00:00:00Z",
          "modifiedAt": "2026-01-01T00:00:00Z"
        }
        """
        let r = try JSON.decoder.decode(StoredRoute.self, from: Data(json.utf8))
        #expect(r.shareDevices == nil)
    }

    @Test("missing latencyMode decodes to .off; missing bufferFrames to nil")
    func legacyMissingFieldsUseDefaults() throws {
        let src = UUID().uuidString
        let dst = UUID().uuidString
        let id  = UUID().uuidString
        // Mirrors a v1-shaped route written before latencyMode landed.
        let json = """
        {
          "id": "\(id)",
          "name": "legacy",
          "isAutoName": true,
          "sourceDevice": {"uid": "\(src)", "lastKnownName": "S"},
          "destDevice":   {"uid": "\(dst)", "lastKnownName": "D"},
          "mapping": [{"src": 0, "dst": 0}],
          "createdAt": "2026-01-01T00:00:00Z",
          "modifiedAt": "2026-01-01T00:00:00Z"
        }
        """
        let r = try JSON.decoder.decode(StoredRoute.self, from: Data(json.utf8))
        #expect(r.latencyMode == .off)
        #expect(r.bufferFrames == nil)
    }
}

// MARK: - StoredScene

@Suite("StoredScene Codable")
struct StoredSceneCodableTests {
    @Test(".exclusive round-trips")
    func exclusiveRoundTrip() throws {
        let s = StoredScene(id: UUID(), name: "Tracking",
                            routeIds: [UUID(), UUID()],
                            activationMode: .exclusive)
        #expect(try JSON.roundTrip(s) == s)
    }

    @Test(".additive round-trips")
    func additiveRoundTrip() throws {
        let s = StoredScene(id: UUID(), name: "Monitoring",
                            routeIds: [UUID()],
                            activationMode: .additive)
        #expect(try JSON.roundTrip(s) == s)
    }

    @Test("activationMode encodes as exact spec string")
    func activationModeEncoding() throws {
        let s = StoredScene(id: UUID(), name: "S", routeIds: [],
                            activationMode: .exclusive)
        let data = try JSON.encoder.encode(s)
        let text = String(decoding: data, as: UTF8.self)
        #expect(text.contains("\"exclusive\""))
    }
}

// MARK: - StoredAppState root

@Suite("StoredAppState Codable")
struct StoredAppStateCodableTests {
    @Test("default initializer yields empty state with schemaVersion == current")
    func defaultInit() {
        let s = StoredAppState()
        #expect(s.schemaVersion == StoredAppState.currentSchemaVersion)
        #expect(s.routes.isEmpty)
        #expect(s.scenes.isEmpty)
        #expect(s.preferences == StoredPreferences())
        #expect(s.lastQuittedAt == nil)
    }

    @Test("empty state round-trips")
    func emptyRoundTrip() throws {
        let s = StoredAppState()
        #expect(try JSON.roundTrip(s) == s)
    }

    @Test("state with a route + scene + non-default prefs round-trips")
    func populatedRoundTrip() throws {
        let rid = UUID()
        let route = StoredRoute(
            id: rid,
            name: "V31 → Apollo", isAutoName: true,
            sourceDevice: DeviceReference(uid: "s", lastKnownName: "V31"),
            destDevice:   DeviceReference(uid: "d", lastKnownName: "Apollo"),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            createdAt:  Date(timeIntervalSince1970: 1_700_000_000),
            modifiedAt: Date(timeIntervalSince1970: 1_700_000_100))
        let scene = StoredScene(id: UUID(), name: "Tracking",
                                routeIds: [rid],
                                activationMode: .exclusive)
        let prefs = StoredPreferences(
            launchAtLogin: true,
            bufferSizePolicy: .explicitOverride(frames: 128),
            resamplerQuality: .highQuality,
            appearance: .dark,
            showMetersInMenuBar: false)
        let s = StoredAppState(
            schemaVersion: 1,
            routes: [route],
            scenes: [scene],
            preferences: prefs,
            lastQuittedAt: Date(timeIntervalSince1970: 1_700_000_500))
        #expect(try JSON.roundTrip(s) == s)
    }

    @Test("root JSON is pretty-printed with top-level schemaVersion")
    func rootIsPrettyPrintedAndTagged() throws {
        let s = StoredAppState()
        let data = try JSON.encoder.encode(s)
        let text = String(decoding: data, as: UTF8.self)
        #expect(text.contains("\"schemaVersion\" : 1"))
        // Pretty-printed output contains newlines — diff-friendly.
        #expect(text.contains("\n"))
    }

    @Test("decoding tolerates missing lastQuittedAt")
    func missingLastQuittedAt() throws {
        let json = """
        {
          "schemaVersion": 1,
          "routes": [],
          "scenes": [],
          "preferences": {}
        }
        """
        let s = try JSON.decoder.decode(StoredAppState.self, from: Data(json.utf8))
        #expect(s.lastQuittedAt == nil)
        #expect(s.schemaVersion == 1)
    }

    @Test("decoding succeeds when schemaVersion == current")
    func schemaVersionMatches() throws {
        let json = #"{"schemaVersion": \#(StoredAppState.currentSchemaVersion), "routes": [], "scenes": [], "preferences": {}}"#
        let s = try JSON.decoder.decode(StoredAppState.self, from: Data(json.utf8))
        #expect(s.schemaVersion == StoredAppState.currentSchemaVersion)
    }

    @Test("routes preserve their order through the round-trip")
    func multipleRoutesPreserveOrder() throws {
        // Route ordering is user-visible in the sidebar; a round-trip
        // that silently re-sorted routes would surprise anyone who has
        // deliberately arranged their list.
        func mk(_ uid: String) -> StoredRoute {
            StoredRoute(
                id: UUID(), name: uid, isAutoName: false,
                sourceDevice: DeviceReference(uid: uid, lastKnownName: uid),
                destDevice:   DeviceReference(uid: "out", lastKnownName: "O"),
                mapping: [ChannelEdge(src: 0, dst: 0)],
                createdAt: Date(), modifiedAt: Date())
        }
        let state = StoredAppState(routes: [mk("a"), mk("b"), mk("c"), mk("d")])
        let copy = try JSON.roundTrip(state)
        #expect(copy.routes.map { $0.name } == ["a", "b", "c", "d"])
    }

    @Test("lastQuittedAt round-trips to the nearest second")
    func lastQuittedAtRoundTrips() throws {
        // Not asserted before — the timestamp is a future-UX hook (spec
        // § 3.1.6) so round-trip discipline matters even while unused.
        let when = Date(timeIntervalSince1970: 1_700_000_321)
        let s = StoredAppState(lastQuittedAt: when)
        #expect(try JSON.roundTrip(s).lastQuittedAt == when)
    }
}
