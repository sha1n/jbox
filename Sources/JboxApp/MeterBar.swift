import SwiftUI
import JboxEngineSwift

/// Expanded-route meter panel. Renders two outlined sections side by
/// side: SOURCE (input bar meters on the left) and DESTINATION
/// (per-channel strips + VCA pinned on the far right). Section widths
/// are partitioned by `MixerPanelLayout.sectionWidths` so the two
/// sections share an identical per-channel-strip area and the
/// destination is wider only by exactly the VCA-strip slot. Spec § 4.5.
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
    private var isCompact: Bool {
        MixerPanelLayout.isCompact(channelCount: channelCount)
    }
    private var panelHeight: CGFloat {
        MixerPanelLayout.panelHeight(isCompact: isCompact)
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: false)) { timeline in
            let now = timeline.date.timeIntervalSinceReferenceDate
            VStack(alignment: .leading, spacing: 16) {
                GeometryReader { geo in
                    let widths = MixerPanelLayout.sectionWidths(
                        panelInnerWidth: geo.size.width,
                        isCompact: isCompact)

                    HStack(alignment: .top,
                           spacing: MixerPanelLayout.sectionSpacing) {
                        SourceSection(
                            routeId: route.id,
                            peaks: peaks.source,
                            store: store,
                            now: now,
                            isCompact: isCompact
                        )
                        .frame(width: widths.source, alignment: .leading)
                        .frame(maxHeight: .infinity)

                        DestinationSection(
                            route: route,
                            store: store,
                            peaks: peaks.destination,
                            now: now,
                            isCompact: isCompact
                        )
                        .frame(width: widths.destination, alignment: .leading)
                        .frame(maxHeight: .infinity)
                    }
                }
                .frame(height: panelHeight)
                .padding(.horizontal, 12)

                if showDiagnostics {
                    DiagnosticsBlock(
                        route: route,
                        counters: store.routeCounters[route.id],
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
}

/// Outlined section frame used to wrap the SOURCE and DESTINATION
/// halves of the mixer panel. Renders the section title above the
/// content and a thin rounded border around the whole thing — the
/// "console meter bridge" look. Content fills the remaining space so
/// every section in the panel HStack ends at the same y.
struct MeterSectionFrame<Content: View>: View {
    let title: String
    let content: Content

    init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(spacing: 12) {
            Text(title)
                .font(.system(size: 11, weight: .semibold))
                .tracking(0.6)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .center)
                .padding(.top, 8)
                .padding(.horizontal, 12)
            content
                .frame(maxHeight: .infinity)
                .padding(.horizontal, 10)
                .padding(.bottom, 10)
        }
        .background(
            RoundedRectangle(cornerRadius: 7)
                .fill(Color(red: 0.115, green: 0.115, blue: 0.13))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 7)
                .stroke(Color.secondary.opacity(0.32), lineWidth: 1)
        )
    }
}

/// Anchored dB scale rendered inside a section frame. Used identically
/// in SOURCE and DESTINATION so the scale visually anchors each
/// section's bar zones at the same vertical extent. Padded by
/// `MixerPanelLayout.barZoneTopOffset` / `barZoneBottomOffset` so its
/// canvas spans exactly the same y-range as the bar zone in
/// `MixerStripColumn` — "0" lines up with 0 dBFS at the top of the
/// bar, "-60" with −60 dBFS at the bottom, every intermediate tick at
/// the matching fill height. Issue #12.
private struct SectionScale: View {
    var body: some View {
        // The Canvas grows by `dbScaleLabelOverflow` above and below
        // the bar zone so the "0" / "-60" labels have room to sit
        // outside the bar-zone subrange — `DbScale` maps tick lines
        // back to the bar-zone edges via `MeterLevel.dbScaleTickY`,
        // so alignment with the adjacent strip column is unchanged.
        DbScale()
            .frame(width: MixerPanelLayout.scaleColumnWidth)
            .padding(.top, MixerPanelLayout.barZoneTopOffset
                     - MixerPanelLayout.dbScaleLabelOverflow)
            .padding(.bottom, MixerPanelLayout.barZoneBottomOffset
                     - MixerPanelLayout.dbScaleLabelOverflow)
    }
}

/// Horizontally-scrolling row of `MixerStripColumn` widgets. Used by
/// both sections so the scroll behavior, indicator visibility, and
/// trailing padding are defined once.
private struct ScrollableStripRow<Content: View>: View {
    let isCompact: Bool
    let content: Content

    init(isCompact: Bool, @ViewBuilder content: () -> Content) {
        self.isCompact = isCompact
        self.content = content()
    }

    var body: some View {
        ScrollView(.horizontal, showsIndicators: true) {
            HStack(alignment: .top,
                   spacing: MixerPanelLayout.stripSpacing(isCompact: isCompact)) {
                content
            }
            .padding(.trailing, 2)
        }
    }
}

/// Left half of the meter panel — pre-fader source bars rendered in the
/// same `MixerStripColumn` widget as the destination strips (style
/// `.sourceMeter`), so source and destination match colors / borders /
/// title position / meter size / bar-zone height by construction.
/// Section-local dB scale on the left mirrors the destination layout.
struct SourceSection: View {
    let routeId: UInt32
    let peaks: [Float]
    let store: EngineStore
    let now: TimeInterval
    let isCompact: Bool

    private var stripCount: Int { max(1, peaks.count) }

    private func peakAt(_ i: Int) -> Float {
        guard i < peaks.count else { return 0 }
        return peaks[i]
    }

    var body: some View {
        MeterSectionFrame(title: "SOURCE") {
            HStack(alignment: .top,
                   spacing: MixerPanelLayout.stripSpacing(isCompact: isCompact)) {
                SectionScale()

                // Source meter strips — scroll horizontally when they
                // overflow the section width. The dB scale stays
                // anchored on the left, mirroring how the destination
                // section pins its scale + VCA at the edges.
                ScrollableStripRow(isCompact: isCompact) {
                    ForEach(0..<stripCount, id: \.self) { i in
                        MixerStripColumn(
                            title: "\(i + 1)",
                            tooltip: nil,
                            trimDb: nil,
                            muted: nil,
                            peak: peakAt(i),
                            hold: store.heldPeak(routeId: routeId,
                                                 side: .source,
                                                 channel: i,
                                                 now: now),
                            isCompact: isCompact,
                            style: .sourceMeter
                        )
                        .frame(maxHeight: .infinity)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Source meters")
        }
    }
}

/// Right half of the meter panel — per-channel strips plus the VCA on
/// the far right. The strips scroll horizontally when the channel count
/// exceeds the section's available width; the VCA strip and the
/// section's own dB scale stay anchored at the edges. Owns its
/// route-derived bindings (trim, channel mute, VCA, route mute) and
/// strip labels — the parent `MeterPanel` only hands over `route` +
/// `store` and the meter snapshot.
struct DestinationSection: View {
    let route: Route
    let store: EngineStore
    let peaks: [Float]
    let now: TimeInterval
    let isCompact: Bool

    private var channelCount: Int { max(1, route.config.mapping.count) }

    private func peakAt(_ i: Int) -> Float {
        guard i < peaks.count else { return 0 }
        return peaks[i]
    }

    // MARK: - Channel labels (Spec § 4.7)

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

    // MARK: - Route-derived bindings

    /// Binding for the VCA fader. Reads / writes `Route.masterGainDb` —
    /// the engine ABI keeps the historical "master_gain_db" name even
    /// though the UI calls the control "VCA".
    private func vcaBinding() -> Binding<Float> {
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

    private func trimBinding(channelIndex i: Int) -> Binding<Float> {
        Binding<Float>(
            get: {
                let r = store.routes.first(where: { $0.id == route.id })
                let trims = r?.trimDbs ?? []
                return i < trims.count ? trims[i] : 0
            },
            set: {
                store.setChannelTrimDb(routeId: route.id,
                                       channelIndex: i, db: $0)
            }
        )
    }

    /// Per-channel mute. Reads / writes `Route.channelMuted[i]` —
    /// toggling it doesn't move the trim fader; the engine just sees
    /// `−∞` for the muted channel until un-mute restores the trim.
    private func channelMutedBinding(channelIndex i: Int) -> Binding<Bool> {
        Binding<Bool>(
            get: {
                let r = store.routes.first(where: { $0.id == route.id })
                let mutes = r?.channelMuted ?? []
                return i < mutes.count ? mutes[i] : false
            },
            set: {
                store.setChannelMuted(routeId: route.id,
                                      channelIndex: i, muted: $0)
            }
        )
    }

    var body: some View {
        MeterSectionFrame(title: "DESTINATION") {
            HStack(alignment: .top,
                   spacing: MixerPanelLayout.stripSpacing(isCompact: isCompact)) {
                SectionScale()

                // Per-channel strips — scroll horizontally when they
                // overflow. For the default 2-channel route the strips
                // fit naturally and no scroll bar is shown.
                ScrollableStripRow(isCompact: isCompact) {
                    ForEach(0..<channelCount, id: \.self) { i in
                        MixerStripColumn(
                            title: stripPrimaryLabel(i),
                            tooltip: stripTooltip(i),
                            trimDb: trimBinding(channelIndex: i),
                            muted: channelMutedBinding(channelIndex: i),
                            peak: peakAt(i),
                            hold: store.heldPeak(routeId: route.id,
                                                 side: .destination,
                                                 channel: i,
                                                 now: now),
                            isCompact: isCompact,
                            style: .channel
                        )
                        .frame(maxHeight: .infinity)
                    }
                }
                .frame(maxHeight: .infinity)

                // VCA strip on the far right — stays put, doesn't scroll.
                MixerStripColumn(
                    title: "VCA",
                    tooltip: nil,
                    trimDb: vcaBinding(),
                    muted: muteBinding(),
                    peak: nil,
                    hold: nil,
                    isCompact: isCompact,
                    style: .vca
                )
                .frame(maxHeight: .infinity)
            }
            .accessibilityElement(children: .contain)
            .accessibilityLabel("Destination strips")
        }
    }
}

/// Shared dB gridline / label strip drawn once per side, next to the
/// bar cluster.
struct DbScale: View {
    private static let marks = MeterLevel.dawScaleMarks

    var body: some View {
        Canvas { ctx, size in
            let overflow = MixerPanelLayout.dbScaleLabelOverflow
            for mark in Self.marks {
                let y = MeterLevel.dbScaleTickY(
                    forDb: mark.dB,
                    canvasHeight: size.height,
                    labelOverflow: overflow)
                var line = Path()
                line.move(to: CGPoint(x: size.width - 2, y: y))
                line.addLine(to: CGPoint(x: size.width, y: y))
                ctx.stroke(line,
                           with: .color(.secondary.opacity(0.5)),
                           lineWidth: 0.5)
                let text = Text(mark.label)
                    .font(.system(size: 8).monospaced())
                    .foregroundColor(.secondary)
                let resolved = ctx.resolve(text)
                // Right-anchor every label at the same x so wider
                // labels ("-12") don't drift left of narrower ones
                // ("0"). The Canvas's vertical headroom keeps the
                // top/bottom labels fully visible even though their
                // text rect is centered on a tick that sits at the
                // bar-zone edge.
                ctx.draw(resolved,
                         at: CGPoint(x: size.width - 4, y: y),
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
/// cached in the store — the collapsed row stays uncluttered. Counters
/// come from `EngineStore.routeCounters` rather than `route.status`
/// because the in-array status carries stale counter values by design
/// (see `RouteCounters` for the drag-cancellation reasoning).
struct DiagnosticsBlock: View {
    let route: Route
    let counters: RouteCounters?
    let components: LatencyComponents?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Divider()

            HStack(alignment: .top, spacing: 24) {
                CountersColumn(counters: counters ?? .zero)
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
    let counters: RouteCounters

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("COUNTERS")
                .font(.caption.weight(.semibold))
                .tracking(0.6)
                .foregroundStyle(.secondary)
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 2) {
                GridRow {
                    Text("Produced").foregroundStyle(.secondary)
                    Text("\(counters.framesProduced)").monospacedDigit()
                }
                GridRow {
                    Text("Consumed").foregroundStyle(.secondary)
                    Text("\(counters.framesConsumed)").monospacedDigit()
                }
                GridRow {
                    Text("Underruns").foregroundStyle(.secondary)
                    Text("\(counters.underrunCount)").monospacedDigit()
                }
                GridRow {
                    Text("Overruns").foregroundStyle(.secondary)
                    Text("\(counters.overrunCount)").monospacedDigit()
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
