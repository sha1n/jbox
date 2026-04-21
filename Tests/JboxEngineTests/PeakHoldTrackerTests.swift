import Foundation
import Testing
@testable import JboxEngineSwift

/// Tests for `PeakHoldTracker`: the per-channel peak-hold state that
/// sits above `MeterPeaks` on the store. Holds decay linearly over
/// `holdDurationSeconds`; new observations that exceed the decayed
/// value promote and reset the hold; lower observations are ignored.
/// Spec § 4.5 — the peak-hold tick on each meter bar.
@Suite("PeakHoldTracker")
struct PeakHoldTrackerTests {

    // MARK: - Empty / cold start

    @Test("empty tracker returns 0 for any channel")
    func empty() {
        let t = PeakHoldTracker()
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0)
        #expect(t.heldValue(routeId: 42, side: .dest, channel: 7, now: 1000) == 0)
    }

    @Test("zero observation on cold start does not create state")
    func coldStartZero() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 10) == 0)
    }

    // MARK: - Promotion / demotion

    @Test("observe promotes held value on increase")
    func observeIncrease() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.5, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0.5)
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.8, now: 0.05)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0.05) == 0.8)
    }

    @Test("observe below decayed held value is ignored")
    func observeLowerWithinWindow() {
        var t = PeakHoldTracker(holdDurationSeconds: 1.0)
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.8, now: 0)
        // 10% of the window has elapsed: decayed value ≈ 0.72.
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.2, now: 0.1)
        let v = t.heldValue(routeId: 1, side: .source, channel: 0, now: 0.1)
        #expect(v > 0.7 && v <= 0.8)
    }

    @Test("observe above decayed held value promotes even late in window")
    func observePromoteLate() {
        var t = PeakHoldTracker(holdDurationSeconds: 1.0)
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.9, now: 0)
        // 80% through the window: decayed value ≈ 0.18.
        // A new 0.5 observation is above 0.18, so it promotes.
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.5, now: 0.8)
        let v = t.heldValue(routeId: 1, side: .source, channel: 0, now: 0.8)
        #expect(v == 0.5)
    }

    // MARK: - Decay

    @Test("held value decays to 0 after full hold duration")
    func fullDecay() {
        var t = PeakHoldTracker(holdDurationSeconds: 1.0)
        t.observe(routeId: 1, side: .source, channel: 0, value: 1.0, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 2.0) == 0)
    }

    @Test("held value at the exact hold duration is 0")
    func decayBoundary() {
        var t = PeakHoldTracker(holdDurationSeconds: 1.0)
        t.observe(routeId: 1, side: .source, channel: 0, value: 1.0, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 1.0) == 0)
    }

    @Test("linear decay half-way through window yields half the held value")
    func halfDecay() {
        var t = PeakHoldTracker(holdDurationSeconds: 1.0)
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.8, now: 0)
        let v = t.heldValue(routeId: 1, side: .source, channel: 0, now: 0.5)
        #expect(abs(v - 0.4) < 0.0001)
    }

    // MARK: - Independence

    @Test("different channels on the same route track independently")
    func independentChannels() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.9, now: 0)
        t.observe(routeId: 1, side: .source, channel: 1, value: 0.1, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0.9)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 1, now: 0) == 0.1)
    }

    @Test("source and dest sides track independently")
    func independentSides() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.9, now: 0)
        t.observe(routeId: 1, side: .dest,   channel: 0, value: 0.2, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0.9)
        #expect(t.heldValue(routeId: 1, side: .dest,   channel: 0, now: 0) == 0.2)
    }

    @Test("different routes track independently")
    func independentRoutes() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.9, now: 0)
        t.observe(routeId: 2, side: .source, channel: 0, value: 0.3, now: 0)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0.9)
        #expect(t.heldValue(routeId: 2, side: .source, channel: 0, now: 0) == 0.3)
    }

    // MARK: - forget

    @Test("forget(routeId:) clears only that route's state")
    func forgetRoute() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.9, now: 0)
        t.observe(routeId: 2, side: .source, channel: 0, value: 0.9, now: 0)
        t.forget(routeId: 1)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0)
        #expect(t.heldValue(routeId: 2, side: .source, channel: 0, now: 0) == 0.9)
    }

    @Test("forget(routeId:) on unknown id is a no-op")
    func forgetUnknown() {
        var t = PeakHoldTracker()
        t.observe(routeId: 1, side: .source, channel: 0, value: 0.9, now: 0)
        t.forget(routeId: 99)
        #expect(t.heldValue(routeId: 1, side: .source, channel: 0, now: 0) == 0.9)
    }

    // MARK: - Public configuration

    @Test("default hold duration matches spec § 4.5 peak-hold semantics")
    func defaultDuration() {
        let t = PeakHoldTracker()
        #expect(t.holdDurationSeconds == 1.0)
    }
}
