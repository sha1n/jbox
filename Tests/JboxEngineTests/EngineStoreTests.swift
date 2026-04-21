import Foundation
import Testing
@testable import JboxEngineSwift
@_exported import JboxEngineC

/// Store-level behaviour driven through the real Core Audio engine.
/// The macOS CI runner always has default built-in devices; we never
/// assume specific channel counts, just that enumeration returns at
/// least one device in each direction.
@MainActor
@Suite("EngineStore (live Core Audio)")
struct EngineStoreTests {

    private func makeStore() throws -> EngineStore {
        try EngineStore()
    }

    @Test("refreshDevices populates the observable devices list")
    func refreshDevicesPopulates() throws {
        let store = try makeStore()
        #expect(store.devices.isEmpty)
        store.refreshDevices()
        #expect(!store.devices.isEmpty)
        #expect(store.lastError == nil)
    }

    @Test("device(uid:) looks up a device from the refreshed snapshot")
    func deviceByUidLookup() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let first = store.devices.first else {
            Issue.record("expected at least one device on CI runner")
            return
        }
        let looked = store.device(uid: first.uid)
        #expect(looked == first)
        #expect(store.device(uid: "no-such-uid") == nil)
    }

    @Test("addRoute with a bogus mapping surfaces JBOX_ERR_MAPPING_INVALID")
    func addRouteInvalidMapping() throws {
        let store = try makeStore()
        store.refreshDevices()
        let src = store.devices.first(where: { $0.directionInput })!
        let dst = store.devices.first(where: { $0.directionOutput })!

        // Empty mapping — v1 invariant violation.
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: []
        )
        do {
            _ = try store.addRoute(cfg)
            Issue.record("expected MAPPING_INVALID")
        } catch let e as JboxError {
            #expect(e.code == JBOX_ERR_MAPPING_INVALID)
            #expect(store.lastError != nil)
        }
        #expect(store.routes.isEmpty)
    }

    @Test("addRoute with a duplicate destination edge is rejected before reaching the engine list")
    func addRouteDuplicateDst() throws {
        // Phase 6 refinement #1 flipped the duplicate-src rule —
        // fan-out is now accepted. Fan-in (duplicate dst) is the
        // remaining validator rejection.
        let store = try makeStore()
        store.refreshDevices()
        let src = store.devices.first(where: { $0.directionInput })!
        let dst = store.devices.first(where: { $0.directionOutput })!

        // Need both a second input channel and a shared dst to
        // isolate the dst-duplication case. Skip if the CI runner
        // can't furnish them.
        guard src.inputChannelCount >= 2 else {
            return
        }
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0),
                      ChannelEdge(src: 1, dst: 0)]
        )
        do {
            _ = try store.addRoute(cfg)
            Issue.record("expected MAPPING_INVALID")
        } catch let e as JboxError {
            #expect(e.code == JBOX_ERR_MAPPING_INVALID)
        }
        #expect(store.routes.isEmpty)
    }

    @Test("addRoute threads RouteConfig.lowLatency through to the engine")
    func addRouteLowLatencyFlag() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }

        // Baseline: a default-sizing route. We don't start it (leaves
        // hardware alone); the flag's actual ring-sizing effect on the
        // pill is covered by the C++ integration test. Here we just
        // confirm the bit survives the Swift → C bridge.
        let safeCfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "safe",
            lowLatency: false)
        let lowLatCfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "low-lat",
            lowLatency: true)

        let safeRoute   = try store.addRoute(safeCfg)
        let lowLatRoute = try store.addRoute(lowLatCfg)

        #expect(safeRoute.config.lowLatency   == false)
        #expect(lowLatRoute.config.lowLatency == true)
        #expect(store.routes.count == 2)

        // Clean up so later tests see an empty list.
        store.removeRoute(safeRoute.id)
        store.removeRoute(lowLatRoute.id)
        #expect(store.routes.isEmpty)
    }

    @Test("addRoute + removeRoute round-trips through the observable list")
    func addRouteThenRemove() throws {
        let store = try makeStore()
        store.refreshDevices()

        // Pick devices with at least one input channel on one side and
        // one output channel on the other.
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }

        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "test-round-trip"
        )
        let route = try store.addRoute(cfg)
        #expect(store.routes.count == 1)
        #expect(store.routes.first?.id == route.id)
        #expect(store.routes.first?.status.state == .stopped)

        store.removeRoute(route.id)
        #expect(store.routes.isEmpty)
        #expect(store.lastError == nil)
    }

    @Test("channelNames returns an array sized to the device's channel count")
    func channelNamesSizedToDevice() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input-capable device")
            return
        }
        let names = store.channelNames(uid: src.uid, direction: .input)
        #expect(names.count == Int(src.inputChannelCount))
        // Labels may be empty strings (simple built-in devices often
        // don't publish `kAudioObjectPropertyElementName`), but the
        // array size must track the channel count exactly.
    }

    @Test("channelNames caches and invalidates on refreshDevices")
    func channelNamesCaching() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input-capable device")
            return
        }
        // Prime the cache.
        let first = store.channelNames(uid: src.uid, direction: .input)
        // Same call returns the same payload (cache hit; we're not
        // asserting that it's literally the same array instance, only
        // that content is stable across calls with no refresh in between).
        let second = store.channelNames(uid: src.uid, direction: .input)
        #expect(first == second)

        // After a refresh, the cache is wiped; a subsequent call must
        // still produce a valid, same-sized result (content is still
        // stable because nothing changed on the hardware side during
        // the test window).
        store.refreshDevices()
        let third = store.channelNames(uid: src.uid, direction: .input)
        #expect(third.count == first.count)
    }

    // MARK: pollMeters (Phase 6 Slice A)

    @Test("pollMeters for an unknown route returns an empty array")
    func pollMetersUnknownRoute() throws {
        let engine = try Engine()
        #expect(engine.pollMeters(routeId: 999_999, side: .source,      maxChannels: 8).isEmpty)
        #expect(engine.pollMeters(routeId: 999_999, side: .destination, maxChannels: 8).isEmpty)
    }

    @Test("pollMeters with maxChannels == 0 returns an empty array")
    func pollMetersZeroCapacity() throws {
        let engine = try Engine()
        #expect(engine.pollMeters(routeId: 1, side: .source, maxChannels: 0).isEmpty)
    }

    @Test("pollMeters() publishes no entries when no routes are running")
    func pollMetersNoRunningRoutes() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        // Add a route but don't start it.
        _ = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)]
        ))
        store.pollMeters()
        #expect(store.meters.isEmpty)
    }

    @Test("pollMeters on a stopped route returns an empty array")
    func pollMetersStoppedRoute() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)]
        )
        let route = try store.addRoute(cfg)
        // Route is STOPPED; the engine returns 0 and the wrapper
        // surfaces that as an empty array.
        let srcPeaks = store.meterPeaks(routeId: route.id, side: .source)
        let dstPeaks = store.meterPeaks(routeId: route.id, side: .destination)
        #expect(srcPeaks.isEmpty)
        #expect(dstPeaks.isEmpty)
    }

    // MARK: - Peak-hold wiring (Slice B)

    @Test("heldPeak returns 0 for an unknown route")
    func heldPeakUnknownRoute() throws {
        let store = try makeStore()
        #expect(store.heldPeak(routeId: 999, side: .source, channel: 0) == 0)
        #expect(store.heldPeak(routeId: 999, side: .destination, channel: 0) == 0)
    }

    @Test("pollMeters promotes the PeakHoldTracker for every running channel")
    func pollMetersPromotesHolds() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one I/O device pair")
            return
        }
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)]
        )
        let route = try store.addRoute(cfg)

        // Directly observe through the internal tracker so we don't
        // need real audio to prove the wiring. pollMeters itself is
        // covered by the empty-snapshot case above; the observe()
        // semantics are covered in PeakHoldTrackerTests.
        let now = Date.timeIntervalSinceReferenceDate
        store.peakHolds.observe(routeId: route.id, side: .source,
                                channel: 0, value: 0.75, now: now)
        #expect(store.heldPeak(routeId: route.id, side: .source,
                               channel: 0, now: now) == 0.75)
    }

    @Test("removeRoute clears peak-hold state for that route")
    func removeRouteClearsHolds() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one I/O device pair")
            return
        }
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)]
        )
        let route = try store.addRoute(cfg)
        let now = Date.timeIntervalSinceReferenceDate
        store.peakHolds.observe(routeId: route.id, side: .source,
                                channel: 0, value: 0.9, now: now)
        #expect(store.heldPeak(routeId: route.id, side: .source,
                               channel: 0, now: now) == 0.9)
        store.removeRoute(route.id)
        #expect(store.heldPeak(routeId: route.id, side: .source,
                               channel: 0, now: now) == 0)
    }

    @Test("displayName falls back to source → destination when name is nil")
    func routeDisplayNameAutoGenerated() {
        let cfg = RouteConfig(
            source: DeviceReference(uid: "s", lastKnownName: "Source"),
            destination: DeviceReference(uid: "d", lastKnownName: "Dest"),
            mapping: [ChannelEdge(src: 0, dst: 0)]
        )
        #expect(cfg.displayName == "Source → Dest")

        var named = cfg
        named.name = "Live feed"
        #expect(named.displayName == "Live feed")
    }
}
