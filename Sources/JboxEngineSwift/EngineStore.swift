import Foundation
import Observation

// MARK: - UI-side value types

/// Persistent pointer to a device by its stable Core Audio UID, plus
/// the last human-readable name we saw for it. `lastKnownName` lets
/// the UI keep rendering a sensible label when the device is
/// temporarily disconnected. See docs/spec.md § 3.1.1.
public struct DeviceReference: Equatable, Hashable, Sendable {
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
public struct RouteConfig: Equatable, Sendable {
    public var source: DeviceReference
    public var destination: DeviceReference
    public var mapping: [ChannelEdge]
    /// When nil, `displayName` auto-generates "source → destination".
    public var name: String?
    /// Opt-in tighter ring-buffer sizing. Engine default (off) uses
    /// the safe sizing that absorbs USB burst-delivery jitter; on
    /// trades headroom for ~30–60 ms of latency reduction. See
    /// docs/spec.md § 2.3.
    public var lowLatency: Bool

    public init(source: DeviceReference,
                destination: DeviceReference,
                mapping: [ChannelEdge],
                name: String? = nil,
                lowLatency: Bool = false) {
        self.source = source
        self.destination = destination
        self.mapping = mapping
        self.name = name
        self.lowLatency = lowLatency
    }

    public var displayName: String {
        if let name, !name.isEmpty { return name }
        return "\(source.lastKnownName) → \(destination.lastKnownName)"
    }
}

/// A live route: the engine-assigned id, the user's config, and the
/// most recently polled runtime status.
public struct Route: Identifiable, Equatable, Sendable {
    public let id: UInt32
    public var config: RouteConfig
    public var status: RouteStatus

    public init(id: UInt32, config: RouteConfig, status: RouteStatus) {
        self.id = id
        self.config = config
        self.status = status
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

    // MARK: Routes

    /// Add a route; returns the new live `Route` value (also appended
    /// to `routes`). Throws `JboxError` on engine-side failure.
    @discardableResult
    public func addRoute(_ config: RouteConfig) throws -> Route {
        do {
            let id = try engine.addRoute(
                sourceUID: config.source.uid,
                destUID: config.destination.uid,
                mapping: config.mapping,
                name: config.name ?? "",
                lowLatency: config.lowLatency
            )
            let status = try engine.pollStatus(id)
            let route = Route(id: id, config: config, status: status)
            routes.append(route)
            lastError = nil
            JboxLog.engine.notice("route added: id=\(id) src=\(config.source.lastKnownName, privacy: .public) dst=\(config.destination.lastKnownName, privacy: .public) channels=\(config.mapping.count)")
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
            peakHolds.forget(routeId: id)
            lastError = nil
            JboxLog.engine.notice("removeRoute id=\(id) ok")
        } catch {
            lastError = String(describing: error)
            JboxLog.engine.error("removeRoute id=\(id) failed: \(String(describing: error), privacy: .public)")
        }
    }

    /// Refresh `status` on every known route. Meant to be driven by a
    /// ~4 Hz timer from the app layer (Phase 6 #4).
    public func pollStatuses() {
        for i in routes.indices {
            if let status = try? engine.pollStatus(routes[i].id) {
                routes[i].status = status
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
