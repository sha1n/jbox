import SwiftUI
import AppKit
import JboxEngineSwift

@main
struct JboxApp: App {
    /// Single shared app model that owns the `EngineStore` and the
    /// init-error slot so both the main window and the menu bar extra
    /// see the same engine instance. Lives for the app's lifetime.
    @State private var appState = AppState()

    /// Live appearance preference. Bound at the top of the scene graph
    /// so both the main window and the menu bar popover track the
    /// user's choice; `colorScheme(from:)` resolves `.system` → `nil`
    /// (follow OS), `.light`/`.dark` → the matching `ColorScheme`.
    @AppStorage(JboxPreferences.appearanceKey) private var appearanceRaw
        = AppearanceMode.default.rawValue

    /// Window-group identifier used by the menu bar's "Open Jbox"
    /// action to reopen the main window after the user has closed it.
    private static let mainWindowId = "main"

    /// App-lifecycle signal so we can flush any debounced save on the
    /// way to `.background`. Without this, a preferences edit parked
    /// inside `StateStore`'s 500 ms debounce window would be lost if
    /// the user quits immediately after.
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        // `Window` (macOS 13+) is a single-instance scene: the framework
        // enforces that exactly one instance exists at a time, drops the
        // default `File ▸ New Window` (Cmd-N) command, and makes
        // `openWindow(id:)` raise the existing instance instead of
        // spawning a duplicate. `WindowGroup` would allow multiple
        // copies, which the spec § 4.2 ("a single main window") and the
        // Phase 6 follow-up (docs/plan.md:358) explicitly rule out.
        Window("JBox", id: Self.mainWindowId) {
            AppRootView(appState: appState)
                .preferredColorScheme(colorScheme)
                .onChange(of: scenePhase) { _, phase in
                    if phase == .background || phase == .inactive {
                        appState.flush()
                    }
                }
        }
        // Default size sized so a single stereo route's expanded mixer
        // panel (SOURCE + DESTINATION sections + VCA) fills the visible
        // area without cropping. Wider channel counts overflow into
        // horizontal scroll inside the sections; more routes overflow
        // into the existing vertical list scroll. Constants live with
        // the layout math in `MixerPanelLayout` so the window default
        // and the panel's natural size stay in sync.
        .defaultSize(width: MixerPanelLayout.defaultWindowSize.width,
                     height: MixerPanelLayout.defaultWindowSize.height)
        .windowResizability(.contentMinSize)

        Settings {
            PreferencesView(appState: appState)
                .preferredColorScheme(colorScheme)
        }

        MenuBarExtra {
            MenuBarScene(appState: appState, mainWindowId: Self.mainWindowId)
                .preferredColorScheme(colorScheme)
        } label: {
            MenuBarIconLabel(store: appState.store)
        }
        .menuBarExtraStyle(.window)
    }

    /// Translate the stored appearance preference into the SwiftUI
    /// `ColorScheme?` that `.preferredColorScheme(_:)` expects. A `nil`
    /// return lets the scenes inherit the OS appearance.
    private var colorScheme: ColorScheme? {
        switch AppearanceMode(rawValueOrDefault: appearanceRaw) {
        case .system: return nil
        case .light:  return .light
        case .dark:   return .dark
        }
    }
}

/// Central default-storage keys for the three-tab Preferences window
/// (docs/spec.md § 4.6). Every `@AppStorage`-backed setting in the app
/// reads from a constant here so the keys can be audited and renamed
/// in one place. Source of truth lives in `state.json` (`StoredPreferences`);
/// these `UserDefaults` keys mirror it so SwiftUI's `@AppStorage` bindings
/// stay simple and `AppState` watches `UserDefaults.didChangeNotification`
/// to snapshot the mirror back into `state.json`. Launch-at-login is the
/// one preference that bypasses `@AppStorage` entirely — its source of
/// truth is `LaunchAtLoginController` plus `SMAppService.mainApp`.
enum JboxPreferences {
    // General
    /// Raw value of `AppearanceMode`. Default: `.system`.
    public static let appearanceKey       = "com.jbox.appearance"
    // Audio
    /// Raw value of `Engine.ResamplerQuality.rawValue`. Default: `.mastering`.
    public static let resamplerQualityKey = "com.jbox.resamplerQuality"
    // Advanced
    /// Bool; diagnostics toggle (Phase 6 refinement #4). Default: off.
    public static let showDiagnosticsKey  = "com.jbox.showDiagnostics"
}

struct PreferencesView: View {
    let appState: AppState

    var body: some View {
        TabView {
            GeneralPreferencesView(appState: appState)
                .tabItem { Label("General", systemImage: "gearshape") }

            AudioPreferencesView(appState: appState)
                .tabItem { Label("Audio", systemImage: "waveform") }

            AdvancedPreferencesView()
                .tabItem { Label("Advanced", systemImage: "wrench.and.screwdriver") }
        }
        .frame(width: 460, height: 320)
    }
}

/// General tab: appearance + menu bar toggles. "Show meters in menu
/// bar icon" still lands with the icon-renderer feature; "Launch at
/// login" is wired here through `LaunchAtLoginController`
/// (`SMAppService.mainApp` under the hood).
struct GeneralPreferencesView: View {
    let appState: AppState

    @AppStorage(JboxPreferences.appearanceKey) private var appearanceRaw
        = AppearanceMode.default.rawValue

    private static let appearanceFooter =
        "Controls the main window and menu bar popover. "
        + "\"Follow System\" inherits the macOS appearance."

    private static let menuBarFooter =
        "Menu-bar meters land with the icon-renderer feature; the "
        + "menu-bar icon currently shows a static state."

    private static let firstTimeNoteMessage =
        "JBox will start automatically when you log in. Routes restored "
        + "from your saved configuration stay stopped until you start "
        + "them — JBox does not auto-start audio on login."

    var body: some View {
        Form {
            appearanceSection
            menuBarSection
        }
        .formStyle(.grouped)
        .padding()
        .alert(
            "Launch at login enabled",
            isPresented: firstTimeNoteBinding,
            actions: {
                Button("OK") {
                    appState.launchAtLogin?.acknowledgeFirstTimeNote()
                }
            },
            message: {
                Text(Self.firstTimeNoteMessage)
            })
        // Reconcile with the live system status whenever the General
        // tab becomes visible. Covers the requiresApproval round-trip:
        // user clicks "Open Login Items…", flips the switch in System
        // Settings, returns to Jbox — without this refresh the
        // controller would keep reporting the stale pre-approval state
        // until the next app launch.
        .task {
            appState.launchAtLogin?.refresh()
        }
    }

    private var firstTimeNoteBinding: Binding<Bool> {
        Binding(
            get: { appState.launchAtLogin?.pendingFirstTimeNote ?? false },
            set: { newValue in
                if !newValue {
                    appState.launchAtLogin?.acknowledgeFirstTimeNote()
                }
            })
    }

    private var appearanceSection: some View {
        Section {
            Picker("Theme", selection: $appearanceRaw) {
                Text("Follow System").tag(AppearanceMode.system.rawValue)
                Text("Light").tag(AppearanceMode.light.rawValue)
                Text("Dark").tag(AppearanceMode.dark.rawValue)
            }
            .pickerStyle(.segmented)
        } header: {
            Text("Appearance")
        } footer: {
            Text(Self.appearanceFooter)
        }
    }

    private var menuBarSection: some View {
        Section {
            launchAtLoginRow
            Toggle("Show meters in menu bar icon", isOn: .constant(false))
                .disabled(true)
        } header: {
            Text("Menu bar")
        } footer: {
            Text(Self.menuBarFooter)
        }
    }

    @ViewBuilder
    private var launchAtLoginRow: some View {
        if let lal = appState.launchAtLogin {
            Toggle("Launch at login",
                   isOn: Binding(
                    get: { lal.isEnabled },
                    set: { newValue in lal.setEnabled(newValue) }))
            if lal.requiresApproval {
                requiresApprovalCallout
            }
            if let err = lal.lastError {
                errorCallout(err)
            }
        } else {
            // Pre-`load()` window — keep the toggle visible but inert
            // so the layout doesn't reflow when the controller spins up.
            Toggle("Launch at login", isOn: .constant(false))
                .disabled(true)
        }
    }

    private var requiresApprovalCallout: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 4) {
                Text("Waiting for approval in System Settings.")
                    .font(.callout)
                Button("Open Login Items…") {
                    openLoginItemsSettings()
                }
                .buttonStyle(.link)
            }
        }
    }

    private func errorCallout(_ err: LaunchAtLoginError) -> some View {
        let message: String
        switch err {
        case .registrationFailed(let s):    message = "Could not enable: \(s)"
        case .unregistrationFailed(let s):  message = "Could not disable: \(s)"
        }
        return HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "exclamationmark.octagon.fill")
                .foregroundStyle(.red)
            Text(message)
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    private func openLoginItemsSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }
}

/// Audio tab: resampler quality + an informational hint about the
/// HAL buffer size. Resampler quality is engine-facing and pushes
/// through to the `AudioConverterWrapper` constructor for newly-
/// started routes; HAL buffer is the user's interface-software job
/// (Phase 7.6 simplification — the engine no longer negotiates it).
struct AudioPreferencesView: View {
    let appState: AppState

    @AppStorage(JboxPreferences.resamplerQualityKey) private var resamplerRaw: Int
        = Int(Engine.ResamplerQuality.mastering.rawValue)

    private static let resamplerFooter =
        "Mastering is Apple's highest-fidelity SRC preset. High Quality "
        + "trades a small amount of SRC transparency for measurable CPU "
        + "savings on multi-route sessions. Changes apply to "
        + "newly-started routes — stop and start a running route to "
        + "re-build its converter."

    private static let bufferHintFooter =
        "JBox has no global HAL buffer-size setting. Per-route, the "
        + "Performance latency tier exposes a Buffer size preference "
        + "(written via kAudioDevicePropertyBufferFrameSize, no hog "
        + "mode); macOS resolves the actual buffer as the max across "
        + "all active clients. Set the device default in your "
        + "interface software (UA Console, RME TotalMix, MOTU CueMix, "
        + "Audio MIDI Setup); routes without a per-route preference "
        + "respect whatever the device is at."

    var body: some View {
        Form {
            resamplerSection
            bufferHintSection
        }
        .formStyle(.grouped)
        .padding()
        .onChange(of: resamplerRaw, initial: true) { _, newRaw in
            applyResampler(newRaw)
        }
    }

    private var bufferHintSection: some View {
        Section {
            // Read-only informational note. There is no global HAL
            // buffer-size preference; per-route, the Performance tier
            // exposes a Buffer size picker that writes
            // kAudioDevicePropertyBufferFrameSize (no hog mode).
            EmptyView()
        } header: {
            Text("Buffer size")
        } footer: {
            Text(Self.bufferHintFooter)
        }
    }

    private var resamplerSection: some View {
        Section {
            Picker("Quality", selection: $resamplerRaw) {
                Text("Mastering (default)")
                    .tag(Int(Engine.ResamplerQuality.mastering.rawValue))
                Text("High Quality (cheaper CPU)")
                    .tag(Int(Engine.ResamplerQuality.highQuality.rawValue))
            }
            .pickerStyle(.radioGroup)
        } header: {
            Text("Resampler quality")
        } footer: {
            Text(Self.resamplerFooter)
        }
    }

    /// Push the resampler-quality preference through to the engine.
    /// Idempotent: the engine stores an atomic and ignores repeat
    /// writes of the same value, so it is safe to run on every view
    /// refresh (we use `initial: true` so the first paint primes the
    /// engine if the user had a non-default stored preference from a
    /// previous launch).
    private func applyResampler(_ raw: Int) {
        guard let store = appState.store else { return }
        let clamped = UInt32(max(0, raw))
        let quality = Engine.ResamplerQuality(rawValue: clamped) ?? .mastering
        store.setResamplerQuality(quality)
    }
}

/// Advanced tab: diagnostics + Open Logs Folder. Export / Import /
/// Reset Configuration buttons are placeholders pending a wider
/// review of the import/export surface (see spec § 4.6).
struct AdvancedPreferencesView: View {
    @AppStorage(JboxPreferences.showDiagnosticsKey) private var showDiagnostics = false

    private static let diagnosticsFooter =
        "When on, the expanded route panel surfaces frame counters and "
        + "the per-component latency breakdown. Off by default — the "
        + "collapsed row stays calm."

    private static let logsFooter =
        "Rotating log files under ~/Library/Logs/Jbox land with Phase 8 "
        + "packaging. Until then, `Console.app` or `log stream "
        + "--predicate 'subsystem == \"com.jbox.app\"'` is the live "
        + "source."

    private static let configFooter =
        "Export / Import / Reset Configuration are not yet wired up — "
        + "pending an import/export surface review."

    var body: some View {
        Form {
            diagnosticsSection
            logsSection
            configSection
        }
        .formStyle(.grouped)
        .padding()
    }

    private var diagnosticsSection: some View {
        Section {
            Toggle("Show engine diagnostics", isOn: $showDiagnostics)
        } footer: {
            Text(Self.diagnosticsFooter)
        }
    }

    private var logsSection: some View {
        Section {
            Button("Open Logs Folder…", action: openLogsFolder)
        } header: {
            Text("Logs")
        } footer: {
            Text(Self.logsFooter)
        }
    }

    private var configSection: some View {
        Section {
            Button("Export Configuration…", action: {})
                .disabled(true)
            Button("Import Configuration…", action: {})
                .disabled(true)
            Button("Reset State…", action: {})
                .disabled(true)
        } header: {
            Text("Configuration")
        } footer: {
            Text(Self.configFooter)
        }
    }

    /// Opens the Jbox log directory in Finder. Creates the directory
    /// on demand so the reveal always succeeds; the rotating file sink
    /// itself (Phase 8) will populate it once it lands. Falls back to
    /// `~/Library/Logs` when the Jbox-scoped path can't be resolved.
    private func openLogsFolder() {
        let fm = FileManager.default
        let libraryLogs = fm.urls(for: .libraryDirectory, in: .userDomainMask)
            .first?.appendingPathComponent("Logs", isDirectory: true)
        let jboxLogs = libraryLogs?.appendingPathComponent("Jbox", isDirectory: true)
        if let jboxLogs {
            try? fm.createDirectory(at: jboxLogs,
                                    withIntermediateDirectories: true)
            NSWorkspace.shared.open(jboxLogs)
            return
        }
        if let libraryLogs {
            NSWorkspace.shared.open(libraryLogs)
        }
    }
}

/// Owner for the `EngineStore`, the init-error slot, and the
/// `StateStore` that persists `StoredAppState` to disk (docs/spec.md
/// § 3.2). Shared between the main window and the menu bar extra via
/// SwiftUI's `@State` propagation — both scenes see the same reference,
/// so either one can drive `load()` the first time it appears.
///
/// Persistence flow:
///   1. `load()` reads `state.json`, falls back to `.bak` on missing,
///      and migrates any `UserDefaults`-only preferences on first run.
///   2. Preferences come through `@AppStorage` bindings in the views;
///      changes fire `UserDefaults.didChangeNotification`, which this
///      class observes and reflects back into `persisted.preferences`
///      before queuing a debounced save.
///   3. Route mutations go through `EngineStore.onRoutesChanged`, which
///      triggers a re-snapshot of `store.routes` into `persisted.routes`
///      followed by a debounced save.
///   4. `flush()` forces any pending debounced save to disk on app
///      shutdown.
@MainActor
@Observable
final class AppState {
    var store: EngineStore?
    var initError: String?

    /// Latest durable state mirrored into memory. Source of truth for
    /// what gets persisted on the next save tick; views continue to
    /// read preferences from `@AppStorage`, which this class keeps in
    /// lockstep with the struct below.
    private(set) var persisted = StoredAppState()

    /// Owns the user-facing "Launch at login" state. Built in `load()`
    /// once we have the persisted `hasShownLaunchAtLoginNote` latch and
    /// can register the persistence callback. `nil` only during the
    /// pre-`load()` window.
    private(set) var launchAtLogin: LaunchAtLoginController?

    /// Service injection seam. Tests / SwiftUI previews can substitute
    /// a `FakeLaunchAtLoginService`-shaped dependency by overriding
    /// before `load()`. Production sets it implicitly to
    /// `SMAppServiceLaunchAtLogin()` on first use.
    var launchAtLoginServiceFactory: () -> LaunchAtLoginService = { SMAppServiceLaunchAtLogin() }

    private var stateStore: StateStore?
    private var userDefaultsObserver: NSObjectProtocol?

    func load() {
        guard store == nil, initError == nil else { return }
        JboxLog.app.notice("JboxApp starting")

        let ss = try? StateStore(directory: StateStore.defaultDirectory())
        if ss == nil {
            JboxLog.app.error("state store init failed — running without persistence")
        }
        self.stateStore = ss

        let loaded: StoredAppState?
        do {
            loaded = try ss?.load()
        } catch let StateStore.LoadError.schemaTooNew(file, supported) {
            initError = "state.json was written by a newer Jbox (\(file) > \(supported)). Refusing to downgrade."
            JboxLog.app.error("refusing to load state.json: fileVersion=\(file) supported=\(supported)")
            return
        } catch {
            JboxLog.app.error("state.json load failed: \(String(describing: error), privacy: .public). Starting with defaults.")
            loaded = nil
        }

        if let loaded {
            persisted = loaded
            // Push loaded preferences into UserDefaults so @AppStorage
            // bindings observe them on first paint.
            writePreferencesIntoDefaults(loaded.preferences)
        } else {
            // First-run / fresh install: migrate whatever the user had
            // set via @AppStorage (pre-Phase-7) so their preferences
            // don't reset on upgrade.
            persisted.preferences = readPreferencesFromDefaults()
            saveNow()
        }

        do {
            let s = try EngineStore()
            s.setResamplerQuality(persisted.preferences.resamplerQuality)
            s.refreshDevices()
            restoreRoutes(into: s)
            s.onRoutesChanged = { [weak self] in
                self?.snapshotRoutes()
            }
            store = s
            JboxLog.app.notice("store ready devices=\(s.devices.count) routes=\(s.routes.count)")
        } catch {
            initError = String(describing: error)
            JboxLog.app.error("engine init failed: \(String(describing: error), privacy: .public)")
            return
        }

        // Watch @AppStorage changes so the state file tracks the user's
        // preferences edits. `UserDefaults.didChangeNotification` fires
        // for any key change in the standard suite; we filter to the
        // jbox-prefixed keys and re-snapshot only when one of those
        // actually differs. StateStore's debounce absorbs the chatter.
        userDefaultsObserver = NotificationCenter.default.addObserver(
            forName: UserDefaults.didChangeNotification,
            object: UserDefaults.standard,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.snapshotPreferences() }
        }

        // Build the launch-at-login controller now that `persisted` is
        // populated. The factory closure is overridable for previews;
        // production lands at `SMAppServiceLaunchAtLogin`.
        let lalController = LaunchAtLoginController(
            service: launchAtLoginServiceFactory(),
            hasShownFirstTimeNote: persisted.preferences.hasShownLaunchAtLoginNote)
        // Assign FIRST so that any persistence callback fired by the
        // refresh+snapshot pair below can find the controller via the
        // weak `self` reference. Order matters: with the assignment
        // after refresh(), a callback firing during refresh() would
        // observe `launchAtLogin == nil` and silently no-op.
        self.launchAtLogin = lalController
        lalController.onPersistableChange = { [weak self] in
            self?.snapshotLaunchAtLogin()
        }
        // Reconcile in-memory state with the live system status (covers
        // out-of-band changes via System Settings while Jbox was
        // closed), then align persisted state with whatever the
        // controller now reflects. Without the explicit
        // `snapshotLaunchAtLogin()`, a stale `state.json`
        // `launchAtLogin` mirror (e.g., "true" persisted but the system
        // says `.notRegistered`) would survive until the next user
        // action; refresh() alone only fires the callback when
        // `isEnabled` actually changed during the refresh, not when
        // the persisted mirror was already wrong on entry.
        lalController.refresh()
        snapshotLaunchAtLogin()
    }

    /// Force any pending debounced save synchronously. Called from the
    /// SwiftUI scene phase hook on `.background` transitions so
    /// mutations parked in the debounce window don't get lost when the
    /// app quits.
    func flush() {
        stateStore?.flush()
    }

    // `AppState` is retained by SwiftUI for the whole process
    // lifetime, so there's no teardown to write here; the observer is
    // cleaned up automatically when the app exits.

    // MARK: - Restore on launch

    private func restoreRoutes(into store: EngineStore) {
        for sr in persisted.routes {
            let cfg = RouteConfig(
                source: sr.sourceDevice,
                destination: sr.destDevice,
                mapping: sr.mapping,
                name: sr.isAutoName ? nil : sr.name,
                latencyMode: sr.latencyMode,
                bufferFrames: sr.bufferFrames)
            do {
                let route = try store.addRoute(cfg,
                                               persistId: sr.id,
                                               createdAt: sr.createdAt)
                // Replay the persisted gain state through the EngineStore
                // setters so the route resumes at the user's last fader
                // / trim / mute settings instead of unity / unmuted.
                if sr.masterGainDb != 0 {
                    store.setMasterGainDb(routeId: route.id, db: sr.masterGainDb)
                }
                for (i, db) in sr.trimDbs.enumerated()
                where i < sr.mapping.count && db != 0 {
                    store.setChannelTrimDb(routeId: route.id,
                                           channelIndex: i, db: db)
                }
                if sr.muted {
                    store.setRouteMuted(routeId: route.id, muted: true)
                }
                for (i, m) in sr.channelMuted.enumerated()
                where i < sr.mapping.count && m {
                    store.setChannelMuted(routeId: route.id,
                                          channelIndex: i, muted: true)
                }
                JboxLog.app.notice("restored route persistId=\(sr.id.uuidString, privacy: .public)")
            } catch {
                JboxLog.app.error("failed to restore route \(sr.id.uuidString, privacy: .public): \(String(describing: error), privacy: .public)")
            }
        }
    }

    // MARK: - Preferences sync (UserDefaults ↔ StoredPreferences)

    private func readPreferencesFromDefaults() -> StoredPreferences {
        let d = UserDefaults.standard
        let appearanceRaw = d.string(forKey: JboxPreferences.appearanceKey) ?? AppearanceMode.default.rawValue
        let resamplerRaw  = UInt32(max(0, d.integer(forKey: JboxPreferences.resamplerQualityKey)))
        return StoredPreferences(
            resamplerQuality: Engine.ResamplerQuality(rawValue: resamplerRaw) ?? .mastering,
            appearance: AppearanceMode(rawValueOrDefault: appearanceRaw),
            showDiagnostics: d.bool(forKey: JboxPreferences.showDiagnosticsKey))
    }

    private func writePreferencesIntoDefaults(_ p: StoredPreferences) {
        let d = UserDefaults.standard
        d.set(p.appearance.rawValue,        forKey: JboxPreferences.appearanceKey)
        d.set(Int(p.resamplerQuality.rawValue),  forKey: JboxPreferences.resamplerQualityKey)
        d.set(p.showDiagnostics,            forKey: JboxPreferences.showDiagnosticsKey)
    }

    private func snapshotPreferences() {
        let fresh = readPreferencesFromDefaults()
        // Preserve the launch-at-login fields — those don't ride
        // `UserDefaults`; the controller's callback owns them.
        var merged = fresh
        merged.launchAtLogin             = persisted.preferences.launchAtLogin
        merged.hasShownLaunchAtLoginNote = persisted.preferences.hasShownLaunchAtLoginNote
        guard merged != persisted.preferences else { return }
        persisted.preferences = merged
        // Keep engine-facing settings aligned with the new preferences
        // so a resampler-quality change takes effect on newly-started
        // routes without needing another code path.
        store?.setResamplerQuality(merged.resamplerQuality)
        scheduleSave()
    }

    /// Mirror the launch-at-login controller's persisted booleans into
    /// `persisted.preferences` and queue a save. Called via
    /// `LaunchAtLoginController.onPersistableChange`.
    private func snapshotLaunchAtLogin() {
        guard let lal = launchAtLogin else { return }
        var fresh = persisted.preferences
        fresh.launchAtLogin             = lal.isEnabled
        fresh.hasShownLaunchAtLoginNote = lal.hasShownFirstTimeNote
        guard fresh != persisted.preferences else { return }
        persisted.preferences = fresh
        scheduleSave()
    }

    // MARK: - Route snapshot

    private func snapshotRoutes() {
        guard let store = store else { return }

        persisted.routes = store.routes.map { r in
            let trimmedName = r.config.name?.trimmingCharacters(in: .whitespaces) ?? ""
            return StoredRoute(
                id: r.persistId,
                name: trimmedName.isEmpty ? r.config.displayName : trimmedName,
                isAutoName: trimmedName.isEmpty,
                sourceDevice: r.config.source,
                destDevice:   r.config.destination,
                mapping: r.config.mapping,
                createdAt: r.createdAt,
                modifiedAt: r.modifiedAt,
                latencyMode: r.config.latencyMode,
                bufferFrames: r.config.bufferFrames,
                masterGainDb: r.masterGainDb,
                trimDbs: r.trimDbs,
                muted: r.muted,
                channelMuted: r.channelMuted)
        }
        scheduleSave()
    }

    // MARK: - Save

    private func scheduleSave() {
        stateStore?.save(persisted)
    }

    private func saveNow() {
        stateStore?.save(persisted)
        stateStore?.flush()
    }
}

/// Top-level view that owns the `EngineStore`. Handles the three
/// startup states: loading the engine, engine-create failure, and
/// ready. SwiftUI previews bypass this by instantiating
/// `RouteListView` with a test store directly.
struct AppRootView: View {
    let appState: AppState

    var body: some View {
        Group {
            if let store = appState.store {
                RouteListView(store: store)
            } else if let initError = appState.initError {
                ContentUnavailableView {
                    Label("Engine failed to start", systemImage: "exclamationmark.triangle")
                } description: {
                    Text(initError)
                }
            } else {
                ProgressView("Starting engine…")
            }
        }
        .frame(minWidth: MixerPanelLayout.minWindowSize.width,
               minHeight: MixerPanelLayout.minWindowSize.height)
        .task {
            appState.load()
        }
    }
}

/// Root view of the `MenuBarExtra` popover. Handles the same three
/// startup states the main window handles, so the user can't end up
/// staring at an empty menu bar after a failed engine init.
struct MenuBarScene: View {
    let appState: AppState
    let mainWindowId: String

    var body: some View {
        Group {
            if let store = appState.store {
                MenuBarContent(store: store, mainWindowId: mainWindowId)
            } else if let err = appState.initError {
                menuBarErrorState(message: err)
            } else {
                menuBarLoadingState
            }
        }
        .task {
            appState.load()
        }
    }

    private var menuBarLoadingState: some View {
        VStack(spacing: 8) {
            ProgressView()
            Text("Starting engine…")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding()
        .frame(width: 240)
    }

    private func menuBarErrorState(message: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Engine failed to start", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
                .font(.headline)
            Text(message)
                .font(.caption)
                .foregroundStyle(.secondary)
            Button("Quit Jbox") {
                NSApp.terminate(nil)
            }
            .frame(maxWidth: .infinity)
        }
        .padding(12)
        .frame(width: 260)
    }
}

/// The menu bar icon. A monochrome route glyph (three horizontal
/// tracks between two columns of dots — an echo of the app icon,
/// simplified for 18 pt) with a small colored status dot composited
/// in the bottom-right corner: absent when idle, green when any route
/// is running, red when any route needs attention.
///
/// `MenuBarExtra`'s label accepts only simple leaf views (`Text` or
/// `Image`), so the icon is pre-rendered as one composite `NSImage`.
/// The glyph is drawn with `NSColor.labelColor` — a dynamic color
/// that resolves to the menu bar's text color at draw time, so
/// light / dark adaptation works automatically without the template
/// flag (we can't mark as template because that would tint the
/// colored status dot monochrome).
struct MenuBarIconLabel: View {
    let store: EngineStore?

    var body: some View {
        Image(nsImage: MenuBarIconRenderer.image(for: overallState))
            .accessibilityLabel(accessibilityLabel)
    }

    private var overallState: OverallState {
        store?.overallState ?? .idle
    }

    private var accessibilityLabel: String {
        guard let store else { return "JBox, starting" }
        switch store.overallState {
        case .idle:      return "JBox, all routes stopped"
        case .running:
            let n = store.runningRouteCount
            if n == 0 { return "JBox, routes starting" }
            return n == 1 ? "JBox, 1 route running" : "JBox, \(n) routes running"
        case .attention: return "JBox, attention needed"
        }
    }
}

/// Produces the menu bar icon NSImage for a given `OverallState`.
/// Drawing runs in the AppKit drawing context at render time, so
/// dynamic colors like `NSColor.labelColor` evaluate against the
/// menu bar's current appearance (light / dark).
enum MenuBarIconRenderer {
    /// Menu bar template images are 18 × 18 pt by HIG convention.
    /// We render into a 20 × 20 canvas so the status dot can sit in
    /// the bottom-right corner with its halo fitting cleanly inside
    /// the canvas (see `haloInset` below).
    static let canvasSize: CGFloat = 20
    static let glyphInset: CGFloat = 1
    static let dotDiameter: CGFloat = 7
    /// Radius of the transparent "halo" cut out of the glyph behind
    /// the status dot so the glyph's crossing lines don't visually
    /// run through the colored circle. Also sets the dot's position:
    /// the halo is anchored to the canvas's bottom-right corner, so
    /// `cx = canvasSize − haloRadius`, `cy = haloRadius`.
    static let haloInset: CGFloat = 1.2

    static func image(for state: OverallState) -> NSImage {
        let size = NSSize(width: canvasSize, height: canvasSize)
        let image = NSImage(size: size, flipped: false) { rect in
            let glyphRect = rect.insetBy(dx: glyphInset, dy: glyphInset)
            drawGlyph(in: glyphRect)
            if let color = dotColor(for: state) {
                drawStatusDot(color: color, in: rect)
            }
            return true
        }
        // Don't mark as template — template tinting would also
        // monochromatize the colored status dot. `NSColor.labelColor`
        // handles light/dark adaptation for the glyph instead.
        image.isTemplate = false
        return image
    }

    private static func dotColor(for state: OverallState) -> NSColor? {
        switch state {
        case .idle:      return nil
        case .running:   return .systemGreen
        case .attention: return .systemRed
        }
    }

    private static func drawGlyph(in rect: CGRect) {
        let s = min(rect.width, rect.height)
        let pad = s * 0.16
        let dotR = s * 0.09
        let stroke = s * 0.09

        // Center a square s×s glyph inside whatever rect we got,
        // then place the six endpoints inside that square.
        let glyph = CGRect(x: rect.midX - s / 2,
                           y: rect.midY - s / 2,
                           width: s, height: s)
        let leftX  = glyph.minX + pad
        let rightX = glyph.maxX - pad
        // AppKit uses a bottom-left origin (flipped: false above).
        let topY   = glyph.maxY - pad
        let midY   = glyph.midY
        let botY   = glyph.minY + pad

        NSColor.labelColor.setFill()
        NSColor.labelColor.setStroke()

        // Three lines first: top straight, middle and bottom crossed.
        // Drawn before the dots so the round-cap endpoints get hidden
        // under the filled dots — the dots read as clean "stations"
        // at each route endpoint, matching the app icon.
        let lines = NSBezierPath()
        lines.lineWidth = stroke
        lines.lineCapStyle  = .round
        lines.lineJoinStyle = .round
        lines.move(to: NSPoint(x: leftX,  y: topY))
        lines.line(to: NSPoint(x: rightX, y: topY))
        lines.move(to: NSPoint(x: leftX,  y: midY))
        lines.line(to: NSPoint(x: rightX, y: botY))
        lines.move(to: NSPoint(x: leftX,  y: botY))
        lines.line(to: NSPoint(x: rightX, y: midY))
        lines.stroke()

        // Six endpoint dots, filled on top.
        for x in [leftX, rightX] {
            for y in [topY, midY, botY] {
                let r = NSRect(x: x - dotR, y: y - dotR,
                               width: dotR * 2, height: dotR * 2)
                NSBezierPath(ovalIn: r).fill()
            }
        }
    }

    private static func drawStatusDot(color: NSColor, in rect: CGRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }

        let r = dotDiameter / 2
        let haloRadius = r + haloInset
        // Anchor the halo to the bottom-right corner so its edges
        // touch the canvas boundary instead of clipping past it.
        let cx = rect.maxX - haloRadius
        let cy = rect.minY + haloRadius
        let haloRect = CGRect(x: cx - haloRadius, y: cy - haloRadius,
                              width: haloRadius * 2, height: haloRadius * 2)
        let dotRect  = CGRect(x: cx - r, y: cy - r,
                              width: dotDiameter, height: dotDiameter)

        // Punch a transparent hole through the glyph we just drew.
        // `CGBlendMode.clear` erases any existing pixels inside the
        // halo — the menu bar's chrome (translucent or solid) shows
        // through, instead of a visible disc baked into the image.
        ctx.saveGState()
        ctx.setBlendMode(.clear)
        ctx.fillEllipse(in: haloRect)
        ctx.restoreGState()

        // Paint the colored dot over the punched-out hole.
        color.setFill()
        NSBezierPath(ovalIn: dotRect).fill()
    }
}
