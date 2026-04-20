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

    public init(source: DeviceReference,
                destination: DeviceReference,
                mapping: [ChannelEdge],
                name: String? = nil) {
        self.source = source
        self.destination = destination
        self.mapping = mapping
        self.name = name
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
                name: config.name ?? ""
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

    // MARK: Internal helpers

    private func refreshStatus(_ id: UInt32) {
        guard let idx = routes.firstIndex(where: { $0.id == id }) else { return }
        if let status = try? engine.pollStatus(id) {
            routes[idx].status = status
        }
    }
}
