import Foundation
import CoreGraphics

/// Dimensions for one column in the mixer-strip panel. Pure value type
/// so the JboxApp layer's `MixerStripColumn` consumes the same numbers
/// the tests assert on. The mirror layout — source-meter strips occupy
/// the leading slot of the bar zone, channel strips render the meter
/// trailing the fader — only works if `.channel` and `.sourceMeter`
/// share `stripWidth` + `meterWidth`. The tests in
/// `MixerLayoutTests.swift` pin these invariants down so a future edit
/// can't silently introduce a misalignment.
///
/// See `docs/spec.md § 4.5` for the panel layout description.
public struct MixerStripDimensions: Equatable, Sendable {
    public enum Style: Equatable, Sendable {
        /// Per-channel destination strip — fader, meter, MUTE.
        case channel
        /// Route-wide VCA strip on the far right of the destination
        /// section — fader, MUTE, no meter.
        case vca
        /// Per-channel source meter strip — meter only, mirrored so it
        /// sits in the slot where the destination strip's fader is.
        case sourceMeter
    }

    public let style: Style
    public let isCompact: Bool

    public init(style: Style, isCompact: Bool) {
        self.style = style
        self.isCompact = isCompact
    }

    /// Outer width of the strip frame. Channel and source-meter share
    /// the same outer width so SOURCE and DESTINATION strips align
    /// 1:1 across the panel.
    public var stripWidth: CGFloat {
        switch style {
        case .channel, .sourceMeter: return isCompact ? 60 : 96
        case .vca:                   return 104
        }
    }

    /// Width of the fader column inside the bar zone. Source-meter
    /// strips don't render a fader; the slot-equivalent is implied by
    /// the meter sitting at the strip's leading edge.
    public var faderWidth: CGFloat {
        switch style {
        case .channel:     return isCompact ? 26 : 38
        case .vca:         return 42
        case .sourceMeter: return 0
        }
    }

    /// Width of the `ChannelBar` meter inside the bar zone. Same on
    /// `.channel` and `.sourceMeter` so meters render at identical
    /// scale on both sides of the panel.
    public var meterWidth: CGFloat {
        return isCompact ? 14 : 22
    }
}

/// Layout constants for the mixer-strip panel in the expanded route
/// view. Band heights anchor the bar zones across SOURCE and
/// DESTINATION sections at the same y; section widths are split
/// equally with the destination section receiving exactly one
/// VCA-strip's worth of extra width.
///
/// See `docs/spec.md` § 4.5.
public enum MixerPanelLayout {
    public static let headerBandHeight:  CGFloat = 16
    public static let capBandHeight:     CGFloat = 10
    public static let readoutBandHeight: CGFloat = 16
    /// Height of the MUTE button band on `.channel` / `.vca` strips.
    /// Source-meter strips reserve the same height with empty space
    /// so SOURCE and DESTINATION bar zones share a baseline inside
    /// their respective section frames.
    public static let actionBandHeight:  CGFloat = 22
    /// `VStack` spacing between adjacent bands inside a strip column.
    /// Single source of truth so `MixerStripColumn` and the dB-scale
    /// column compute the same bar-zone vertical offsets.
    public static let bandStackInnerSpacing: CGFloat = 4
    /// Outer vertical padding wrapping the band stack inside a strip
    /// column — same source-of-truth role as `bandStackInnerSpacing`.
    public static let bandStackOuterPadding: CGFloat = 6
    /// Distance from the strip column's top edge to the top of the
    /// bar zone (the `barZone` view inside `MixerStripColumn`). The
    /// section's `DbScale` column pads its top by exactly this value
    /// so the "0" tick lines up with the bar's 0 dBFS top edge.
    /// Issue #12 — keep the formula in lock-step with the band stack
    /// in `MixerStripColumn.body`.
    public static let barZoneTopOffset: CGFloat =
        bandStackOuterPadding
        + headerBandHeight   + bandStackInnerSpacing
        + capBandHeight      + bandStackInnerSpacing
    /// Distance from the strip column's bottom edge to the bottom of
    /// the bar zone, mirror of `barZoneTopOffset`. The dB-scale
    /// column pads its bottom by exactly this value so the "-60" tick
    /// lines up with the bar's −60 dBFS floor.
    public static let barZoneBottomOffset: CGFloat =
        bandStackOuterPadding
        + actionBandHeight   + bandStackInnerSpacing
        + readoutBandHeight  + bandStackInnerSpacing
        + capBandHeight      + bandStackInnerSpacing
    /// Spacing between the SOURCE and DESTINATION section frames.
    public static let sectionSpacing:    CGFloat = 14
    /// Floor on each section's width when the window is too narrow to
    /// honor natural strip widths — sections clamp here and rely on
    /// horizontal scroll inside each section.
    public static let minSectionWidth:   CGFloat = 120
    /// Below this we stop subtracting section spacing from the panel
    /// inner width and let the floor do the work.
    public static let minPanelInnerWidth: CGFloat = 240
    /// Width of the section-local dB scale column (`DbScale`). Same
    /// inside both SOURCE and DESTINATION sections so the bar zones
    /// next to it have the same starting x within their frames.
    public static let scaleColumnWidth: CGFloat = 28
    /// Vertical headroom the dB scale's Canvas reserves above the bar
    /// zone's top edge and below its bottom edge. The "0" and "-60"
    /// labels live inside this headroom — full text rect fits without
    /// being clipped by Canvas bounds, while the tick line still
    /// terminates exactly at the bar zone's edge. `SectionScale`
    /// shrinks its top / bottom padding by this amount so the Canvas
    /// grows symmetrically; the bar-zone alignment to the strip
    /// column next to it is preserved.
    public static let dbScaleLabelOverflow: CGFloat = 6
    /// At-or-above this channel count, the panel switches to compact
    /// tier (narrower strips, numeric strip headers, shorter panel).
    /// Single source of truth for the threshold.
    public static let compactChannelThreshold: Int = 6

    /// `true` when the route's channel count exceeds the compact
    /// tier threshold.
    public static func isCompact(channelCount: Int) -> Bool {
        channelCount >= compactChannelThreshold
    }

    /// Spacing between adjacent strips inside one section's HStack —
    /// also the spacing between the last channel strip and the VCA
    /// strip in `vcaSlotWidth`.
    public static func stripSpacing(isCompact: Bool) -> CGFloat {
        isCompact ? 6 : 10
    }

    /// Total panel height — the height the section frames span.
    /// Bumped vs. the original 240/280 so faders read as
    /// console-length on a default-size window.
    public static func panelHeight(isCompact: Bool) -> CGFloat {
        isCompact ? 320 : 400
    }

    /// Width of the VCA strip plus the spacing between it and the
    /// preceding channel strip — the "extra real-estate" the
    /// destination section receives over the source section.
    public static func vcaSlotWidth(isCompact: Bool) -> CGFloat {
        let vcaStripWidth = MixerStripDimensions(style: .vca, isCompact: isCompact).stripWidth
        return vcaStripWidth + stripSpacing(isCompact: isCompact)
    }

    /// Allocates the panel's inner width to the two outlined sections
    /// so SOURCE and DESTINATION have the same per-channel-strip area,
    /// with DESTINATION receiving exactly `vcaSlotWidth` more for the
    /// VCA. Both widths clamp to `minSectionWidth` on a too-narrow
    /// window.
    public static func sectionWidths(
        panelInnerWidth: CGFloat,
        isCompact: Bool
    ) -> (source: CGFloat, destination: CGFloat) {
        let avail = max(panelInnerWidth - sectionSpacing, minPanelInnerWidth)
        let extra = vcaSlotWidth(isCompact: isCompact)
        let baseWidth = max(minSectionWidth, (avail - extra) / 2)
        return (source: baseWidth, destination: baseWidth + extra)
    }

    /// Default size of the main window — sized so a single stereo
    /// route's expanded mixer panel (SOURCE + DESTINATION + VCA) fits
    /// without cropping. Multi-channel routes overflow into horizontal
    /// scroll inside each section; multi-route lists overflow into
    /// the existing vertical list scroll.
    public static let defaultWindowSize = CGSize(width: 920, height: 680)

    /// Hard minimum window size — below this the panel becomes
    /// unusable (strips clip, scroll doesn't help).
    public static let minWindowSize = CGSize(width: 760, height: 540)
}
