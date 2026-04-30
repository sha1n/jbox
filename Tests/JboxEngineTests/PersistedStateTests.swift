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

// MARK: - StoredPreferences

@Suite("StoredPreferences Codable")
struct StoredPreferencesCodableTests {
    @Test("defaults match spec § 3.1.4 (+ showDiagnostics + hasShownLaunchAtLoginNote extensions)")
    func defaultsMatchSpec() {
        let p = StoredPreferences()
        #expect(p.launchAtLogin == false)
        #expect(p.resamplerQuality == .mastering)
        #expect(p.appearance == .system)
        #expect(p.showMetersInMenuBar == false)
        #expect(p.showDiagnostics == false)
        #expect(p.hasShownLaunchAtLoginNote == false)
    }

    @Test("showDiagnostics round-trips and defaults to false on missing key")
    func showDiagnosticsRoundTrip() throws {
        let on = StoredPreferences(showDiagnostics: true)
        #expect(try JSON.roundTrip(on).showDiagnostics == true)
        let data = Data(#"{"showDiagnostics": false}"#.utf8)
        let decoded = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(decoded.showDiagnostics == false)
    }

    @Test("hasShownLaunchAtLoginNote round-trips when true")
    func hasShownLaunchAtLoginNoteRoundTripTrue() throws {
        let p = StoredPreferences(hasShownLaunchAtLoginNote: true)
        #expect(try JSON.roundTrip(p).hasShownLaunchAtLoginNote == true)
    }

    @Test("hasShownLaunchAtLoginNote defaults to false on missing key (pre-Phase-7 launch-at-login files)")
    func hasShownLaunchAtLoginNoteMissingKey() throws {
        // Files written before the launch-at-login wiring landed will not
        // carry the key. Decode must fall back to false so existing users
        // see the explanatory note on their first toggle, not silently
        // skip it because of a missing key.
        let data = Data(#"{"launchAtLogin": false}"#.utf8)
        let p = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(p.hasShownLaunchAtLoginNote == false)
    }

    @Test("hasShownLaunchAtLoginNote tolerates extra keys around it")
    func hasShownLaunchAtLoginNoteExtraKeys() throws {
        let data = Data(#"""
        {"hasShownLaunchAtLoginNote": true, "futureToggle": "x"}
        """#.utf8)
        let p = try JSON.decoder.decode(StoredPreferences.self, from: data)
        #expect(p.hasShownLaunchAtLoginNote == true)
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

    @Test("latencyMode survives the round-trip")
    func latencyFieldsRoundTrip() throws {
        // Extension to spec § 3.1.3: without persisting `latencyMode`,
        // users lose their tier choice on every relaunch, which
        // defeats the point of per-route Performance mode.
        let r = StoredRoute(
            id: UUID(),
            name: "custom", isAutoName: false,
            sourceDevice: DeviceReference(uid: "a", lastKnownName: "A"),
            destDevice:   DeviceReference(uid: "b", lastKnownName: "B"),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            createdAt: Date(), modifiedAt: Date(),
            latencyMode: .performance)
        let copy = try JSON.roundTrip(r)
        #expect(copy.latencyMode == .performance)
    }

    @Test("bufferFrames survives the round-trip and decodes to nil when absent")
    func bufferFramesRoundTrip() throws {
        // ABI v11: per-route HAL buffer-frame-size preference. Pinned
        // on disk so the user's drum-monitoring 16-frame override
        // doesn't reset to "no preference" on every relaunch.
        let r = StoredRoute(
            id: UUID(),
            name: "perf-16", isAutoName: false,
            sourceDevice: DeviceReference(uid: "a", lastKnownName: "A"),
            destDevice:   DeviceReference(uid: "b", lastKnownName: "B"),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            createdAt: Date(), modifiedAt: Date(),
            latencyMode: .performance,
            bufferFrames: 16)
        let copy = try JSON.roundTrip(r)
        #expect(copy.bufferFrames == 16)

        // Missing key on a pre-v11 state.json decodes to nil
        // ("no preference"), preserving the relaunch contract.
        let id = UUID().uuidString
        let json = """
        {
          "id": "\(id)",
          "name": "no-buf",
          "isAutoName": false,
          "sourceDevice": {"uid": "a", "lastKnownName": "A"},
          "destDevice":   {"uid": "b", "lastKnownName": "B"},
          "mapping": [{"src": 0, "dst": 0}],
          "createdAt":  "2026-01-01T00:00:00Z",
          "modifiedAt": "2026-01-01T00:00:00Z",
          "latencyMode": 2
        }
        """
        let decoded = try JSON.decoder.decode(StoredRoute.self, from: Data(json.utf8))
        #expect(decoded.bufferFrames == nil)
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

    @Test("missing latencyMode decodes to .off (pre-Phase-7 v1 shape)")
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
    }

    @Test("Phase-7.5/Phase-6-shape state.json with dropped fields decodes cleanly (silent discard)")
    func phase75ShapeDecodesWithDroppedFieldsDiscarded() throws {
        // Phase 7.6 removed `bufferFrames` from StoredRoute and
        // `bufferSizePolicy` / `shareDevicesByDefault` from
        // StoredPreferences (along with the per-route `shareDevices`).
        // The plan-deviation note claims old `state.json` files still
        // decode cleanly because JSONDecoder ignores unknown keys and
        // surviving fields use `decodeIfPresent` with defaults. This
        // test pins that promise: a payload carrying every dropped
        // key on both StoredRoute and StoredPreferences must decode
        // into the new shape with the dropped values silently
        // discarded and the surviving fields intact.
        let id = UUID().uuidString
        let routeJson = """
        {
          "id": "\(id)",
          "name": "v75-route",
          "isAutoName": false,
          "sourceDevice": {"uid": "s", "lastKnownName": "S"},
          "destDevice":   {"uid": "d", "lastKnownName": "D"},
          "mapping": [{"src": 0, "dst": 0}],
          "createdAt":  "2026-01-01T00:00:00Z",
          "modifiedAt": "2026-01-01T00:00:00Z",
          "latencyMode": 2,
          "bufferFrames": 64,
          "shareDevices": true
        }
        """
        let route = try JSON.decoder.decode(StoredRoute.self, from: Data(routeJson.utf8))
        #expect(route.name == "v75-route")
        #expect(route.latencyMode == .performance)

        let prefsJson = """
        {
          "launchAtLogin": true,
          "bufferSizePolicy": 128,
          "resamplerQuality": 1,
          "appearance": "dark",
          "showMetersInMenuBar": false,
          "showDiagnostics": true,
          "shareDevicesByDefault": true
        }
        """
        let prefs = try JSON.decoder.decode(StoredPreferences.self, from: Data(prefsJson.utf8))
        #expect(prefs.launchAtLogin == true)
        #expect(prefs.resamplerQuality == .highQuality)
        #expect(prefs.appearance == .dark)
        #expect(prefs.showDiagnostics == true)
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
        #expect(s.preferences == StoredPreferences())
        #expect(s.lastQuittedAt == nil)
    }

    @Test("empty state round-trips")
    func emptyRoundTrip() throws {
        let s = StoredAppState()
        #expect(try JSON.roundTrip(s) == s)
    }

    @Test("state with a route + non-default prefs round-trips")
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
        let prefs = StoredPreferences(
            launchAtLogin: true,
            resamplerQuality: .highQuality,
            appearance: .dark,
            showMetersInMenuBar: false)
        let s = StoredAppState(
            schemaVersion: 1,
            routes: [route],
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
          "preferences": {}
        }
        """
        let s = try JSON.decoder.decode(StoredAppState.self, from: Data(json.utf8))
        #expect(s.lastQuittedAt == nil)
        #expect(s.schemaVersion == 1)
    }

    @Test("decoding succeeds when schemaVersion == current")
    func schemaVersionMatches() throws {
        let json = #"{"schemaVersion": \#(StoredAppState.currentSchemaVersion), "routes": [], "preferences": {}}"#
        let s = try JSON.decoder.decode(StoredAppState.self, from: Data(json.utf8))
        #expect(s.schemaVersion == StoredAppState.currentSchemaVersion)
    }

    @Test("routes preserve their order through the round-trip")
    func multipleRoutesPreserveOrder() throws {
        // Route ordering is user-visible in the route list; a round-trip
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
        // § 3.1.5) so round-trip discipline matters even while unused.
        let when = Date(timeIntervalSince1970: 1_700_000_321)
        let s = StoredAppState(lastQuittedAt: when)
        #expect(try JSON.roundTrip(s).lastQuittedAt == when)
    }
}
