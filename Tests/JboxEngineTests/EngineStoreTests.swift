import CoreAudio
import Foundation
import Testing
@testable import JboxEngineSwift
@_exported import JboxEngineC

/// Suite-level precondition for the live-Core-Audio tests. True on every
/// developer Mac (built-in mic + speakers always show up); false on
/// runners whose audio subsystem can't enumerate at least one input- and
/// one output-capable device. Closes refactoring-backlog item R2: the
/// previous `guard … else { Issue.record(…); return }` pattern recorded
/// a failed expectation and Swift Testing flagged the test as a
/// regression rather than a skip.
///
/// `nonisolated` so the autoclosure handed to `.enabled(if:)` stays
/// `@Sendable` — the predicate reads system properties via the raw Core
/// Audio C API and never touches `@MainActor`-isolated `EngineStore`.
private func hasIOCapableDevices() -> Bool {
    var addr = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &addr, 0, nil, &size) == noErr,
          size > 0
    else { return false }

    let count = Int(size) / MemoryLayout<AudioDeviceID>.size
    var ids = [AudioDeviceID](repeating: 0, count: count)
    let err = ids.withUnsafeMutableBufferPointer { buf -> OSStatus in
        AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &addr, 0, nil, &size, buf.baseAddress!)
    }
    guard err == noErr else { return false }

    var hasInput = false
    var hasOutput = false
    for id in ids {
        if !hasInput, deviceChannelCount(id, scope: kAudioDevicePropertyScopeInput) > 0 {
            hasInput = true
        }
        if !hasOutput, deviceChannelCount(id, scope: kAudioDevicePropertyScopeOutput) > 0 {
            hasOutput = true
        }
        if hasInput && hasOutput { return true }
    }
    return false
}

private func deviceChannelCount(_ deviceID: AudioDeviceID,
                                scope: AudioObjectPropertyScope) -> Int {
    var addr = AudioObjectPropertyAddress(
        mSelector: kAudioDevicePropertyStreamConfiguration,
        mScope: scope,
        mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(deviceID, &addr, 0, nil, &size) == noErr,
          size > 0
    else { return 0 }

    let buf = UnsafeMutableRawPointer.allocate(
        byteCount: Int(size),
        alignment: MemoryLayout<AudioBufferList>.alignment)
    defer { buf.deallocate() }
    guard AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &size, buf) == noErr
    else { return 0 }

    let abl = UnsafeMutableAudioBufferListPointer(
        buf.assumingMemoryBound(to: AudioBufferList.self))
    return abl.reduce(0) { $0 + Int($1.mNumberChannels) }
}

/// Store-level behaviour driven through the real Core Audio engine.
/// The macOS CI runner always has default built-in devices; we never
/// assume specific channel counts, just that enumeration returns at
/// least one device in each direction. The `.enabled(if:)` trait gates
/// the whole suite so a runner with no audio subsystem cleanly skips
/// rather than reporting every test as a failure.
@MainActor
@Suite("EngineStore (live Core Audio)", .enabled(if: hasIOCapableDevices()))
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
        let first = try #require(store.devices.first)
        let looked = store.device(uid: first.uid)
        #expect(looked == first)
        #expect(store.device(uid: "no-such-uid") == nil)
    }

    @Test("addRoute with a bogus mapping surfaces JBOX_ERR_MAPPING_INVALID")
    func addRouteInvalidMapping() throws {
        let store = try makeStore()
        store.refreshDevices()
        let src = try #require(store.devices.first(where: { $0.directionInput }))
        let dst = try #require(store.devices.first(where: { $0.directionOutput }))

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
        let src = try #require(store.devices.first(where: { $0.directionInput }))
        let dst = try #require(store.devices.first(where: { $0.directionOutput }))

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
        let src = try #require(store.devices.first(where: { $0.directionInput }))
        let dst = try #require(store.devices.first(where: { $0.directionOutput }))

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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))

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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))

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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))

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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
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

        // Settle one poll so `routeCounters[route.id]` is populated;
        // otherwise the prune assertion below would be vacuous (the
        // key was never present, so a refactor that drops the prune
        // line in `replaceRoute` would still pass).
        store.pollStatuses()
        #expect(store.routeCounters[route.id] != nil)

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
        // Pin the `routeCounters` prune at the old engine id —
        // `replaceRoute` removes the old route through the engine,
        // and the wrapper must drop its `routeCounters` entry along
        // with the existing `meters` / `latencyComponents` cleanup.
        #expect(store.routeCounters[route.id] == nil,
                "replaceRoute must prune routeCounters for the old engine id")

        store.removeRoute(updated.id)
    }

    // MARK: - moveRoute

    /// Helper: build three routes with distinct names, all over the
    /// same single-input / single-output device pair (matching the
    /// existing `addRouteLatencyMode` idiom at line ~127). Returns the
    /// store + the three route IDs in insertion order.
    private func makeStoreWithThreeRoutes() throws
        -> (EngineStore, UInt32, UInt32, UInt32)
    {
        let store = try makeStore()
        store.refreshDevices()
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
        func cfg(_ name: String) -> RouteConfig {
            RouteConfig(
                source: DeviceReference(device: src),
                destination: DeviceReference(device: dst),
                mapping: [ChannelEdge(src: 0, dst: 0)],
                name: name)
        }
        let a = try store.addRoute(cfg("a"))
        let b = try store.addRoute(cfg("b"))
        let c = try store.addRoute(cfg("c"))
        return (store, a.id, b.id, c.id)
    }

    @Test("moveRoute moves a single row down and fires onRoutesChanged once")
    func moveRouteSingleRowDown() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Move row 0 ('a') to position 3 (after 'c').
        // List.onMove convention: destination is "before this index".
        store.moveRoute(from: IndexSet(integer: 0), to: 3)

        #expect(store.routes.map(\.id) == [b, c, a])
        #expect(fireCount == 1)
    }

    @Test("moveRoute moves a single row up and fires onRoutesChanged once")
    func moveRouteSingleRowUp() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Move row 2 ('c') to position 0 (before 'a').
        store.moveRoute(from: IndexSet(integer: 2), to: 0)

        #expect(store.routes.map(\.id) == [c, a, b])
        #expect(fireCount == 1)
    }

    @Test("moveRoute with multi-row IndexSet preserves the relative order of the moved set")
    func moveRouteMultiRow() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Drag rows {0, 1} ({a, b}) to position 3 (after c).
        // Expected: c first, then a then b in their original
        // relative order.
        store.moveRoute(from: IndexSet([0, 1]), to: 3)

        #expect(store.routes.map(\.id) == [c, a, b])
        #expect(fireCount == 1)
    }

    @Test("moveRoute is a no-op when destination equals source position")
    func moveRouteSamePositionNoOp() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Drag row 1 ('b') to position 1 — same place.
        // Swift's Array.move treats this as identity; we must NOT
        // fire onRoutesChanged for it (avoid spurious state.json
        // snapshots).
        store.moveRoute(from: IndexSet(integer: 1), to: 1)

        #expect(store.routes.map(\.id) == [a, b, c])
        #expect(fireCount == 0)
    }

    @Test("moveRoute is a no-op when destination is one past the source (List.onMove identity)")
    func moveRouteAdjacentPositionNoOp() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // SwiftUI's List.onMove uses "destination is *before* this
        // index" semantics. Dragging row 1 ('b') to position 2 lands
        // it right after itself — same array as before. The
        // implementation reaches the `before != after` short-circuit
        // through the index-adjustment branch (1 < 2 → adjusted -= 1
        // → reinsert at 1), distinct from the `→ 1` path (1 < 1 is
        // false → adjusted unchanged). This case pins both paths.
        store.moveRoute(from: IndexSet(integer: 1), to: 2)

        #expect(store.routes.map(\.id) == [a, b, c])
        #expect(fireCount == 0)
    }

    @Test("moveRoute is a no-op for an empty IndexSet")
    func moveRouteEmptyIndexSet() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        store.moveRoute(from: IndexSet(), to: 2)

        #expect(store.routes.map(\.id) == [a, b, c])
        #expect(fireCount == 0)
    }

    @Test("moveRoute fires onRoutesChanged exactly once per non-trivial move")
    func moveRouteFiresExactlyOnce() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        store.moveRoute(from: IndexSet(integer: 0), to: 2)  // a → after b
        store.moveRoute(from: IndexSet(integer: 2), to: 0)  // last → first

        // Two non-trivial moves should fire exactly twice. Regression
        // guard against accidentally double-firing onRoutesChanged.
        #expect(fireCount == 2)
    }

    @Test("moveRoute with a non-contiguous IndexSet straddling the destination preserves the relative order of the moved set")
    func moveRouteNonContiguousIndexSet() throws {
        // Defensive guard against future refactors of the manual
        // `Array.move`-equivalent body (back-to-front removal +
        // index adjustment). SwiftUI's `List.onMove` only ever
        // produces contiguous selections in practice, but `IndexSet`
        // is the public API surface, so we must behave correctly on
        // any well-formed shape — and the straddling-destination
        // case (`{0, 3} → 2` over a four-element list) is where the
        // index-adjustment math is most likely to drift: one
        // removed index sits below `destination` and decrements
        // `adjusted`; the other sits above and must NOT decrement
        // `adjusted`. Expected SwiftUI semantics: a (index 0) and
        // d (index 3) move together to position 2 (before c),
        // preserving their relative order, with b and c staying.
        let store = try makeStore()
        store.refreshDevices()
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
        func cfg(_ name: String) -> RouteConfig {
            RouteConfig(
                source: DeviceReference(device: src),
                destination: DeviceReference(device: dst),
                mapping: [ChannelEdge(src: 0, dst: 0)],
                name: name)
        }
        let a = try store.addRoute(cfg("a"))
        let b = try store.addRoute(cfg("b"))
        let c = try store.addRoute(cfg("c"))
        let d = try store.addRoute(cfg("d"))
        defer {
            store.removeRoute(a.id)
            store.removeRoute(b.id)
            store.removeRoute(c.id)
            store.removeRoute(d.id)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        store.moveRoute(from: IndexSet([0, 3]), to: 2)

        #expect(store.routes.map(\.id) == [b.id, a.id, d.id, c.id])
        #expect(fireCount == 1)
    }

    @Test("moveRoute on a single-row list is identity for any valid destination")
    func moveRouteSingleRowList() throws {
        let store = try makeStore()
        store.refreshDevices()
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
        let route = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "solo"))
        defer { store.removeRoute(route.id) }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // For a single-row list, the only valid List.onMove destinations
        // are 0 (drop-before-self) and 1 (drop-after-self) — both
        // identity. Both must short-circuit before onRoutesChanged.
        store.moveRoute(from: IndexSet(integer: 0), to: 0)
        store.moveRoute(from: IndexSet(integer: 0), to: 1)

        #expect(store.routes.map(\.id) == [route.id])
        #expect(fireCount == 0)
    }

    // MARK: - polling diff-before-write (drag-cancellation regression guard)

    /// Reference-type holder so the `@Sendable` `onChange` closure of
    /// `withObservationTracking` can flip a flag the test then asserts
    /// on. The closure fires synchronously on the main actor for our
    /// usage (writes happen on `@MainActor` `EngineStore`), so
    /// `@unchecked Sendable` is safe here.
    private final class FireFlag: @unchecked Sendable {
        var fired = false
    }

    /// `pollStatuses` writes to `routes[i].status` and `latencyComponents[id]`
    /// — both subscript-through-collection paths. The `@Observable` macro's
    /// equal-value short-circuit applies only to *direct* property setters,
    /// not to `_modify`-accessor mutations through Array / Dictionary
    /// subscripts. Without an explicit `if status != current` guard, the
    /// 4 Hz tick fires 12 spurious "data source did change" notifications
    /// per second (4 Hz × 3 routes) and `NSTableView` under SwiftUI's
    /// `List` invalidates in-flight `List.onMove` drag drops. The guard
    /// is load-bearing for the drag UX, NOT a perf micro-optimization.
    /// Removing it silently regresses the gesture under any audio activity.
    @Test("pollStatuses does not invalidate route observers when nothing changed")
    func pollStatusesIsQuietOnNoChange() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }

        // Settle: one poll to establish baseline `routes[i].status` and
        // `latencyComponents[id]` against the engine's current view.
        store.pollStatuses()

        let flag = FireFlag()
        withObservationTracking {
            for r in store.routes {
                _ = r.status
            }
            _ = store.latencyComponents
        } onChange: { [flag] in
            flag.fired = true
        }

        // Engine state hasn't changed between calls — stopped routes
        // have stable `RouteStatus` and `LatencyComponents == .zero`.
        // Diff-before-write must skip the assignments.
        store.pollStatuses()

        #expect(flag.fired == false,
                "pollStatuses() on unchanged engine state must not invalidate observers")
    }

    /// `refreshStatus` is the one-route counterpart to `pollStatuses`'s
    /// loop and is called from `startRoute` / `stopRoute` / `replaceRoute`
    /// after each engine action. Same `_modify`-accessor concern as
    /// `pollStatuses`: without the `status != routes[idx].status` guard,
    /// every successful idempotent action (e.g. a `stopRoute` on a route
    /// that's already `.stopped`) would write the same `RouteStatus`
    /// back through the subscript path and fire willSet — and any
    /// keyboard-shortcut "Stop" while a drag is in flight would cancel
    /// the drop. The shipped guard at `EngineStore.swift:881` is short
    /// enough that drift is unlikely, but its callers all also change
    /// state on the success path, so without an *isolated* regression
    /// guard a refactor could remove only the `refreshStatus` arm and
    /// no test would catch it.
    @Test("refreshStatus does not invalidate route observers when status is unchanged")
    func refreshStatusIsQuietOnNoChange() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }

        // Settle: pollStatuses once so `routes[i].status` matches the
        // engine's current view. All three routes are `.stopped`.
        store.pollStatuses()

        let flag = FireFlag()
        withObservationTracking {
            for r in store.routes {
                _ = r.status
            }
        } onChange: { [flag] in
            flag.fired = true
        }

        // `engine.stopRoute(id)` on an already-stopped route returns
        // `JBOX_OK` (idempotent — see `RouteManager::stopRoute`), so
        // `EngineStore.stopRoute` lands in the success arm and calls
        // `refreshStatus(a)`. The engine's poll returns the same
        // `.stopped` status; the diff-before-write must skip the
        // `routes[idx].status = …` assignment.
        store.stopRoute(a)

        #expect(flag.fired == false,
                "refreshStatus on unchanged engine state must not invalidate observers")
    }

    /// `refreshStatus` is the explicit-action counterpart to
    /// `pollStatuses`'s loop and must publish per-route counters into
    /// `routeCounters` independently of the periodic poller — otherwise
    /// the dict only ever rehydrates on the next 4 Hz tick after a
    /// `startRoute` / `stopRoute` / `replaceRoute` success arm fires,
    /// and the expanded `MeterPanel.DiagnosticsBlock` shows zeroes for
    /// up to a quarter-second after every route transition. Pinned
    /// independently of `pollStatusesPublishesRouteCounters` so a
    /// refactor that drops only `refreshStatus`'s
    /// `routeCounters[id] = newCounters` assignment fails *this* test
    /// rather than slipping through.
    @Test("refreshStatus publishes per-route counters into the routeCounters dict")
    func refreshStatusPublishesRouteCounters() throws {
        let store = try makeStore()
        store.refreshDevices()
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
        let route = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "refresh-counters"))
        defer { store.removeRoute(route.id) }

        // `addRoute` polls status once into `routes[i].status` but does
        // NOT touch `routeCounters` — only the periodic poller and the
        // post-action `refreshStatus` are wired to publish there. Pin
        // that pre-condition so the test isn't vacuous.
        #expect(store.routeCounters[route.id] == nil)

        // `startRoute`'s success arm calls `refreshStatus(id)`, which
        // must populate `routeCounters` for the route's id even when
        // the engine has not yet been polled by the periodic loop.
        store.startRoute(route.id)
        #expect(store.routeCounters[route.id] != nil,
                "refreshStatus on startRoute success must publish counters into the dict")
    }

    /// `pollMeters` writes the `meters` dictionary via a *direct* property
    /// setter (`self.meters = next`). Apple's `@Observable` macro
    /// short-circuits willSet on equal-value direct-setter writes — so
    /// even without an `if meters != next` guard, an unchanged snapshot
    /// does not fire onChange. This test pins that framework contract:
    /// if Apple ever changes Observation to fire on every assignment
    /// regardless of equality, this test fails and we'd need to add an
    /// explicit guard at `pollMeters` (matching the `pollStatuses` /
    /// `refreshStatus` subscript-write paths, which DO need guards
    /// because the `_modify` accessor doesn't get the same short-circuit).
    @Test("pollMeters does not invalidate meters observers when the snapshot is unchanged")
    func pollMetersIsQuietOnNoChange() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }

        // Routes are stopped so `pollMeters` produces an empty snapshot.
        // Settle once so `meters` matches the next tick's `next`.
        store.pollMeters()

        let flag = FireFlag()
        withObservationTracking {
            // `.count` engages the dict's getter through the observation
            // registrar; a `_ = store.meters` rebind is sometimes elided
            // by the optimizer and fails to register the dependency.
            _ = store.meters.count
        } onChange: { [flag] in
            flag.fired = true
        }

        store.pollMeters()

        #expect(flag.fired == false,
                "pollMeters() with unchanged snapshot must not invalidate observers")
    }

    // MARK: - statusFieldsAreObservablyEqual (drag-cancellation regression guard, part 2)

    /// Stable `RouteStatus` parts only. Two values that differ in the
    /// monotonic counter fields (`framesProduced` / `framesConsumed` /
    /// `underrunCount` / `overrunCount`) are still "observably equal"
    /// for `routes[i].status` purposes — those tick every poll while a
    /// route is running and would otherwise re-fire the array-subscript
    /// willSet at 4 Hz × N-running per second, killing in-flight
    /// `List.onMove` drops while audio flows. Fresh counters live in
    /// the separate `routeCounters` direct-setter dict.
    @Test("statusFieldsAreObservablyEqual treats counter-only changes as no-ops")
    func statusFieldsAreObservablyEqualIgnoresCounterTicks() {
        let baseline = RouteStatus(
            state: .running, lastError: JBOX_OK,
            framesProduced: 1_000, framesConsumed: 1_000,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 5_000)
        let later = RouteStatus(
            state: .running, lastError: JBOX_OK,
            framesProduced: 2_500, framesConsumed: 2_490,
            underrunCount: 1, overrunCount: 0,
            estimatedLatencyUs: 5_000)
        #expect(EngineStore.statusFieldsAreObservablyEqual(baseline, later),
                "ticking counters alone must NOT count as an observable change")
        #expect(EngineStore.statusFieldsAreObservablyEqual(baseline, baseline),
                "identical status values must compare observably equal")
    }

    /// Stable-field changes still invalidate. `state`, `lastError`,
    /// `estimatedLatencyUs` are bound by `RouteRow` (status glyph,
    /// error text, latency pill) and a change in any of them is the
    /// real reason to re-fire `routes[i].status`. The fix split the
    /// diff predicate so these survive while counter-only ticks are
    /// silenced — both halves must hold.
    @Test("statusFieldsAreObservablyEqual flags state changes")
    func statusFieldsAreObservablyEqualFlagsStateChange() {
        let stopped = RouteStatus(
            state: .stopped, lastError: JBOX_OK,
            framesProduced: 0, framesConsumed: 0,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 0)
        let starting = RouteStatus(
            state: .starting, lastError: JBOX_OK,
            framesProduced: 0, framesConsumed: 0,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 0)
        #expect(!EngineStore.statusFieldsAreObservablyEqual(stopped, starting),
                "state transition must count as an observable change")
    }

    @Test("statusFieldsAreObservablyEqual flags lastError changes")
    func statusFieldsAreObservablyEqualFlagsLastErrorChange() {
        let ok = RouteStatus(
            state: .stopped, lastError: JBOX_OK,
            framesProduced: 0, framesConsumed: 0,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 0)
        let errored = RouteStatus(
            state: .stopped, lastError: JBOX_ERR_DEVICE_GONE,
            framesProduced: 0, framesConsumed: 0,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 0)
        #expect(!EngineStore.statusFieldsAreObservablyEqual(ok, errored),
                "lastError change must count as an observable change")
    }

    @Test("statusFieldsAreObservablyEqual flags estimatedLatencyUs changes")
    func statusFieldsAreObservablyEqualFlagsLatencyChange() {
        let before = RouteStatus(
            state: .running, lastError: JBOX_OK,
            framesProduced: 1_000, framesConsumed: 1_000,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 5_000)
        let after = RouteStatus(
            state: .running, lastError: JBOX_OK,
            framesProduced: 1_000, framesConsumed: 1_000,
            underrunCount: 0, overrunCount: 0,
            estimatedLatencyUs: 7_500)
        #expect(!EngineStore.statusFieldsAreObservablyEqual(before, after),
                "estimatedLatencyUs change must count as an observable change")
    }

    /// `pollStatuses` must publish fresh per-route counters into
    /// `routeCounters` even when it skips the `routes[i].status` write
    /// (i.e. when only counters changed). `RouteCounters(from:)` mirrors
    /// the engine's poll, so a fresh poll on a stopped route lands a
    /// zero entry; non-zero counters appear once a route has been
    /// running. Keyed by route id; pruned on `removeRoute`.
    @Test("pollStatuses publishes per-route counters into the routeCounters dict")
    func pollStatusesPublishesRouteCounters() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }

        // Stopped routes — counters are all zero.
        store.pollStatuses()
        #expect(store.routeCounters[a] == RouteCounters.zero)
        #expect(store.routeCounters[b] == RouteCounters.zero)
        #expect(store.routeCounters[c] == RouteCounters.zero)
        #expect(store.routeCounters.count == 3)
    }

    /// `removeRoute` must drop the route's `routeCounters` entry along
    /// with the existing `meters` / `latencyComponents` cleanup.
    /// Without this, removed routes leave behind stale counter entries
    /// that grow unbounded over the session.
    @Test("removeRoute prunes the routeCounters dict entry for the removed id")
    func removeRoutePrunesRouteCounters() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        // Settle so all three ids have counter entries.
        store.pollStatuses()
        #expect(store.routeCounters.count == 3)

        store.removeRoute(b)
        #expect(store.routeCounters[b] == nil)
        #expect(store.routeCounters.count == 2)

        store.removeRoute(a)
        store.removeRoute(c)
        #expect(store.routeCounters.isEmpty)
    }

    /// Live-Core-Audio regression for the user-visible drag-cancellation
    /// bug: while a route is *running*, its counters tick on every
    /// 4 Hz `pollStatuses` pass. The original guard
    /// (`status != routes[i].status`) compared all of `RouteStatus`,
    /// so ticking counters slipped past it and re-fired
    /// `routes[i].status = …` every poll — which `NSTableView`
    /// underneath `List` treats as "data source did change" and uses
    /// to invalidate the in-flight `List.onMove` drop. The fix splits
    /// the diff: stable fields drive the array write; counters publish
    /// into `routeCounters` via a direct-setter dict that only
    /// invalidates observers of `routeCounters` (which the `routes`
    /// ForEach does not subscribe to).
    ///
    /// This test depends on the host actually being able to bring a
    /// route to `.running` and on the IOProc actually advancing
    /// `framesProduced` — true for `swift test` on a developer machine
    /// (no Hardened Runtime, so the missing `audio-input` entitlement
    /// doesn't silence the IOProc) but not guaranteed on every CI
    /// runner. The suite-level `.enabled(if:)` trait gates on device
    /// *enumeration* but can't predict whether the host's audio engine
    /// will actually run. When IOProc doesn't tick on this host we
    /// silently `return` rather than `Issue.record` — the regression
    /// coverage still fires on dev machines and the pure-logic
    /// `statusFieldsAreObservablyEqual…` tests above pin the predicate
    /// itself deterministically.
    @Test("pollStatuses does not invalidate routes observers when only counters tick (live)")
    func pollStatusesIsQuietOnRoutesWhenRunningCountersTick() throws {
        let store = try makeStore()
        store.refreshDevices()
        let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
        let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
        let route = try store.addRoute(RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: [ChannelEdge(src: 0, dst: 0)],
            name: "tick-regression"))
        defer { store.removeRoute(route.id) }

        store.startRoute(route.id)
        // Wait up to 2 s for the route to bring devices up. Some
        // hosts settle in `.waiting` (no usable hardware) — treat
        // that as a skip rather than a failure.
        let deadline = Date().addingTimeInterval(2.0)
        while Date() < deadline {
            store.pollStatuses()
            if store.routes.first?.status.state == .running { break }
            Thread.sleep(forTimeInterval: 0.05)
        }
        guard store.routes.first?.status.state == .running else {
            // IOProc didn't reach `.running` — host's audio engine isn't
            // actually running (CI sandbox or similar). Silent skip.
            return
        }

        // Settle once more so `routes[0].status` matches the engine's
        // current view exactly, then capture counters as a baseline.
        store.pollStatuses()
        let baseline = store.routeCounters[route.id] ?? .zero

        // Let the IOProc run a bit so counters definitely advance
        // before the next poll.
        Thread.sleep(forTimeInterval: 0.10)

        let flag = FireFlag()
        withObservationTracking {
            for r in store.routes {
                _ = r.status
            }
        } onChange: { [flag] in
            flag.fired = true
        }

        store.pollStatuses()

        let advanced = store.routeCounters[route.id] ?? .zero
        // Sanity: counters DID advance — otherwise the test is vacuous
        // (e.g., IOProc silently silenced because the runner sandbox
        // blocked audio input). Silent skip rather than pass with no
        // signal; dev-machine runs still exercise the regression.
        guard advanced.framesProduced > baseline.framesProduced else {
            return
        }

        #expect(flag.fired == false,
                "pollStatuses() must not invalidate `routes` observers while a route's counters tick")
    }
}
