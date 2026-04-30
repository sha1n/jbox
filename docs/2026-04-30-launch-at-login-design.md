# Launch-at-login — design

**Status:** Shipped 2026-04-30. Implementation matches the design with the deltas captured in `docs/plan.md` § "Phase 7 launch-at-login slice summary of deviations" (notably the `@Observable`-vs-`ObservableObject` decision, the `acknowledgeFirstTimeNote()` guard fix, the C6 / D5 / E4 / F3a regression tests, and the `AppState.load()` ordering for `snapshotLaunchAtLogin()`).

**Goal.** Close out Phase 7 by wiring the "Launch at login" toggle in the General Preferences tab to a real `SMAppService.mainApp` registration, with a one-time explanatory note on first enable. Currently the toggle is a disabled placeholder (`Sources/JboxApp/JboxApp.swift:159-170`).

**Non-goal.** Privileged login items (`SMAppService.loginItem(_:)`, `SMAppService.agent(_:)`, `SMAppService.daemon(_:)`). spec.md § 7.6 line 419 ("no `SMAppService` registration") was written about *those* — privileged helpers and notarization-gated registrations that the no-paid-Developer-Program constraint forbids. `SMAppService.mainApp.register()` is a different code path: it works ad-hoc-signed, requires no entitlements, requires no Developer ID, and is the documented post-macOS-13 replacement for `LSSharedFileList`. The doc-update note at the bottom captures the spec sentence that needs reconciling so a future reader doesn't read line 419 as a blanket ban.

---

## 1. Architecture

Four new types in `Sources/JboxEngineSwift/`, plus one additive persistence field, plus one toggle wire-up + alert in `JboxApp.swift`.

### 1.1 `LaunchAtLoginStatus` — value type

```swift
public enum LaunchAtLoginStatus: Sendable, Equatable {
    case notRegistered
    case enabled
    case requiresApproval
    case notFound
}
```

Mirrors `SMAppService.Status` 1:1 so the production wrapper is a trivial map. Lives in its own file for testability.

### 1.2 `LaunchAtLoginError` — typed error

```swift
public enum LaunchAtLoginError: Error, Equatable, Sendable {
    case registrationFailed(String)    // .register() threw / status read .notFound after success
    case unregistrationFailed(String)
}
```

The `String` payload carries the underlying NSError's `localizedDescription` so the SwiftUI alert can show it without leaking the full error type.

### 1.3 `LaunchAtLoginService` — protocol

```swift
@MainActor
public protocol LaunchAtLoginService: AnyObject {
    var status: LaunchAtLoginStatus { get }
    func register() throws
    func unregister() throws
}
```

`@MainActor`-bound because `SMAppService` API is documented as main-thread.

Two implementations:
- **`SMAppServiceLaunchAtLogin`** — production wrapper around `SMAppService.mainApp`. Maps `SMAppService.Status` → our enum. Catches `register()` / `unregister()` errors and re-throws as `LaunchAtLoginError`. Lives in `Sources/JboxEngineSwift/` and imports `ServiceManagement`.
- **`FakeLaunchAtLoginService`** — test fixture, lives under `Tests/JboxEngineTests/` (test-target-only, never shipped). Programmable initial status + per-call failure injection.

### 1.4 `LaunchAtLoginController` — `@MainActor` orchestrator

```swift
@MainActor
@Observable
public final class LaunchAtLoginController {
    public private(set) var isEnabled: Bool
    public private(set) var requiresApproval: Bool
    public private(set) var lastError: LaunchAtLoginError?
    public private(set) var pendingFirstTimeNote: Bool
    public private(set) var hasShownFirstTimeNote: Bool

    @ObservationIgnored
    public var onPersistableChange: (() -> Void)?

    @ObservationIgnored
    private let service: LaunchAtLoginService

    public init(service: LaunchAtLoginService,
                hasShownFirstTimeNote: Bool = false)

    public func refresh()
    public func setEnabled(_ enabled: Bool)
    public func acknowledgeFirstTimeNote()
}
```

Owns the policy. The view binds its toggle to a closure that calls `setEnabled(_:)`; the service stays behind the protocol so unit tests don't touch real LaunchServices.

Uses Swift's `@Observable` macro (matching `EngineStore` and `AppState`) rather than `ObservableObject + @Published`. The view accesses the controller through an `@Observable` `AppState` parent, and `@Observable`'s observation tracking auto-subscribes the SwiftUI view body to every read of the controller's state — without requiring an explicit `@ObservedObject` declaration in the view, which the AppState→controller path doesn't provide. `service` and `onPersistableChange` are `@ObservationIgnored` so a service swap or callback re-wire does not invalidate views that read the published state.

### 1.5 `StoredPreferences.hasShownLaunchAtLoginNote: Bool`

Additive Codable field, default `false`. Lets the explanatory note fire only on the *first* successful enable per user, surviving relaunches. Decoded via `decodeIfPresent(...) ?? false` to keep pre-Phase-7-launch-at-login state files loadable.

### 1.6 UI

`Sources/JboxApp/JboxApp.swift` — `GeneralPreferencesView`:
- Replace the `Toggle("Launch at login", isOn: .constant(false)).disabled(true)` with a real binding driven by `appState.launchAtLogin?.isEnabled` (read) + `setEnabled(_:)` (write).
- Add `.alert(...)` modifier on the form bound to `pendingFirstTimeNote`. Alert copy explains: "Jbox will start automatically when you log in. Routes restored from your saved configuration stay stopped until you start them — Jbox does not auto-start audio on login." Dismissal calls `acknowledgeFirstTimeNote()`.
- Add a yellow `requiresApproval` callout inline in the menu-bar section when status is `.requiresApproval`: "Waiting for approval in System Settings." with an "Open Login Items…" button that invokes `NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")!)`.
- Add `.task { appState.launchAtLogin?.refresh() }` on the form so the controller reconciles with the live system status every time the General tab becomes visible. Closes the requiresApproval round-trip: the user clicks "Open Login Items…", flips the switch in System Settings, returns to Jbox — without this `.task`, the controller would keep reporting the stale pre-approval state until the next app launch.
- Also remove the existing menu-bar footer's "Both land with Phase 7…" copy since launch-at-login no longer lands later.

`Sources/JboxApp/JboxApp.swift` — `AppState`:
- Build the controller during `load()`, after `persisted` is populated, with `hasShownFirstTimeNote: persisted.preferences.hasShownLaunchAtLoginNote` and a `SMAppServiceLaunchAtLogin()` service instance (production) or an injected one (for previews).
- Wire `controller.onPersistableChange = { [weak self] in self?.snapshotLaunchAtLogin() }` where `snapshotLaunchAtLogin()` reads the controller's two persisted booleans into `persisted.preferences.{launchAtLogin, hasShownLaunchAtLoginNote}` and calls `scheduleSave()`.
- On launch, after `controller` is built, call `controller.refresh()` once so the in-memory model reconciles with the system's actual login-item status. If they diverge (e.g., user toggled the login item off in System Settings between sessions), the controller's flag wins on the *next* save and we re-converge.

---

## 2. Behavior contract (what the tests pin)

### 2.1 Initial state

The controller's published booleans mirror the service's `status` at construction:

| service.status      | isEnabled | requiresApproval | lastError |
|---------------------|-----------|------------------|-----------|
| `.notRegistered`    | false     | false            | nil       |
| `.enabled`          | true      | false            | nil       |
| `.requiresApproval` | false     | true             | nil       |
| `.notFound`         | false     | false            | nil       |

`.notFound` is "we are not in /Applications" — surfacing it as `requiresApproval=true` would be misleading (the user can't fix it from System Settings). It stays silently false-and-no-error at construction; only an attempted `setEnabled(true)` while in `.notFound` produces an error.

### 2.2 setEnabled(true) — happy path

1. Service status was `.notRegistered`.
2. Controller calls `service.register()` (no throw).
3. Re-reads `service.status`, observes `.enabled`.
4. Updates: `isEnabled=true`, `requiresApproval=false`, `lastError=nil`.
5. **First-time-note arming.** If `hasShownFirstTimeNote` was false before this call, sets `pendingFirstTimeNote=true`. (If it was already true, the flag stays false — the user has already seen the note in a prior session.)
6. Fires `onPersistableChange` so AppState writes through.

### 2.3 setEnabled(true) — register() throws

1. Service status was `.notRegistered`.
2. `service.register()` throws.
3. Controller catches: `lastError = .registrationFailed(error.localizedDescription)`.
4. **No optimistic flip:** `isEnabled` stays false.
5. **First-time-note NOT armed:** the user hasn't actually enabled anything.
6. `onPersistableChange` fires — `lastError` is in-memory only (not persisted) but `isEnabled` is, and we want the persistence path consistent.

### 2.4 setEnabled(true) — register succeeds but status reads `.requiresApproval`

This is the "user previously rejected the login item in System Settings" path — `register()` succeeds but the OS marks it as awaiting re-approval.

1. After `register()`, `service.status` is `.requiresApproval`.
2. `isEnabled=false, requiresApproval=true, lastError=nil`.
3. **First-time-note armed** if not already shown — the explanation is more important here, not less, because the user has to open System Settings to complete the flow.

### 2.5 setEnabled(true) — register succeeds but status reads `.notFound`

The app isn't in /Applications. `SMAppService.register()` doesn't throw on this on every macOS version; it can succeed and leave status at `.notFound`. We treat that as a hard error.

1. `isEnabled=false, requiresApproval=false`.
2. `lastError = .registrationFailed("Move Jbox.app to /Applications and try again.")` — synthetic message; the SMAppService API doesn't give us one.
3. First-time-note NOT armed.

### 2.6 setEnabled(false) — happy path

1. `isEnabled` was true.
2. `service.unregister()` (no throw).
3. Status re-read; expected `.notRegistered`.
4. `isEnabled=false, requiresApproval=false, lastError=nil`.
5. **`pendingFirstTimeNote` cleared** — if the user toggled on then immediately off without dismissing the alert, dismissing isn't required.
6. `onPersistableChange` fires.

### 2.7 setEnabled(false) — unregister throws

1. `service.unregister()` throws.
2. `lastError = .unregistrationFailed(...)`.
3. `isEnabled` stays true (the system service is presumably still registered; UI doesn't lie about the system state).
4. `pendingFirstTimeNote` untouched.

### 2.8 setEnabled — idempotent calls

- `setEnabled(true)` when `isEnabled` is already true: skip the call entirely. Don't re-register, don't re-arm the note, don't clear `lastError`. The controller's job is to converge to the requested state, not to act blindly.
- `setEnabled(false)` when `isEnabled` is already false: same. No-op.

### 2.9 acknowledgeFirstTimeNote()

1. Sets `pendingFirstTimeNote=false`.
2. **Sets `hasShownFirstTimeNote=true`** (one-way latch; never resets).
3. Fires `onPersistableChange` so the flag persists.
4. Idempotent — calling when nothing is pending is a no-op (no spurious save).

### 2.10 refresh()

Reconciles with the live system. Used at launch and on Preferences-window appear.

1. Re-reads `service.status`.
2. Updates `isEnabled` / `requiresApproval` to mirror the new status.
3. **Does NOT clear `lastError`** — a refresh after a failed register() shouldn't make the error vanish. The error clears only on the next successful operation.
4. **Does NOT touch `pendingFirstTimeNote`** — that's user-driven.
5. Fires `onPersistableChange` if anything actually changed (idempotent).

### 2.11 Last-error replacement, not accumulation

`lastError` is a single slot. A new failure replaces the old one. A successful operation clears it. The UI only ever shows the most recent.

---

## 3. Test list

48 cases across three files / additions (34 controller + 5 fake-service + 9 persistence).

### 3.1 `Tests/JboxEngineTests/LaunchAtLoginServiceTests.swift` — 5 cases

Pins the `FakeLaunchAtLoginService` test fixture so the controller tests can rely on it.

1. Initial status reflects the value passed to the fake's init.
2. `register()` flips status to `.enabled` when not erroring.
3. `unregister()` flips status to `.notRegistered` when not erroring.
4. Programmable failure on register / unregister throws on the next call only (one-shot).
5. Post-register status override lets tests pin `.requiresApproval` / `.notFound` / `.notRegistered` outcomes after a non-throwing `register()` (the seam C4–C6 rely on).

### 3.2 `Tests/JboxEngineTests/LaunchAtLoginControllerTests.swift` — 34 cases

Grouped:

**A. Initial state (4)**
- A1: `.notRegistered` → `isEnabled=false, requiresApproval=false, lastError=nil`.
- A2: `.enabled` → `isEnabled=true`.
- A3: `.requiresApproval` → `requiresApproval=true, isEnabled=false`.
- A4: `.notFound` → `isEnabled=false, requiresApproval=false, lastError=nil` (silent at construction).

**B. setEnabled(true) happy path (4)**
- B1: calls `service.register()` exactly once.
- B2: status reflects post-register `.enabled` → `isEnabled=true`.
- B3: arms `pendingFirstTimeNote` when `hasShownFirstTimeNote` was false.
- B4: does NOT arm when `hasShownFirstTimeNote` was already true.

**C. setEnabled(true) failure paths (6)**
- C1: register() throws → `lastError = .registrationFailed(...)`.
- C2: register() throws → `isEnabled` stays false.
- C3: register() throws → `pendingFirstTimeNote` not armed.
- C4: register() succeeds but status is `.requiresApproval` → `requiresApproval=true, isEnabled=false, pendingFirstTimeNote` armed.
- C5: register() succeeds but status is `.notFound` → `lastError = .registrationFailed(...)`, `isEnabled=false`, `pendingFirstTimeNote` not armed.
- C6: register() succeeds but status stays `.notRegistered` → defensive `lastError`, no note arming.

**D. setEnabled(false) (5)**
- D1: from enabled → calls `service.unregister()`, `isEnabled=false`, `requiresApproval=false`.
- D2: from disabled → does NOT call unregister (idempotent).
- D3: unregister throws → `lastError = .unregistrationFailed(...)`, `isEnabled` stays true.
- D4: clears `pendingFirstTimeNote` if it was armed.
- D5: successful disable clears a prior `lastError` from a failed unregister attempt (parallel to H1's enable-clears-error path; the two success branches set `lastError = nil` independently).

**E. acknowledgeFirstTimeNote (4)**
- E1: clears `pendingFirstTimeNote` when armed, sets `hasShownFirstTimeNote=true`.
- E2: idempotent when not armed AND latch already true (no-op, no spurious onPersistableChange).
- E3: re-arming is impossible — once `hasShownFirstTimeNote` is true, subsequent enables don't re-arm.
- E4: idempotent when not armed AND latch is false — must NOT silently consume the first-time-note allowance (regression for a guard-logic bug where `pendingFirstTimeNote || !hasShownFirstTimeNote` proceeded incorrectly).

**F. refresh() reconciliation (6)**
- F1: external `.notRegistered → .enabled` (user enabled outside Jbox) → `isEnabled` becomes true.
- F2: external `.enabled → .notRegistered` → `isEnabled` becomes false.
- F3: external `→ .requiresApproval` → `requiresApproval=true`; persistence callback NOT fired (requiresApproval isn't persisted).
- F3a: external `.requiresApproval → .enabled` (user approves in System Settings) → `isEnabled` flips true, `requiresApproval` clears.
- F4: refresh does NOT clear `lastError` set by a prior failed register.
- F5: refresh is idempotent (no-op when status is unchanged — `onPersistableChange` not called).

**G. Idempotent enables / setEnabled re-entry (2)**
- G1: `setEnabled(true)` when `isEnabled=true` is a no-op (register not called, lastError not cleared).
- G2: `setEnabled(true)` when service is already `.enabled` (e.g., reconciled from system) does NOT re-arm `pendingFirstTimeNote` if `hasShownFirstTimeNote=true`.

**H. lastError lifecycle (2)**
- H1: a successful operation clears a prior `lastError`.
- H2: a new failure replaces (does not append) the prior `lastError`.

**I. onPersistableChange callback (1)**
- I1: fires on isEnabled flip + on hasShownFirstTimeNote flip; does NOT fire on `lastError` change alone (lastError isn't persisted).

### 3.3 `Tests/JboxEngineTests/PersistedStateTests.swift` — 3 added cases + JSON-pin updates

- P1: `StoredPreferences.hasShownLaunchAtLoginNote` defaults to `false`.
- P2: round-trip with `hasShownLaunchAtLoginNote=true`.
- P3: missing-key decode returns `false`.

Plus updates to the existing JSON-shape pin tests at lines 246, 256, 294 so they include the new key.

---

## 4. Edge cases explicitly NOT in scope (or covered by manual smoke)

- **Real `SMAppService` integration.** The production wrapper (`SMAppServiceLaunchAtLogin`) has no automated test coverage — exercising it would mutate the user's actual login items. The wrapper is intentionally trivial (a 1:1 enum map + try/catch wrapping); its behavior is verified by manual smoke on a real `Jbox.app` install.
- **SwiftUI alert presentation.** SPM-only, no XCUITest, no ViewInspector — confirmed by `make verify` compile + manual smoke. The controller's published `pendingFirstTimeNote` flag is the testable seam; the view binding is conventional SwiftUI.
- **Race between `refresh()` and `setEnabled(_:)`.** Both are `@MainActor`-bound and synchronous (the underlying `SMAppService` API is sync). No interleaving is possible.
- **App-bundle path detection.** `SMAppService.register()` itself enforces "must be in /Applications" via its `.notFound` status. We surface that through the controller's error path; we do NOT implement a separate bundle-path probe.
- **Auto-restart routes on launch.** Out of scope per `docs/spec.md § 4.10`-equivalent — routes restored from `state.json` stay stopped on launch. The first-time-note copy mentions this explicitly.

---

## 5. Doc updates that ride the implementation commit

1. **`docs/plan.md` Phase 7** — flip the three "Launch at login" boxes (lines 488-490) to `[x]` and add a Phase 7 deviations entry describing the controller / service split + the first-time-note state machine + which spec lines moved.
2. **`docs/spec.md` § 4.6** — replace "Launch-at-login and 'Show meters in menu bar' are disabled placeholders…" with the live behavior. (Menu-bar meters stay disabled.)
3. **`docs/spec.md` § 7.6 line 419 packaging note** — disambiguate "no `SMAppService` registration" → "no privileged `SMAppService` helper (no `agent`, `daemon`, or third-party `loginItem` registrations); `SMAppService.mainApp.register()` for the user-facing launch-at-login toggle is in scope and shipped in Phase 7."
4. **`README.md`** — only if user-facing behavior copy in the README references the disabled placeholder. (Quick check during implementation; likely no change.)
5. **No `docs/followups.md` / `docs/refactoring-backlog.md` entries expected** — the design is self-contained.

---

## 6. Implementation order (after review)

1. Stub the four new types just enough to compile (empty bodies, default returns).
2. Run `swift test --filter LaunchAtLoginControllerTests` — confirm every test fails for the right reason (assertion failure, not type error).
3. Implement `LaunchAtLoginStatus` + `LaunchAtLoginError` first; controller next; `SMAppServiceLaunchAtLogin` last.
4. Add `hasShownLaunchAtLoginNote` to `StoredPreferences` + decoder; rerun persistence tests.
5. Wire into `JboxApp.swift` (`AppState.load()` + `GeneralPreferencesView`).
6. `make verify` green.
7. Manual smoke on a real `Jbox.app` install in /Applications: enable → log out → log back in → app launches → check.
8. Update plan.md + spec.md + README.md per § 5.
