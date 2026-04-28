import SwiftUI
import AppKit
import JboxEngineSwift

/// Vertical fader bound to a Float dB value. Renders a track + cap inside
/// a fixed-height frame; converts to/from a 0...1 throw position via
/// `FaderTaper`. Master and per-channel trims use the same dimensions
/// (master visually distinguished only by its strip background, not by
/// fader size).
///
/// Supports:
///   - drag (vertical) to adjust — sensitivity tuned so a full-height
///     drag covers ~40% of the position range (full range needs ~2.5×
///     the bar-zone height of drag);
///   - shift-drag for ×0.25 fine adjust;
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
    /// master and trim, but track / cap dimensions are now uniform —
    /// the master strip differentiates itself via its outer container
    /// (heavier border + slightly different background), not via a
    /// chunkier fader. Earlier divergent sizes felt visually inconsistent
    /// in the mixer-strip layout.
    enum Style {
        case trim
        case master
    }

    @Binding var dB: Float
    let style: Style

    /// Uniform across both styles — was previously divergent (master 32/14/5,
    /// trim 22/9/3). Bumped slightly from the trim baseline for readability.
    private var capWidth: CGFloat   { 26 }
    private var capHeight: CGFloat  { 11 }
    private var trackWidth: CGFloat { 4 }

    /// Drag-sensitivity divisor. A drag of `height * dragSensitivity`
    /// pixels covers the full position range (0..1). With the default
    /// 2.5, a 200-px bar-zone needs 500 px of drag for a full sweep —
    /// usable without overshooting on small finger movements.
    private static let dragSensitivity: CGFloat = 2.5

    @FocusState private var focused: Bool

    /// Position at the start of the current drag — captured on the first
    /// onChanged tick so subsequent ticks compute newPos = startPos +
    /// translation, instead of accumulating translation onto an already-
    /// moved dB value (the original v1 bug — each tick re-read dB and
    /// re-applied the cumulative translation, double-counting).
    @State private var dragStartPos: CGFloat? = nil

    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            let position = CGFloat(FaderTaper.positionFor(db: dB))
            ZStack {
                // Track (centered).
                RoundedRectangle(cornerRadius: 2)
                    .fill(Color(red: 0.10, green: 0.10, blue: 0.11))
                    .frame(width: trackWidth, height: h)
                    .overlay(
                        RoundedRectangle(cornerRadius: 2)
                            .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
                    )
                // Cap.
                RoundedRectangle(cornerRadius: 2)
                    .fill(LinearGradient(colors: [Color(white: 0.85),
                                                  Color(white: 0.45)],
                                         startPoint: .top, endPoint: .bottom))
                    .overlay(
                        RoundedRectangle(cornerRadius: 2)
                            .stroke(Color.black.opacity(0.5), lineWidth: 1)
                    )
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

    private func dragGesture(height: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 1, coordinateSpace: .local)
            .onChanged { value in
                // Capture starting position on first tick. All subsequent
                // ticks compute newPos = startPos + translation; without
                // this, ticks accumulate translation over a moving dB and
                // overshoot wildly.
                let startPos: CGFloat
                if let cached = dragStartPos {
                    startPos = cached
                } else {
                    startPos = CGFloat(FaderTaper.positionFor(db: dB))
                    dragStartPos = startPos
                }
                // Divide by `height * dragSensitivity` so a full-height
                // drag covers only `1/dragSensitivity` of the position
                // range. At 2.5× this means full sweep needs a ~500-px
                // drag on a 200-px bar zone — usable without accidentally
                // slamming from −∞ to +12 on a small flick.
                let dy = -value.translation.height /
                         (height * Self.dragSensitivity)
                let scale: CGFloat =
                    NSEvent.modifierFlags.contains(.shift) ? 0.25 : 1.0
                let newPos = max(0, min(1, startPos + dy * scale))
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
