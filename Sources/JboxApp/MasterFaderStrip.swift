import SwiftUI
import JboxEngineSwift

/// Master strip on the right edge of the mixer panel. Composes the
/// master FaderSlider, its dB readout, and the MUTE toggle. Heavier
/// border so it reads as a separate group from the per-channel strips.
///
/// See docs/2026-04-28-route-gain-mixer-strip-design.md §§ 4.1, 4.5.
struct MasterFaderStrip: View {
    @Binding var masterDb: Float
    @Binding var muted: Bool

    var body: some View {
        VStack(spacing: 4) {
            // Header band — same height as ChannelStripColumn's title so
            // the bar zones across the panel anchor at the same y.
            Text("MASTER")
                .font(.system(size: 10, weight: .bold))
                .tracking(0.4)
                .foregroundStyle(.primary)
                .frame(height: MeterPanelLayout.headerBandHeight)
            // +12 cap band.
            Text("+12")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // Bar zone — fader. Flex height; same fader width as trims.
            FaderSlider(dB: $masterDb, style: .master)
                .frame(width: 32)
                .frame(maxHeight: .infinity)
            // −∞ cap band.
            Text("−∞")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
                .frame(height: MeterPanelLayout.capBandHeight)
            // dB readout band — same height as the channel strips' readout.
            Text(formattedDb(masterDb, muted: muted))
                .font(.system(size: 10, weight: .semibold).monospacedDigit())
                .foregroundStyle(muted ? Color.red : Color.primary)
                .frame(height: MeterPanelLayout.readoutBandHeight)
            Button(action: { muted.toggle() }) {
                Text("MUTE")
                    .font(.system(size: 9, weight: .semibold))
                    .padding(.vertical, 2)
                    .padding(.horizontal, 8)
                    .background(
                        RoundedRectangle(cornerRadius: 3)
                            .fill(muted ? Color.red.opacity(0.85)
                                        : Color(red: 0.23, green: 0.23, blue: 0.25))
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 3)
                            .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
                    )
                    .foregroundStyle(muted ? Color.white : Color.secondary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(muted ? "Unmute" : "Mute")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .frame(width: 84)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(Color(red: 0.155, green: 0.155, blue: 0.17))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 5)
                .stroke(Color.secondary.opacity(0.42), lineWidth: 1)
        )
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Master strip")
    }

    private func formattedDb(_ db: Float, muted: Bool) -> String {
        if muted { return "MUTED" }
        if db <= -.infinity { return "−∞" }
        return String(format: "%+.1f dB", db)
    }
}

#if DEBUG
#Preview("MasterFaderStrip — unmuted at -3 dB") {
    PreviewUnmuted()
}

#Preview("MasterFaderStrip — muted") {
    PreviewMuted()
}

private struct PreviewUnmuted: View {
    @State private var master: Float = -3.0
    @State private var muted: Bool = false
    var body: some View {
        MasterFaderStrip(masterDb: $master, muted: $muted)
            .frame(maxHeight: .infinity)
            .padding()
            .frame(width: 110, height: 320)
            .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}

private struct PreviewMuted: View {
    @State private var master: Float = 0.0
    @State private var muted: Bool = true
    var body: some View {
        MasterFaderStrip(masterDb: $master, muted: $muted)
            .frame(maxHeight: .infinity)
            .padding()
            .frame(width: 110, height: 320)
            .background(Color(red: 0.10, green: 0.10, blue: 0.11))
    }
}
#endif
