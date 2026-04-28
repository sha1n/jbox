import SwiftUI
import JboxEngineSwift

/// Expanded-route meter panel. Renders the source-side bar group, a
/// direction arrow, and the destination-side bar group, with each
/// group growing to fill available row width. Spec § 4.5.
///
/// A local `TimelineView` redraws at ~30 Hz so peak-hold ticks decay
/// smoothly between the store's `pollMeters()` passes (which run at
/// the same cadence but on a separate clock).
struct MeterPanel: View {
    let route: Route
    let store: EngineStore
    let peaks: MeterPeaks

    @AppStorage(JboxPreferences.showDiagnosticsKey)
    private var showDiagnostics = false

    private var channelCount: Int { max(1, route.config.mapping.count) }
    private var isCompact: Bool   { channelCount >= 6 }
    /// Total height of the mixer-strip row. Every column expands to this
    /// height; bar zones inside each column take whatever is left after
    /// the fixed-height top/bottom bands. This is the "everything same
    /// height, use most of the panel height, just keep some padding"
    /// rule.
    private var panelHeight: CGFloat { isCompact ? 240 : 280 }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: false)) { timeline in
            let now = timeline.date.timeIntervalSinceReferenceDate
            VStack(alignment: .leading, spacing: 16) {
                // Every column in the row uses the same band layout —
                // header, top cap, bar zone (flex), bottom cap, readout,
                // optional action — so bar zones across SOURCE, the dB
                // scale, the channel strips, and the master strip all
                // anchor at the same y AND share the same bar-zone
                // height.
                HStack(alignment: .top, spacing: isCompact ? 6 : 10) {

                    // SOURCE column — pre-fader bars in the bar zone.
                    SourceColumn(
                        routeId: route.id,
                        peaks: peaks.source,
                        labels: sourceLabels,
                        store: store,
                        now: now,
                        isCompact: isCompact
                    )
                    .frame(width: sourceColumnWidth)
                    .frame(maxHeight: .infinity)

                    // dB scale column.
                    ScaleColumn()
                        .frame(width: 36)
                        .frame(maxHeight: .infinity)

                    // Per-channel strips.
                    ForEach(0..<channelCount, id: \.self) { i in
                        ChannelStripColumn(
                            routeId: route.id,
                            channelIndex: i,
                            primaryLabel: stripPrimaryLabel(i),
                            tooltipLabel: stripTooltip(i),
                            peak: peakAt(i),
                            hold: store.heldPeak(routeId: route.id,
                                                 side: .destination,
                                                 channel: i,
                                                 now: now),
                            trimDb: trimBinding(channelIndex: i),
                            isCompact: isCompact
                        )
                        .frame(maxHeight: .infinity)
                    }

                    // Master strip on the far right.
                    MasterFaderStrip(
                        masterDb: masterBinding(),
                        muted: muteBinding()
                    )
                    .frame(maxHeight: .infinity)
                    .padding(.leading, 4)
                }
                .frame(height: panelHeight)
                .padding(.horizontal, 12)

                if showDiagnostics {
                    DiagnosticsBlock(
                        route: route,
                        components: store.latencyComponents[route.id])
                }
            }
            .padding(.top, 20)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(MeterAccessibilityLabel.summary(
            source: peaks.source,
            destination: peaks.destination))
    }

    // MARK: - Channel labels (Task 16 § 4.7)

    private var sourceNames: [String] {
        store.channelNames(uid: route.config.source.uid, direction: .input)
    }
    private var destNames: [String] {
        store.channelNames(uid: route.config.destination.uid, direction: .output)
    }

    private func stripPrimaryLabel(_ i: Int) -> String {
        if isCompact { return "\(i + 1)" }
        let srcCh = Int(route.config.mapping[i].src)
        return ChannelLabel.format(index: srcCh, names: sourceNames)
    }

    private func stripTooltip(_ i: Int) -> String {
        let srcCh = Int(route.config.mapping[i].src)
        let dstCh = Int(route.config.mapping[i].dst)
        let src = ChannelLabel.format(index: srcCh, names: sourceNames)
        let dst = ChannelLabel.format(index: dstCh, names: destNames)
        return "\(src) → \(dst)"
    }

    /// SOURCE column width scales with channel count so each bar gets a
    /// reasonable footprint (~16-20 px per bar in non-compact, ~10 px in
    /// compact).
    private var sourceColumnWidth: CGFloat {
        let perBar: CGFloat = isCompact ? 12 : 18
        let extra: CGFloat = 24       // padding + label header
        return min(220, max(80, CGFloat(channelCount) * perBar + extra))
    }

    private var sourceLabels: [String] {
        (1...channelCount).map { "\($0)" }
    }

    private func peakAt(_ i: Int) -> Float {
        guard i < peaks.destination.count else { return 0 }
        return peaks.destination[i]
    }

    // MARK: - Bindings into the store's Route model

    private func masterBinding() -> Binding<Float> {
        Binding<Float>(
            get: { store.routes.first(where: { $0.id == route.id })?.masterGainDb ?? 0 },
            set: { store.setMasterGainDb(routeId: route.id, db: $0) }
        )
    }

    private func muteBinding() -> Binding<Bool> {
        Binding<Bool>(
            get: { store.routes.first(where: { $0.id == route.id })?.muted ?? false },
            set: { store.setRouteMuted(routeId: route.id, muted: $0) }
        )
    }

    private func trimBinding(channelIndex: Int) -> Binding<Float> {
        Binding<Float>(
            get: {
                let r = store.routes.first(where: { $0.id == route.id })
                let trims = r?.trimDbs ?? []
                return channelIndex < trims.count ? trims[channelIndex] : 0
            },
            set: {
                store.setChannelTrimDb(
                    routeId: route.id,
                    channelIndex: channelIndex,
                    db: $0)
            }
        )
    }
}

/// SOURCE column for the mixer-strip layout — pre-fader bars in the
/// bar zone, using the same band structure as `ChannelStripColumn` and
/// `MasterFaderStrip` so its bar zone aligns with theirs in the
/// MeterPanel HStack.
struct SourceColumn: View {
    let routeId: UInt32
    let peaks: [Float]
    let labels: [String]
    let store: EngineStore
    let now: TimeInterval
    let isCompact: Bool

    var body: some View {
        VStack(spacing: 4) {
            // Header band.
            Text("SOURCE")
                .font(.system(size: isCompact ? 9 : 10, weight: .bold))
                .tracking(0.4)
                .foregroundStyle(.secondary)
                .frame(height: MeterPanelLayout.headerBandHeight)
            // Top cap band.
            Text("0")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // Bar zone — flex height; per-channel bars row.
            HStack(alignment: .bottom, spacing: isCompact ? 3 : 5) {
                ForEach(Array(peaks.enumerated()), id: \.offset) { i, v in
                    let hold = store.heldPeak(routeId: routeId,
                                              side: .source,
                                              channel: i,
                                              now: now)
                    ChannelBar(peak: v, hold: hold)
                        .frame(width: isCompact ? 10 : 14)
                }
            }
            .frame(maxHeight: .infinity)
            // Bottom cap band.
            Text("−60")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // Readout band — channel-number labels (no dB readout for the
            // source side; the fader sits on the dest side).
            HStack(spacing: isCompact ? 3 : 5) {
                ForEach(Array(labels.enumerated()), id: \.offset) { _, label in
                    Text(label)
                        .font(.system(size: 9, weight: .medium).monospacedDigit())
                        .frame(width: isCompact ? 10 : 14)
                        .foregroundStyle(.secondary)
                }
            }
            .frame(height: MeterPanelLayout.readoutBandHeight)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(Color(red: 0.13, green: 0.13, blue: 0.15))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 5)
                .stroke(Color.secondary.opacity(0.22), lineWidth: 1)
        )
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Source meters")
    }
}

/// Shared dBFS scale column. Empty header / cap / readout bands keep its
/// bar zone aligned with the strip columns; the only visible content is
/// the `DbScale` canvas itself.
struct ScaleColumn: View {
    var body: some View {
        VStack(spacing: 4) {
            Color.clear.frame(height: MeterPanelLayout.headerBandHeight)
            Color.clear.frame(height: MeterPanelLayout.capBandHeight)
            DbScale()
                .frame(maxHeight: .infinity)
            Color.clear.frame(height: MeterPanelLayout.capBandHeight)
            Color.clear.frame(height: MeterPanelLayout.readoutBandHeight)
        }
        .padding(.vertical, 6)
        .accessibilityHidden(true)
    }
}

/// Legacy bar group, retained for any out-of-tree consumers; the
/// new mixer-strip layout uses `SourceColumn` instead. Bars use
/// `maxWidth: .infinity` inside a capped container so small channel
/// counts spread to comfortable widths while large channel counts still
/// fit without pushing the row.
struct BarGroup: View {
    let title: String
    let labels: [String]
    let peaks: [Float]
    let routeId: UInt32
    let side: Engine.MeterSide
    let store: EngineStore
    let now: TimeInterval
    let barHeight: CGFloat

    /// Individual bars never grow wider than this. Past ~10 channels
    /// the group starts shrinking bars from `maxBarWidth` toward
    /// `minBarWidth` while keeping the group centered.
    private let maxBarWidth: CGFloat = 36
    private let minBarWidth: CGFloat = 10
    private let barSpacing: CGFloat  = 8
    private let scaleWidth: CGFloat  = 36

    var body: some View {
        VStack(spacing: 10) {
            Text(title)
                .font(.caption.weight(.semibold))
                .tracking(0.6)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity)

            HStack(spacing: 0) {
                Spacer(minLength: 0)
                VStack(spacing: 6) {
                    HStack(alignment: .bottom, spacing: 0) {
                        DbScale()
                            .frame(width: scaleWidth, height: barHeight)

                        HStack(alignment: .bottom, spacing: barSpacing) {
                            ForEach(Array(peaks.enumerated()), id: \.offset) { i, v in
                                let hold = store.heldPeak(routeId: routeId,
                                                          side: side,
                                                          channel: i,
                                                          now: now)
                                ChannelBar(peak: v, hold: hold)
                                    .frame(minWidth: minBarWidth,
                                           maxWidth: maxBarWidth,
                                           minHeight: barHeight,
                                           maxHeight: barHeight)
                            }
                        }

                        // Phantom column mirroring the scale so the bars
                        // (not the scale+bars block) are what gets
                        // horizontally centered inside the card.
                        Spacer().frame(width: scaleWidth)
                    }

                    HStack(spacing: 0) {
                        Spacer().frame(width: scaleWidth)
                        HStack(spacing: barSpacing) {
                            ForEach(Array(labels.enumerated()), id: \.offset) { _, label in
                                Text(label)
                                    .font(.system(size: 10, weight: .medium).monospaced())
                                    .foregroundStyle(.secondary)
                                    .frame(minWidth: minBarWidth,
                                           maxWidth: maxBarWidth)
                            }
                        }
                        Spacer().frame(width: scaleWidth)
                    }
                }
                Spacer(minLength: 0)
            }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(Color.secondary.opacity(0.05))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(Color.secondary.opacity(0.22), lineWidth: 1)
        )
    }
}

/// Shared dB gridline / label strip drawn once per side, next to the
/// bar cluster.
struct DbScale: View {
    private static let marks = MeterLevel.dawScaleMarks

    var body: some View {
        Canvas { ctx, size in
            for mark in Self.marks {
                let dB = mark.dB
                let label = mark.label
                let frac = MeterLevel.fractionFor(dB: dB)
                let y = size.height * (1 - CGFloat(frac))
                var line = Path()
                line.move(to: CGPoint(x: size.width - 2, y: y))
                line.addLine(to: CGPoint(x: size.width, y: y))
                ctx.stroke(line,
                           with: .color(.secondary.opacity(0.5)),
                           lineWidth: 0.5)
                let text = Text(label)
                    .font(.system(size: 8).monospaced())
                    .foregroundColor(.secondary)
                let resolved = ctx.resolve(text)
                let tSize = resolved.measure(in: size)
                ctx.draw(resolved,
                         at: CGPoint(x: size.width - 4 - tSize.width / 2, y: y),
                         anchor: .trailing)
            }
        }
    }
}

/// Single channel bar. Frame + zone-colored fill + peak-hold tick.
struct ChannelBar: View {
    let peak: Float
    let hold: Float

    var body: some View {
        Canvas { ctx, size in
            let frame = CGRect(origin: .zero, size: size)
                .insetBy(dx: 0.5, dy: 0.5)
            ctx.stroke(Path(frame),
                       with: .color(.secondary.opacity(0.4)),
                       lineWidth: 1)

            let frac = MeterLevel.fractionFor(peak: peak)
            let fillH = size.height * CGFloat(frac)
            let fillRect = CGRect(x: 1,
                                  y: size.height - fillH,
                                  width: size.width - 2,
                                  height: fillH)
            ctx.fill(Path(fillRect),
                     with: .color(color(for: MeterLevel.zone(for: peak))))

            if hold > 0 {
                let holdFrac = MeterLevel.fractionFor(peak: hold)
                let y = size.height * (1 - CGFloat(holdFrac))
                var tick = Path()
                tick.move(to: CGPoint(x: 0, y: y))
                tick.addLine(to: CGPoint(x: size.width, y: y))
                ctx.stroke(tick,
                           with: .color(.white.opacity(0.85)),
                           lineWidth: 1.2)
            }
        }
    }

    private func color(for zone: MeterLevel.Zone) -> Color {
        switch zone {
        case .silent: return .secondary.opacity(0.3)
        case .normal: return .green
        case .near:   return .yellow
        case .clip:   return .red
        }
    }
}

/// Advanced-only block shown at the bottom of the expanded meter panel
/// when the "Show engine diagnostics" preference is on. Surfaces the
/// engine-internal counters and the per-component latency breakdown
/// cached in the store — the collapsed row stays uncluttered.
struct DiagnosticsBlock: View {
    let route: Route
    let components: LatencyComponents?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Divider()

            HStack(alignment: .top, spacing: 24) {
                CountersColumn(status: route.status)
                if let components {
                    LatencyBreakdown(components: components)
                } else {
                    Text("Latency breakdown unavailable")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                Spacer(minLength: 0)
            }
        }
    }
}

private struct CountersColumn: View {
    let status: RouteStatus

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("COUNTERS")
                .font(.caption.weight(.semibold))
                .tracking(0.6)
                .foregroundStyle(.secondary)
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 2) {
                GridRow {
                    Text("Produced").foregroundStyle(.secondary)
                    Text("\(status.framesProduced)").monospacedDigit()
                }
                GridRow {
                    Text("Consumed").foregroundStyle(.secondary)
                    Text("\(status.framesConsumed)").monospacedDigit()
                }
                GridRow {
                    Text("Underruns").foregroundStyle(.secondary)
                    Text("\(status.underrunCount)").monospacedDigit()
                }
                GridRow {
                    Text("Overruns").foregroundStyle(.secondary)
                    Text("\(status.overrunCount)").monospacedDigit()
                }
            }
            .font(.caption2)
        }
    }
}

private struct LatencyBreakdown: View {
    let components: LatencyComponents

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("LATENCY BREAKDOWN")
                .font(.caption.weight(.semibold))
                .tracking(0.6)
                .foregroundStyle(.secondary)
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 2) {
                row("Src HAL",       frames: components.sourceHalLatencyFrames,
                    rate: components.sourceSampleRateHz)
                row("Src safety",    frames: components.sourceSafetyOffsetFrames,
                    rate: components.sourceSampleRateHz)
                row("Src buffer",    frames: components.sourceBufferFrames,
                    rate: components.sourceSampleRateHz)
                row("Ring setpoint", frames: components.ringTargetFillFrames,
                    rate: components.sourceSampleRateHz)
                row("SRC prime",     frames: components.converterPrimeFrames,
                    rate: components.destSampleRateHz)
                row("Dst buffer",    frames: components.destBufferFrames,
                    rate: components.destSampleRateHz)
                row("Dst safety",    frames: components.destSafetyOffsetFrames,
                    rate: components.destSampleRateHz)
                row("Dst HAL",       frames: components.destHalLatencyFrames,
                    rate: components.destSampleRateHz)
                Divider().gridCellColumns(3)
                GridRow {
                    Text("Total")
                        .font(.caption2.weight(.semibold))
                    Text("")
                    Text(LatencyFormatter.pillText(
                            microseconds: components.totalUs) ?? "—")
                        .font(.caption2.monospacedDigit().weight(.semibold))
                }
            }
            .font(.caption2)
        }
    }

    @ViewBuilder
    private func row(_ label: String,
                     frames: UInt32,
                     rate: Double) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text("\(frames) fr")
                .monospacedDigit()
                .foregroundStyle(.secondary)
            Text(LatencyFormatter.breakdownLabel(frames: frames, rate: rate))
                .monospacedDigit()
        }
    }
}

// MARK: - Previews

#if DEBUG
@MainActor
private func meterPanelPreview(channels: Int) -> some View {
    let store = PreviewFixtures.runningStore(channels: channels)
    let route = store.routes[0]
    return MeterPanel(route: route, store: store, peaks: store.meters[route.id]!)
        .padding()
        .frame(width: 720)
}

#Preview("MeterPanel — 2 channels") {
    meterPanelPreview(channels: 2)
}

#Preview("MeterPanel — 4 channels (default tier)") {
    meterPanelPreview(channels: 4)
}

#Preview("MeterPanel — 8 channels (compact tier)") {
    meterPanelPreview(channels: 8)
}

#Preview("ChannelBar — colour zones") {
    HStack(alignment: .bottom, spacing: 12) {
        ForEach([0.0, 0.05, 0.3, 0.7, 0.95, 1.5], id: \.self) { (peak: Double) in
            VStack {
                ChannelBar(peak: Float(peak), hold: Float(peak))
                    .frame(width: 28, height: 200)
                Text(String(format: "%.2f", peak))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }
    .padding()
}
#endif
