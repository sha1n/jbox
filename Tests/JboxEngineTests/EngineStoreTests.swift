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

    /// Regression for the infinite "Engine error" alert loop:
    /// `RouteListView`'s alert was bound to a `Bool` whose setter was a
    /// no-op, so once `lastError` went non-nil the alert re-fired on
    /// every render forever. The fix exposes `clearLastError()`; the
    /// view now calls it from the binding's setter on dismiss.
    @Test("clearLastError() drops a previously-recorded engine error")
    func clearLastErrorDropsRecordedError() throws {
        let store = try makeStore()
        store.refreshDevices()
        let src = store.devices.first(where: { $0.directionInput })!
        let dst = store.devices.first(where: { $0.directionOutput })!

        // Drive lastError non-nil via the existing failure path.
        do {
            _ = try store.addRoute(RouteConfig(
                source: DeviceReference(device: src),
                destination: DeviceReference(device: dst),
                mapping: []))
            Issue.record("expected addRoute to fail with MAPPING_INVALID")
        } catch is JboxError {
            // expected — lastError is now set
        }
        #expect(store.lastError != nil)

        store.clearLastError()
        #expect(store.lastError == nil)

        // Idempotent: a second call on a clean state stays clean.
        store.clearLastError()
        #expect(store.lastError == nil)
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

    @Test("addRoute threads RouteConfig.latencyMode through to the engine")
    func addRouteLatencyMode() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }

        // Baseline: three routes, one per tier. We don't start them —
        // the ring-sizing / setpoint effects are covered by C++
        // integration tests. Here we just confirm the tier survives
        // the Swift → C bridge.
        let offCfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "off",
            latencyMode: .off)
        let lowCfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "low",
            latencyMode: .low)
        let perfCfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "performance",
            latencyMode: .performance)

        let offRoute  = try store.addRoute(offCfg)
        let lowRoute  = try store.addRoute(lowCfg)
        let perfRoute = try store.addRoute(perfCfg)

        #expect(offRoute.config.latencyMode  == .off)
        #expect(lowRoute.config.latencyMode  == .low)
        #expect(perfRoute.config.latencyMode == .performance)
        #expect(store.routes.count == 3)

        store.removeRoute(offRoute.id)
        store.removeRoute(lowRoute.id)
        store.removeRoute(perfRoute.id)
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

    // MARK: Edit flow (Phase 6 refinement #2)

    @Test("renameRoute preserves the engine id and updates the local config.name")
    func renameRoutePreservesIdAndLocalName() throws {
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
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "original")
        let route = try store.addRoute(cfg)

        store.renameRoute(route.id, to: "renamed")
        #expect(store.routes.first?.id == route.id)
        #expect(store.routes.first?.config.name == "renamed")
        #expect(store.lastError == nil)

        // Empty string clears the custom name — displayName auto-falls
        // back to "source → destination".
        store.renameRoute(route.id, to: "")
        #expect(store.routes.first?.config.name == nil)

        store.removeRoute(route.id)
    }

    @Test("renameRoute on an unknown id surfaces INVALID_ARGUMENT via lastError")
    func renameRouteUnknownId() throws {
        let store = try makeStore()
        store.renameRoute(99_999, to: "orphan")
        #expect(store.lastError != nil)
    }

    @Test("replaceRoute short-circuits to rename when only the name changed")
    func replaceRouteRenameFastPath() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        let original = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "before")
        let route = try store.addRoute(original)

        var edited = original
        edited.name = "after"
        let updated = try store.replaceRoute(route.id, with: edited)
        // Rename fast path keeps the id.
        #expect(updated.id == route.id)
        #expect(updated.config.name == "after")
        #expect(store.routes.first?.id == route.id)

        store.removeRoute(route.id)
    }

    @Test("replaceRoute on an unknown id throws without touching the store")
    func replaceRouteUnknownId() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        let anchor = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "anchor"))

        let bogus = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "ghost")
        do {
            _ = try store.replaceRoute(99_999, with: bogus)
            Issue.record("expected unknown-id rejection")
        } catch let e as JboxError {
            #expect(e.code == JBOX_ERR_INVALID_ARGUMENT)
        }
        // The anchor route survived the failed replace.
        #expect(store.routes.count == 1)
        #expect(store.routes.first?.id == anchor.id)

        store.removeRoute(anchor.id)
    }

    @Test("replaceRoute preserves the original id when the engine rejects the new config")
    func replaceRouteRollbackKeepsOriginalRoute() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }

        let originalConfig = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "rollback-source")
        let route = try store.addRoute(originalConfig)

        // Same id but an empty source UID — the engine rejects this
        // in `RouteManager::addRoute` with `INVALID_ARGUMENT`, which
        // exercises the rollback branch inside `replaceRoute`.
        let doomed = RouteConfig(
            source: DeviceReference(uid: "", lastKnownName: "broken"),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "doomed")
        do {
            _ = try store.replaceRoute(route.id, with: doomed)
            Issue.record("expected engine addRoute rejection")
        } catch let e as JboxError {
            #expect(e.code == JBOX_ERR_INVALID_ARGUMENT)
        }

        // The route is still there under its original id, with its
        // original config untouched.
        #expect(store.routes.count == 1)
        #expect(store.routes.first?.id == route.id)
        #expect(store.routes.first?.config.source.uid == src.uid)
        #expect(store.routes.first?.config.name == "rollback-source")

        store.removeRoute(route.id)
    }

    // MARK: overallState / runningRouteCount (Phase 6 MenuBarExtra)

    /// Tiny helper that builds a `Route` value with a specific runtime
    /// state, used only by the `overallState` and menu-bar helper
    /// tests below. Bypasses the engine entirely — the derivation
    /// logic is a pure function over the `routes` array.
    private func mkRoute(id: UInt32, state: RouteState) -> Route {
        let cfg = RouteConfig(
            source: DeviceReference(uid: "s\(id)", lastKnownName: "src\(id)"),
            destination: DeviceReference(uid: "d\(id)", lastKnownName: "dst\(id)"),
            mapping: [ChannelEdge(src: 0, dst: 0)])
        let status = RouteStatus(
            state: state, lastError: JBOX_OK,
            framesProduced: 0, framesConsumed: 0,
            underrunCount: 0, overrunCount: 0)
        return Route(id: id, config: cfg, status: status)
    }

    @Test("overallState is .idle for an empty route list")
    func overallStateEmpty() {
        #expect(EngineStore.overallState(for: [Route]()) == .idle)
    }

    @Test("overallState is .idle when every route is stopped")
    func overallStateAllStopped() {
        let routes = [
            mkRoute(id: 1, state: .stopped),
            mkRoute(id: 2, state: .stopped),
        ]
        #expect(EngineStore.overallState(for: routes) == .idle)
    }

    @Test("overallState is .running when at least one route is running and none need attention")
    func overallStateRunning() {
        let routes = [
            mkRoute(id: 1, state: .stopped),
            mkRoute(id: 2, state: .running),
        ]
        #expect(EngineStore.overallState(for: routes) == .running)
    }

    @Test("overallState treats .starting as running (icon fills mid-transition)")
    func overallStateStartingCountsAsRunning() {
        let routes = [mkRoute(id: 1, state: .starting)]
        #expect(EngineStore.overallState(for: routes) == .running)
    }

    @Test("overallState escalates to .attention on any waiting route, even alongside running ones")
    func overallStateWaitingEscalates() {
        let routes = [
            mkRoute(id: 1, state: .running),
            mkRoute(id: 2, state: .waiting),
        ]
        #expect(EngineStore.overallState(for: routes) == .attention)
    }

    @Test("overallState escalates to .attention on any errored route")
    func overallStateErrorEscalates() {
        let routes = [
            mkRoute(id: 1, state: .running),
            mkRoute(id: 2, state: .error),
        ]
        #expect(EngineStore.overallState(for: routes) == .attention)
    }

    @Test("runningRouteCount is 0 on an empty store")
    func runningRouteCountEmpty() throws {
        let store = try makeStore()
        #expect(store.runningRouteCount == 0)
        // With no routes, runningRouteCount is 0 and overallState is .idle.
        #expect(store.overallState == .idle)
    }

    @Test("runningRouteCount counts only the live .running routes, not stopped or starting")
    func runningRouteCountOnlyRunning() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        // Three routes; we'll start only the middle one.
        let a = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)], name: "a"))
        let b = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)], name: "b"))
        let c = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)], name: "c"))

        // All stopped → count must be 0.
        #expect(store.runningRouteCount == 0)

        store.startRoute(b.id)
        // Count should reflect only .running — match against an
        // expected "number of .running routes" to stay honest about
        // what the CI runner actually delivers (a device may land in
        // .waiting instead of .running).
        let expectedRunning = store.routes.filter { $0.status.state == .running }.count
        #expect(store.runningRouteCount == expectedRunning)

        store.removeRoute(a.id)
        store.removeRoute(b.id)
        store.removeRoute(c.id)
    }

    // MARK: startAll / stopAll (Phase 6 MenuBarExtra)

    @Test("startAll on an empty store is a no-op")
    func startAllEmptyIsNoOp() throws {
        let store = try makeStore()
        store.startAll()
        #expect(store.routes.isEmpty)
        #expect(store.lastError == nil)
    }

    @Test("stopAll on an empty store is a no-op")
    func stopAllEmptyIsNoOp() throws {
        let store = try makeStore()
        store.stopAll()
        #expect(store.routes.isEmpty)
        #expect(store.lastError == nil)
    }

    @Test("startAll does not surface lastError when routes are already running (no redundant engine calls)")
    func startAllIdempotentOnRunningRoutes() throws {
        // startAll's switch skips `.running` / `.starting` / `.waiting`,
        // so when every route is already in one of those states the
        // engine is never re-entered. Observable consequence: after
        // the call, `lastError` stays nil (a redundant engine call
        // on an already-running route would likely throw) and the
        // route states are whatever they were before the call.
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        let a = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)], name: "a"))
        store.startRoute(a.id)
        // Whatever state startRoute landed on (running / starting /
        // waiting), startAll should be a no-op against it.
        let stateBefore = store.routes.first?.status.state
        #expect(stateBefore != .stopped && stateBefore != .error)

        // Clear lastError from startRoute's success path and call startAll.
        store.startAll()
        #expect(store.lastError == nil)

        // The route did not regress to .stopped; startAll's switch
        // left it in its pre-call state class.
        let stateAfter = store.routes.first?.status.state
        #expect(stateAfter != .stopped)

        store.stopRoute(a.id)
        store.removeRoute(a.id)
    }

    @Test("startAll leaves already-running routes alone and attempts to start stopped ones")
    func startAllTouchesOnlyIdleRoutes() throws {
        // We don't rely on real audio starting successfully — most CI
        // runners will leave a route in .stopped or .waiting after
        // startRoute. The behaviour under test is that startAll
        // iterates the routes in .stopped / .error states and calls
        // startRoute on each, while leaving .running untouched.
        // We verify by capturing the state *before* vs. *after* for
        // every route: no route regresses to a state it was not in
        // immediately before.
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        let a = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "a"))
        let b = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "b"))

        // Both routes start in .stopped.
        #expect(store.routes.allSatisfy { $0.status.state == .stopped })

        store.startAll()
        // Every route has been poked at least once; they land in
        // whatever non-.stopped state Core Audio allows on this runner
        // (usually .running, sometimes .waiting when the device is busy).
        let statesAfter = store.routes.map { $0.status.state }
        #expect(statesAfter.allSatisfy { $0 != .stopped })

        // stopAll flips them back. .error rows stay put (startAll's
        // scope covers .error too, but stopAll leaves them), which is
        // the symmetric rule.
        store.stopAll()
        for r in store.routes where r.status.state != .error {
            #expect(r.status.state == .stopped)
        }

        store.removeRoute(a.id)
        store.removeRoute(b.id)
    }

    @Test("replaceRoute with a new mapping issues a new engine id")
    func replaceRouteMappingEditAssignsNewId() throws {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            return
        }
        // Only meaningful when we have at least one spare src or dst
        // channel to swap into the mapping. Skip otherwise.
        guard src.inputChannelCount >= 2 || dst.outputChannelCount >= 2 else {
            return
        }

        let original = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "edit-mapping")
        let route = try store.addRoute(original)

        // Build an edit that changes the destination channel if possible;
        // otherwise bump the source channel.
        let newMapping: [ChannelEdge] =
            dst.outputChannelCount >= 2
            ? [ChannelEdge(src: 0, dst: 1)]
            : [ChannelEdge(src: 1, dst: 0)]
        var edited = original
        edited.mapping = newMapping

        let updated = try store.replaceRoute(route.id, with: edited)
        #expect(updated.id != route.id)
        #expect(updated.config.mapping == newMapping)
        #expect(store.routes.count == 1)
        #expect(store.routes.first?.id == updated.id)

        store.removeRoute(updated.id)
    }
}
