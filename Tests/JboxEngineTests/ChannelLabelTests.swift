import Testing
@testable import JboxEngineSwift

/// Pure-function tests for `ChannelLabel.format`. The pickers in the
/// add-route sheet compose this for every channel entry; regressions
/// here show up as wrong labels the moment the user opens the sheet,
/// so it's worth pinning the contract here.
@Suite("ChannelLabel formatting")
struct ChannelLabelTests {

    @Test("numeric fallback when the names array is empty")
    func numericFallbackEmpty() {
        #expect(ChannelLabel.format(index: 0, names: []) == "Ch 1")
        #expect(ChannelLabel.format(index: 7, names: []) == "Ch 8")
    }

    @Test("name present renders as 'Ch N · <name>'")
    func namedRow() {
        let names = ["Line 1", "Monitor L", "Monitor R"]
        #expect(ChannelLabel.format(index: 0, names: names) == "Ch 1 · Line 1")
        #expect(ChannelLabel.format(index: 1, names: names) == "Ch 2 · Monitor L")
        #expect(ChannelLabel.format(index: 2, names: names) == "Ch 3 · Monitor R")
    }

    @Test("empty or whitespace-only name falls back to numeric")
    func whitespaceFallsBack() {
        #expect(ChannelLabel.format(index: 0, names: [""])       == "Ch 1")
        #expect(ChannelLabel.format(index: 0, names: ["   "])    == "Ch 1")
        #expect(ChannelLabel.format(index: 0, names: ["\n\t "])  == "Ch 1")
    }

    @Test("leading/trailing whitespace is trimmed from the name")
    func trimming() {
        #expect(ChannelLabel.format(index: 0, names: [" Line 1 "]) == "Ch 1 · Line 1")
        #expect(ChannelLabel.format(index: 0, names: ["\tMon L\n"]) == "Ch 1 · Mon L")
    }

    @Test("index past the end of names falls back to numeric")
    func outOfBoundsHighIndex() {
        let names = ["A", "B"]
        #expect(ChannelLabel.format(index: 2, names: names) == "Ch 3")
        #expect(ChannelLabel.format(index: 99, names: names) == "Ch 100")
    }

    @Test("negative index falls back to numeric (no crash)")
    func outOfBoundsNegativeIndex() {
        // Not reachable from the current UI — the pickers iterate
        // 0..<max — but the function has to be total. Pin it.
        #expect(ChannelLabel.format(index: -1, names: ["A"]) == "Ch 0")
    }

    @Test("names longer than the device is still safe at lower indices")
    func moreNamesThanNeeded() {
        // The backend may return a name per channel for an N-channel
        // device; the UI only asks for indices 0..<N. Extra entries
        // don't affect earlier ones.
        let names = ["A", "B", "C", "D", "E"]
        #expect(ChannelLabel.format(index: 1, names: names) == "Ch 2 · B")
    }
}
