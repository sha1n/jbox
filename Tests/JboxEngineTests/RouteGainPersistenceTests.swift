import Foundation
import Testing
@testable import JboxEngineSwift

// Regression coverage for Task 10 of the Route Gain + Mixer-Strip
// implementation plan: `StoredRoute` must persist the v14 master /
// per-channel / mute fields additively, so existing pre-v14 state.json
// files load unchanged and the on-disk shape gains the new keys when
// they are non-default.
//
// Companion to PersistedStateTests.swift — that suite already pins the
// pre-v14 StoredRoute shape (route round-trip, missing-key defaults,
// extra-key tolerance). This file narrows in on the gain triplet.

@Suite("RouteGainPersistence")
struct RouteGainPersistenceTests {

    /// Build a baseline StoredRoute with the v14 fields at their defaults.
    private static func makeBaseline() -> StoredRoute {
        StoredRoute(
            id: UUID(uuidString: "11111111-2222-3333-4444-555555555555")!,
            name: "test",
            isAutoName: false,
            sourceDevice: DeviceReference(uid: "src.uid", lastKnownName: "Src"),
            destDevice:   DeviceReference(uid: "dst.uid", lastKnownName: "Dst"),
            mapping: [ChannelEdge(src: 0, dst: 0), ChannelEdge(src: 1, dst: 1)],
            createdAt: Date(timeIntervalSince1970: 0),
            modifiedAt: Date(timeIntervalSince1970: 0)
        )
    }

    @Test("StoredRoute round-trips with non-default gain fields")
    func roundTripWithFields() throws {
        var stored = Self.makeBaseline()
        stored.masterGainDb = -3.5
        stored.trimDbs = [-1.0, 0.5]
        stored.muted = true
        stored.channelMuted = [true, false]

        let data = try JSONEncoder().encode(stored)
        let decoded = try JSONDecoder().decode(StoredRoute.self, from: data)

        #expect(decoded.masterGainDb == -3.5)
        #expect(decoded.trimDbs == [-1.0, 0.5])
        #expect(decoded.muted == true)
        #expect(decoded.channelMuted == [true, false])
        #expect(decoded == stored)   // Equatable should match end-to-end
    }

    @Test("StoredRoute decodes legacy JSON without channelMuted defaults to []")
    func channelMutedLegacyDefault() throws {
        // Pre-channelMuted JSON — has masterGainDb/trimDbs/muted but
        // not channelMuted. Should decode with channelMuted == [].
        let legacyJson = """
        {
            "id": "11111111-2222-3333-4444-555555555555",
            "name": "legacy",
            "isAutoName": false,
            "sourceDevice": { "uid": "src.uid", "lastKnownName": "Src" },
            "destDevice":   { "uid": "dst.uid", "lastKnownName": "Dst" },
            "mapping": [{"src":0,"dst":0}, {"src":1,"dst":1}],
            "createdAt": 0,
            "modifiedAt": 0,
            "masterGainDb": -3.0,
            "trimDbs": [-1.0, 0.5],
            "muted": true
        }
        """

        let decoded = try JSONDecoder().decode(
            StoredRoute.self,
            from: Data(legacyJson.utf8))

        #expect(decoded.channelMuted == [])
        #expect(decoded.muted == true)
    }

    @Test("StoredRoute decodes legacy JSON without the gain keys")
    func decodesLegacyJSONWithoutGain() throws {
        // Pre-v14 JSON shape — no masterGainDb / trimDbs / muted.
        let legacyJson = """
        {
            "id": "11111111-2222-3333-4444-555555555555",
            "name": "legacy",
            "isAutoName": false,
            "sourceDevice": { "uid": "src.uid", "lastKnownName": "Src" },
            "destDevice":   { "uid": "dst.uid", "lastKnownName": "Dst" },
            "mapping": [{"src":0,"dst":0}],
            "createdAt": 0,
            "modifiedAt": 0
        }
        """

        let decoded = try JSONDecoder().decode(
            StoredRoute.self,
            from: Data(legacyJson.utf8))

        #expect(decoded.masterGainDb == 0.0)
        #expect(decoded.trimDbs == [])
        #expect(decoded.muted == false)
    }

    @Test("StoredRoute encoded JSON contains the gain keys when set")
    func encodedShape() throws {
        var stored = Self.makeBaseline()
        stored.masterGainDb = -3.0
        stored.muted = true

        let data = try JSONEncoder().encode(stored)
        let json = String(decoding: data, as: UTF8.self)

        #expect(json.contains("masterGainDb"))
        #expect(json.contains("muted"))
        // trimDbs is empty (not set non-default), but for additive keys
        // we still expect them serialized — Swift's JSONEncoder includes
        // the empty array. Document this expectation.
        #expect(json.contains("trimDbs"))
    }

    @Test("StoredRoute defaults match unity / no-trim / unmuted")
    func defaultsAreUnity() {
        let stored = Self.makeBaseline()
        #expect(stored.masterGainDb == 0.0)
        #expect(stored.trimDbs == [])
        #expect(stored.muted == false)
    }

    @Test("StoredAppState with mixed-version routes round-trips")
    func mixedVersionRoutes() throws {
        // Two routes — one with v14 gain fields set, one without (default).
        var stored1 = Self.makeBaseline()
        stored1.masterGainDb = -6.0
        stored1.trimDbs = [-1.0, -0.5]
        stored1.muted = false

        var stored2 = Self.makeBaseline()
        stored2.masterGainDb = 0.0   // unity, default
        stored2.muted = true

        let app = StoredAppState(routes: [stored1, stored2])
        let data = try JSONEncoder().encode(app)
        let decoded = try JSONDecoder().decode(StoredAppState.self, from: data)

        #expect(decoded.routes.count == 2)
        #expect(decoded.routes[0].masterGainDb == -6.0)
        #expect(decoded.routes[0].trimDbs == [-1.0, -0.5])
        #expect(decoded.routes[1].masterGainDb == 0.0)
        #expect(decoded.routes[1].muted == true)
    }

    // The realistic mute-position case: setting `masterGainDb` to
    // -infinity is how the engine ABI accepts "silence". We don't
    // expect users to ever land in this state through the UI (Task 11
    // clamps), but a corrupted state.json could carry it and we want
    // the persistence layer to round-trip it without crashing — at
    // least when the encoder is configured to allow non-finite floats.
    //
    // The default `JSONEncoder` THROWS on non-finite floats unless
    // `nonConformingFloatEncodingStrategy` is set. The runtime encoder
    // in `StateStore.swift` uses defaults (no override), so the engine
    // would refuse to persist `-inf` today. This test pins the
    // currently-tolerated shape: a tagged-string strategy lets the
    // value survive a round-trip when callers explicitly opt in.
    @Test("StoredRoute round-trips -infinity masterGainDb when encoder tolerates non-finite floats")
    func negativeInfinityRoundTripsWithStrategy() throws {
        var stored = Self.makeBaseline()
        stored.masterGainDb = -.infinity

        let enc = JSONEncoder()
        enc.nonConformingFloatEncodingStrategy =
            .convertToString(positiveInfinity: "+inf",
                             negativeInfinity: "-inf",
                             nan: "nan")
        let dec = JSONDecoder()
        dec.nonConformingFloatDecodingStrategy =
            .convertFromString(positiveInfinity: "+inf",
                               negativeInfinity: "-inf",
                               nan: "nan")

        let data = try enc.encode(stored)
        let decoded = try dec.decode(StoredRoute.self, from: data)
        #expect(decoded.masterGainDb == -.infinity)
    }
}
