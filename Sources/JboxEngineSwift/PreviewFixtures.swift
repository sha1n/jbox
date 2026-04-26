#if DEBUG
import Foundation
import JboxEngineC

/// Stub data for SwiftUI `#Preview` blocks and lightweight tests.
/// Lives in `JboxEngineSwift` (not `JboxApp`) so the fixtures can
/// reference C-bridge types like `jbox_error_code_t` without forcing
/// `JboxApp` to depend on `JboxEngineC`.
///
/// All entries are purely synthetic — there is no Core Audio
/// interaction. The factory in `EngineStore.preview(...)` snaps these
/// onto the store's observable arrays so a `#Preview` canvas renders
/// realistic state without spawning routes.
public enum PreviewFixtures {

    // MARK: Device fixtures

    public static let micUid  = "preview:mic-2ch"
    public static let intfUid = "preview:interface-8ch"
    public static let aggUid  = "preview:aggregate-16ch"

    public static let devices: [Device] = [
        Device(uid: micUid,
               name: "USB Mic",
               directionInput: true, directionOutput: false,
               inputChannelCount: 2, outputChannelCount: 0,
               nominalSampleRate: 48_000, bufferFrameSize: 256),
        Device(uid: intfUid,
               name: "Audio Interface 8x8",
               directionInput: true, directionOutput: true,
               inputChannelCount: 8, outputChannelCount: 8,
               nominalSampleRate: 48_000, bufferFrameSize: 128),
        Device(uid: aggUid,
               name: "Studio Aggregate",
               directionInput: true, directionOutput: true,
               inputChannelCount: 16, outputChannelCount: 16,
               nominalSampleRate: 48_000, bufferFrameSize: 64),
    ]

    public static let micRef  = DeviceReference(uid: micUid,  lastKnownName: "USB Mic")
    public static let intfRef = DeviceReference(uid: intfUid, lastKnownName: "Audio Interface 8x8")
    public static let aggRef  = DeviceReference(uid: aggUid,  lastKnownName: "Studio Aggregate")

    // MARK: Route fixtures

    public static func runningRoute(id: UInt32 = 1,
                                    name: String = "Mic → Interface") -> Route {
        Route(
            id: id,
            config: RouteConfig(
                source: micRef,
                destination: intfRef,
                mapping: [ChannelEdge(src: 0, dst: 0),
                          ChannelEdge(src: 1, dst: 1)],
                name: name,
                latencyMode: .low),
            status: RouteStatus(
                state: .running,
                lastError: JBOX_OK,
                framesProduced: 1_234_567,
                framesConsumed: 1_234_500,
                underrunCount: 0,
                overrunCount: 0,
                estimatedLatencyUs: 5_300))
    }

    public static func stoppedRoute(id: UInt32 = 2,
                                    name: String = "Interface → Aggregate") -> Route {
        Route(
            id: id,
            config: RouteConfig(
                source: intfRef,
                destination: aggRef,
                mapping: (0..<4).map { ChannelEdge(src: UInt32($0), dst: UInt32($0)) },
                name: name),
            status: RouteStatus(
                state: .stopped,
                lastError: JBOX_OK,
                framesProduced: 0, framesConsumed: 0,
                underrunCount: 0, overrunCount: 0,
                estimatedLatencyUs: 0))
    }

    public static func waitingRoute(id: UInt32 = 3) -> Route {
        Route(
            id: id,
            config: RouteConfig(
                source: micRef,
                destination: aggRef,
                mapping: [ChannelEdge(src: 0, dst: 0)],
                name: "Mic → Aggregate"),
            status: RouteStatus(
                state: .waiting,
                lastError: JBOX_ERR_DEVICE_NOT_FOUND,
                framesProduced: 0, framesConsumed: 0,
                underrunCount: 0, overrunCount: 0,
                estimatedLatencyUs: 0))
    }

    public static func startingRoute(id: UInt32 = 4) -> Route {
        Route(
            id: id,
            config: RouteConfig(
                source: intfRef,
                destination: aggRef,
                mapping: (0..<2).map { ChannelEdge(src: UInt32($0), dst: UInt32($0)) },
                name: "Interface → Aggregate (starting)"),
            status: RouteStatus(
                state: .starting,
                lastError: JBOX_OK,
                framesProduced: 0, framesConsumed: 0,
                underrunCount: 0, overrunCount: 0,
                estimatedLatencyUs: 0))
    }

    public static func errorRoute(id: UInt32 = 5) -> Route {
        Route(
            id: id,
            config: RouteConfig(
                source: intfRef,
                destination: aggRef,
                mapping: [ChannelEdge(src: 0, dst: 0)],
                name: "Errored route"),
            status: RouteStatus(
                state: .error,
                lastError: JBOX_ERR_INTERNAL,
                framesProduced: 0, framesConsumed: 0,
                underrunCount: 0, overrunCount: 1,
                estimatedLatencyUs: 0))
    }

    // MARK: Meters and latency

    /// Stub `MeterPeaks` shaped like a real route's per-channel array.
    /// Spreads peaks from -20..-3 dBFS so the bar previews show all
    /// four colour zones at once.
    public static func meters(channels: Int) -> MeterPeaks {
        guard channels > 0 else { return MeterPeaks() }
        let levels: [Float] = (0..<channels).map { i in
            let span = Double(channels - 1)
            let dB   = -20.0 + (span > 0 ? Double(i) * 17.0 / span : 0)
            return Float(pow(10.0, dB / 20.0))
        }
        return MeterPeaks(source: levels, destination: levels)
    }

    public static func latency(for route: Route) -> LatencyComponents {
        LatencyComponents(
            sourceHalLatencyFrames: 64,
            sourceSafetyOffsetFrames: 32,
            sourceBufferFrames: 256,
            ringTargetFillFrames: 256,
            converterPrimeFrames: 16,
            destBufferFrames: 256,
            destSafetyOffsetFrames: 32,
            destHalLatencyFrames: 64,
            sourceSampleRateHz: 48_000,
            destSampleRateHz: 48_000,
            totalUs: route.status.estimatedLatencyUs)
    }

    // MARK: Composite stores

    /// Mixed bag of route states + meters + latency suitable for
    /// previewing the main `RouteListView`.
    @MainActor
    public static func sampleStore() -> EngineStore {
        let running = runningRoute(id: 1)
        let stopped = stoppedRoute(id: 2)
        let waiting = waitingRoute(id: 3)
        return EngineStore.preview(
            routes: [running, stopped, waiting],
            devices: devices,
            meters: [running.id: meters(channels: running.config.mapping.count)],
            latencyComponents: [running.id: latency(for: running)])
    }

    /// Empty store — used to preview the empty-list "no routes yet"
    /// placeholder.
    @MainActor
    public static func emptyStore() -> EngineStore {
        EngineStore.preview(routes: [], devices: devices)
    }

    /// Single-running-route store for `MeterPanel` and `RouteRow`
    /// expanded-state previews.
    @MainActor
    public static func runningStore(channels: Int = 4) -> EngineStore {
        let r = Route(
            id: 1,
            config: RouteConfig(
                source: intfRef,
                destination: aggRef,
                mapping: (0..<channels).map { ChannelEdge(src: UInt32($0), dst: UInt32($0)) },
                name: "Interface → Aggregate",
                latencyMode: .low),
            status: RouteStatus(
                state: .running,
                lastError: JBOX_OK,
                framesProduced: 9_876_543,
                framesConsumed: 9_876_500,
                underrunCount: 0,
                overrunCount: 0,
                estimatedLatencyUs: 5_300))
        return EngineStore.preview(
            routes: [r],
            devices: devices,
            meters: [r.id: meters(channels: channels)],
            latencyComponents: [r.id: latency(for: r)])
    }
}
#endif
