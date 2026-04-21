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

    private let barHeight: CGFloat = 200

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: false)) { timeline in
            let now = timeline.date.timeIntervalSinceReferenceDate
            HStack(alignment: .top, spacing: 16) {
                BarGroup(
                    title: "SOURCE",
                    labels: sourceLabels,
                    peaks: peaks.source,
                    routeId: route.id,
                    side: .source,
                    store: store,
                    now: now,
                    barHeight: barHeight
                )
                .frame(maxWidth: .infinity)

                VStack(spacing: 6) {
                    Spacer(minLength: 0)
                    Image(systemName: "arrow.right")
                        .font(.title2.weight(.light))
                        .foregroundStyle(.tertiary)
                    Spacer(minLength: 0)
                }
                .frame(width: 32, height: barHeight)

                BarGroup(
                    title: "DEST",
                    labels: destLabels,
                    peaks: peaks.destination,
                    routeId: route.id,
                    side: .destination,
                    store: store,
                    now: now,
                    barHeight: barHeight
                )
                .frame(maxWidth: .infinity)
            }
            .padding(.top, 4)
        }
    }

    /// Short labels for the source bars — one per mapped channel.
    /// A future enhancement could fold in `store.channelNames(...)`
    /// for hardware that exposes per-channel labels; kept numeric here
    /// so labels stay compact under the 36 pt bar cap.
    private var sourceLabels: [String] {
        (1...max(1, route.config.mapping.count)).map { "\($0)" }
    }

    private var destLabels: [String] {
        (1...max(1, route.config.mapping.count)).map { "\($0)" }
    }
}

/// One side of the meter strip. Bars use `maxWidth: .infinity` inside
/// a capped container so small channel counts spread to comfortable
/// widths while large channel counts still fit without pushing the
/// row.
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
        VStack(alignment: .leading, spacing: 6) {
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
                .frame(maxWidth: CGFloat(peaks.count) * maxBarWidth
                       + CGFloat(max(0, peaks.count - 1)) * barSpacing,
                       alignment: .leading)
            }

            HStack(alignment: .center, spacing: 0) {
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
                .frame(maxWidth: CGFloat(labels.count) * maxBarWidth
                       + CGFloat(max(0, labels.count - 1)) * barSpacing,
                       alignment: .leading)
            }

            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.leading, scaleWidth + 2)
        }
    }
}

/// Shared dB gridline / label strip drawn once per side, next to the
/// bar cluster.
struct DbScale: View {
    private static let marks: [(dB: Float, label: String)] = [
        (0, "0"), (-3, "-3"), (-6, "-6"), (-20, "-20"), (-40, "-40"), (-60, "-60")
    ]

    var body: some View {
        Canvas { ctx, size in
            for (dB, label) in Self.marks {
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
