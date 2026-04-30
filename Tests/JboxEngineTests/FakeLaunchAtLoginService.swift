import Foundation
@testable import JboxEngineSwift

// Test fixture for `LaunchAtLoginService`. Programmable initial status,
// programmable per-call failure (one-shot, register / unregister
// independent), and a post-register status override so controller tests
// can simulate "register() succeeds but the system says
// .requiresApproval / .notFound" — those are real SMAppService outcomes.
//
// Lives under Tests/JboxEngineTests/ so it's never linked into the
// shipping app. Its own contract is pinned by
// `LaunchAtLoginServiceTests.swift`.
@MainActor
final class FakeLaunchAtLoginService: LaunchAtLoginService {
    private var _status: LaunchAtLoginStatus
    private var pendingRegisterFailure: String?
    private var pendingUnregisterFailure: String?
    private var postRegisterStatusOverride: LaunchAtLoginStatus?

    private(set) var registerCallCount: Int = 0
    private(set) var unregisterCallCount: Int = 0

    var status: LaunchAtLoginStatus { _status }

    init(initialStatus: LaunchAtLoginStatus) {
        self._status = initialStatus
    }

    func register() throws {
        registerCallCount += 1
        if let msg = pendingRegisterFailure {
            pendingRegisterFailure = nil
            throw LaunchAtLoginError.registrationFailed(msg)
        }
        if let override = postRegisterStatusOverride {
            postRegisterStatusOverride = nil
            _status = override
        } else {
            _status = .enabled
        }
    }

    func unregister() throws {
        unregisterCallCount += 1
        if let msg = pendingUnregisterFailure {
            pendingUnregisterFailure = nil
            throw LaunchAtLoginError.unregistrationFailed(msg)
        }
        _status = .notRegistered
    }

    // MARK: - Test seams

    /// Make the next `register()` call throw; one-shot.
    func failNextRegister(message: String) {
        pendingRegisterFailure = message
    }

    /// Make the next `unregister()` call throw; one-shot.
    func failNextUnregister(message: String) {
        pendingUnregisterFailure = message
    }

    /// Override the status the next successful `register()` lands at.
    /// Used by the controller's "register succeeds but status reads
    /// .requiresApproval / .notFound" tests.
    func setPostRegisterStatus(_ status: LaunchAtLoginStatus) {
        postRegisterStatusOverride = status
    }

    /// Simulate a status change driven by something outside Jbox
    /// (e.g., the user toggled the login item in System Settings).
    /// Used by the controller's `refresh()` reconciliation tests.
    func setStatus(_ status: LaunchAtLoginStatus) {
        _status = status
    }
}
