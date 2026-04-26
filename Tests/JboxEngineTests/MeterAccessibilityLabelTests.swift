import Foundation
import Testing
@testable import JboxEngineSwift

/// Pure-function tests for `MeterAccessibilityLabel.summary`. Pins the
/// VoiceOver-readable contract that the expanded `MeterPanel` advertises
/// to assistive tech (spec § 4.8). The SwiftUI wiring itself is verified
/// by hand in the running app — what we lock down here is the strings
/// the user actually hears.
@Suite("MeterAccessibilityLabel")
struct MeterAccessibilityLabelTests {

    @Test("Empty arrays produce a no-channels summary on both sides")
    func emptyArrays() {
        let label = MeterAccessibilityLabel.summary(source: [], destination: [])
        #expect(label == "Source no channels. Destination no channels.")
    }

    @Test("Silent channels report 'silent' and keep their channel index")
    func allSilent() {
        let label = MeterAccessibilityLabel.summary(
            source: [0.0, 0.0],
            destination: [0.0])
        #expect(label.contains("Source channel 1 silent, channel 2 silent"))
        #expect(label.contains("Destination channel 1 silent"))
    }

    @Test("Sub-floor peaks are silenced rather than read as a huge negative dB")
    func subFloor() {
        // -80 dBFS is well below the -60 dB display floor.
        let belowFloor = Float(pow(10.0, -80.0 / 20.0))
        let label = MeterAccessibilityLabel.summary(
            source: [belowFloor],
            destination: [])
        #expect(label.contains("Source channel 1 silent"))
    }

    @Test("Audible peaks report rounded dBFS")
    func audiblePeaks() {
        // -6 dBFS ≈ 0.5012; -20 dBFS = 0.1
        let minus6  = Float(pow(10.0, -6.0  / 20.0))
        let minus20 = Float(pow(10.0, -20.0 / 20.0))
        let label = MeterAccessibilityLabel.summary(
            source: [minus6, minus20],
            destination: [])
        #expect(label.contains("channel 1 -6 dBFS"))
        #expect(label.contains("channel 2 -20 dBFS"))
    }

    @Test("Over-full-scale peaks report a positive dBFS rather than clamping")
    func overFullScale() {
        // +3 dBFS ≈ 1.413
        let plus3 = Float(pow(10.0, 3.0 / 20.0))
        let label = MeterAccessibilityLabel.summary(
            source: [plus3],
            destination: [])
        #expect(label.contains("channel 1 3 dBFS"))
    }

    @Test("Channel indices are 1-based to match the on-screen labels")
    func channelIndexing() {
        let minus6 = Float(pow(10.0, -6.0 / 20.0))
        let label = MeterAccessibilityLabel.summary(
            source: [0, minus6, 0],
            destination: [])
        #expect(label.contains("channel 2 -6 dBFS"))
    }

    @Test("Source and destination sides are independent")
    func sidesAreIndependent() {
        let minus6 = Float(pow(10.0, -6.0 / 20.0))
        let label = MeterAccessibilityLabel.summary(
            source: [minus6],
            destination: [0.0, 0.0])
        #expect(label.contains("Source channel 1 -6 dBFS"))
        #expect(label.contains("Destination channel 1 silent, channel 2 silent"))
    }
}
