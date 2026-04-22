import SwiftUI
import JboxEngineSwift

/// One row in the mapping editor. `src` and `dst` are 0-indexed
/// internally; the UI renders them as 1-indexed per spec § 2.3.
/// Shared between `AddRouteSheet` and `EditRouteSheet`.
struct MappingPair: Identifiable, Equatable {
    let id = UUID()
    var src: Int
    var dst: Int

    init(src: Int, dst: Int) {
        self.src = src
        self.dst = dst
    }

    static func from(edges: [ChannelEdge]) -> [MappingPair] {
        edges.map { MappingPair(src: Int($0.src), dst: Int($0.dst)) }
    }

    static func toEdges(_ pairs: [MappingPair]) -> [ChannelEdge] {
        pairs.map { ChannelEdge(src: UInt32($0.src), dst: UInt32($0.dst)) }
    }
}

/// Shared client-side mapping validator. Mirrors the v1 ChannelMapper
/// invariants (docs/spec.md § 3.1): non-empty; no duplicate dst.
/// Duplicate src is allowed — fan-out lands per Phase 6 refinement #1.
enum ChannelMappingValidator {
    static func issue(pairs: [MappingPair],
                      srcChannels: Int,
                      dstChannels: Int) -> String? {
        if pairs.isEmpty { return "Add at least one channel pair." }
        var seenDst = Set<Int>()
        for (i, p) in pairs.enumerated() {
            if p.src < 0 || p.src >= srcChannels {
                return "Row \(i + 1): source channel out of range."
            }
            if p.dst < 0 || p.dst >= dstChannels {
                return "Row \(i + 1): destination channel out of range."
            }
            if !seenDst.insert(p.dst).inserted {
                return "Row \(i + 1): destination channel \(p.dst + 1) is already in use."
            }
        }
        return nil
    }
}

/// Form section showing one editable row per mapping pair, with an
/// "Add pair" button at the bottom. The parent owns `pairs`; the
/// editor only mutates it through the binding.
struct ChannelMappingEditor: View {
    @Binding var pairs: [MappingPair]
    let srcChannels: Int
    let dstChannels: Int
    let srcChannelNames: [String]
    let dstChannelNames: [String]

    var body: some View {
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
}
