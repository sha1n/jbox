import SwiftUI
import JboxEngineSwift

/// Single channel strip in the mixer panel. Composes a trim FaderSlider
/// next to a `ChannelBar` (dest meter), inside the shared bar-zone
/// rectangle. Header shows the source channel name (formatted via
/// `ChannelLabel`); footer shows the trim's dB readout.
///
/// Layout uses fixed-height label bands above and below the bar zone so
/// every column in the MeterPanel HStack — SOURCE group, this strip, and
/// the master strip — share the same vertical extent. Without the fixed
/// bands, intrinsic-text-height variation between the title font sizes
/// would walk the bar zones a few pixels apart from each other.
///
/// See docs/2026-04-28-route-gain-mixer-strip-design.md §§ 4.1-4.4, 4.7.
struct ChannelStripColumn: View {
    let routeId: UInt32
    let channelIndex: Int
    let primaryLabel: String          // truncates with ... at strip width
    let tooltipLabel: String          // full src → dst pair
    let peak: Float
    let hold: Float
    @Binding var trimDb: Float
    let isCompact: Bool               // true when ≥ 6 channels

    /// Pre-mute trim cache — per-strip @State so it survives the SwiftUI
    /// view lifetime but resets on view recreation. When the user clicks
    /// MUTE, we stash the current trim here and push trim to −∞; un-mute
    /// restores from this cache (falling back to 0 dB if it's nil).
    @State private var preMuteTrimDb: Float? = nil

    /// Strip width — wider in non-compact than in v1 because the bar zone
    /// (fader + meter) needs visual breathing room around the cap and the
    /// label header looks cramped at less than ~70 pt.
    private var stripWidth: CGFloat { isCompact ? 56 : 84 }

    /// Mute is implicit in trim ≤ minFiniteDb. EngineStore.setChannelTrimDb
    /// clamps a `-∞` setter to `minFiniteDb` (Option A in Task 11), so the
    /// "muted" state is exactly trim sitting at the floor — which also
    /// means mute survives a relaunch without needing a separate persisted
    /// boolean.
    private var isMuted: Bool { trimDb <= FaderTaper.minFiniteDb }

    var body: some View {
        VStack(spacing: 4) {
            // Header band (fixed height — keeps every column's bar zone
            // anchored at the same y, no matter the title's font metrics).
            Text(primaryLabel)
                .font(.system(size: isCompact ? 9 : 10,
                              weight: .medium))
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(.secondary)
                .help(tooltipLabel)
                .frame(height: MeterPanelLayout.headerBandHeight)
            // +12 cap band.
            Text("+12")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // Bar zone — fader + meter, side by side. Flex height fills
            // whatever space the parent column has after fixed bands.
            HStack(spacing: 6) {
                FaderSlider(dB: $trimDb, style: .trim)
                    .frame(width: isCompact ? 24 : 32)
                ChannelBar(peak: peak, hold: hold)
                    .frame(width: isCompact ? 12 : 18)
            }
            .frame(maxHeight: .infinity)
            // −∞ cap band.
            Text("−∞")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // dB readout band.
            Text(formattedDb(trimDb))
                .font(.system(size: 10, weight: .semibold).monospacedDigit())
                .foregroundStyle(.primary)
                .frame(height: MeterPanelLayout.readoutBandHeight)
            // Action band — MUTE button. Same height as the VCA strip's
            // MUTE so total column heights match and bar zones across
            // columns end up identical.
            MuteButton(isMuted: isMuted, action: toggleMute)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .frame(width: stripWidth)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(Color(red: 0.13, green: 0.13, blue: 0.15))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 5)
                .stroke(Color.secondary.opacity(0.22), lineWidth: 1)
        )
        .accessibilityElement(children: .contain)
        .accessibilityLabel(tooltipLabel)
    }

    private func toggleMute() {
        if isMuted {
            // Unmute — restore the pre-mute trim, falling back to 0 dB
            // if there's no cache (typical first launch after a saved
            // muted route).
            trimDb = preMuteTrimDb ?? 0
            preMuteTrimDb = nil
        } else {
            // Mute — cache the current trim and push to −∞. EngineStore
            // clamps the setter to minFiniteDb on the way through.
            preMuteTrimDb = trimDb
            trimDb = -.infinity
        }
    }

    private func formattedDb(_ db: Float) -> String {
        if db <= -.infinity { return "−∞" }
        return String(format: "%+.1f", db)
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
/// one place lets SOURCE / channel-strips / master all anchor their bar
/// zones at the same y when the panel HStack uses a flex height. Every
/// column's VStack uses `spacing: 4`, and the bar zone uses
/// `frame(maxHeight: .infinity)` so it fills whatever the parent
/// allocates after the fixed-height bands.
enum MeterPanelLayout {
    static let headerBandHeight:  CGFloat = 16
    static let capBandHeight:     CGFloat = 10
    static let readoutBandHeight: CGFloat = 16
    /// Action band (master MUTE button row). `0` for columns without an
    /// action — they instead fall through to no band, since the master
    /// strip's MUTE row only renders inside MasterFaderStrip itself.
    static let actionBandHeight:  CGFloat = 24
}

#if DEBUG
#Preview("ChannelStripColumn — non-compact") {
    PreviewWrapperNonCompact()
}

#Preview("ChannelStripColumn — compact") {
    PreviewWrapperCompact()
}

private struct PreviewWrapperNonCompact: View {
    @State private var trim: Float = -3.0
    var body: some View {
        ChannelStripColumn(
            routeId: 1,
            channelIndex: 0,
            primaryLabel: "Ch 1 · Mic In",
            tooltipLabel: "Ch 1 · Mic In → Ch 1 · Monitor L",
            peak: 0.5,
            hold: 0.6,
            trimDb: $trim,
            isCompact: false
        )
        .frame(maxHeight: .infinity)
        .padding()
        .frame(width: 110, height: 300)
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}

private struct PreviewWrapperCompact: View {
    @State private var trim: Float = 0.0
    var body: some View {
        ChannelStripColumn(
            routeId: 1,
            channelIndex: 0,
            primaryLabel: "1",
            tooltipLabel: "Ch 1 → Ch 1",
            peak: 0.3,
            hold: 0.5,
            trimDb: $trim,
            isCompact: true
        )
        .frame(maxHeight: .infinity)
        .padding()
        .frame(width: 80, height: 260)
        .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}
#endif
