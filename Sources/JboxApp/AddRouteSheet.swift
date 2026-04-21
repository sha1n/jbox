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
    @State private var errorMessage: String?

    /// Standard buffer-size options offered to the user. The menu
    /// filters down to the subset the selected device supports.
    private static let kBufferSizeChoices: [UInt32] = [
        32, 64, 128, 256, 512, 1024, 2048]

    /// Row in the mapping editor. `src` and `dst` are 0-indexed
    /// internally; the UI renders them as 1-indexed per spec § 2.3.
    private struct MappingPair: Identifiable, Equatable {
        let id = UUID()
        var src: Int
        var dst: Int
    }

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
        if pairs.isEmpty     { return "Add at least one channel pair." }
        var seenDst = Set<Int>()
        for (i, p) in pairs.enumerated() {
            if p.src < 0 || p.src >= srcChannels {
                return "Row \(i + 1): source channel out of range."
            }
            if p.dst < 0 || p.dst >= dstChannels {
                return "Row \(i + 1): destination channel out of range."
            }
            // Duplicate src is fine (fan-out: one source feeding many
            // destinations). Duplicate dst is still rejected —
            // summing / fan-in is deferred per spec.md Appendix A.
            if !seenDst.insert(p.dst).inserted {
                return "Row \(i + 1): destination channel \(p.dst + 1) is already in use."
            }
        }
        return nil
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

                Section("Channel mapping") {
                    ForEach(pairs.indices, id: \.self) { idx in
                        mappingRow(idx: idx)
                    }
                    Button {
                        pairs.append(MappingPair(src: 0, dst: 0))
                    } label: {
                        Label("Add pair", systemImage: "plus")
                    }
                }

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

    @ViewBuilder
    private func mappingRow(idx: Int) -> some View {
        HStack(spacing: 12) {
            Picker("Source", selection: $pairs[idx].src) {
                ForEach(0..<max(srcChannels, 0), id: \.self) { ch in
                    Text(ChannelLabel.format(index: ch, names: srcChannelNames)).tag(ch)
                }
            }
            .labelsHidden()
            .disabled(srcChannels == 0)
            .frame(maxWidth: .infinity, alignment: .leading)

            Image(systemName: "arrow.right")
                .foregroundStyle(.secondary)

            Picker("Destination", selection: $pairs[idx].dst) {
                ForEach(0..<max(dstChannels, 0), id: \.self) { ch in
                    Text(ChannelLabel.format(index: ch, names: dstChannelNames)).tag(ch)
                }
            }
            .labelsHidden()
            .disabled(dstChannels == 0)
            .frame(maxWidth: .infinity, alignment: .leading)

            Button(role: .destructive) {
                pairs.remove(at: idx)
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .disabled(pairs.count <= 1)
            .help("Remove this channel pair")
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
    }

    private func save() {
        guard let src = srcDevice, let dst = dstDevice else { return }
        let mapping = pairs.map {
            ChannelEdge(src: UInt32($0.src), dst: UInt32($0.dst))
        }
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
