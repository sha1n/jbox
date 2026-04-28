import Foundation
import Testing
@testable import JboxEngineSwift
@_exported import JboxEngineC

/// Coverage for Task 11 of the Route Gain + Mixer-Strip plan: the
/// `EngineStore` setters that wrap the ABI v14 per-route gain symbols
/// (`jbox_engine_set_route_master_gain_db` / `_channel_trim_db` /
/// `_mute`). These tests exercise the in-memory model updates and the
/// non-finite handling policy (Option A: NaN rejected, -infinity
/// clamped to `FaderTaper.minFiniteDb` so the persisted `Route` is
/// always finite — keeps `StateStore`'s default `JSONEncoder` safe).
///
/// Driven through the live Core Audio engine in the same style as
/// `EngineStoreTests.swift`. Tests that need a live route skip
/// gracefully on CI runners that don't expose at least one input- and
/// one output-capable device.
@MainActor
@Suite("EngineStoreGain")
struct EngineStoreGainTests {

    /// Build a store with one stopped route configured against the
    /// first available input + output devices on the runner. Returns
    /// `nil` when the runner cannot furnish the requested mapping
    /// width — callers should treat that as "skip the test".
    private func makeStoreWithRoute(channels: Int) throws -> (store: EngineStore, routeId: UInt32)? {
        let store = try EngineStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= UInt32(channels) }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= UInt32(channels) })
        else {
            return nil
        }
        let mapping: [ChannelEdge] = (0..<channels).map {
            ChannelEdge(src: UInt32($0), dst: UInt32($0))
        }
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: mapping,
            name: "gain-test")
        let route = try store.addRoute(cfg)
        return (store, route.id)
    }

    @Test("setMasterGainDb updates the in-memory model")
    func setsMaster() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }

        store.setMasterGainDb(routeId: routeId, db: -3.0)

        #expect(store.routes[0].masterGainDb == -3.0)
        #expect(store.lastError == nil)
        store.removeRoute(routeId)
    }

    @Test("setChannelTrimDb updates only the indexed channel")
    func setsTrim() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }

        store.setChannelTrimDb(routeId: routeId, channelIndex: 1, db: -2.5)

        #expect(store.routes[0].trimDbs.count == 2)
        #expect(store.routes[0].trimDbs[0] == 0.0)
        #expect(store.routes[0].trimDbs[1] == -2.5)
        store.removeRoute(routeId)
    }

    @Test("setRouteMuted toggles the model")
    func togglesMute() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }

        store.setRouteMuted(routeId: routeId, muted: true)
        #expect(store.routes[0].muted == true)

        store.setRouteMuted(routeId: routeId, muted: false)
        #expect(store.routes[0].muted == false)
        store.removeRoute(routeId)
    }

    @Test("setMasterGainDb on unknown route id is a no-op (no crash)")
    func setMasterUnknownIdNoOps() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }

        store.setMasterGainDb(routeId: 9999, db: -3.0)

        // No crash; existing routes unchanged.
        #expect(store.routes[0].masterGainDb == 0.0)
        store.removeRoute(routeId)
    }

    @Test("setMasterGainDb rejects NaN (model unchanged)")
    func setMasterRejectsNaN() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }
        store.setMasterGainDb(routeId: routeId, db: 5.0)
        #expect(store.routes[0].masterGainDb == 5.0)

        store.setMasterGainDb(routeId: routeId, db: .nan)

        #expect(store.routes[0].masterGainDb == 5.0)   // unchanged
        store.removeRoute(routeId)
    }

    @Test("setMasterGainDb clamps -infinity to minFiniteDb (Option A)")
    func setMasterClampsNegativeInfinity() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }

        store.setMasterGainDb(routeId: routeId, db: -.infinity)

        // Per Option A: stored value is clamped to FaderTaper.minFiniteDb
        // so StateStore's default encoder (.throw on non-finite) stays safe.
        #expect(store.routes[0].masterGainDb == FaderTaper.minFiniteDb)
        store.removeRoute(routeId)
    }

    @Test("setChannelTrimDb extends a default-empty trimDbs to mapping length")
    func setChannelTrimExtendsDefaultEmptyArray() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }
        // Initially: trimDbs == []
        #expect(store.routes[0].trimDbs.isEmpty)

        store.setChannelTrimDb(routeId: routeId, channelIndex: 0, db: -1.0)

        #expect(store.routes[0].trimDbs.count == 2)
        #expect(store.routes[0].trimDbs[0] == -1.0)
        #expect(store.routes[0].trimDbs[1] == 0.0)
        store.removeRoute(routeId)
    }

    @Test("setChannelTrimDb on out-of-range channel is a no-op")
    func setChannelTrimOutOfRangeNoOps() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }

        store.setChannelTrimDb(routeId: routeId, channelIndex: 5, db: -1.0)

        // No crash; trimDbs not extended (out-of-range guard fires
        // before the pad-to-mapping-length branch).
        #expect(store.routes[0].trimDbs.isEmpty)
        store.removeRoute(routeId)
    }

    // MARK: - Per-channel mute (Route.channelMuted, UI-only)

    @Test("setChannelMuted toggles the model and pads the array")
    func setChannelMutedTogglesModel() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }
        // Initially: channelMuted == [].
        #expect(store.routes[0].channelMuted.isEmpty)

        store.setChannelMuted(routeId: routeId, channelIndex: 0, muted: true)
        #expect(store.routes[0].channelMuted == [true, false])

        store.setChannelMuted(routeId: routeId, channelIndex: 1, muted: true)
        #expect(store.routes[0].channelMuted == [true, true])

        store.setChannelMuted(routeId: routeId, channelIndex: 0, muted: false)
        #expect(store.routes[0].channelMuted == [false, true])

        store.removeRoute(routeId)
    }

    @Test("setChannelMuted leaves trimDbs alone (fader doesn't move)")
    func setChannelMutedPreservesTrim() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }
        store.setChannelTrimDb(routeId: routeId, channelIndex: 0, db: -3.0)
        store.setChannelTrimDb(routeId: routeId, channelIndex: 1, db: -1.5)

        store.setChannelMuted(routeId: routeId, channelIndex: 0, muted: true)

        // Trim model unchanged — the user's intended trim is preserved
        // for restore on un-mute, the engine separately receives -∞
        // for the muted channel.
        #expect(store.routes[0].trimDbs == [-3.0, -1.5])
        #expect(store.routes[0].channelMuted == [true, false])

        store.setChannelMuted(routeId: routeId, channelIndex: 0, muted: false)
        #expect(store.routes[0].trimDbs == [-3.0, -1.5])
        #expect(store.routes[0].channelMuted == [false, false])

        store.removeRoute(routeId)
    }

    @Test("setChannelTrimDb while muted updates the model but not the engine")
    func setChannelTrimDbWhileMuted() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }
        store.setChannelMuted(routeId: routeId, channelIndex: 0, muted: true)

        // Drag the fader while muted — the model picks up the new
        // intended trim, the engine stays silent until un-mute.
        store.setChannelTrimDb(routeId: routeId, channelIndex: 0, db: -6.0)
        #expect(store.routes[0].trimDbs[0] == -6.0)
        #expect(store.routes[0].channelMuted[0] == true)

        // Un-mute — engine now sees the dragged-while-muted trim.
        store.setChannelMuted(routeId: routeId, channelIndex: 0, muted: false)
        #expect(store.routes[0].trimDbs[0] == -6.0)
        #expect(store.routes[0].channelMuted[0] == false)

        store.removeRoute(routeId)
    }

    @Test("setChannelMuted on out-of-range channel is a no-op")
    func setChannelMutedOutOfRange() async throws {
        guard let (store, routeId) = try makeStoreWithRoute(channels: 2) else {
            Issue.record("CI runner expected to expose at least one 2-channel input- and one 2-channel output-capable device")
            return
        }
        store.setChannelMuted(routeId: routeId, channelIndex: 9, muted: true)
        #expect(store.routes[0].channelMuted.isEmpty)
        store.removeRoute(routeId)
    }
}
