import SwiftUI
import JboxEngineSwift

@main
struct JboxApp: App {
    var body: some Scene {
        WindowGroup("Jbox") {
            AppRootView()
        }
        .windowResizability(.contentMinSize)

        Settings {
            PreferencesView()
        }
    }
}

/// Default-storage key for the Advanced → "Show engine diagnostics"
/// toggle (Phase 6 refinement #4). Exported so views can read it via
/// `@AppStorage(JboxPreferences.showDiagnosticsKey)`. Persisted in
/// `NSUserDefaults` under this exact key until Phase 7 rolls its
/// own `AppState.preferences`; migration is a key-read away.
enum JboxPreferences {
    public static let showDiagnosticsKey = "com.jbox.showDiagnostics"
}

struct PreferencesView: View {
    var body: some View {
        TabView {
            AdvancedPreferencesView()
                .tabItem { Label("Advanced", systemImage: "wrench.and.screwdriver") }
        }
        .frame(width: 420, height: 180)
    }
}

struct AdvancedPreferencesView: View {
    @AppStorage(JboxPreferences.showDiagnosticsKey) private var showDiagnostics = false

    var body: some View {
        Form {
            Section {
                Toggle("Show engine diagnostics", isOn: $showDiagnostics)
            } footer: {
                Text("When on, the expanded route panel surfaces frame "
                     + "counters and the per-component latency breakdown. "
                     + "Off by default — the collapsed row stays calm.")
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}

/// Top-level view that owns the `EngineStore`. Handles the three
/// startup states: loading the engine, engine-create failure, and
/// ready. SwiftUI previews bypass this by instantiating
/// `RouteListView` with a test store directly.
struct AppRootView: View {
    @State private var store: EngineStore?
    @State private var initError: String?

    var body: some View {
        Group {
            if let store {
                RouteListView(store: store)
            } else if let initError {
                ContentUnavailableView {
                    Label("Engine failed to start", systemImage: "exclamationmark.triangle")
                } description: {
                    Text(initError)
                }
            } else {
                ProgressView("Starting engine…")
            }
        }
        .frame(minWidth: 820, minHeight: 520)
        .task {
            JboxLog.app.notice("JboxApp starting")
            do {
                let s = try EngineStore()
                s.refreshDevices()
                store = s
                JboxLog.app.notice("store ready devices=\(s.devices.count)")
            } catch {
                initError = String(describing: error)
                JboxLog.app.error("engine init failed: \(String(describing: error), privacy: .public)")
            }
        }
    }
}
