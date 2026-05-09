import SwiftUI
import JboxEngineSwift

/// Single column in the mixer-strip layout. One widget for the three
/// kinds of column the panel needs:
///
///   - `.channel`     — destination channel strip (fader + meter + MUTE)
///   - `.vca`         — route-wide VCA strip (fader + MUTE, no meter)
///   - `.sourceMeter` — source-side meter strip (meter only, no fader,
///                      no MUTE) — mirrored so the meter sits in the
///                      slot where `.channel` puts the fader
///
/// Every column uses the same band stack (header / +12 cap / bar zone /
/// −∞ cap / readout / mute) at fixed band heights, so when each
/// instance is given the same outer height (`frame(maxHeight:
/// .infinity)` inside the panel HStack), all bar zones align top +
/// bottom. The 0-dB point on every fader sits at the same y by
/// construction. `trimDb` / `muted` are nil for `.sourceMeter`, where
/// the corresponding bands render as empty space of the same height
/// for cross-section alignment.
///
/// See `docs/spec.md` § 4.5.
struct MixerStripColumn: View {

    typealias Style = MixerStripDimensions.Style

    let title: String
    /// Optional tooltip rendered via `.help(...)` on the header. Pass nil
    /// for columns without a richer label (the VCA strip).
    let tooltip: String?
    /// Fader binding — required for `.channel` / `.vca`, must be `nil`
    /// for `.sourceMeter` (no fader is rendered).
    let trimDb: Binding<Float>?
    /// Mute binding — same shape as `trimDb`. `.channel` / `.vca` pass
    /// non-nil; `.sourceMeter` passes `nil`.
    let muted: Binding<Bool>?
    /// Optional meter alongside the fader. Channel strips pass post-fader
    /// peak / hold; the VCA strip passes nil and renders only the fader;
    /// source-meter strips pass the pre-fader peak / hold.
    let peak: Float?
    let hold: Float?
    let isCompact: Bool
    let style: Style

    private var dims: MixerStripDimensions {
        MixerStripDimensions(style: style, isCompact: isCompact)
    }
    private var stripWidth: CGFloat { dims.stripWidth }
    private var faderWidth: CGFloat { dims.faderWidth }
    private var meterWidth: CGFloat { dims.meterWidth }

    private var background: Color {
        switch style {
        case .channel, .sourceMeter:
            return Color(red: 0.13,  green: 0.13,  blue: 0.15)
        case .vca:
            return Color(red: 0.155, green: 0.155, blue: 0.17)
        }
    }
    private var borderColor: Color {
        switch style {
        case .channel, .sourceMeter:
            return Color.secondary.opacity(0.22)
        case .vca:
            return Color.secondary.opacity(0.42)
        }
    }
    private var titleFont: Font {
        switch style {
        case .channel, .sourceMeter:
            return .system(size: isCompact ? 9 : 10, weight: .medium)
        case .vca:
            return .system(size: 10, weight: .bold)
        }
    }
    private var titleTracking: CGFloat {
        style == .vca ? 0.4 : 0
    }
    private var titleColor: Color {
        style == .vca ? Color.primary : Color.secondary
    }

    var body: some View {
        // The VStack spacing and outer vertical padding flow through
        // `MixerPanelLayout.bandStack{InnerSpacing,OuterPadding}` so a
        // change here cascades into `barZoneTopOffset` /
        // `barZoneBottomOffset` and keeps the section dB scale
        // aligned with the bar zone (issue #12).
        VStack(spacing: MixerPanelLayout.bandStackInnerSpacing) {
            // Header band — fixed height keeps the bar zone anchored.
            Text(title)
                .font(titleFont)
                .tracking(titleTracking)
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(titleColor)
                .help(tooltip ?? "")
                .frame(height: MixerPanelLayout.headerBandHeight)
            // +12 cap.
            Text("+12")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MixerPanelLayout.capBandHeight)
            // Bar zone — fader + optional meter, flex height.
            //
            // .channel:      [fader | meter]   — fader leading, meter trailing
            // .vca:          [fader]           — fader only, no meter
            // .sourceMeter:  [meter | spacer]  — horizontal MIRROR of .channel:
            //                meter takes the slot where the fader sits in
            //                .channel; the fader-width slot becomes a phantom
            //                spacer. The two strip kinds therefore have meters
            //                on opposite sides of the strip — like two halves
            //                of a mixer facing each other across the panel.
            barZone
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            // −∞ cap.
            Text("−∞")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MixerPanelLayout.capBandHeight)
            // Readout band — shows "MUTED" red when muted, otherwise
            // formatted dB. Source-meter strips have no fader / mute,
            // so the readout band is left empty (preserves vertical
            // alignment with the destination strips).
            readoutBand
                .frame(height: MixerPanelLayout.readoutBandHeight)
            // Action band — MUTE button on .channel / .vca strips.
            // Source-meter strips reserve the same vertical space so
            // their bar zones line up with destination strips' bar
            // zones inside the panel HStack.
            actionBand
                .frame(height: MixerPanelLayout.actionBandHeight)
        }
        .padding(.horizontal, style == .vca ? 12 : 8)
        .padding(.vertical, MixerPanelLayout.bandStackOuterPadding)
        .frame(width: stripWidth)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(background)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 5)
                .stroke(borderColor, lineWidth: 1)
        )
        .accessibilityElement(children: .contain)
        .accessibilityLabel(tooltip ?? title)
    }

    /// Bar-zone content: meter only for `.sourceMeter` (mirrored
    /// position), fader + optional meter for `.channel` / `.vca`.
    @ViewBuilder
    private var barZone: some View {
        HStack(spacing: 6) {
            if style == .sourceMeter {
                if let peak = peak, let hold = hold {
                    ChannelBar(peak: peak, hold: hold)
                        .frame(width: meterWidth)
                }
                Color.clear.frame(width: faderWidth)
            } else if let trimBinding = trimDb {
                FaderSlider(dB: trimBinding,
                            style: style == .vca ? .master : .trim)
                    .frame(width: faderWidth)
                if let peak = peak, let hold = hold {
                    ChannelBar(peak: peak, hold: hold)
                        .frame(width: meterWidth)
                }
            }
        }
    }

    /// Readout band — empty space for `.sourceMeter`; for the other
    /// styles, the formatted trim/VCA dB value (or "MUTED" in red).
    @ViewBuilder
    private var readoutBand: some View {
        if style == .sourceMeter {
            Color.clear
        } else if let trimBinding = trimDb, let mutedBinding = muted {
            Text(formattedReadout(trimDb: trimBinding.wrappedValue,
                                  muted: mutedBinding.wrappedValue))
                .font(.system(size: style == .vca ? 11 : 10,
                              weight: .semibold).monospacedDigit())
                .foregroundStyle(mutedBinding.wrappedValue
                                 ? Color.red : Color.primary)
        }
    }

    /// Action band — MUTE button for `.channel` / `.vca`; empty space
    /// of the same height for `.sourceMeter` so bar zones align across
    /// sections.
    @ViewBuilder
    private var actionBand: some View {
        if style == .sourceMeter {
            Color.clear
        } else if let mutedBinding = muted {
            MuteButton(isMuted: mutedBinding.wrappedValue,
                       action: { mutedBinding.wrappedValue.toggle() })
        }
    }

    private func formattedReadout(trimDb: Float, muted: Bool) -> String {
        if muted { return "MUTED" }
        if trimDb <= -.infinity { return "−∞" }
        let suffix = style == .vca ? " dB" : ""
        return String(format: "%+.1f\(suffix)", trimDb)
    }
}

/// MUTE toggle shared between channel strips and the VCA strip so the
/// look-and-feel is consistent and the action band is the same height
/// in every column.
struct MuteButton: View {
    let isMuted: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text("MUTE")
                .font(.system(size: 9, weight: .semibold))
                .padding(.vertical, 2)
                .padding(.horizontal, 8)
                .background(
                    RoundedRectangle(cornerRadius: 3)
                        .fill(isMuted ? Color.red.opacity(0.85)
                                      : Color(red: 0.23, green: 0.23, blue: 0.25))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 3)
                        .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
                )
                .foregroundStyle(isMuted ? Color.white : Color.secondary)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(isMuted ? "Unmute" : "Mute")
    }
}

#if DEBUG
#Preview("MixerStripColumn — channel, non-compact") {
    PreviewChannelNonCompact()
}

#Preview("MixerStripColumn — channel, compact") {
    PreviewChannelCompact()
}

#Preview("MixerStripColumn — VCA") {
    PreviewVCA()
}

#Preview("MixerStripColumn — source meter") {
    PreviewSourceMeter()
}

private struct PreviewChannelNonCompact: View {
    @State private var trim: Float = -3.0
    @State private var muted: Bool = false
    var body: some View {
        MixerStripColumn(
            title: "Ch 1 · Mic In",
            tooltip: "Ch 1 · Mic In → Ch 1 · Monitor L",
            trimDb: $trim,
            muted: $muted,
            peak: 0.5,
            hold: 0.6,
            isCompact: false,
            style: .channel
        )
        .frame(maxHeight: .infinity)
        .padding()
        .frame(width: 110, height: 320)
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}

private struct PreviewChannelCompact: View {
    @State private var trim: Float = 0.0
    @State private var muted: Bool = false
    var body: some View {
        MixerStripColumn(
            title: "1",
            tooltip: "Ch 1 → Ch 1",
            trimDb: $trim,
            muted: $muted,
            peak: 0.3,
            hold: 0.5,
            isCompact: true,
            style: .channel
        )
        .frame(maxHeight: .infinity)
        .padding()
        .frame(width: 80, height: 280)
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}

private struct PreviewVCA: View {
    @State private var vca: Float = -2.0
    @State private var muted: Bool = false
    var body: some View {
        MixerStripColumn(
            title: "VCA",
            tooltip: nil,
            trimDb: $vca,
            muted: $muted,
            peak: nil,
            hold: nil,
            isCompact: false,
            style: .vca
        )
        .frame(maxHeight: .infinity)
        .padding()
        .frame(width: 120, height: 320)
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}

private struct PreviewSourceMeter: View {
    var body: some View {
        MixerStripColumn(
            title: "Ch 1",
            tooltip: nil,
            trimDb: nil,
            muted: nil,
            peak: 0.4,
            hold: 0.55,
            isCompact: false,
            style: .sourceMeter
        )
        .frame(maxHeight: .infinity)
        .padding()
        .frame(width: 110, height: 320)
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}
#endif
