import Foundation
import Observation

// MARK: - UI-side value types

/// Persistent pointer to a device by its stable Core Audio UID, plus
/// the last human-readable name we saw for it. `lastKnownName` lets
/// the UI keep rendering a sensible label when the device is
/// temporarily disconnected. See docs/spec.md § 3.1.1.
public struct DeviceReference: Codable, Equatable, Hashable, Sendable {
    public var uid: String
    public var lastKnownName: String

    public init(uid: String, lastKnownName: String) {
        self.uid = uid
        self.lastKnownName = lastKnownName
    }

    /// Build a reference from a currently-visible device snapshot.
    public init(device: Device) {
        self.uid = device.uid
        self.lastKnownName = device.name
    }
}

/// A route as the user configures it. IDs are assigned by the engine
/// on `addRoute`; `RouteConfig` is what the UI edits before submission
/// and what gets persisted later in Phase 7.
/// Tiered latency preset picked per route. Controls both the ring-
/// buffer sizing and the drift sampler's steady-state setpoint
/// (docs/spec.md § 2.3). Higher tiers trade reliability for latency.
public enum LatencyMode: UInt32, Sendable, Equatable, Hashable, CaseIterable {
    /// Safe default: 8× / 4096-floor ring; ring/2 setpoint. Absorbs
    /// USB-burst jitter — the sensible choice for general routing.
    case off         = 0
    /// Tighter ring (3× / 512-floor), same ring/2 setpoint. USB-burst
    /// sources may underrun; acceptable trade-off for most rigs.
    case low         = 1
    /// Drum-monitoring tier: 2× / 256-floor ring + ring/4 setpoint.
    /// Halves the ring's residency contribution at the cost of
    /// asymmetric underrun margin. Opt in when you need real-time
    /// monitoring and accept occasional clicks on bursty sources.
    case performance = 2
}

public struct RouteConfig: Equatable, Sendable {
    public var source: DeviceReference
    public var destination: DeviceReference
    public var mapping: [ChannelEdge]
    /// When nil, `displayName` auto-generates "source → destination".
    public var name: String?
    /// Tiered latency preset; see `LatencyMode`.
    public var latencyMode: LatencyMode
    /// Optional HAL buffer-frame-size override for Performance-mode
    /// same-device routes. `nil` means "use the tier default"
    /// (currently 64). Non-nil is clamped by the HAL into the
    /// device's supported range when the route starts.
    public var bufferFrames: UInt32?

    public init(source: DeviceReference,
                destination: DeviceReference,
                mapping: [ChannelEdge],
                name: String? = nil,
                latencyMode: LatencyMode = .off,
                bufferFrames: UInt32? = nil) {
        self.source = source
        self.destination = destination
        self.mapping = mapping
        self.name = name
        self.latencyMode = latencyMode
        self.bufferFrames = bufferFrames
    }

    public var displayName: String {
        if let name, !name.isEmpty { return name }
        return "\(source.lastKnownName) → \(destination.lastKnownName)"
    }
}

/// Overall app status summary used by the menu bar extra (spec § 4.2)
/// to pick an icon variant. Derived purely from the current route
/// snapshot, so it tracks `EngineStore.routes` automatically without
/// any timer of its own.
public enum OverallState: Equatable, Hashable, Sendable {
    /// No routes or every route is stopped.
    case idle
    /// At least one route is running and no route is in error /
    /// waiting. The normal "audio is flowing" indicator.
    case running
    /// Any route is waiting for its device or has errored out — the
    /// user needs to look. Outranks `.running` when both are present.
    case attention
}

/// A live route: the engine-assigned id, the user's config, and the
/// most recently polled runtime status. `persistId` is the durable
/// UUID that rides with the route through `state.json` and outlives
/// the engine-assigned `id` (which is re-minted on every process
/// launch). `createdAt` / `modifiedAt` mirror the spec § 3.1.3 fields
/// so the app layer can snapshot a route into `StoredRoute` without
/// looking up a side-table.
public struct Route: Identifiable, Equatable, Sendable {
    public let id: UInt32
    public let persistId: UUID
    public var config: RouteConfig
    public var status: RouteStatus
    public let createdAt: Date
    public var modifiedAt: Date

    public init(id: UInt32,
                config: RouteConfig,
                status: RouteStatus,
                persistId: UUID = UUID(),
                createdAt: Date = Date(),
                modifiedAt: Date = Date()) {
        self.id         = id
        self.persistId  = persistId
        self.config     = config
        self.status     = status
        self.createdAt  = createdAt
        self.modifiedAt = modifiedAt
    }
}

/// Peak snapshot for a single route, one linear peak per mapped
/// channel on each side. Read-and-reset: each `pollMeters()` pass on
/// the store replaces this with the peak since the previous pass.
public struct MeterPeaks: Equatable, Sendable {
    public var source: [Float]
    public var destination: [Float]

    public init(source: [Float] = [], destination: [Float] = []) {
        self.source = source
        self.destination = destination
    }

    /// "Signal present" threshold, chosen to sit just above the typical
    /// digital noise floor (~-60 dBFS = 10^(-60/20) ≈ 0.001). Values
    /// strictly greater count as active in the UI dot renderer.
    public static let signalThreshold: Float = 0.001

    public func hasSignal(at index: Int) -> Bool {
        guard index >= 0, index < source.count || index < destination.count else {
            return false
        }
        let s = index < source.count      ? source[index]      : 0
        let d = index < destination.count ? destination[index] : 0
        return s > Self.signalThreshold || d > Self.signalThreshold
    }
}

// MARK: - Store

/// UI-facing façade over `Engine`. SwiftUI views bind to `devices`,
/// `routes`, and `lastError`; actions (`addRoute`, `startRoute`,
/// `stopRoute`, `removeRoute`) call through to the engine and update
/// the observable state in place.
///
/// Thread model: `@MainActor`-isolated. Engine calls are safe from any
/// non-RT thread, so running them on the main actor is fine at v1
/// scale (no blocking I/O). If route counts or polling cadence ever
/// make this a hot loop, move the polling off-actor and post results
/// back — the public surface stays the same.
@MainActor
@Observable
public final class EngineStore {

    // MARK: Observable state

    public private(set) var devices: [Device] = []
    public private(set) var routes: [Route] = []

    /// Latest per-route peak snapshot, keyed by engine-assigned route
    /// id. Entries are present only for running routes; stopping /
    /// removing a route clears its entry on the next `pollMeters()`
    /// pass. Each entry carries a source-side and a destination-side
    /// array, one linear peak per mapped channel.
    public private(set) var meters: [UInt32: MeterPeaks] = [:]

    /// Per-route latency component breakdown, populated by the engine
    /// at route start. Refreshed by `pollStatuses()` so the UI sees a
    /// zeroed entry immediately on stop and the current values on a
    /// fresh start. Keyed by route id; absent for never-started routes.
    public private(set) var latencyComponents: [UInt32: LatencyComponents] = [:]

    /// Per-channel peak-hold tracker driven by `pollMeters()`. Not
    /// `@Observable` — the UI re-reads `heldPeak(...)` on each
    /// `TimelineView` tick with a fresh `now`, and the decay is
    /// computed purely on read. Held values are cleared when the
    /// owning route is removed (see `removeRoute`).
    internal var peakHolds = PeakHoldTracker()

    /// Human-readable description of the most recent engine error, or
    /// nil when the last action succeeded. Cleared by successful calls.
    public private(set) var lastError: String?

    /// Cache of per-channel names keyed by device UID and direction.
    /// Populated lazily on demand and invalidated wholesale whenever
    /// `refreshDevices()` runs (channel counts or labels may have
    /// changed on the hardware side). Empty array means "we asked the
    /// engine and got nothing"; missing key means "we haven't asked".
    private var channelNameCache: [ChannelCacheKey: [String]] = [:]

    private struct ChannelCacheKey: Hashable {
        let uid: String
        let direction: Engine.ChannelDirection
    }

    // MARK: Construction

    private let engine: Engine

    /// Invoked after every mutation that changes the persistable shape
    /// of `routes` (add / remove / rename / replace). The app-layer
    /// persistence wiring (`AppState`) uses this to trigger a debounced
    /// save against `state.json`. Intentionally nullable — tests and
    /// the CLI keep the store alive without any persistence layer.
    public var onRoutesChanged: (() -> Void)?

    public init(engine: Engine) {
        self.engine = engine
    }

    /// Convenience: create a production engine backed by Core Audio.
    public convenience init() throws {
        do {
            self.init(engine: try Engine())
            JboxLog.engine.notice("engine ready abi=\(JboxEngine.abiVersion, privacy: .public)")
        } catch {
            JboxLog.engine.error("engine create failed: \(String(describing: error), privacy: .public)")
            throw error
        }
    }

    // MARK: Devices

    /// Re-enumerate visible devices and update `devices`. On error,
    /// preserves the previous snapshot and sets `lastError`.
    public func refreshDevices() {
        do {
            devices = try engine.enumerateDevices()
            lastError = nil
            channelNameCache.removeAll(keepingCapacity: true)
            JboxLog.engine.info("enumerated devices: count=\(self.devices.count)")
        } catch {
            lastError = String(describing: error)
            JboxLog.engine.error("enumerateDevices failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// Look up a device by UID in the cached snapshot.
    public func device(uid: String) -> Device? {
        devices.first(where: { $0.uid == uid })
    }

    /// Per-channel names for the given device and direction, cached on
    /// first access. Returned array length matches the device's
    /// channel count in that direction; entries may be empty strings.
    /// On engine failure the UI gets an empty array and the error is
    /// logged (no throw — channel labels are a UX nicety, not critical).
    public func channelNames(uid: String,
                             direction: Engine.ChannelDirection) -> [String] {
        let key = ChannelCacheKey(uid: uid, direction: direction)
        if let cached = channelNameCache[key] { return cached }
        do {
            let names = try engine.enumerateChannels(uid: uid, direction: direction)
            channelNameCache[key] = names
            return names
        } catch {
            JboxLog.engine.error("enumerateChannels uid=\(uid, privacy: .public) dir=\(String(describing: direction), privacy: .public) failed: \(String(describing: error), privacy: .public)")
            channelNameCache[key] = []
            return []
        }
    }

    /// Supported HAL buffer-frame-size range for the given device
    /// UID, or `nil` when the device exposes no range. Forwarded
    /// from `Engine.supportedBufferFrameSizeRange`; never throws
    /// past the caller — errors become `nil`.
    public func bufferFrameRange(forDeviceUid uid: String) -> ClosedRange<UInt32>? {
        (try? engine.supportedBufferFrameSizeRange(forDeviceUid: uid)) ?? nil
    }

    // MARK: Engine-wide preferences

    /// Push the engine-wide resampler quality preset (ABI v8+). The
    /// change applies to newly-started routes only — already-running
    /// routes keep the preset their converter was built with until
    /// stopped and started again. Non-throwing: a preference push on
    /// an otherwise-healthy engine should never surface errors to the
    /// UI, so we log and move on.
    public func setResamplerQuality(_ quality: Engine.ResamplerQuality) {
        do {
            try engine.setResamplerQuality(quality)
            JboxLog.engine.info("resampler quality set: \(String(describing: quality), privacy: .public)")
        } catch {
            JboxLog.engine.error("setResamplerQuality failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// Current engine-wide resampler quality preset.
    public var resamplerQuality: Engine.ResamplerQuality {
        engine.resamplerQuality
    }

    // MARK: Routes

    /// Add a route; returns the new live `Route` value (also appended
    /// to `routes`). Throws `JboxError` on engine-side failure.
    ///
    /// `persistId` / `createdAt` default to fresh values for UI-driven
    /// adds; the restore-on-launch path passes the originals from
    /// `StoredRoute` so the UUID and timestamps survive relaunch.
    @discardableResult
    public func addRoute(_ config: RouteConfig,
                         persistId: UUID = UUID(),
                         createdAt: Date = Date()) throws -> Route {
        do {
            let id = try engine.addRoute(
                sourceUID: config.source.uid,
                destUID: config.destination.uid,
                mapping: config.mapping,
                name: config.name ?? "",
                latencyMode: config.latencyMode,
                bufferFrames: config.bufferFrames ?? 0
            )
            let status = try engine.pollStatus(id)
            let route = Route(id: id, config: config, status: status,
                              persistId: persistId,
                              createdAt: createdAt, modifiedAt: createdAt)
            routes.append(route)
            lastError = nil
            JboxLog.engine.notice("route added: id=\(id) src=\(config.source.lastKnownName, privacy: .public) dst=\(config.destination.lastKnownName, privacy: .public) channels=\(config.mapping.count)")
            onRoutesChanged?()
            return route
        } catch {
            lastError = String(describing: error)
            JboxLog.engine.error("addRoute failed src=\(config.source.uid, privacy: .public) dst=\(config.destination.uid, privacy: .public) err=\(String(describing: error), privacy: .public)")
            throw error
        }
    }

    public func startRoute(_ id: UInt32) {
        do {
            try engine.startRoute(id)
            refreshStatus(id)
            lastError = nil
            JboxLog.engine.notice("startRoute id=\(id) ok")
        } catch {
            lastError = String(describing: error)
            refreshStatus(id)
            JboxLog.engine.error("startRoute id=\(id) failed: \(String(describing: error), privacy: .public)")
        }
    }

    public func stopRoute(_ id: UInt32) {
        do {
            try engine.stopRoute(id)
            refreshStatus(id)
            lastError = nil
            JboxLog.engine.notice("stopRoute id=\(id) ok")
        } catch {
            lastError = String(describing: error)
            refreshStatus(id)
            JboxLog.engine.error("stopRoute id=\(id) failed: \(String(describing: error), privacy: .public)")
        }
    }

    public func removeRoute(_ id: UInt32) {
        do {
            try engine.removeRoute(id)
            routes.removeAll(where: { $0.id == id })
            meters.removeValue(forKey: id)
            latencyComponents.removeValue(forKey: id)
            peakHolds.forget(routeId: id)
            lastError = nil
            JboxLog.engine.notice("removeRoute id=\(id) ok")
            onRoutesChanged?()
        } catch {
            lastError = String(describing: error)
            JboxLog.engine.error("removeRoute id=\(id) failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// Derived state for the menu bar icon (spec.md § 4.2). `.attention`
    /// outranks `.running` — a single errored or waiting route turns the
    /// icon red even if other routes are flowing.
    public var overallState: OverallState {
        Self.overallState(for: routes)
    }

    /// Pure helper exposed for tests. Factored out so the derivation
    /// rule can be exercised against hand-crafted `Route` arrays without
    /// booting a real Core Audio engine.
    public static func overallState<S: Sequence>(for routes: S) -> OverallState
        where S.Element == Route
    {
        var anyRunning = false
        for r in routes {
            switch r.status.state {
            case .error, .waiting:
                return .attention
            case .running, .starting:
                anyRunning = true
            case .stopped:
                continue
            }
        }
        return anyRunning ? .running : .idle
    }

    /// Count of routes currently flowing audio (`.running`). Exposed for
    /// the menu bar header "N routes running" line.
    public var runningRouteCount: Int {
        routes.lazy.filter { $0.status.state == .running }.count
    }

    /// Start every route that isn't already running or in the middle of
    /// a transition. Errors on individual routes are logged through
    /// `startRoute`'s usual path (last-error plumbing, os_log) but do
    /// not abort the batch — the user sees each surviving failure in
    /// the main window alert.
    public func startAll() {
        for route in routes {
            switch route.status.state {
            case .stopped, .error:
                startRoute(route.id)
            case .waiting, .starting, .running:
                continue
            }
        }
    }

    /// Stop every route that isn't already stopped. Mirror of
    /// `startAll()` for the menu bar's Stop All action.
    public func stopAll() {
        for route in routes {
            switch route.status.state {
            case .running, .starting, .waiting:
                stopRoute(route.id)
            case .stopped, .error:
                continue
            }
        }
    }

    /// Rename a route non-disruptively. The engine's internal name is
    /// updated via `jbox_engine_rename_route`; the local `RouteConfig`
    /// gets the new name too so the UI's displayName follows. Empty
    /// string clears the custom name, letting `displayName` fall back
    /// to the "source → destination" auto-label.
    public func renameRoute(_ id: UInt32, to newName: String) {
        do {
            try engine.renameRoute(id, to: newName)
            if let idx = routes.firstIndex(where: { $0.id == id }) {
                let trimmed = newName.trimmingCharacters(in: .whitespaces)
                routes[idx].config.name = trimmed.isEmpty ? nil : trimmed
                routes[idx].modifiedAt = Date()
            }
            lastError = nil
            JboxLog.engine.notice("renameRoute id=\(id) ok")
            onRoutesChanged?()
        } catch {
            lastError = String(describing: error)
            JboxLog.engine.error("renameRoute id=\(id) failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// Apply edits to an existing route. When only the name differs,
    /// this short-circuits to `renameRoute`. Any other change (mapping,
    /// devices, latency mode, buffer frames) requires a reconfig —
    /// the engine's mapping is immutable after `addRoute`, so the
    /// store stops and removes the old route and adds a replacement.
    /// If the old route was running, the replacement is started and
    /// the returned `Route` reflects that. The engine-assigned id
    /// changes on reconfig; callers keeping ids around must refresh
    /// from the returned value.
    ///
    /// On `addRoute` failure during reconfig, the old route is
    /// restarted (if it was previously running) and the error is
    /// rethrown — the user sees an error message and their route
    /// stays in place.
    @discardableResult
    public func replaceRoute(_ id: UInt32, with newConfig: RouteConfig) throws -> Route {
        guard let idx = routes.firstIndex(where: { $0.id == id }) else {
            throw JboxError(code: JBOX_ERR_INVALID_ARGUMENT, message: "unknown route id")
        }
        let old = routes[idx]

        // Fast path: only the user-chosen name changed.
        if old.config.source == newConfig.source,
           old.config.destination == newConfig.destination,
           old.config.mapping == newConfig.mapping,
           old.config.latencyMode == newConfig.latencyMode,
           old.config.bufferFrames == newConfig.bufferFrames {
            renameRoute(id, to: newConfig.name ?? "")
            return routes[idx]
        }

        let wasActive = (old.status.state == .running || old.status.state == .waiting)
        if wasActive {
            try engine.stopRoute(id)
            refreshStatus(id)
        }

        let newId: UInt32
        do {
            newId = try engine.addRoute(
                sourceUID: newConfig.source.uid,
                destUID: newConfig.destination.uid,
                mapping: newConfig.mapping,
                name: newConfig.name ?? "",
                latencyMode: newConfig.latencyMode,
                bufferFrames: newConfig.bufferFrames ?? 0)
        } catch {
            // Best-effort rollback so the user isn't left without their route.
            if wasActive {
                try? engine.startRoute(id)
                refreshStatus(id)
            }
            lastError = String(describing: error)
            JboxLog.engine.error("replaceRoute id=\(id) addRoute failed: \(String(describing: error), privacy: .public)")
            throw error
        }

        try? engine.removeRoute(id)
        meters.removeValue(forKey: id)
        latencyComponents.removeValue(forKey: id)
        peakHolds.forget(routeId: id)

        let initialStatus = (try? engine.pollStatus(newId)) ?? RouteStatus(
            state: .stopped,
            lastError: JBOX_OK,
            framesProduced: 0, framesConsumed: 0,
            underrunCount: 0, overrunCount: 0)
        // Preserve the original persistId and createdAt so the route
        // keeps its identity across an edit; only modifiedAt advances.
        routes[idx] = Route(id: newId, config: newConfig, status: initialStatus,
                            persistId: old.persistId,
                            createdAt: old.createdAt,
                            modifiedAt: Date())

        if wasActive {
            startRoute(newId)
        }

        lastError = nil
        JboxLog.engine.notice("replaceRoute old=\(id) new=\(newId) wasActive=\(wasActive)")
        onRoutesChanged?()
        return routes[idx]
    }

    /// Refresh `status` on every known route. Meant to be driven by a
    /// ~4 Hz timer from the app layer (Phase 6 #4). Also refreshes the
    /// cached `latencyComponents` so the diagnostics panel sees the
    /// current breakdown as routes start / stop.
    public func pollStatuses() {
        for i in routes.indices {
            let id = routes[i].id
            if let status = try? engine.pollStatus(id) {
                routes[i].status = status
            }
            if let components = try? engine.pollLatencyComponents(id) {
                latencyComponents[id] = components
            }
        }
    }

    // MARK: Meters (Phase 6 Slice A)

    /// Drain peak meters from every currently-running route and publish
    /// them into `meters`. Routes that are not running are absent from
    /// the new snapshot (so stopping a route clears its dots on the
    /// next pass). Non-throwing; a mid-pass engine failure leaves any
    /// already-collected entries in place and silently skips the rest.
    ///
    /// Driven by a ~30 Hz task in the app layer while the main window
    /// is visible. Called on the main actor; the underlying C call is
    /// a sequence of atomic exchanges — cheap enough at this cadence.
    public func pollMeters() {
        var next: [UInt32: MeterPeaks] = [:]
        next.reserveCapacity(routes.count)
        let now = Date.timeIntervalSinceReferenceDate
        for route in routes where route.status.state == .running {
            let src = engine.pollMeters(routeId: route.id, side: .source)
            let dst = engine.pollMeters(routeId: route.id, side: .destination)
            next[route.id] = MeterPeaks(source: src, destination: dst)
            for (i, v) in src.enumerated() {
                peakHolds.observe(routeId: route.id, side: .source,
                                  channel: i, value: v, now: now)
            }
            for (i, v) in dst.enumerated() {
                peakHolds.observe(routeId: route.id, side: .dest,
                                  channel: i, value: v, now: now)
            }
        }
        meters = next
    }

    /// Current decayed peak-hold value for one channel on one side of
    /// a running route. Returns 0 for unknown routes / channels / after
    /// the hold has decayed. Spec § 4.5.
    public func heldPeak(routeId: UInt32,
                         side: Engine.MeterSide,
                         channel: Int,
                         now: TimeInterval = Date.timeIntervalSinceReferenceDate) -> Float {
        let trackerSide: PeakHoldTracker.Side = (side == .source ? .source : .dest)
        return peakHolds.heldValue(routeId: routeId,
                                   side: trackerSide,
                                   channel: channel,
                                   now: now)
    }

    /// Convenience accessor for one side of a route's last published
    /// peak snapshot. Reads from `meters`, so values are only as fresh
    /// as the most recent `pollMeters()` pass. Returns an empty array
    /// for routes that are not running (and therefore absent from the
    /// snapshot) or that have never been polled.
    public func meterPeaks(routeId: UInt32, side: Engine.MeterSide) -> [Float] {
        guard let p = meters[routeId] else { return [] }
        switch side {
        case .source:      return p.source
        case .destination: return p.destination
        }
    }

    // MARK: Internal helpers

    private func refreshStatus(_ id: UInt32) {
        guard let idx = routes.firstIndex(where: { $0.id == id }) else { return }
        if let status = try? engine.pollStatus(id) {
            routes[idx].status = status
        }
    }
}
