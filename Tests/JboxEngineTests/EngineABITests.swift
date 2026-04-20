import Testing
@_exported import JboxEngineC
@testable import JboxEngineSwift

@Suite("Engine ABI")
struct EngineABITests {
    @Test("abiVersion is positive")
    func abiVersionIsPositive() {
        #expect(JboxEngine.abiVersion > 0)
    }

    @Test("runtime ABI matches header constant")
    func runtimeMatchesHeader() {
        #expect(JboxEngine.abiVersion == JBOX_ENGINE_ABI_VERSION)
    }
}

@Suite("Engine wrapper (live Core Audio)")
struct EngineWrapperTests {
    // These run on CI's macOS runner which always has default built-in
    // audio devices (e.g., BuiltInMicrophone, BuiltInSpeakers). They
    // exercise the Swift wrapper's full path through the C bridge
    // and Core Audio. Route-lifecycle tests that would require
    // specific hardware are deferred to the manual Phase 3 acceptance
    // procedure described in docs/plan.md.

    @Test("Engine can be created and destroyed")
    func createAndDestroy() throws {
        _ = try Engine()
        // Engine releases its handle on deinit.
    }

    @Test("enumerateDevices returns at least one device")
    func enumerateReturnsDevices() throws {
        let engine = try Engine()
        let devices = try engine.enumerateDevices()
        #expect(!devices.isEmpty)
    }

    @Test("every enumerated device has a non-empty UID")
    func enumerationHasUIDs() throws {
        let engine = try Engine()
        let devices = try engine.enumerateDevices()
        for d in devices {
            #expect(!d.uid.isEmpty)
        }
    }

    @Test("enumerated devices report a positive sample rate")
    func enumerationHasSampleRate() throws {
        let engine = try Engine()
        let devices = try engine.enumerateDevices()
        for d in devices {
            #expect(d.nominalSampleRate > 0)
        }
    }

    @Test("at least one input-capable and one output-capable device")
    func hasInputAndOutputDevices() throws {
        let engine = try Engine()
        let devices = try engine.enumerateDevices()
        let anyInput  = devices.contains(where: { $0.directionInput  })
        let anyOutput = devices.contains(where: { $0.directionOutput })
        #expect(anyInput)
        #expect(anyOutput)
    }

    @Test("enumeration is stable across calls")
    func enumerationIsStable() throws {
        let engine = try Engine()
        let first  = try engine.enumerateDevices().map { $0.uid }.sorted()
        let second = try engine.enumerateDevices().map { $0.uid }.sorted()
        #expect(first == second)
    }

    @Test("route actions on nonexistent id return INVALID_ARGUMENT")
    func invalidRouteActionsAreRejected() throws {
        let engine = try Engine()
        // ID 99999 was never added.
        do {
            try engine.startRoute(99999)
            Issue.record("expected error")
        } catch let e as JboxError {
            #expect(e.code == JBOX_ERR_INVALID_ARGUMENT)
        }
    }

    @Test("enumerateChannels returns one entry per channel in the requested direction")
    func enumerateChannelsMatchesChannelCount() throws {
        let engine = try Engine()
        let devices = try engine.enumerateDevices()
        guard let d = devices.first(where: { $0.inputChannelCount >= 1 }) else {
            Issue.record("expected at least one input-capable device on the CI runner")
            return
        }
        let names = try engine.enumerateChannels(uid: d.uid, direction: .input)
        #expect(names.count == Int(d.inputChannelCount))
        // Every element is a valid String (could be empty — some
        // drivers don't publish names). The fact that we got here
        // without throwing or crashing already validates the
        // CFString → std::string → C buffer → Swift String path.
    }

    @Test("enumerateChannels on an unknown UID returns an empty array")
    func enumerateChannelsUnknownUidReturnsEmpty() throws {
        let engine = try Engine()
        // Force the DeviceManager to refresh so the empty-uid lookup
        // takes the normal path (not a fresh-engine early-out).
        _ = try engine.enumerateDevices()
        let names = try engine.enumerateChannels(
            uid: "com.jbox.test.no-such-device",
            direction: .input)
        #expect(names.isEmpty)
    }

    @Test("enumerateChannels for input and output sides both work")
    func enumerateChannelsBothDirections() throws {
        let engine = try Engine()
        let devices = try engine.enumerateDevices()
        if let input = devices.first(where: { $0.inputChannelCount >= 1 }) {
            let names = try engine.enumerateChannels(uid: input.uid, direction: .input)
            #expect(names.count == Int(input.inputChannelCount))
        }
        if let output = devices.first(where: { $0.outputChannelCount >= 1 }) {
            let names = try engine.enumerateChannels(uid: output.uid, direction: .output)
            #expect(names.count == Int(output.outputChannelCount))
        }
    }
}
