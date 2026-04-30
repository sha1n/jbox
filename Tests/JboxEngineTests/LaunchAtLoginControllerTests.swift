import Testing
@testable import JboxEngineSwift

// Behavior contract for `LaunchAtLoginController` (Phase 7 closeout).
// The controller orchestrates the user-facing "Launch at login" toggle:
// it owns the in-memory state (isEnabled, requiresApproval, lastError,
// pendingFirstTimeNote, hasShownFirstTimeNote), drives a
// LaunchAtLoginService for the system call, and emits onPersistableChange
// so the AppState layer can write through to state.json.
//
// Tests are grouped by behavior:
//   A. Initial state mirroring the service's status at construction
//   B. setEnabled(true) happy path + first-time-note arming
//   C. setEnabled(true) failure paths
//   D. setEnabled(false)
//   E. acknowledgeFirstTimeNote
//   F. refresh() reconciliation
//   G. Idempotent enables
//   H. lastError lifecycle
//   I. onPersistableChange callback
//
// See docs/2026-04-30-launch-at-login-design.md for the full design.

// MARK: - Helpers

@MainActor
private func makeController(
    initialStatus: LaunchAtLoginStatus,
    hasShownFirstTimeNote: Bool = false
) -> (LaunchAtLoginController, FakeLaunchAtLoginService, ChangeCounter) {
    let svc = FakeLaunchAtLoginService(initialStatus: initialStatus)
    let ctrl = LaunchAtLoginController(service: svc,
                                       hasShownFirstTimeNote: hasShownFirstTimeNote)
    let counter = ChangeCounter()
    ctrl.onPersistableChange = { [counter] in counter.bump() }
    return (ctrl, svc, counter)
}

@MainActor
private final class ChangeCounter {
    private(set) var count: Int = 0
    func bump() { count += 1 }
}

// MARK: - A. Initial state

@MainActor
@Suite("LaunchAtLoginController — initial state")
struct LaunchAtLoginControllerInitialStateTests {
    @Test("notRegistered → isEnabled=false, requiresApproval=false, lastError=nil")
    func initialNotRegistered() {
        let (c, _, _) = makeController(initialStatus: .notRegistered)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == false)
        #expect(c.lastError == nil)
        #expect(c.pendingFirstTimeNote == false)
    }

    @Test("enabled → isEnabled=true")
    func initialEnabled() {
        let (c, _, _) = makeController(initialStatus: .enabled)
        #expect(c.isEnabled == true)
        #expect(c.requiresApproval == false)
        #expect(c.lastError == nil)
    }

    @Test("requiresApproval → requiresApproval=true, isEnabled=false")
    func initialRequiresApproval() {
        let (c, _, _) = makeController(initialStatus: .requiresApproval)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == true)
        #expect(c.lastError == nil)
    }

    @Test("notFound → silent at construction (no error, isEnabled=false)")
    func initialNotFound() {
        // .notFound at *construction* means the app hasn't been registered
        // yet AND the bundle isn't where the system can find it. That's not
        // a user-actionable state until the user actually tries to enable;
        // surfacing an error proactively would be noise.
        let (c, _, _) = makeController(initialStatus: .notFound)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == false)
        #expect(c.lastError == nil)
    }
}

// MARK: - B. setEnabled(true) happy path

@MainActor
@Suite("LaunchAtLoginController — setEnabled(true) happy path")
struct LaunchAtLoginControllerEnableHappyPathTests {
    @Test("calls service.register() exactly once")
    func callsRegisterOnce() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        c.setEnabled(true)
        #expect(svc.registerCallCount == 1)
    }

    @Test("post-register .enabled status flips isEnabled to true")
    func flipsEnabledTrue() {
        let (c, _, _) = makeController(initialStatus: .notRegistered)
        c.setEnabled(true)
        #expect(c.isEnabled == true)
        #expect(c.requiresApproval == false)
        #expect(c.lastError == nil)
    }

    @Test("arms pendingFirstTimeNote when hasShownFirstTimeNote was false")
    func armsFirstTimeNoteWhenUnshown() {
        let (c, _, _) = makeController(initialStatus: .notRegistered,
                                       hasShownFirstTimeNote: false)
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == true)
    }

    @Test("does NOT arm pendingFirstTimeNote when hasShownFirstTimeNote was already true")
    func skipsFirstTimeNoteOnceShown() {
        let (c, _, _) = makeController(initialStatus: .notRegistered,
                                       hasShownFirstTimeNote: true)
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == false)
    }
}

// MARK: - C. setEnabled(true) failure paths

@MainActor
@Suite("LaunchAtLoginController — setEnabled(true) failure paths")
struct LaunchAtLoginControllerEnableFailureTests {
    @Test("register() throws → lastError = .registrationFailed")
    func registerThrowsSetsLastError() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        svc.failNextRegister(message: "denied")
        c.setEnabled(true)
        if case .registrationFailed(let msg) = c.lastError {
            #expect(msg.contains("denied"))
        } else {
            Issue.record("expected .registrationFailed, got \(String(describing: c.lastError))")
        }
    }

    @Test("register() throws → isEnabled stays false (no optimistic flip)")
    func registerThrowsLeavesIsEnabledFalse() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        svc.failNextRegister(message: "denied")
        c.setEnabled(true)
        #expect(c.isEnabled == false)
    }

    @Test("register() throws → pendingFirstTimeNote not armed")
    func registerThrowsDoesNotArmNote() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered,
                                         hasShownFirstTimeNote: false)
        svc.failNextRegister(message: "denied")
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == false)
    }

    @Test("register() ok but status .requiresApproval → requiresApproval=true, note armed")
    func postRegisterRequiresApproval() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered,
                                         hasShownFirstTimeNote: false)
        svc.setPostRegisterStatus(.requiresApproval)
        c.setEnabled(true)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == true)
        #expect(c.lastError == nil)
        #expect(c.pendingFirstTimeNote == true)
    }

    @Test("register() ok but status .notFound → lastError, no note arming")
    func postRegisterNotFound() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered,
                                         hasShownFirstTimeNote: false)
        svc.setPostRegisterStatus(.notFound)
        c.setEnabled(true)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == false)
        if case .registrationFailed = c.lastError {
            // ok
        } else {
            Issue.record("expected .registrationFailed for .notFound, got \(String(describing: c.lastError))")
        }
        #expect(c.pendingFirstTimeNote == false)
    }

    @Test("register() ok but status stays .notRegistered → defensive lastError, no note arming")
    func postRegisterStillNotRegistered() {
        // Defensive path: the system service silently no-oped instead
        // of advancing state. We treat that as a failure rather than
        // pretending it succeeded.
        let (c, svc, _) = makeController(initialStatus: .notRegistered,
                                         hasShownFirstTimeNote: false)
        svc.setPostRegisterStatus(.notRegistered)
        c.setEnabled(true)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == false)
        if case .registrationFailed = c.lastError {
            // ok
        } else {
            Issue.record("expected .registrationFailed for .notRegistered post-register, got \(String(describing: c.lastError))")
        }
        #expect(c.pendingFirstTimeNote == false)
    }
}

// MARK: - D. setEnabled(false)

@MainActor
@Suite("LaunchAtLoginController — setEnabled(false)")
struct LaunchAtLoginControllerDisableTests {
    @Test("from enabled → calls unregister, isEnabled=false, requiresApproval=false")
    func disableFromEnabled() {
        let (c, svc, _) = makeController(initialStatus: .enabled)
        c.setEnabled(false)
        #expect(svc.unregisterCallCount == 1)
        #expect(c.isEnabled == false)
        #expect(c.requiresApproval == false,
                "requiresApproval must clear after a successful disable — service.status is now .notRegistered")
        #expect(c.lastError == nil)
    }

    @Test("successful disable clears a prior lastError")
    func disableClearsPriorError() {
        // Set up: enabled state, prior lastError from a failed unregister.
        let (c, svc, _) = makeController(initialStatus: .enabled)
        svc.failNextUnregister(message: "stale")
        c.setEnabled(false)
        #expect(c.lastError != nil)
        #expect(c.isEnabled == true)        // failure preserved isEnabled

        // Now disable again, succeeds — must clear lastError.
        c.setEnabled(false)
        #expect(c.lastError == nil)
        #expect(c.isEnabled == false)
    }

    @Test("from disabled → does NOT call unregister (idempotent)")
    func disableFromDisabledIsNoOp() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        c.setEnabled(false)
        #expect(svc.unregisterCallCount == 0)
        #expect(c.isEnabled == false)
    }

    @Test("unregister throws → lastError set, isEnabled stays true")
    func unregisterThrows() {
        let (c, svc, _) = makeController(initialStatus: .enabled)
        svc.failNextUnregister(message: "system busy")
        c.setEnabled(false)
        if case .unregistrationFailed(let msg) = c.lastError {
            #expect(msg.contains("system busy"))
        } else {
            Issue.record("expected .unregistrationFailed, got \(String(describing: c.lastError))")
        }
        #expect(c.isEnabled == true)
    }

    @Test("clears pendingFirstTimeNote if it was armed")
    func disableClearsPendingNote() {
        // User toggles on → note is armed. User toggles back off without
        // dismissing the note. We expect the note to clear, since there
        // is no longer anything to explain.
        let (c, _, _) = makeController(initialStatus: .notRegistered,
                                       hasShownFirstTimeNote: false)
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == true)
        c.setEnabled(false)
        #expect(c.pendingFirstTimeNote == false)
    }
}

// MARK: - E. acknowledgeFirstTimeNote

@MainActor
@Suite("LaunchAtLoginController — acknowledgeFirstTimeNote")
struct LaunchAtLoginControllerAcknowledgeNoteTests {
    @Test("clears pendingFirstTimeNote when armed, sets hasShownFirstTimeNote=true")
    func acknowledgeClearsAndLatches() {
        let (c, _, _) = makeController(initialStatus: .notRegistered,
                                       hasShownFirstTimeNote: false)
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == true)
        #expect(c.hasShownFirstTimeNote == false)

        c.acknowledgeFirstTimeNote()

        #expect(c.pendingFirstTimeNote == false)
        #expect(c.hasShownFirstTimeNote == true)
    }

    @Test("idempotent when not armed — no spurious onPersistableChange")
    func acknowledgeIsNoOpWhenNotArmed() {
        let (c, _, counter) = makeController(initialStatus: .notRegistered,
                                             hasShownFirstTimeNote: true)
        let baseline = counter.count
        c.acknowledgeFirstTimeNote()
        #expect(c.pendingFirstTimeNote == false)
        #expect(c.hasShownFirstTimeNote == true)
        #expect(counter.count == baseline,
                "onPersistableChange must not fire when nothing changed")
    }

    @Test("idempotent when not armed AND latch is false — does not silently consume the first-time allowance (regression)")
    func acknowledgeNoOpDoesNotConsumeAllowance() {
        // Regression for a logic bug where `acknowledgeFirstTimeNote()`
        // proceeded whenever `pendingFirstTimeNote || !hasShownFirstTimeNote`
        // — meaning a fresh user (latch=false, nothing pending) calling
        // acknowledge would silently flip the latch and burn their
        // first-time-note allowance without ever seeing the note.
        let (c, _, counter) = makeController(initialStatus: .notRegistered,
                                             hasShownFirstTimeNote: false)
        #expect(c.pendingFirstTimeNote == false)
        #expect(c.hasShownFirstTimeNote == false)
        let baseline = counter.count

        c.acknowledgeFirstTimeNote()

        #expect(c.pendingFirstTimeNote == false)
        #expect(c.hasShownFirstTimeNote == false,
                "latch must NOT flip when there was nothing pending — that would consume the user's first-time allowance silently")
        #expect(counter.count == baseline,
                "no persistence callback when nothing actually changed")

        // Subsequent setEnabled(true) MUST still arm the note — this
        // is the user's first time, latch is genuinely unset.
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == true)
    }

    @Test("once latched, subsequent enables do not re-arm the note")
    func latchedFirstTimeNoteStaysLatched() {
        let (c, _, _) = makeController(initialStatus: .notRegistered,
                                       hasShownFirstTimeNote: false)
        c.setEnabled(true)
        c.acknowledgeFirstTimeNote()
        #expect(c.hasShownFirstTimeNote == true)

        // Disable then re-enable in the same session — note must not re-arm.
        c.setEnabled(false)
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == false)
    }
}

// MARK: - F. refresh() reconciliation

@MainActor
@Suite("LaunchAtLoginController — refresh()")
struct LaunchAtLoginControllerRefreshTests {
    @Test("external .notRegistered → .enabled is picked up")
    func externalEnable() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        #expect(c.isEnabled == false)
        svc.setStatus(.enabled)        // simulate external change
        c.refresh()
        #expect(c.isEnabled == true)
    }

    @Test("external .enabled → .notRegistered is picked up")
    func externalDisable() {
        let (c, svc, _) = makeController(initialStatus: .enabled)
        #expect(c.isEnabled == true)
        svc.setStatus(.notRegistered)
        c.refresh()
        #expect(c.isEnabled == false)
    }

    @Test("external transition to .requiresApproval is picked up; persistence callback NOT fired (requiresApproval isn't persisted)")
    func externalRequiresApproval() {
        let (c, svc, counter) = makeController(initialStatus: .notRegistered)
        let baseline = counter.count
        svc.setStatus(.requiresApproval)
        c.refresh()
        #expect(c.requiresApproval == true)
        #expect(c.isEnabled == false)
        #expect(counter.count == baseline,
                "requiresApproval-only flip must not trigger persistence — only isEnabled and hasShownFirstTimeNote ride state.json")
    }

    @Test("external transition .requiresApproval → .enabled (user approved in System Settings) is picked up")
    func externalApprovalCompletes() {
        let (c, svc, _) = makeController(initialStatus: .requiresApproval)
        #expect(c.requiresApproval == true)
        #expect(c.isEnabled == false)

        // User goes to System Settings, approves, returns. Status flips
        // .requiresApproval → .enabled. refresh() must clear
        // requiresApproval and flip isEnabled.
        svc.setStatus(.enabled)
        c.refresh()
        #expect(c.requiresApproval == false)
        #expect(c.isEnabled == true)
    }

    @Test("refresh does NOT clear lastError set by a prior failed register")
    func refreshPreservesLastError() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        svc.failNextRegister(message: "boom")
        c.setEnabled(true)
        #expect(c.lastError != nil)

        // Simulate the user toggling on outside Jbox afterwards.
        svc.setStatus(.enabled)
        c.refresh()
        // lastError persists — refresh is reconciliation, not remediation.
        #expect(c.lastError != nil)
        // But isEnabled does converge to truth.
        #expect(c.isEnabled == true)
    }

    @Test("refresh is a no-op when status is unchanged (no onPersistableChange)")
    func refreshIdempotent() {
        let (c, _, counter) = makeController(initialStatus: .enabled)
        let baseline = counter.count
        c.refresh()
        c.refresh()
        #expect(counter.count == baseline,
                "refresh with no actual change must not fire onPersistableChange")
    }
}

// MARK: - G. Idempotent enables

@MainActor
@Suite("LaunchAtLoginController — idempotent enables")
struct LaunchAtLoginControllerIdempotentTests {
    @Test("setEnabled(true) when isEnabled=true is a no-op (register not called, lastError untouched)")
    func enableWhenAlreadyEnabledIsNoOp() {
        let (c, svc, _) = makeController(initialStatus: .enabled)
        // Plant a stale lastError on the controller via a failed disable.
        svc.failNextUnregister(message: "stale error")
        c.setEnabled(false)
        let priorError = c.lastError
        #expect(priorError != nil)

        // Now setEnabled(true) — already enabled. Register must not fire,
        // and lastError must not be cleared (only successful operations
        // clear it).
        let registerCallsBefore = svc.registerCallCount
        c.setEnabled(true)
        #expect(svc.registerCallCount == registerCallsBefore)
        #expect(c.lastError == priorError)
    }

    @Test("idempotent enable does not re-arm pendingFirstTimeNote when hasShownFirstTimeNote=true")
    func idempotentEnableDoesNotReArmNote() {
        let (c, _, _) = makeController(initialStatus: .enabled,
                                       hasShownFirstTimeNote: true)
        c.setEnabled(true)
        #expect(c.pendingFirstTimeNote == false)
    }
}

// MARK: - H. lastError lifecycle

@MainActor
@Suite("LaunchAtLoginController — lastError lifecycle")
struct LaunchAtLoginControllerErrorLifecycleTests {
    @Test("a successful operation clears a prior lastError")
    func successClearsPriorError() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        svc.failNextRegister(message: "boom")
        c.setEnabled(true)
        #expect(c.lastError != nil)

        // Next attempt succeeds — error must clear.
        c.setEnabled(true)
        #expect(c.lastError == nil)
        #expect(c.isEnabled == true)
    }

    @Test("a new failure replaces (does not append) the prior lastError")
    func newFailureReplacesPriorError() {
        let (c, svc, _) = makeController(initialStatus: .notRegistered)
        svc.failNextRegister(message: "first")
        c.setEnabled(true)
        if case .registrationFailed(let msg) = c.lastError {
            #expect(msg.contains("first"))
        } else {
            Issue.record("expected first failure")
        }

        svc.failNextRegister(message: "second")
        c.setEnabled(true)
        if case .registrationFailed(let msg) = c.lastError {
            #expect(msg.contains("second"))
            #expect(!msg.contains("first"))
        } else {
            Issue.record("expected second failure to replace first")
        }
    }
}

// MARK: - I. onPersistableChange callback

@MainActor
@Suite("LaunchAtLoginController — onPersistableChange callback")
struct LaunchAtLoginControllerPersistenceCallbackTests {
    @Test("fires on isEnabled flip and on hasShownFirstTimeNote flip; not on lastError-only change")
    func persistableChangeFiresOnPersistedFieldsOnly() {
        let (c, svc, counter) = makeController(initialStatus: .notRegistered,
                                               hasShownFirstTimeNote: false)
        let initial = counter.count

        // Successful enable flips isEnabled (persisted) — callback fires.
        c.setEnabled(true)
        let afterEnable = counter.count
        #expect(afterEnable > initial)

        // Acknowledge note — flips hasShownFirstTimeNote (persisted) and
        // pendingFirstTimeNote (UI-only). Persisted change → callback fires.
        c.acknowledgeFirstTimeNote()
        let afterAck = counter.count
        #expect(afterAck > afterEnable)

        // Now provoke a lastError-only change (failed disable on already-
        // enabled): isEnabled stays true, hasShownFirstTimeNote stays true.
        // Only lastError changes — that is in-memory, NOT persisted, so
        // the callback must NOT fire.
        svc.failNextUnregister(message: "transient")
        c.setEnabled(false)
        #expect(c.lastError != nil)
        #expect(counter.count == afterAck,
                "lastError-only mutation must not trigger persistence")
    }
}
