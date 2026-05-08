import Foundation
import Testing
@testable import JboxEngineSwift

/// Pure-function tests for `FaderTaper`: the dB ↔ slider-position taper
/// and dB → linear amplitude conversion used by the mixer-strip fader.
///
/// See `docs/spec.md` § 4.5 for the contract; the "Non-finite-input
/// policy" comment in `FaderTaper.swift` is the source of truth for
/// NaN / ±infinity handling.
@Suite("FaderTaper")
struct FaderTaperTests {

    // MARK: - Plan-mandated coverage

    @Test("dbForPosition lands at 0 dB at unity position (75%)")
    func unityPosition() {
        let db = FaderTaper.dbForPosition(FaderTaper.unityPosition)
        #expect(abs(db - 0.0) < 0.01)
    }

    @Test("dbForPosition lands at +12 dB at the top")
    func topPosition() {
        #expect(abs(FaderTaper.dbForPosition(1.0) - 12.0) < 0.01)
    }

    @Test("dbForPosition lands at -60 dB just above the mute threshold")
    func nearFloor() {
        let db = FaderTaper.dbForPosition(FaderTaper.muteThresholdPosition + 0.001)
        // Above mute threshold the value is finite and at-or-below -60 dB.
        #expect(db.isFinite)
        #expect(db <= FaderTaper.minFiniteDb + 0.5)
    }

    @Test("dbForPosition snaps to -infinity below the mute threshold")
    func belowMuteThreshold() {
        let db = FaderTaper.dbForPosition(FaderTaper.muteThresholdPosition * 0.5)
        #expect(db == -Float.infinity)
    }

    @Test("Round-trip of position → dB → position is approximately stable")
    func roundTrip() {
        for sample: Float in stride(from: 0.10, through: 1.0, by: 0.05) {
            let p = sample
            let db = FaderTaper.dbForPosition(p)
            let p2 = FaderTaper.positionFor(db: db)
            #expect(abs(p - p2) < 0.01,
                    "round-trip drift at p=\(p): got \(p2)")
        }
    }

    @Test("amplitudeFor(db:) maps known dB values to known amplitudes")
    func amplitude() {
        #expect(abs(FaderTaper.amplitudeFor(db: 0) - 1.0) < 1e-5)
        #expect(abs(FaderTaper.amplitudeFor(db: -6) - 0.5012) < 0.001)
        #expect(abs(FaderTaper.amplitudeFor(db: -12) - 0.2512) < 0.001)
        #expect(abs(FaderTaper.amplitudeFor(db: 6) - 1.9953) < 0.001)
        #expect(FaderTaper.amplitudeFor(db: -.infinity) == 0.0)
    }

    @Test("amplitudeFor clamps very negative finite dB values to 0")
    func amplitudeClampsBelowFloor() {
        #expect(FaderTaper.amplitudeFor(db: -120) == 0.0)
        #expect(FaderTaper.amplitudeFor(db: -200) == 0.0)
    }

    @Test("positionFor(-infinity) returns 0")
    func mutePosition() {
        #expect(FaderTaper.positionFor(db: -.infinity) == 0.0)
    }

    // MARK: - Non-finite-input policy
    //
    // NaN policy: clamp to the bottom of the legal range.
    //   dbForPosition(NaN)  → -infinity (mute)
    //   positionFor(NaN)    → 0          (slider at the bottom)
    //   amplitudeFor(NaN)   → 0          (silence)
    //
    // +infinity policy:
    //   dbForPosition(+inf) → +12 dB  (pos clamped to 1)
    //   positionFor(+inf)   → 1.0     (top of the slider)
    //   amplitudeFor(+inf)  → amplitude at maxDb (12 dB ≈ 3.9811),
    //     i.e. clamped, not unbounded — keeps the engine away from
    //     denormals / ±inf gain on the audio path. We intentionally
    //     refuse to amplify beyond the documented [-60, +12] dB range.

    @Test("dbForPosition(NaN) snaps to -infinity (mute)")
    func dbForPositionNaN() {
        #expect(FaderTaper.dbForPosition(.nan) == -.infinity)
    }

    @Test("positionFor(NaN) returns 0")
    func positionForNaN() {
        #expect(FaderTaper.positionFor(db: .nan) == 0.0)
    }

    @Test("amplitudeFor(NaN) returns 0")
    func amplitudeForNaN() {
        #expect(FaderTaper.amplitudeFor(db: .nan) == 0.0)
    }

    @Test("dbForPosition(+infinity) clamps to +12 dB")
    func dbForPositionPlusInf() {
        #expect(FaderTaper.dbForPosition(.infinity) == FaderTaper.maxDb)
    }

    @Test("positionFor(+infinity) clamps to 1.0")
    func positionForPlusInf() {
        #expect(FaderTaper.positionFor(db: .infinity) == 1.0)
    }

    @Test("amplitudeFor(+infinity) clamps to amplitude at maxDb")
    func amplitudeForPlusInf() {
        let cap = FaderTaper.amplitudeFor(db: FaderTaper.maxDb)
        #expect(FaderTaper.amplitudeFor(db: .infinity) == cap)
        #expect(cap.isFinite)
    }

    @Test("dbForPosition(-infinity) snaps to -infinity via the position clamp")
    func dbForPositionMinusInf() {
        // -infinity clamps to position 0 via max(0, min(1, pos)), which is
        // below muteThresholdPosition and therefore returns -infinity.
        #expect(FaderTaper.dbForPosition(-.infinity) == -.infinity)
    }

    // MARK: - Out-of-range positions

    @Test("dbForPosition clamps negative position to 0 (mute)")
    func dbForPositionNegative() {
        #expect(FaderTaper.dbForPosition(-0.5) == -.infinity)
    }

    @Test("dbForPosition clamps positions above 1 to maxDb")
    func dbForPositionAboveOne() {
        #expect(abs(FaderTaper.dbForPosition(2.0) - FaderTaper.maxDb) < 1e-5)
        #expect(abs(FaderTaper.dbForPosition(1000.0) - FaderTaper.maxDb) < 1e-5)
    }

    // MARK: - Boundary positions

    @Test("dbForPosition exactly at muteThresholdPosition is the lowest finite dB")
    func dbAtMuteThreshold() {
        // Spec choice: at-or-above the threshold is finite (-60 dB);
        // strictly below is -infinity. The half-open interval
        // [muteThresholdPosition, unityPosition) covers the lower segment.
        let db = FaderTaper.dbForPosition(FaderTaper.muteThresholdPosition)
        #expect(db.isFinite)
        #expect(abs(db - FaderTaper.minFiniteDb) < 1e-4)
    }

    @Test("dbForPosition exactly at unityPosition is exactly 0 dB")
    func dbAtUnityExact() {
        // Tighter than the unityPosition() test — must be bit-exact 0
        // because that's the value we send to the engine on a click-to-unity.
        #expect(FaderTaper.dbForPosition(FaderTaper.unityPosition) == 0.0)
    }

    @Test("dbForPosition exactly at 1.0 is exactly +12 dB")
    func dbAtTopExact() {
        #expect(FaderTaper.dbForPosition(1.0) == FaderTaper.maxDb)
    }

    // MARK: - Round-trip stability at boundaries

    @Test("Round-trip at the top: positionFor(maxDb) → dbForPosition is exactly maxDb")
    func roundTripTop() {
        let p = FaderTaper.positionFor(db: FaderTaper.maxDb)
        #expect(p == 1.0)
        let db = FaderTaper.dbForPosition(p)
        #expect(db == FaderTaper.maxDb)
    }

    @Test("Round-trip at unity: positionFor(0) → dbForPosition is exactly 0")
    func roundTripUnity() {
        let p = FaderTaper.positionFor(db: 0)
        #expect(p == FaderTaper.unityPosition)
        #expect(FaderTaper.dbForPosition(p) == 0.0)
    }

    @Test("Round-trip at the floor: positionFor(minFiniteDb) → dbForPosition is at-or-below minFiniteDb")
    func roundTripFloor() {
        let p = FaderTaper.positionFor(db: FaderTaper.minFiniteDb)
        #expect(p == FaderTaper.muteThresholdPosition)
        // p is now exactly at the threshold, which is the lowest finite point.
        let db = FaderTaper.dbForPosition(p)
        #expect(db.isFinite)
        #expect(abs(db - FaderTaper.minFiniteDb) < 1e-4)
    }

    @Test("Round-trip across the full range stays within tolerance")
    func roundTripFullRange() {
        // Wider sweep than the plan-mandated test: 0.05 (below mute threshold)
        // through 1.0 inclusive. Below the mute threshold the input gets
        // snapped to -infinity, which projects back to position 0 — that's
        // a *deliberate* discontinuity, not a round-trip failure, so we
        // skip those samples and verify the snap separately.
        for sample: Float in stride(from: 0.05, through: 1.0, by: 0.05) {
            let p = sample
            let db = FaderTaper.dbForPosition(p)
            if db == -.infinity {
                // Below the mute threshold: must round-trip to position 0.
                #expect(FaderTaper.positionFor(db: db) == 0.0)
                continue
            }
            let p2 = FaderTaper.positionFor(db: db)
            #expect(abs(p - p2) < 0.01,
                    "round-trip drift at p=\(p): got \(p2)")
        }
    }

    // MARK: - Monotonicity

    @Test("dbForPosition is non-decreasing across [0, 1]")
    func dbForPositionMonotonic() {
        var prev = FaderTaper.dbForPosition(0)
        for i in 1...100 {
            let p = Float(i) / 100.0
            let cur = FaderTaper.dbForPosition(p)
            // -infinity ≤ -infinity is true under Swift's Float comparison,
            // and -infinity ≤ any finite is true, so this works across the
            // mute snap.
            #expect(prev <= cur, "non-monotonic at p=\(p): prev=\(prev) cur=\(cur)")
            prev = cur
        }
    }

    @Test("positionFor is non-decreasing across [-80, +20] dB")
    func positionForMonotonic() {
        var prev = FaderTaper.positionFor(db: -80)
        var dB: Float = -80
        while dB <= 20 {
            let cur = FaderTaper.positionFor(db: dB)
            #expect(prev <= cur, "non-monotonic at dB=\(dB): prev=\(prev) cur=\(cur)")
            prev = cur
            dB += 0.5
        }
    }

    @Test("amplitudeFor is non-decreasing across [-130, +20] dB")
    func amplitudeForMonotonic() {
        var prev = FaderTaper.amplitudeFor(db: -130)
        var dB: Float = -130
        while dB <= 20 {
            let cur = FaderTaper.amplitudeFor(db: dB)
            #expect(prev <= cur, "non-monotonic at dB=\(dB): prev=\(prev) cur=\(cur)")
            prev = cur
            dB += 0.5
        }
    }

    // MARK: - amplitudeFor symmetry

    @Test("amplitudeFor(db) * amplitudeFor(-db) ≈ 1 for representative dB values")
    func amplitudeSymmetry() {
        for dB: Float in [0.5, 1, 3, 6, 12, 24, 40, 60, 100] {
            let product = FaderTaper.amplitudeFor(db: dB) * FaderTaper.amplitudeFor(db: -dB)
            #expect(abs(product - 1.0) < 1e-3,
                    "symmetry broken at ±\(dB) dB: product=\(product)")
        }
    }
}
