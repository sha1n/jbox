import SwiftUI
import AppKit
import JboxEngineSwift

/// Vertical fader bound to a Float dB value. Renders a track + cap inside
/// a fixed-height frame; converts to/from a 0...1 throw position via
/// `FaderTaper`. Trim and VCA-style instances share the same dimensions
/// — the VCA strip differentiates itself via its outer container, not
/// via a chunkier fader.
///
/// Cap is sized and styled to look like a console fader knob: rectangular,
/// taller than wide, with a horizontal grip line at the center and a
/// metallic gradient. A subtle drop shadow gives it depth above the track.
///
/// Drag tracking is **1:1** — the cap follows the cursor pixel-for-pixel.
/// A 100-pixel cursor drag moves the cap 100 pixels (capped at the bar
/// zone). Shift-drag scales by 0.25 for fine adjust.
///
/// Supports:
///   - 1:1 vertical drag (drops shift to fine adjust × 0.25);
///   - double-tap to reset to 0 dB;
///   - keyboard arrows for ±0.5 dB / shift ±0.1 dB when focused.
///
/// Note: shift detection uses `NSEvent.modifierFlags` (read at each
/// gesture / keypress event), so toggling shift mid-drag changes the
/// per-pixel sensitivity from that point onward — the gesture does not
/// retroactively rescale earlier movement. Acceptable for v1.
///
/// The drag gesture is attached to the cap, not the full track frame,
/// so clicking on bare track does not jump the cap. Click-to-jump is a
/// future polish item; for v1 the cap is the only drag handle.
///
/// See docs/2026-04-28-route-gain-mixer-strip-design.md § 4.4.
struct FaderSlider: View {

    /// Style is retained for any future visual differentiation between
    /// VCA and trim, but track / cap dimensions are uniform — the VCA
    /// strip differentiates itself via its outer container (heavier
    /// border + slightly different background), not via a chunkier
    /// fader.
    enum Style {
        case trim
        case master  // historic name; corresponds to the VCA strip today
    }

    @Binding var dB: Float
    let style: Style

    /// Console-style cap — taller than wide, with a horizontal grip
    /// line. Sized so it reads as a tactile knob without dominating the
    /// strip width.
    private var capWidth: CGFloat   { 32 }
    private var capHeight: CGFloat  { 26 }
    private var trackWidth: CGFloat { 4 }

    @FocusState private var focused: Bool

    /// Position at the start of the current drag — captured on the first
    /// onChanged tick so subsequent ticks compute newPos = startPos +
    /// (translation / height), instead of accumulating translation onto
    /// an already-moved dB value (each tick reads dB and would otherwise
    /// re-apply the cumulative translation, double-counting).
    @State private var dragStartPos: CGFloat? = nil

    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            let position = CGFloat(FaderTaper.positionFor(db: dB))
            ZStack {
                // Track (centered).
                RoundedRectangle(cornerRadius: 2)
                    .fill(Color(red: 0.08, green: 0.08, blue: 0.09))
                    .frame(width: trackWidth, height: h)
                    .overlay(
                        RoundedRectangle(cornerRadius: 2)
                            .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
                    )
                // Cap — console-style fader knob: metallic gradient body,
                // horizontal grip line at center, soft drop shadow.
                consoleFaderCap
                    .frame(width: capWidth, height: capHeight)
                    .position(x: geo.size.width / 2,
                              y: h - position * h)
                    .gesture(dragGesture(height: h))
                    .onTapGesture(count: 2) { dB = 0 }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .focusable()
            .focused($focused)
            .onKeyPress(.upArrow)   { adjust(by: 0.5);  return .handled }
            .onKeyPress(.downArrow) { adjust(by: -0.5); return .handled }
            .accessibilityLabel("Fader")
            .accessibilityValue(dB.isFinite
                                ? "\(String(format: "%+.1f", dB)) dB"
                                : "muted")
        }
    }

    /// The fader cap, drawn as a stack of: shadow underlay, gradient
    /// body, top highlight, horizontal grip line, frame. Each layer is
    /// a thin rectangle — no images, no compositing surprises.
    @ViewBuilder
    private var consoleFaderCap: some View {
        ZStack {
            // Drop shadow (rendered as a slightly larger rect below).
            RoundedRectangle(cornerRadius: 3)
                .fill(Color.black.opacity(0.45))
                .frame(width: capWidth + 1, height: capHeight + 1)
                .offset(y: 1.5)
                .blur(radius: 1.5)
            // Gradient body — darker bottom, lighter top, with a faint
            // metallic sheen via a 3-stop gradient.
            RoundedRectangle(cornerRadius: 3)
                .fill(LinearGradient(
                    colors: [
                        Color(white: 0.78),
                        Color(white: 0.55),
                        Color(white: 0.30),
                    ],
                    startPoint: .top,
                    endPoint: .bottom))
            // Top highlight strip.
            RoundedRectangle(cornerRadius: 1.5)
                .fill(Color.white.opacity(0.22))
                .frame(height: 2)
                .offset(y: -capHeight / 2 + 3)
            // Horizontal grip line at center — the iconic console
            // marker that tells you where the fader sits.
            Rectangle()
                .fill(Color.black.opacity(0.65))
                .frame(width: capWidth - 6, height: 1.5)
            Rectangle()
                .fill(Color.white.opacity(0.18))
                .frame(width: capWidth - 6, height: 0.5)
                .offset(y: 1)
            // Outer frame.
            RoundedRectangle(cornerRadius: 3)
                .stroke(Color.black.opacity(0.6), lineWidth: 0.75)
        }
    }

    private func dragGesture(height: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 1, coordinateSpace: .local)
            .onChanged { value in
                // Capture starting position on first tick. All subsequent
                // ticks compute newPos = startPos + (translation / height)
                // — without this, ticks accumulate translation over an
                // already-moved dB and overshoot wildly.
                let startPos: CGFloat
                if let cached = dragStartPos {
                    startPos = cached
                } else {
                    startPos = CGFloat(FaderTaper.positionFor(db: dB))
                    dragStartPos = startPos
                }
                // 1:1 cursor tracking — divide by `height` so a drag of
                // N pixels moves the cap exactly N pixels. The cap stays
                // under the cursor for the full extent of the bar zone.
                // Shift scales the translation × 0.25 for fine adjust.
                let scale: CGFloat =
                    NSEvent.modifierFlags.contains(.shift) ? 0.25 : 1.0
                let dy = (-value.translation.height * scale) / height
                let newPos = max(0, min(1, startPos + dy))
                dB = FaderTaper.dbForPosition(Float(newPos))
            }
            .onEnded { _ in
                dragStartPos = nil
            }
    }

    private func adjust(by step: Float) {
        let modifier: Float =
            NSEvent.modifierFlags.contains(.shift) ? 0.2 : 1.0
        let newDb = (dB.isFinite ? dB : FaderTaper.minFiniteDb) + step * modifier
        // Snap to mute when stepping below the floor — matches the drag
        // gesture's behavior, where positionFor → dbForPosition cliffs to
        // -∞ at muteThresholdPosition. Without this, keyboard down-arrow
        // from -60 dB would land at -60.5 dB (a finite value below
        // minFiniteDb) and the readout would show "-60.5 dB" instead of
        // "muted" until the next drag normalizes it.
        if newDb < FaderTaper.minFiniteDb {
            dB = -.infinity
        } else {
            dB = min(FaderTaper.maxDb, newDb)
        }
    }
}

#if DEBUG
#Preview("FaderSlider — trim & master at -3 dB") {
    PreviewContainer()
}

private struct PreviewContainer: View {
    @State var trim: Float = -3.0
    @State var master: Float = -3.0
    var body: some View {
        HStack(spacing: 30) {
            VStack {
                Text("Trim")
                    .font(.caption2)
                FaderSlider(dB: $trim, style: .trim)
                    .frame(width: 30, height: 200)
                Text("\(trim, specifier: "%.1f") dB")
                    .font(.system(size: 10).monospacedDigit())
            }
            VStack {
                Text("Master")
                    .font(.caption2)
                FaderSlider(dB: $master, style: .master)
                    .frame(width: 30, height: 200)
                Text("\(master, specifier: "%.1f") dB")
                    .font(.system(size: 10).monospacedDigit())
            }
        }
        .padding()
        .frame(width: 200, height: 280)
        .background(Color(red: 0.11, green: 0.11, blue: 0.12))
    }
}
#endif
