import Testing
import JboxEngineSwift
import JboxEngineC

@Suite("Engine ABI")
struct EngineABITests {
    @Test("abiVersion is positive")
    func abiVersionIsPositive() {
        #expect(JboxEngine.abiVersion > 0)
    }

    @Test("runtime ABI matches header constant")
    func runtimeMatchesHeader() {
        // Swift imports the C macro as a constant; verify the runtime
        // value from the engine matches the compile-time constant.
        #expect(JboxEngine.abiVersion == JBOX_ENGINE_ABI_VERSION)
    }
}
