import SwiftUI
import JboxEngineSwift

/// Modal sheet presented from the main window's "+" toolbar button.
/// Picks source and destination devices, edits a channel mapping as
/// a list of 1-indexed pairs (v1 rules: non-empty, no duplicate src,
/// no duplicate dst), and submits to `store.addRoute`. Engine-side
/// validation errors are surfaced inline.
struct AddRouteSheet: View {
    let store: EngineStore
    let onClose: () -> Void

    @State private var sourceUID: String = ""
    @State private var destUID: String = ""
    @State private var pairs: [MappingPair] = [MappingPair(src: 0, dst: 0)]
    @State private var customName: String = ""
    @State private var lowLatency: Bool = false
    @State private var errorMessage: String?

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
        var seenSrc = Set<Int>()
        var seenDst = Set<Int>()
        for (i, p) in pairs.enumerated() {
            if p.src < 0 || p.src >= srcChannels {
                return "Row \(i + 1): source channel out of range."
            }
            if p.dst < 0 || p.dst >= dstChannels {
                return "Row \(i + 1): destination channel out of range."
            }
            if !seenSrc.insert(p.src).inserted {
                return "Row \(i + 1): source channel \(p.src + 1) is already used."
            }
            if !seenDst.insert(p.dst).inserted {
                return "Row \(i + 1): destination channel \(p.dst + 1) is already used."
            }
        }
        return nil
    }

    private var canSave: Bool { validationIssue == nil }

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
                    Toggle("Low latency", isOn: $lowLatency)
                } footer: {
                    Text("Uses a tighter ring buffer to reduce latency. "
                         + "Some USB interfaces that deliver samples in "
                         + "bursts may underrun — turn this off if you "
                         + "hear clicks.")
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
            lowLatency: lowLatency
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
