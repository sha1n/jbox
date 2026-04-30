import Foundation
import ServiceManagement

// Production wrapper for `SMAppService.mainApp`. Maps Apple's status
// enum to ours and re-throws errors as `LaunchAtLoginError` so the
// controller's behavior contract is independent of the platform
// framework.
//
// Intentionally NOT covered by automated tests — exercising it would
// mutate the runner's actual login items. The controller's tests use
// `FakeLaunchAtLoginService`; this wrapper is verified by manual smoke
// on a real Jbox.app install in /Applications.
//
// Notes on `SMAppService.mainApp` (macOS 13+):
// - Works ad-hoc-signed; no Developer ID or notarization required.
// - Bundle must be in `/Applications` (or the system surfaces
//   `.notFound`).
// - `register()` may succeed and leave status at `.requiresApproval`
//   when the user has previously rejected the login item in System
//   Settings → General → Login Items. The controller arms its
//   first-time note in that case so the alert points the user there.
@MainActor
public final class SMAppServiceLaunchAtLogin: LaunchAtLoginService {
    private let appService: SMAppService

    public init() {
        self.appService = SMAppService.mainApp
    }

    public var status: LaunchAtLoginStatus {
        switch appService.status {
        case .notRegistered:    return .notRegistered
        case .enabled:          return .enabled
        case .requiresApproval: return .requiresApproval
        case .notFound:         return .notFound
        @unknown default:       return .notRegistered
        }
    }

    public func register() throws {
        do {
            try appService.register()
        } catch {
            throw LaunchAtLoginError.registrationFailed(error.localizedDescription)
        }
    }

    public func unregister() throws {
        do {
            try appService.unregister()
        } catch {
            throw LaunchAtLoginError.unregistrationFailed(error.localizedDescription)
        }
    }
}
