import Testing
@testable import JboxEngineSwift

// Tests for the FakeLaunchAtLoginService test fixture itself. The fixture
// is a test double for SMAppService.mainApp; the controller tests in
// LaunchAtLoginControllerTests rely on its programmable behavior, so its
// contract is pinned here.
//
// The production wrapper SMAppServiceLaunchAtLogin is intentionally
// untested — it would mutate the runner's real login items. Its trivial
// 1:1 enum mapping is verified by manual smoke on a real Jbox.app install.

@MainActor
@Suite("FakeLaunchAtLoginService")
struct FakeLaunchAtLoginServiceTests {

    @Test("initial status reflects the value passed to init")
    func initialStatus() {
        let s1 = FakeLaunchAtLoginService(initialStatus: .notRegistered)
        let s2 = FakeLaunchAtLoginService(initialStatus: .enabled)
        let s3 = FakeLaunchAtLoginService(initialStatus: .requiresApproval)
        let s4 = FakeLaunchAtLoginService(initialStatus: .notFound)
        #expect(s1.status == .notRegistered)
        #expect(s2.status == .enabled)
        #expect(s3.status == .requiresApproval)
        #expect(s4.status == .notFound)
    }

    @Test("register() flips status to .enabled when not erroring")
    func registerSucceedsByDefault() throws {
        let s = FakeLaunchAtLoginService(initialStatus: .notRegistered)
        try s.register()
        #expect(s.status == .enabled)
        #expect(s.registerCallCount == 1)
    }

    @Test("unregister() flips status to .notRegistered when not erroring")
    func unregisterSucceedsByDefault() throws {
        let s = FakeLaunchAtLoginService(initialStatus: .enabled)
        try s.unregister()
        #expect(s.status == .notRegistered)
        #expect(s.unregisterCallCount == 1)
    }

    @Test("programmable failure throws on the next call only (one-shot)")
    func failureIsOneShot() throws {
        let s = FakeLaunchAtLoginService(initialStatus: .notRegistered)
        s.failNextRegister(message: "permission denied")
        #expect(throws: LaunchAtLoginError.self) {
            try s.register()
        }
        // status not advanced when register threw
        #expect(s.status == .notRegistered)
        // second call succeeds — failure was one-shot
        try s.register()
        #expect(s.status == .enabled)
        #expect(s.registerCallCount == 2)
    }

    @Test("post-register status override lets tests pin .requiresApproval / .notFound")
    func postRegisterStatusOverride() throws {
        // The controller's contract for "register succeeds but status reads
        // .requiresApproval / .notFound" requires the fake to land at a
        // chosen post-register status independent of the default flip-to-
        // .enabled behavior. Pin that seam here so the controller tests
        // can exercise it.
        let s = FakeLaunchAtLoginService(initialStatus: .notRegistered)
        s.setPostRegisterStatus(.requiresApproval)
        try s.register()
        #expect(s.status == .requiresApproval)

        let s2 = FakeLaunchAtLoginService(initialStatus: .notRegistered)
        s2.setPostRegisterStatus(.notFound)
        try s2.register()
        #expect(s2.status == .notFound)
    }
}
