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
    /// 0 == "use the tier default" (currently 64 for Performance).
    @State private var bufferFrames: UInt32 = 0
    /// Phase 7.5 per-route opt-out. Defaults to the global preference;
    /// the user can flip per-route before saving.
    @State private var shareDevices: Bool = false
    @State private var errorMessage: String?

    /// Buffer-size policy preference (Preferences → Audio). Seeds the
    /// Performance-mode picker the first time the user switches into
    /// that tier; they can still override it per-route before saving.
    /// 0 == "use each device's current setting"; non-zero is an
    /// explicit override in frames. Matches
    /// `BufferSizePolicy.storedRaw` in `JboxEngineSwift`.
    @AppStorage(JboxPreferences.bufferSizePolicyKey) private var bufferPolicyRaw: Int = 0
    /// Phase 7.5 global default for the share-device toggle. Seeds
    /// `shareDevices` when the sheet opens; per-route edits don't
    /// write back to this key.
    @AppStorage(JboxPreferences.shareDevicesByDefaultKey) private var shareDevicesByDefault: Bool = false

    /// Standard buffer-size options offered to the user. The menu
    /// filters down to the subset the selected device supports.
    private static let kBufferSizeChoices: [UInt32] = [
        32, 64, 128, 256, 512, 1024, 2048]

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

    /// Buffer-size choices the Performance-mode picker offers —
    /// intersection of our standard choices with the source
    /// device's HAL-reported range (and the destination's, if they
    /// differ). Returns the full list when the device does not
    /// expose a range.
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
                        Text(shareDevices ? "Performance — unavailable when sharing" : "Performance")
                            .tag(LatencyMode.performance)
                    }
                    .pickerStyle(.menu)
                    .disabled(false)  // keep the picker enabled; just snap below.

                    Toggle("Share device with other apps", isOn: $shareDevices)
                    if shareDevices {
                        Text("Performance requires exclusive device access and is unavailable while sharing.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    if latencyMode == .performance && !shareDevices {
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
        .onChange(of: latencyMode) { _, newMode in
            seedBufferFromPolicyIfNeeded(latencyMode: newMode)
        }
        .onChange(of: shareDevices) { _, sharing in
            // Snap back when the user toggles sharing on with
            // Performance selected. Unchecking sharing doesn't
            // auto-restore Performance — that was a deliberate choice.
            if sharing && latencyMode == .performance {
                latencyMode = .low
            }
        }
    }

    // MARK: Actions

    private func preselectDefaults() {
        if sourceUID.isEmpty, let first = inputDevices.first {
            sourceUID = first.uid
        }
        if destUID.isEmpty, let first = outputDevices.first {
            destUID = first.uid
        }
        // Seed the share toggle from the global default on first paint.
        shareDevices = shareDevicesByDefault
        seedBufferFromPolicyIfNeeded(latencyMode: latencyMode)
    }

    /// If the user switches into Performance mode (or opens the sheet
    /// already on Performance) and hasn't picked a buffer size, seed
    /// `bufferFrames` from the global policy. We only touch an
    /// untouched `0` so the user's explicit override is never
    /// overwritten. Policy values that the currently-selected devices
    /// can't honour fall back to 0 (tier default) instead of forcing
    /// the HAL to clamp.
    private func seedBufferFromPolicyIfNeeded(latencyMode: LatencyMode) {
        guard latencyMode == .performance, bufferFrames == 0 else { return }
        let raw = UInt32(max(0, bufferPolicyRaw))
        guard raw != 0, bufferSizeOptions.contains(raw) else { return }
        bufferFrames = raw
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
            bufferFrames: bufferFrames == 0 ? nil : bufferFrames,
            shareDevices: shareDevices
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
