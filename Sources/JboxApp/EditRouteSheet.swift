import SwiftUI
import JboxEngineSwift

/// Sheet for editing an existing route. Pre-fills every field from
/// the route's current `RouteConfig`. On Apply:
///   - If only the user-chosen name changed, the store routes the call
///     through `jbox_engine_rename_route` — no audio interruption.
///   - Any other change (devices, mapping, latency mode, buffer frames)
///     forces a reconfig: the old route is stopped and removed, and a
///     replacement is added with a fresh engine id. If the old route
///     was running, the replacement is started automatically.
///
/// The apply button's copy reflects whether the route is active
/// ("Apply and restart" vs "Apply") so the user sees the restart
/// happen by design rather than as a surprise.
struct EditRouteSheet: View {
    let route: Route
    let store: EngineStore
    let onClose: () -> Void

    @State private var sourceUID: String = ""
    @State private var destUID: String = ""
    @State private var pairs: [MappingPair] = []
    @State private var customName: String = ""
    @State private var latencyMode: LatencyMode = .off
    @State private var bufferFrames: UInt32 = 0
    @State private var errorMessage: String?

    private static let kBufferSizeChoices: [UInt32] = [
        32, 64, 128, 256, 512, 1024, 2048]

    // MARK: Derived

    private var inputDevices: [Device] {
        store.devices.filter { $0.directionInput && $0.inputChannelCount > 0 }
    }
    private var outputDevices: [Device] {
        store.devices.filter { $0.directionOutput && $0.outputChannelCount > 0 }
    }
    private var srcDevice: Device? { store.device(uid: sourceUID) }
    private var dstDevice: Device? { store.device(uid: destUID) }
    private var srcChannels: Int { Int(srcDevice?.inputChannelCount ?? 0) }
    private var dstChannels: Int { Int(dstDevice?.outputChannelCount ?? 0) }

    private var srcChannelNames: [String] {
        srcDevice.map { store.channelNames(uid: $0.uid, direction: .input) } ?? []
    }
    private var dstChannelNames: [String] {
        dstDevice.map { store.channelNames(uid: $0.uid, direction: .output) } ?? []
    }

    private var validationIssue: String? {
        if sourceUID.isEmpty { return "Pick a source device." }
        if destUID.isEmpty   { return "Pick a destination device." }
        return ChannelMappingValidator.issue(
            pairs: pairs, srcChannels: srcChannels, dstChannels: dstChannels)
    }

    /// Did the user change anything that requires a full reconfig?
    /// Name-only changes take the non-disruptive engine rename path.
    private var hasConfigChanges: Bool {
        let edges = MappingPair.toEdges(pairs)
        if sourceUID != route.config.source.uid          { return true }
        if destUID   != route.config.destination.uid     { return true }
        if edges     != route.config.mapping             { return true }
        if latencyMode != route.config.latencyMode       { return true }
        let effectiveBuffer = bufferFrames == 0 ? nil : bufferFrames
        if effectiveBuffer != route.config.bufferFrames  { return true }
        return false
    }

    private var trimmedName: String {
        customName.trimmingCharacters(in: .whitespaces)
    }

    private var hasAnyChange: Bool {
        if hasConfigChanges { return true }
        let newName: String? = trimmedName.isEmpty ? nil : trimmedName
        return newName != route.config.name
    }

    private var canSave: Bool { validationIssue == nil && hasAnyChange }

    private var wasActive: Bool {
        route.status.state == .running || route.status.state == .waiting
    }

    private var applyLabel: String {
        if hasConfigChanges && wasActive { return "Apply and restart" }
        return "Apply"
    }

    private var bufferSizeOptions: [UInt32] {
        var range: ClosedRange<UInt32>? = nil
        if !sourceUID.isEmpty,
           let r = store.bufferFrameRange(forDeviceUid: sourceUID) {
            range = r
        }
        if sourceUID != destUID, !destUID.isEmpty,
           let r = store.bufferFrameRange(forDeviceUid: destUID) {
            if let existing = range {
                let lo = max(existing.lowerBound, r.lowerBound)
                let hi = min(existing.upperBound, r.upperBound)
                range = lo <= hi ? lo...hi : nil
            } else {
                range = r
            }
        }
        if let range {
            return Self.kBufferSizeChoices.filter { range.contains($0) }
        }
        return Self.kBufferSizeChoices
    }

    private var latencyModeFooter: String {
        switch latencyMode {
        case .off:
            return "Absorbs USB-burst jitter. Choose this unless you "
                 + "need noticeably lower latency."
        case .low:
            return "Tighter ring buffer. Some USB interfaces that "
                 + "deliver samples in bursts may underrun — step back "
                 + "if you hear clicks."
        case .performance:
            return "Lowest latency tier. Smallest ring + aggressive "
                 + "drift setpoint. When source and destination are "
                 + "the same device (e.g. an aggregate), takes "
                 + "exclusive control of the device to force a small "
                 + "HAL buffer — other apps using the device are "
                 + "disconnected while the route is running. Underruns "
                 + "are expected on bursty sources."
        }
    }

    // MARK: View

    var body: some View {
        NavigationStack {
            Form {
                Section("Devices") {
                    Picker("Source", selection: $sourceUID) {
                        Text("Select…").tag("")
                        ForEach(inputDevices, id: \.uid) { d in
                            Text("\(d.name)  (\(d.inputChannelCount) in)").tag(d.uid)
                        }
                    }
                    Picker("Destination", selection: $destUID) {
                        Text("Select…").tag("")
                        ForEach(outputDevices, id: \.uid) { d in
                            Text("\(d.name)  (\(d.outputChannelCount) out)").tag(d.uid)
                        }
                    }
                }

                ChannelMappingEditor(
                    pairs: $pairs,
                    srcChannels: srcChannels,
                    dstChannels: dstChannels,
                    srcChannelNames: srcChannelNames,
                    dstChannelNames: dstChannelNames)

                Section("Name") {
                    TextField("Auto from devices", text: $customName)
                }

                Section {
                    Picker("Latency mode", selection: $latencyMode) {
                        Text("Off — safe default").tag(LatencyMode.off)
                        Text("Low").tag(LatencyMode.low)
                        Text("Performance").tag(LatencyMode.performance)
                    }
                    .pickerStyle(.menu)

                    if latencyMode == .performance {
                        Picker("Buffer size", selection: $bufferFrames) {
                            Text("Default (64)").tag(UInt32(0))
                            ForEach(bufferSizeOptions, id: \.self) { frames in
                                Text("\(frames) frames").tag(frames)
                            }
                        }
                        .pickerStyle(.menu)
                    }
                } footer: {
                    Text(latencyModeFooter)
                }

                if let message = errorMessage ?? validationIssue {
                    Section {
                        Label(message, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(errorMessage != nil ? .red : .secondary)
                    }
                }
            }
            .formStyle(.grouped)
            .navigationTitle("Edit route")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", action: onClose)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(applyLabel, action: apply)
                        .disabled(!canSave)
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
        .frame(minWidth: 520, minHeight: 460)
        .onAppear(perform: prefillFromRoute)
    }

    // MARK: Actions

    private func prefillFromRoute() {
        sourceUID    = route.config.source.uid
        destUID      = route.config.destination.uid
        pairs        = MappingPair.from(edges: route.config.mapping)
        customName   = route.config.name ?? ""
        latencyMode  = route.config.latencyMode
        bufferFrames = route.config.bufferFrames ?? 0
    }

    private func apply() {
        guard let src = srcDevice, let dst = dstDevice else { return }
        let newConfig = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: MappingPair.toEdges(pairs),
            name: trimmedName.isEmpty ? nil : trimmedName,
            latencyMode: latencyMode,
            bufferFrames: bufferFrames == 0 ? nil : bufferFrames)
        do {
            _ = try store.replaceRoute(route.id, with: newConfig)
            onClose()
        } catch let e as JboxError {
            errorMessage = e.description
        } catch {
            errorMessage = String(describing: error)
        }
    }
}
