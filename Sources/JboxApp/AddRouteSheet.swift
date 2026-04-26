import SwiftUI
import JboxEngineSwift

/// Modal sheet presented from the main window's "+" toolbar button.
/// Picks source and destination devices, edits a channel mapping as
/// a list of 1-indexed pairs, and submits to `store.addRoute`.
/// Mapping rules (docs/spec.md § 3.1): non-empty; no duplicate dst
/// (fan-in / summing is deferred per Appendix A). Duplicate src is
/// **allowed** — it produces fan-out, one source feeding multiple
/// destinations. Engine-side validation errors are surfaced inline.
struct AddRouteSheet: View {
    let store: EngineStore
    let onClose: () -> Void

    @State private var sourceUID: String = ""
    @State private var destUID: String = ""
    @State private var pairs: [MappingPair] = [MappingPair(src: 0, dst: 0)]
    @State private var customName: String = ""
    @State private var latencyMode: LatencyMode = .off
    /// 0 == "no preference" (the route runs at whatever buffer the
    /// device is currently at); non-zero issues a single SD-style
    /// `kAudioDevicePropertyBufferFrameSize` write per device at
    /// route start.
    @State private var bufferFrames: UInt32 = 0
    @State private var errorMessage: String?

    /// Standard buffer-size choices the picker offers when the user
    /// selects Performance.
    private static let kBufferSizeChoices: [UInt32] = [
        16, 32, 64, 128, 256, 512, 1024, 2048]

    // `MappingPair` and the mapping-row view live in
    // ChannelMappingEditor.swift so the Add and Edit sheets can share
    // them. Same for `ChannelMappingValidator`.

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

    private var canSave: Bool { validationIssue == nil }

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
                 + "drift setpoint; for same-device routes (e.g. an "
                 + "aggregate) the engine takes a duplex fast path "
                 + "that bypasses the ring entirely. The Buffer size "
                 + "below is a *preference*: macOS resolves the actual "
                 + "value as the max across all active clients, so "
                 + "another app asking for a bigger buffer (Music, a "
                 + "video call, a DAW with a larger session) will "
                 + "pull the buffer up while it's running."
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
                            Text("No preference (use device's current)").tag(UInt32(0))
                            ForEach(Self.kBufferSizeChoices, id: \.self) { frames in
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
            .navigationTitle("New route")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", action: onClose)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save", action: save)
                        .disabled(!canSave)
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
        .frame(minWidth: 520, minHeight: 460)
        .onAppear(perform: preselectDefaults)
    }

    // MARK: Actions

    private func preselectDefaults() {
        if sourceUID.isEmpty, let first = inputDevices.first {
            sourceUID = first.uid
        }
        if destUID.isEmpty, let first = outputDevices.first {
            destUID = first.uid
        }
    }

    private func save() {
        guard let src = srcDevice, let dst = dstDevice else { return }
        let mapping = MappingPair.toEdges(pairs)
        let cfg = RouteConfig(
            source: DeviceReference(device: src),
            destination: DeviceReference(device: dst),
            mapping: mapping,
            name: customName.trimmingCharacters(in: .whitespaces).isEmpty ? nil : customName,
            latencyMode: latencyMode,
            bufferFrames: bufferFrames == 0 ? nil : bufferFrames
        )
        do {
            _ = try store.addRoute(cfg)
            onClose()
        } catch let e as JboxError {
            errorMessage = e.description
        } catch {
            errorMessage = String(describing: error)
        }
    }
}
