import Foundation
import CoreGraphics
import Testing
@testable import JboxEngineSwift

/// Pins down the layout invariants the expanded mixer panel relies on
/// (`docs/spec.md § 4.5`):
///   • SOURCE and DESTINATION sections share the same per-channel-strip
///     width — the destination is wider only by the VCA-strip slot.
///   • `.channel` and `.sourceMeter` strips have identical outer
///     dimensions and meter widths so the source meter mirrors the
///     destination meter across the panel.
///   • The window default size matches the panel's natural size for a
///     stereo route.
///
/// These checks guard against accidental layout regressions in future
/// commits — if any of these invariants drift the visual symmetry
/// between SOURCE and DESTINATION breaks.
@Suite("MixerLayout")
struct MixerLayoutTests {

    // MARK: - Strip dimensions

    @Test("source-meter strip shares outer width with channel strip",
          arguments: [false, true])
    func sourceMeterMatchesChannelStripWidth(isCompact: Bool) {
        let channel = MixerStripDimensions(style: .channel,
                                           isCompact: isCompact)
        let source = MixerStripDimensions(style: .sourceMeter,
                                          isCompact: isCompact)
        #expect(source.stripWidth == channel.stripWidth)
    }

    @Test("source-meter strip uses the same meter width as channel strip",
          arguments: [false, true])
    func sourceMeterMatchesChannelMeterWidth(isCompact: Bool) {
        let channel = MixerStripDimensions(style: .channel,
                                           isCompact: isCompact)
        let source = MixerStripDimensions(style: .sourceMeter,
                                          isCompact: isCompact)
        #expect(source.meterWidth == channel.meterWidth)
    }

    @Test("source-meter strip carries no fader width",
          arguments: [false, true])
    func sourceMeterFaderIsZero(isCompact: Bool) {
        let source = MixerStripDimensions(style: .sourceMeter,
                                          isCompact: isCompact)
        #expect(source.faderWidth == 0)
    }

    @Test("VCA strip is wider than the channel strip",
          arguments: [false, true])
    func vcaWiderThanChannel(isCompact: Bool) {
        let channel = MixerStripDimensions(style: .channel,
                                           isCompact: isCompact)
        let vca = MixerStripDimensions(style: .vca,
                                       isCompact: isCompact)
        #expect(vca.stripWidth > channel.stripWidth)
    }

    @Test("VCA fader is wider than the channel fader",
          arguments: [false, true])
    func vcaFaderWider(isCompact: Bool) {
        let channel = MixerStripDimensions(style: .channel,
                                           isCompact: isCompact)
        let vca = MixerStripDimensions(style: .vca,
                                       isCompact: isCompact)
        #expect(vca.faderWidth > channel.faderWidth)
    }

    @Test("compact tier produces narrower strips than normal tier")
    func compactNarrowerThanNormal() {
        let normal  = MixerStripDimensions(style: .channel, isCompact: false)
        let compact = MixerStripDimensions(style: .channel, isCompact: true)
        #expect(compact.stripWidth < normal.stripWidth)
        #expect(compact.meterWidth < normal.meterWidth)
        #expect(compact.faderWidth < normal.faderWidth)
    }

    // MARK: - Section widths (the "destination wider by exactly the VCA")

    @Test("destination section is wider than source by exactly vcaSlotWidth",
          arguments: [false, true])
    func destinationWiderByVcaSlot(isCompact: Bool) {
        // Use a generously wide panel so the floor-clamp doesn't kick in.
        let widths = MixerPanelLayout.sectionWidths(
            panelInnerWidth: 1000, isCompact: isCompact)
        let extra = MixerPanelLayout.vcaSlotWidth(isCompact: isCompact)
        #expect(widths.destination - widths.source == extra)
    }

    @Test("section widths fit inside the panel inner width when wide enough",
          arguments: [false, true])
    func sectionWidthsFitWidePanel(isCompact: Bool) {
        let panelInner: CGFloat = 1000
        let widths = MixerPanelLayout.sectionWidths(
            panelInnerWidth: panelInner, isCompact: isCompact)
        let total = widths.source + widths.destination
            + MixerPanelLayout.sectionSpacing
        #expect(total <= panelInner + 0.001)
    }

    @Test("section widths exhaust the panel inner width when wide enough",
          arguments: [false, true])
    func sectionWidthsExhaustPanel(isCompact: Bool) {
        let panelInner: CGFloat = 1000
        let widths = MixerPanelLayout.sectionWidths(
            panelInnerWidth: panelInner, isCompact: isCompact)
        let total = widths.source + widths.destination
            + MixerPanelLayout.sectionSpacing
        // No leftover space — sections fully partition the panel.
        #expect(abs(total - panelInner) < 0.001)
    }

    @Test("section widths clamp to a minimum when the panel is too narrow")
    func sectionWidthsClampWhenTooNarrow() {
        let widths = MixerPanelLayout.sectionWidths(
            panelInnerWidth: 100, isCompact: false)
        #expect(widths.source >= MixerPanelLayout.minSectionWidth)
        #expect(widths.destination >= widths.source)
    }

    @Test("vcaSlotWidth equals VCA strip width plus the strip spacing",
          arguments: [false, true])
    func vcaSlotMath(isCompact: Bool) {
        let vca = MixerStripDimensions(style: .vca,
                                       isCompact: isCompact)
        let stripSpacing: CGFloat = isCompact ? 6 : 10
        let expected = vca.stripWidth + stripSpacing
        #expect(MixerPanelLayout.vcaSlotWidth(isCompact: isCompact) == expected)
    }

    // MARK: - Panel height

    @Test("compact tier panel is shorter than normal tier panel")
    func compactPanelShorter() {
        #expect(MixerPanelLayout.panelHeight(isCompact: true) <
                MixerPanelLayout.panelHeight(isCompact: false))
    }

    @Test("panel height defaults are tall enough for a console-feel fader")
    func panelHeightReadsAsConsoleFader() {
        // Loose lower bound — anything significantly under this and the
        // throw on a fader feels stunted on a default-size window.
        // Tweaks below should be deliberate; the test catches drift.
        #expect(MixerPanelLayout.panelHeight(isCompact: false) >= 360)
        #expect(MixerPanelLayout.panelHeight(isCompact: true)  >= 280)
    }

    // MARK: - Window size

    @Test("default window size accommodates a stereo route's panel")
    func defaultWindowFitsStereoPanel() {
        // For a 2-channel route the destination section width is at
        // least the natural strip-content width: scale + 2 strips +
        // VCA + paddings. This sanity-checks that the default window
        // is at least that wide plus reasonable chrome.
        let isCompact = false  // 2 channels is non-compact tier
        let widths = MixerPanelLayout.sectionWidths(
            panelInnerWidth: MixerPanelLayout.defaultWindowSize.width - 60,
            isCompact: isCompact)
        // Destination width must be at least: scale (28) + 2 strips +
        // VCA slot + section padding (~30). This is a generous lower
        // bound; the actual destination width will be larger due to
        // equal-base partitioning.
        let channelStripW = MixerStripDimensions(style: .channel,
                                                  isCompact: isCompact).stripWidth
        let minDest = 28 + 2 * channelStripW
            + MixerPanelLayout.vcaSlotWidth(isCompact: isCompact) + 30
        #expect(widths.destination >= minDest)
    }

    @Test("default window size is at least the minimum window size")
    func defaultIsAtLeastMin() {
        let def = MixerPanelLayout.defaultWindowSize
        let min = MixerPanelLayout.minWindowSize
        #expect(def.width  >= min.width)
        #expect(def.height >= min.height)
    }

    // MARK: - Band heights

    @Test("band heights are positive non-zero values")
    func bandHeightsArePositive() {
        #expect(MixerPanelLayout.headerBandHeight  > 0)
        #expect(MixerPanelLayout.capBandHeight     > 0)
        #expect(MixerPanelLayout.readoutBandHeight > 0)
        #expect(MixerPanelLayout.actionBandHeight  > 0)
    }

    @Test("section spacing is at least 8 px (visual breathing room)")
    func sectionSpacingHasBreathingRoom() {
        #expect(MixerPanelLayout.sectionSpacing >= 8)
    }

    // MARK: - Compact tier threshold

    @Test("compact tier kicks in at the documented channel threshold")
    func compactKicksInAtThreshold() {
        // Exactly at the threshold — compact tier engaged.
        #expect(MixerPanelLayout.isCompact(channelCount:
                MixerPanelLayout.compactChannelThreshold) == true)
        // Just below the threshold — non-compact.
        #expect(MixerPanelLayout.isCompact(channelCount:
                MixerPanelLayout.compactChannelThreshold - 1) == false)
        // Default stereo — definitely non-compact.
        #expect(MixerPanelLayout.isCompact(channelCount: 2) == false)
        // 0 / 1 channel — non-compact (degenerate but defined).
        #expect(MixerPanelLayout.isCompact(channelCount: 1) == false)
        #expect(MixerPanelLayout.isCompact(channelCount: 0) == false)
    }

    // MARK: - Strip + scale spacing

    @Test("compact tier strip spacing is tighter than normal tier")
    func compactStripSpacingTighter() {
        let normal = MixerPanelLayout.stripSpacing(isCompact: false)
        let compact = MixerPanelLayout.stripSpacing(isCompact: true)
        #expect(compact < normal)
    }

    @Test("strip spacing is positive on both tiers",
          arguments: [false, true])
    func stripSpacingPositive(isCompact: Bool) {
        #expect(MixerPanelLayout.stripSpacing(isCompact: isCompact) > 0)
    }

    @Test("scale column width is positive")
    func scaleColumnWidthPositive() {
        #expect(MixerPanelLayout.scaleColumnWidth > 0)
    }

    @Test("vcaSlotWidth is built from VCA strip width plus stripSpacing",
          arguments: [false, true])
    func vcaSlotWidthFromStripSpacing(isCompact: Bool) {
        // The vcaSlotWidth formula must reuse `stripSpacing(isCompact:)`
        // — if it ever forks into a private constant the destination
        // section's "extra width" would drift from the actual visual
        // gap to the VCA. Pin them together.
        let vca = MixerStripDimensions(style: .vca, isCompact: isCompact)
        let expected = vca.stripWidth
            + MixerPanelLayout.stripSpacing(isCompact: isCompact)
        #expect(MixerPanelLayout.vcaSlotWidth(isCompact: isCompact) == expected)
    }

    // MARK: - Bar-zone vertical offsets (scale ↔ bar alignment)

    // Regression coverage for issue #12: the section dB scale's canvas
    // must occupy the same vertical span as the bar zone inside a
    // `MixerStripColumn`, otherwise the "0 / -3 / -6 / … / -60" marks
    // don't line up with the bar fill or the peak-hold tick. The
    // offsets below name the gap between the column's top/bottom and
    // the bar zone, derived from the explicit band-stack composition
    // in `MixerStripColumn`. `SectionScale` consumes the same
    // constants for its top/bottom padding so a band-height edit on
    // either side flows into the other.

    @Test("bandStack inner spacing and outer padding are positive non-zero")
    func bandStackConstantsPositive() {
        #expect(MixerPanelLayout.bandStackInnerSpacing > 0)
        #expect(MixerPanelLayout.bandStackOuterPadding > 0)
    }

    @Test("barZoneTopOffset = outer pad + header + +12 cap + 2× inner spacing")
    func barZoneTopOffsetMatchesBandStack() {
        // Mirrors the cumulative offset above the `barZone` in
        // `MixerStripColumn`: outer padding, then the header band, an
        // inner-spacing gap, the "+12" cap band, and one more
        // inner-spacing gap before the bar zone begins.
        let expected = MixerPanelLayout.bandStackOuterPadding
            + MixerPanelLayout.headerBandHeight
            + MixerPanelLayout.bandStackInnerSpacing
            + MixerPanelLayout.capBandHeight
            + MixerPanelLayout.bandStackInnerSpacing
        #expect(MixerPanelLayout.barZoneTopOffset == expected)
        // Concrete value pin so a band-stack composition change (e.g.
        // a band added or removed) is visible at review time.
        #expect(MixerPanelLayout.barZoneTopOffset == 40)
    }

    @Test("barZoneBottomOffset = outer pad + action + readout + −∞ cap + 3× inner spacing")
    func barZoneBottomOffsetMatchesBandStack() {
        // Mirrors the cumulative offset below the `barZone` in
        // `MixerStripColumn`: inner-spacing, "−∞" cap, inner-spacing,
        // readout band, inner-spacing, action band, then outer
        // padding at the very bottom.
        let expected = MixerPanelLayout.bandStackOuterPadding
            + MixerPanelLayout.actionBandHeight
            + MixerPanelLayout.bandStackInnerSpacing
            + MixerPanelLayout.readoutBandHeight
            + MixerPanelLayout.bandStackInnerSpacing
            + MixerPanelLayout.capBandHeight
            + MixerPanelLayout.bandStackInnerSpacing
        #expect(MixerPanelLayout.barZoneBottomOffset == expected)
        #expect(MixerPanelLayout.barZoneBottomOffset == 66)
    }
}
