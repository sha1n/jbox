import Foundation
import Testing
@testable import JboxEngineSwift

/// Pure-function tests for `MeterLevel`: the dB-to-fraction conversion
/// and color-zone classification used by the per-channel meter bars.
/// These are the only observable pieces of the bar renderer; SwiftUI
/// `Canvas` drawing is verified by hand in the running app.
@Suite("MeterLevel")
struct MeterLevelTests {

    // MARK: - fractionFor(peak:)

    @Test("full-scale peak maps to fraction 1.0")
    func fullScale() {
        #expect(MeterLevel.fractionFor(peak: 1.0) == 1.0)
    }

    @Test("above-full-scale peak clamps to fraction 1.0")
    func overdrive() {
        #expect(MeterLevel.fractionFor(peak: 2.5) == 1.0)
    }

    @Test("zero peak maps to fraction 0.0")
    func zero() {
        #expect(MeterLevel.fractionFor(peak: 0.0) == 0.0)
    }

    @Test("negative peak clamps to fraction 0.0")
    func negative() {
        #expect(MeterLevel.fractionFor(peak: -0.5) == 0.0)
    }

    @Test("-60 dBFS (linear 0.001) maps to fraction 0.0")
    func minus60() {
        let f = MeterLevel.fractionFor(peak: 0.001)
        #expect(abs(f - 0.0) < 0.001)
    }

    @Test("-30 dBFS maps to ~0.5 fraction")
    func minus30() {
        let peak = Float(pow(10.0, -30.0 / 20.0))  // ≈ 0.0316
        let f = MeterLevel.fractionFor(peak: peak)
        #expect(abs(f - 0.5) < 0.01)
    }

    @Test("below -60 dBFS clamps to 0.0")
    func belowFloor() {
        #expect(MeterLevel.fractionFor(peak: 0.0001) == 0.0)  // -80 dBFS
    }

    // MARK: - fractionFor(dB:) direct

    @Test("0 dBFS direct maps to 1.0 fraction")
    func dbZero() {
        #expect(MeterLevel.fractionFor(dB: 0) == 1.0)
    }

    @Test("-60 dBFS direct maps to 0.0 fraction")
    func dbMinus60() {
        #expect(MeterLevel.fractionFor(dB: -60) == 0.0)
    }

    @Test("+6 dBFS direct clamps to 1.0 fraction")
    func dbPositive() {
        #expect(MeterLevel.fractionFor(dB: 6) == 1.0)
    }

    // MARK: - Zone classification

    @Test("zero peak classifies as .silent")
    func zoneZero() {
        #expect(MeterLevel.zone(for: 0.0) == .silent)
    }

    @Test("peak below -60 dBFS classifies as .silent")
    func zoneBelowFloor() {
        #expect(MeterLevel.zone(for: 0.0005) == .silent)
    }

    @Test("peak between -60 and -6 dBFS classifies as .normal")
    func zoneNormal() {
        // -20 dBFS ≈ 0.1
        #expect(MeterLevel.zone(for: 0.1) == .normal)
        // -10 dBFS ≈ 0.316
        #expect(MeterLevel.zone(for: 0.316) == .normal)
    }

    @Test("peak just above -6 dBFS classifies as .near")
    func zoneNear() {
        // -5 dBFS ≈ 0.562
        #expect(MeterLevel.zone(for: 0.562) == .near)
        // -3.5 dBFS ≈ 0.668
        #expect(MeterLevel.zone(for: 0.668) == .near)
    }

    @Test("peak at or above -3 dBFS classifies as .clip")
    func zoneClipBoundary() {
        // -3 dBFS ≈ 0.708
        #expect(MeterLevel.zone(for: 0.708) == .clip)
        // -2 dBFS ≈ 0.794
        #expect(MeterLevel.zone(for: 0.794) == .clip)
        // full-scale / over
        #expect(MeterLevel.zone(for: 1.0) == .clip)
        #expect(MeterLevel.zone(for: 1.5) == .clip)
    }

    // MARK: - Pinned public threshold constants

    @Test("public threshold constants match the spec § 4.5 palette")
    func publicConstants() {
        // Pin so a careless tweak to the header trips this test.
        #expect(MeterLevel.floorDb == -60)
        #expect(MeterLevel.nearDb  == -6)
        #expect(MeterLevel.clipDb  == -3)
    }

    // MARK: - DAW scale marks

    @Test("dawScaleMarks lists the standard DAW dBFS marks in descending order")
    func dawScaleMarks() {
        let dbs = MeterLevel.dawScaleMarks.map { $0.dB }
        #expect(dbs == [0, -3, -6, -12, -18, -24, -36, -48, -60])
        // Strictly descending.
        #expect(zip(dbs, dbs.dropFirst()).allSatisfy { $0 > $1 })
    }

    @Test("dawScaleMarks labels match their dB values")
    func dawScaleMarkLabels() {
        for mark in MeterLevel.dawScaleMarks {
            // Labels are integer dB strings (e.g. "0", "-3", "-12").
            #expect(mark.label == String(Int(mark.dB)))
        }
    }

    @Test("dawScaleMarks all fall within the meter's renderable range")
    func dawScaleMarksWithinFloor() {
        for mark in MeterLevel.dawScaleMarks {
            #expect(mark.dB <= 0)
            #expect(mark.dB >= MeterLevel.floorDb)
        }
    }

    // MARK: - dbScaleTickY (dB-scale label-headroom layout)
    //
    // `DbScale` draws its Canvas slightly taller than the bar zone so
    // the top / bottom labels ("0" and "-60") have room to sit
    // outside the bar zone without being clipped by the Canvas's own
    // bounds. The bar zone occupies the subrange
    // `[labelOverflow, canvasHeight - labelOverflow]` inside the
    // Canvas — `dbScaleTickY` maps a dB mark to its tick y inside
    // that subrange, so the "0" tick line lands exactly on the bar
    // zone's top edge and "-60" on the bottom edge regardless of how
    // much vertical headroom is reserved for labels.

    @Test("0 dBFS tick lands on the bar zone's top edge inside the canvas")
    func tickYTopEdge() {
        let y = MeterLevel.dbScaleTickY(forDb: 0,
                                        canvasHeight: 212,
                                        labelOverflow: 6)
        #expect(y == 6)
    }

    @Test("-60 dBFS tick lands on the bar zone's bottom edge inside the canvas")
    func tickYBottomEdge() {
        let y = MeterLevel.dbScaleTickY(forDb: -60,
                                        canvasHeight: 212,
                                        labelOverflow: 6)
        #expect(y == 206)
    }

    @Test("-30 dBFS tick lands at the midpoint of the bar zone subrange")
    func tickYMidpoint() {
        // Bar zone is [6, 206] → midpoint 106.
        let y = MeterLevel.dbScaleTickY(forDb: -30,
                                        canvasHeight: 212,
                                        labelOverflow: 6)
        #expect(y == 106)
    }

    @Test("zero labelOverflow collapses to the canvas-edge mapping")
    func tickYZeroOverflow() {
        // No headroom → the bar zone fills the whole canvas, matching
        // the pre-headroom mapping `size.height * (1 - frac)`.
        #expect(MeterLevel.dbScaleTickY(forDb: 0,
                                        canvasHeight: 200,
                                        labelOverflow: 0) == 0)
        #expect(MeterLevel.dbScaleTickY(forDb: -60,
                                        canvasHeight: 200,
                                        labelOverflow: 0) == 200)
    }

    @Test("canvas smaller than 2x labelOverflow degenerates without negative inner range")
    func tickYDegenerateCanvas() {
        // Defensive: avoid a negative inner span when the canvas is
        // too short to fit the headroom. Both top and bottom ticks
        // collapse to `labelOverflow`.
        let yTop = MeterLevel.dbScaleTickY(forDb: 0,
                                           canvasHeight: 8,
                                           labelOverflow: 6)
        let yBot = MeterLevel.dbScaleTickY(forDb: -60,
                                           canvasHeight: 8,
                                           labelOverflow: 6)
        #expect(yTop == 6)
        #expect(yBot == 6)
    }
}
