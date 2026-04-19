import Foundation
@_exported import JboxEngineC

/// Swift-facing wrapper over the Jbox C bridge.
///
/// `Engine` is the entry point. It owns a `jbox_engine_t*` handle and
/// tears it down on deinit. All engine operations are thin wrappers
/// that translate Swift arguments into the C API and map error codes
/// into `JboxError` throwables.
///
/// This wrapper lives under the `JboxEngine` namespace rather than
/// being called `JboxEngine` directly, to avoid colliding with the
/// target's module name.
public enum JboxEngine {
    /// Runtime ABI version reported by the engine. Matches
    /// `JBOX_ENGINE_ABI_VERSION` at the C header level.
    public static var abiVersion: UInt32 {
        jbox_engine_abi_version()
    }
}

// MARK: - Error type

public struct JboxError: Error, CustomStringConvertible {
    public let code: jbox_error_code_t
    public let message: String

    public init(code: jbox_error_code_t, message: String = "") {
        self.code = code
        self.message = message
    }

    public var description: String {
        let codeName = String(cString: jbox_error_code_name(code))
        return message.isEmpty ? codeName : "\(codeName): \(message)"
    }
}

// MARK: - Device / route value types

public struct Device {
    public let uid: String
    public let name: String
    public let directionInput: Bool
    public let directionOutput: Bool
    public let inputChannelCount: UInt32
    public let outputChannelCount: UInt32
    public let nominalSampleRate: Double
    public let bufferFrameSize: UInt32
}

public struct ChannelEdge {
    public let src: UInt32
    public let dst: UInt32
    public init(src: UInt32, dst: UInt32) {
        self.src = src
        self.dst = dst
    }
}

public enum RouteState {
    case stopped, waiting, starting, running, error

    init(_ c: jbox_route_state_t) {
        switch c {
        case JBOX_ROUTE_STATE_STOPPED:  self = .stopped
        case JBOX_ROUTE_STATE_WAITING:  self = .waiting
        case JBOX_ROUTE_STATE_STARTING: self = .starting
        case JBOX_ROUTE_STATE_RUNNING:  self = .running
        case JBOX_ROUTE_STATE_ERROR:    self = .error
        default:                        self = .error
        }
    }
}

public struct RouteStatus {
    public let state: RouteState
    public let lastError: jbox_error_code_t
    public let framesProduced: UInt64
    public let framesConsumed: UInt64
    public let underrunCount: UInt64
    public let overrunCount: UInt64
}

// MARK: - Engine

public final class Engine {
    private var handle: OpaquePointer?

    /// Create a production engine backed by Core Audio.
    public init() throws {
        var err = jbox_error_t(code: JBOX_OK, message: nil)
        var cfg = jbox_engine_config_t()
        guard let h = jbox_engine_create(&cfg, &err) else {
            throw JboxError(code: err.code,
                            message: err.message.map { String(cString: $0) } ?? "")
        }
        self.handle = h
    }

    deinit {
        if let h = handle {
            jbox_engine_destroy(h)
        }
    }

    public func enumerateDevices() throws -> [Device] {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        var err = jbox_error_t(code: JBOX_OK, message: nil)
        guard let list = jbox_engine_enumerate_devices(
            h, &err) else {
            throw JboxError(code: err.code,
                            message: err.message.map { String(cString: $0) } ?? "")
        }
        defer { jbox_device_list_free(list) }

        let count = Int(list.pointee.count)
        var out: [Device] = []
        out.reserveCapacity(count)
        for i in 0..<count {
            let info = list.pointee.devices.advanced(by: i).pointee
            out.append(Device(
                uid: withUnsafePointer(to: info.uid) {
                    $0.withMemoryRebound(to: CChar.self,
                                         capacity: Int(JBOX_UID_MAX_LEN)) {
                        String(cString: $0)
                    }
                },
                name: withUnsafePointer(to: info.name) {
                    $0.withMemoryRebound(to: CChar.self,
                                         capacity: Int(JBOX_NAME_MAX_LEN)) {
                        String(cString: $0)
                    }
                },
                directionInput:  (info.direction & UInt32(JBOX_DEVICE_DIRECTION_INPUT.rawValue))  != 0,
                directionOutput: (info.direction & UInt32(JBOX_DEVICE_DIRECTION_OUTPUT.rawValue)) != 0,
                inputChannelCount: info.input_channel_count,
                outputChannelCount: info.output_channel_count,
                nominalSampleRate: info.nominal_sample_rate,
                bufferFrameSize: info.buffer_frame_size
            ))
        }
        return out
    }

    @discardableResult
    public func addRoute(sourceUID: String,
                         destUID: String,
                         mapping: [ChannelEdge],
                         name: String = "") throws -> UInt32 {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        let cEdges = mapping.map {
            jbox_channel_edge_t(src: $0.src, dst: $0.dst)
        }
        return try sourceUID.withCString { srcPtr in
            try destUID.withCString { dstPtr in
                try name.withCString { namePtr in
                    try cEdges.withUnsafeBufferPointer { edgesPtr in
                        var cfg = jbox_route_config_t(
                            source_uid: srcPtr,
                            dest_uid: dstPtr,
                            mapping: edgesPtr.baseAddress,
                            mapping_count: cEdges.count,
                            name: name.isEmpty ? nil : namePtr
                        )
                        var err = jbox_error_t(code: JBOX_OK, message: nil)
                        let id = jbox_engine_add_route(
                            h, &cfg, &err)
                        if id == JBOX_INVALID_ROUTE_ID {
                            throw JboxError(code: err.code,
                                            message: err.message.map { String(cString: $0) } ?? "")
                        }
                        return id
                    }
                }
            }
        }
    }

    public func startRoute(_ id: UInt32) throws {
        try callRouteAction(id, jbox_engine_start_route)
    }

    public func stopRoute(_ id: UInt32) throws {
        try callRouteAction(id, jbox_engine_stop_route)
    }

    public func removeRoute(_ id: UInt32) throws {
        try callRouteAction(id, jbox_engine_remove_route)
    }

    public func pollStatus(_ id: UInt32) throws -> RouteStatus {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        var out = jbox_route_status_t()
        let code = jbox_engine_poll_route_status(
            h, id, &out)
        if code != JBOX_OK {
            throw JboxError(code: code)
        }
        return RouteStatus(
            state: RouteState(out.state),
            lastError: out.last_error,
            framesProduced: out.frames_produced,
            framesConsumed: out.frames_consumed,
            underrunCount: out.underrun_count,
            overrunCount: out.overrun_count
        )
    }

    private func callRouteAction(_ id: UInt32,
                                 _ f: (OpaquePointer?, UInt32) -> jbox_error_code_t) throws {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        let code = f(h, id)
        if code != JBOX_OK {
            throw JboxError(code: code)
        }
    }
}
