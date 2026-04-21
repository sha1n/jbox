import Testing
@testable import JboxEngineSwift

/// Pure-logic formatter for the `~NN ms` latency pill described in
/// docs/spec.md § 4.6 and plan.md Phase 6 refinement #3. The engine
/// reports microseconds; the UI renders a compact approximation.
@Suite("LatencyFormatter")
struct LatencyFormatterTests {
    @Test("zero microseconds returns nil so the UI can hide the pill")
    func zeroHidesPill() {
        #expect(LatencyFormatter.pillText(microseconds: 0) == nil)
    }

    @Test("sub-millisecond rounds to the ‘<1 ms’ token")
    func subMillisecond() {
        #expect(LatencyFormatter.pillText(microseconds: 1)   == "<1 ms")
        #expect(LatencyFormatter.pillText(microseconds: 500) == "<1 ms")
        #expect(LatencyFormatter.pillText(microseconds: 999) == "<1 ms")
    }

    @Test("single-digit ms keeps one decimal for resolution")
    func lowMs() {
        #expect(LatencyFormatter.pillText(microseconds: 1_000) == "~1.0 ms")
        #expect(LatencyFormatter.pillText(microseconds: 5_500) == "~5.5 ms")
        #expect(LatencyFormatter.pillText(microseconds: 9_900) == "~9.9 ms")
    }

    @Test("≥ 10 ms uses integer ms; rounding is half-up")
    func wholeMs() {
        #expect(LatencyFormatter.pillText(microseconds: 10_000)  == "~10 ms")
        #expect(LatencyFormatter.pillText(microseconds: 42_400)  == "~42 ms")
        #expect(LatencyFormatter.pillText(microseconds: 42_500)  == "~43 ms")
        #expect(LatencyFormatter.pillText(microseconds: 999_499) == "~999 ms")
    }

    @Test("≥ 1 s switches to seconds with one decimal")
    func seconds() {
        // 1 000 000 µs is exactly 1.0 s.
        #expect(LatencyFormatter.pillText(microseconds: 1_000_000)  == "~1.0 s")
        #expect(LatencyFormatter.pillText(microseconds: 1_234_000)  == "~1.2 s")
        #expect(LatencyFormatter.pillText(microseconds: 55_000_000) == "~55.0 s")
    }

    @Test("transition points pick the cleaner bucket at boundaries")
    func bucketTransitions() {
        // 9999 µs is just under 10 ms — still in the one-decimal bucket
        // and rounds to "10.0 ms" by the one-decimal rule, but the
        // bucket threshold is on microseconds (< 10_000), so this
        // number must render with the decimal form.
        #expect(LatencyFormatter.pillText(microseconds: 9_999) == "~10.0 ms")
        // 999_999 µs is just under 1 s — integer-ms bucket, renders as
        // the rounded-up "~1000 ms" before flipping to seconds at 1e6.
        #expect(LatencyFormatter.pillText(microseconds: 999_999) == "~1000 ms")
    }
}
