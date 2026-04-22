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

public struct Device: Equatable, Hashable, Sendable {
    public let uid: String
    public let name: String
    public let directionInput: Bool
    public let directionOutput: Bool
    public let inputChannelCount: UInt32
    public let outputChannelCount: UInt32
    public let nominalSampleRate: Double
    public let bufferFrameSize: UInt32

    public init(uid: String,
                name: String,
                directionInput: Bool,
                directionOutput: Bool,
                inputChannelCount: UInt32,
                outputChannelCount: UInt32,
                nominalSampleRate: Double,
                bufferFrameSize: UInt32) {
        self.uid = uid
        self.name = name
        self.directionInput = directionInput
        self.directionOutput = directionOutput
        self.inputChannelCount = inputChannelCount
        self.outputChannelCount = outputChannelCount
        self.nominalSampleRate = nominalSampleRate
        self.bufferFrameSize = bufferFrameSize
    }
}

public struct ChannelEdge: Equatable, Hashable, Sendable {
    public let src: UInt32
    public let dst: UInt32
    public init(src: UInt32, dst: UInt32) {
        self.src = src
        self.dst = dst
    }
}

public enum RouteState: Equatable, Hashable, Sendable {
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

public struct RouteStatus: Equatable, Hashable, Sendable {
    public let state: RouteState
    public let lastError: jbox_error_code_t
    public let framesProduced: UInt64
    public let framesConsumed: UInt64
    public let underrunCount: UInt64
    public let overrunCount: UInt64
    /// End-to-end estimate computed once at route start, per
    /// docs/spec.md § 2.12. 0 when the route is not running or the
    /// engine could not determine a sample rate. Not updated while
    /// running; a stop + start refreshes the estimate.
    public let estimatedLatencyUs: UInt64

    public init(state: RouteState,
                lastError: jbox_error_code_t,
                framesProduced: UInt64,
                framesConsumed: UInt64,
                underrunCount: UInt64,
                overrunCount: UInt64,
                estimatedLatencyUs: UInt64 = 0) {
        self.state = state
        self.lastError = lastError
        self.framesProduced = framesProduced
        self.framesConsumed = framesConsumed
        self.underrunCount = underrunCount
        self.overrunCount = overrunCount
        self.estimatedLatencyUs = estimatedLatencyUs
    }
}

/// Per-component breakdown of a route's end-to-end latency, mirroring
/// `jbox_route_latency_components_t` (ABI v4+). All frame counts are
/// expressed at the sample rate of the side they belong to — `src*`
/// at `sourceSampleRateHz`, `dst*` + `converterPrimeFrames` at
/// `destSampleRateHz`, `ringTargetFillFrames` at `sourceSampleRateHz`.
/// All values are 0 for routes that are not currently running.
public struct LatencyComponents: Equatable, Hashable, Sendable {
    public let sourceHalLatencyFrames:   UInt32
    public let sourceSafetyOffsetFrames: UInt32
    public let sourceBufferFrames:       UInt32
    public let ringTargetFillFrames:     UInt32
    public let converterPrimeFrames:     UInt32
    public let destBufferFrames:         UInt32
    public let destSafetyOffsetFrames:   UInt32
    public let destHalLatencyFrames:     UInt32
    public let sourceSampleRateHz:       Double
    public let destSampleRateHz:         Double
    public let totalUs:                  UInt64

    public static let zero = LatencyComponents(
        sourceHalLatencyFrames: 0, sourceSafetyOffsetFrames: 0,
        sourceBufferFrames: 0, ringTargetFillFrames: 0,
        converterPrimeFrames: 0, destBufferFrames: 0,
        destSafetyOffsetFrames: 0, destHalLatencyFrames: 0,
        sourceSampleRateHz: 0, destSampleRateHz: 0, totalUs: 0)

    public init(sourceHalLatencyFrames: UInt32,
                sourceSafetyOffsetFrames: UInt32,
                sourceBufferFrames: UInt32,
                ringTargetFillFrames: UInt32,
                converterPrimeFrames: UInt32,
                destBufferFrames: UInt32,
                destSafetyOffsetFrames: UInt32,
                destHalLatencyFrames: UInt32,
                sourceSampleRateHz: Double,
                destSampleRateHz: Double,
                totalUs: UInt64) {
        self.sourceHalLatencyFrames   = sourceHalLatencyFrames
        self.sourceSafetyOffsetFrames = sourceSafetyOffsetFrames
        self.sourceBufferFrames       = sourceBufferFrames
        self.ringTargetFillFrames     = ringTargetFillFrames
        self.converterPrimeFrames     = converterPrimeFrames
        self.destBufferFrames         = destBufferFrames
        self.destSafetyOffsetFrames   = destSafetyOffsetFrames
        self.destHalLatencyFrames     = destHalLatencyFrames
        self.sourceSampleRateHz       = sourceSampleRateHz
        self.destSampleRateHz         = destSampleRateHz
        self.totalUs                  = totalUs
    }
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

    /// Direction selector for per-channel name queries. Mirrors the C
    /// bitmask but the wrapper accepts exactly one direction per call.
    public enum ChannelDirection: Sendable, Hashable {
        case input, output

        fileprivate var cValue: jbox_device_direction_t {
            switch self {
            case .input:  return JBOX_DEVICE_DIRECTION_INPUT
            case .output: return JBOX_DEVICE_DIRECTION_OUTPUT
            }
        }
    }

    /// Which side of a route to meter. Source reflects the pre-ring-
    /// buffer peaks of the mapped source channels; destination reflects
    /// the post-converter peaks of the mapped destination channels.
    /// Mirrors `jbox_meter_side_t` in the C bridge.
    public enum MeterSide: Sendable, Hashable {
        case source, destination

        fileprivate var cValue: jbox_meter_side_t {
            switch self {
            case .source:      return JBOX_METER_SIDE_SOURCE
            case .destination: return JBOX_METER_SIDE_DEST
            }
        }
    }

    /// Drain peak meters for a route. Returns an array of linear peak-
    /// amplitude values (|sample|) with read-and-reset semantics —
    /// each call yields the peak since the previous call and resets
    /// the underlying atomic to zero.
    ///
    /// Returns an empty array when:
    ///   - the engine handle is gone
    ///   - `maxChannels` is zero
    ///   - the route is unknown or not currently `.running`
    ///
    /// Non-throwing: meter polling is a high-frequency UI path; a
    /// thrown exception on the hot loop is not appropriate, and any
    /// engine-side failure maps to "no data".
    public func pollMeters(routeId: UInt32,
                           side: MeterSide,
                           maxChannels: Int = 64) -> [Float] {
        guard let h = handle, maxChannels > 0 else { return [] }
        var buf = [Float](repeating: 0, count: maxChannels)
        let written = buf.withUnsafeMutableBufferPointer { ptr -> Int in
            Int(jbox_engine_poll_meters(h, routeId, side.cValue,
                                        ptr.baseAddress, maxChannels))
        }
        if written == 0 { return [] }
        if written < maxChannels {
            buf.removeLast(maxChannels - written)
        }
        return buf
    }

    /// Per-channel names for a device. The returned array has one
    /// entry per channel in the requested direction; entries are empty
    /// strings when the driver does not publish a label. Callers
    /// typically render `"Ch N · <name>"` when present and `"Ch N"`
    /// when empty.
    public func enumerateChannels(uid: String,
                                  direction: ChannelDirection) throws -> [String] {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        var err = jbox_error_t(code: JBOX_OK, message: nil)
        let listPtr = uid.withCString { uidPtr in
            jbox_engine_enumerate_device_channels(h, uidPtr, direction.cValue, &err)
        }
        guard let list = listPtr else {
            throw JboxError(code: err.code,
                            message: err.message.map { String(cString: $0) } ?? "")
        }
        defer { jbox_channel_list_free(list) }

        let count = Int(list.pointee.count)
        var out: [String] = []
        out.reserveCapacity(count)
        for i in 0..<count {
            let info = list.pointee.channels.advanced(by: i).pointee
            let name = withUnsafePointer(to: info.name) {
                $0.withMemoryRebound(to: CChar.self,
                                     capacity: Int(JBOX_NAME_MAX_LEN)) {
                    String(cString: $0)
                }
            }
            out.append(name)
        }
        return out
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
                         name: String = "",
                         latencyMode: LatencyMode = .off,
                         bufferFrames: UInt32 = 0) throws -> UInt32 {
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
                            name: name.isEmpty ? nil : namePtr,
                            latency_mode: latencyMode.rawValue,
                            buffer_frames: bufferFrames
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

    /// Rename a route in place. Non-disruptive — the engine keeps the
    /// route flowing audio if it was running. Pass an empty string to
    /// clear the stored name.
    public func renameRoute(_ id: UInt32, to newName: String) throws {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        let code = newName.withCString { ptr in
            jbox_engine_rename_route(h, id, ptr)
        }
        if code != JBOX_OK {
            throw JboxError(code: code)
        }
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
            overrunCount: out.overrun_count,
            estimatedLatencyUs: out.estimated_latency_us
        )
    }

    /// Supported HAL buffer-frame-size range for the device with UID
    /// `uid`. Returns `nil` if the HAL does not expose a range or
    /// the device is unknown. For aggregate devices this is the
    /// intersection of every active sub-device's range.
    public func supportedBufferFrameSizeRange(
        forDeviceUid uid: String
    ) throws -> ClosedRange<UInt32>? {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        var low: UInt32 = 0
        var high: UInt32 = 0
        let code = uid.withCString { ptr in
            jbox_engine_supported_buffer_frame_size_range(h, ptr, &low, &high)
        }
        if code != JBOX_OK { throw JboxError(code: code) }
        if low == 0 && high == 0 { return nil }
        if high < low { return nil }
        return low...high
    }

    /// Latency component breakdown for `id`. Returns `.zero` for
    /// non-running routes (all fields 0).
    public func pollLatencyComponents(_ id: UInt32) throws -> LatencyComponents {
        guard let h = handle else {
            throw JboxError(code: JBOX_ERR_INTERNAL, message: "engine not initialised")
        }
        var out = jbox_route_latency_components_t()
        let code = jbox_engine_poll_route_latency_components(h, id, &out)
        if code != JBOX_OK {
            throw JboxError(code: code)
        }
        return LatencyComponents(
            sourceHalLatencyFrames:   out.src_hal_latency_frames,
            sourceSafetyOffsetFrames: out.src_safety_offset_frames,
            sourceBufferFrames:       out.src_buffer_frames,
            ringTargetFillFrames:     out.ring_target_fill_frames,
            converterPrimeFrames:     out.converter_prime_frames,
            destBufferFrames:         out.dst_buffer_frames,
            destSafetyOffsetFrames:   out.dst_safety_offset_frames,
            destHalLatencyFrames:     out.dst_hal_latency_frames,
            sourceSampleRateHz:       out.src_sample_rate_hz,
            destSampleRateHz:         out.dst_sample_rate_hz,
            totalUs:                  out.total_us)
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
