import Foundation

// Phase 7 closeout — user-facing "Launch at login" toggle.
// Design: docs/2026-04-30-launch-at-login-design.md.
//
// The types here intentionally stay framework-free: the production
// SMAppService.mainApp wrapper lives in its own file
// (`SMAppServiceLaunchAtLogin.swift`) so the controller, status enum,
// and error type can be unit-tested without dragging
// ServiceManagement into the test target.

// MARK: - LaunchAtLoginStatus

/// Mirrors `SMAppService.Status` 1:1. Living in our own namespace so
/// `JboxEngineSwift` does not re-export `ServiceManagement` and the
/// controller's behavior contract is independent of the platform
/// framework.
public enum LaunchAtLoginStatus: Sendable, Equatable {
    case notRegistered
    case enabled
    case requiresApproval
    case notFound
}

// MARK: - LaunchAtLoginError

/// Surfaced to the SwiftUI alert via `LaunchAtLoginController.lastError`.
/// The associated `String` carries a user-facing message — either the
/// underlying NSError's `localizedDescription` (when register / unregister
/// throw) or a synthetic explanation (e.g., the ".notFound" hard-error
/// path where SMAppService has nothing to give us).
public enum LaunchAtLoginError: Error, Equatable, Sendable {
    case registrationFailed(String)
    case unregistrationFailed(String)
}

// MARK: - LaunchAtLoginService

/// Behavioural surface for whatever drives the system login-item
/// registration. `SMAppServiceLaunchAtLogin` is the production
/// implementation; tests use a fake that lives under
/// `Tests/JboxEngineTests/`.
@MainActor
public protocol LaunchAtLoginService: AnyObject {
    var status: LaunchAtLoginStatus { get }
    func register() throws
    func unregister() throws
}

// MARK: - LaunchAtLoginController

/// Owns the user-facing state for the "Launch at login" toggle. The
/// SwiftUI view binds to its tracked properties; `AppState` wires
/// `onPersistableChange` so flips of `isEnabled` /
/// `hasShownFirstTimeNote` round-trip through `state.json` on the
/// next debounced save.
///
/// Uses Swift's `@Observable` macro (matching `EngineStore` and
/// `AppState`); `service` and `onPersistableChange` are
/// `@ObservationIgnored` so a service swap or callback re-wire does
/// not invalidate views that read the controller's published state.
///
/// `lastError` is intentionally NOT persisted — a stale error from a
/// previous session is more confusing than helpful, and the view
/// already shows whatever the system says about the current status on
/// launch.
@MainActor
@Observable
public final class LaunchAtLoginController {
    public private(set) var isEnabled: Bool
    public private(set) var requiresApproval: Bool
    public private(set) var lastError: LaunchAtLoginError?
    public private(set) var pendingFirstTimeNote: Bool
    public private(set) var hasShownFirstTimeNote: Bool

    /// Fired by AppState; runs the snapshot+save path on the next
    /// debounce tick. Intentionally callback-shaped (not a Combine
    /// publisher) to mirror `EngineStore.onRoutesChanged`.
    @ObservationIgnored
    public var onPersistableChange: (() -> Void)?

    @ObservationIgnored
    private let service: LaunchAtLoginService

    public init(service: LaunchAtLoginService,
                hasShownFirstTimeNote: Bool = false) {
        self.service               = service
        self.hasShownFirstTimeNote = hasShownFirstTimeNote
        self.pendingFirstTimeNote  = false
        self.lastError             = nil
        let s = service.status
        self.isEnabled        = (s == .enabled)
        self.requiresApproval = (s == .requiresApproval)
    }

    /// Reconcile model state with the live system status. Called on
    /// app launch and when the Preferences window appears, in case the
    /// user toggled the login item via System Settings between
    /// sessions.
    public func refresh() {
        let s = service.status
        let newIsEnabled            = (s == .enabled)
        let newRequiresApproval     = (s == .requiresApproval)
        let isEnabledChanged        = (newIsEnabled != isEnabled)
        let requiresApprovalChanged = (newRequiresApproval != requiresApproval)

        // Conditional writes avoid spurious `@Observable` notifications
        // when the value is unchanged.
        if isEnabledChanged        { isEnabled        = newIsEnabled }
        if requiresApprovalChanged { requiresApproval = newRequiresApproval }

        // refresh() is reconciliation, not remediation — leave
        // `lastError` and `pendingFirstTimeNote` alone. Persist only
        // when something the user cares about durably actually moved.
        if isEnabledChanged {
            onPersistableChange?()
        }
    }

    /// User flipped the toggle. Synchronously drives the system call,
    /// re-reads status, updates the published fields, and arms the
    /// first-time note when applicable.
    public func setEnabled(_ enabled: Bool) {
        if enabled {
            performEnable()
        } else {
            performDisable()
        }
    }

    /// User dismissed the explanatory alert. One-way latch:
    /// `hasShownFirstTimeNote` never resets, so the note never
    /// reappears in subsequent sessions or after toggle cycles.
    /// Idempotent — when nothing is pending, this is a no-op (no
    /// state change, no persistence callback). The guard checks only
    /// `pendingFirstTimeNote`: silently flipping the latch when
    /// nothing was pending would consume the user's first-time-note
    /// allowance without ever showing them the note.
    public func acknowledgeFirstTimeNote() {
        guard pendingFirstTimeNote else { return }
        pendingFirstTimeNote  = false
        hasShownFirstTimeNote = true
        onPersistableChange?()
    }

    // MARK: - Private

    private func performEnable() {
        // Idempotent when already enabled — do not re-register, do not
        // re-arm the note, do not clear `lastError`. The contract is
        // "converge to requested state", and we're already there.
        guard !isEnabled else { return }

        do {
            try service.register()
        } catch let e as LaunchAtLoginError {
            lastError = e
            // Don't fire onPersistableChange — `lastError` is in-memory
            // and `isEnabled` didn't change.
            return
        } catch {
            lastError = .registrationFailed(error.localizedDescription)
            return
        }

        // register() succeeded — read the resolved status. SMAppService
        // sometimes succeeds-with-pending-approval or
        // succeeds-with-bundle-not-found, so the post-call status is
        // the source of truth, not the absence of a throw.
        let s = service.status
        switch s {
        case .enabled:
            isEnabled        = true
            requiresApproval = false
            lastError        = nil
            armFirstTimeNoteIfNeeded()
            onPersistableChange?()

        case .requiresApproval:
            isEnabled        = false
            requiresApproval = true
            lastError        = nil
            // Note matters MORE here, not less — the user has to open
            // System Settings to complete the flow. Arm it so the
            // alert fires and points them there.
            armFirstTimeNoteIfNeeded()

        case .notFound:
            // Hard error: bundle isn't where SMAppService can find it.
            // Don't arm the note — there's nothing to celebrate.
            isEnabled        = false
            requiresApproval = false
            lastError        = .registrationFailed(
                "Move Jbox.app to /Applications and try again.")

        case .notRegistered:
            // register() returned without throwing but the system says
            // we're still not registered. Treat as a failure — the user
            // toggled and nothing happened, which is worse than an
            // explicit throw.
            isEnabled        = false
            requiresApproval = false
            lastError        = .registrationFailed(
                "The system did not register Jbox as a login item.")
        }
    }

    private func performDisable() {
        // Idempotent when already disabled.
        guard isEnabled else { return }

        do {
            try service.unregister()
        } catch let e as LaunchAtLoginError {
            lastError = e
            return
        } catch {
            lastError = .unregistrationFailed(error.localizedDescription)
            return
        }

        isEnabled            = false
        requiresApproval     = (service.status == .requiresApproval)
        lastError            = nil
        // If the user toggled on then off without dismissing the
        // explanatory note, drop it — there's nothing to explain
        // anymore.
        pendingFirstTimeNote = false
        onPersistableChange?()
    }

    private func armFirstTimeNoteIfNeeded() {
        guard !hasShownFirstTimeNote else { return }
        pendingFirstTimeNote = true
    }
}
