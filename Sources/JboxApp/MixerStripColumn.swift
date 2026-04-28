import SwiftUI
import JboxEngineSwift

/// Single column in the mixer-strip layout. One widget for both per-channel
/// strips and the route-wide VCA strip — they differ only in the optional
/// meter alongside the fader and a few visual touches gated by `style`.
///
/// Every column uses the same band stack (header / +12 cap / bar zone /
/// −∞ cap / readout / mute) at fixed band heights, so when each instance
/// is given the same outer height (`frame(maxHeight: .infinity)` inside
/// the panel HStack), all bar zones align top + bottom. The 0-dB point
/// on every fader sits at the same y by construction.
///
/// See docs/2026-04-28-route-gain-mixer-strip-design.md §§ 4.1–4.5, 4.7.
struct MixerStripColumn: View {

    enum Style {
        case channel  // standard background / border, plus a meter slot
        case vca      // heavier border + slightly different background
    }

    let title: String
    /// Optional tooltip rendered via `.help(...)` on the header. Pass nil
    /// for columns without a richer label (the VCA strip).
    let tooltip: String?
    @Binding var trimDb: Float
    @Binding var muted: Bool
    /// Optional meter alongside the fader. Channel strips pass post-fader
    /// peak / hold; the VCA strip passes nil and renders only the fader.
    let peak: Float?
    let hold: Float?
    let isCompact: Bool
    let style: Style

    private var stripWidth: CGFloat {
        switch style {
        case .channel: return isCompact ? 56 : 84
        case .vca:     return 88
        }
    }
    private var faderWidth: CGFloat {
        switch style {
        case .channel: return isCompact ? 24 : 32
        case .vca:     return 36
        }
    }
    private var meterWidth: CGFloat { isCompact ? 12 : 18 }

    private var background: Color {
        switch style {
        case .channel: return Color(red: 0.13,  green: 0.13,  blue: 0.15)
        case .vca:     return Color(red: 0.155, green: 0.155, blue: 0.17)
        }
    }
    private var borderColor: Color {
        switch style {
        case .channel: return Color.secondary.opacity(0.22)
        case .vca:     return Color.secondary.opacity(0.42)
        }
    }
    private var titleFont: Font {
        switch style {
        case .channel:
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
        VStack(spacing: 4) {
            // Header band — fixed height keeps the bar zone anchored.
            Text(title)
                .font(titleFont)
                .tracking(titleTracking)
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(titleColor)
                .help(tooltip ?? "")
                .frame(height: MeterPanelLayout.headerBandHeight)
            // +12 cap.
            Text("+12")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // Bar zone — fader + optional meter, flex height.
            HStack(spacing: 6) {
                FaderSlider(dB: $trimDb, style: style == .vca ? .master : .trim)
                    .frame(width: faderWidth)
                if let peak = peak, let hold = hold {
                    ChannelBar(peak: peak, hold: hold)
                        .frame(width: meterWidth)
                }
            }
            .frame(maxHeight: .infinity)
            // −∞ cap.
            Text("−∞")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // Readout band — shows "MUTED" red when muted regardless of
            // the fader's dB, so both strip styles communicate mute the
            // same way.
            Text(formattedReadout(trimDb: trimDb, muted: muted))
                .font(.system(size: style == .vca ? 11 : 10,
                              weight: .semibold).monospacedDigit())
                .foregroundStyle(muted ? Color.red : Color.primary)
                .frame(height: MeterPanelLayout.readoutBandHeight)
            // Action band — MUTE button. Identical button on every column
            // so the action band height matches across the row.
            MuteButton(isMuted: muted, action: { muted.toggle() })
        }
        .padding(.horizontal, style == .vca ? 12 : 8)
        .padding(.vertical, 6)
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

/// Shared layout constants for the mixer-strip columns. Keeping them in
/// one place lets every column anchor its bar zone at the same y when
/// the panel HStack uses a flex height. Every column's VStack uses
/// `spacing: 4`, and the bar zone uses `frame(maxHeight: .infinity)` so
/// it fills whatever the parent allocates after the fixed-height bands.
enum MeterPanelLayout {
    static let headerBandHeight:  CGFloat = 16
    static let capBandHeight:     CGFloat = 10
    static let readoutBandHeight: CGFloat = 16
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
#endif
