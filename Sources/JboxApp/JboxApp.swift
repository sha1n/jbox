import SwiftUI
import JboxEngineSwift

@main
struct JboxApp: App {
    var body: some Scene {
        WindowGroup("Jbox") {
            AppRootView()
        }
        .windowResizability(.contentMinSize)
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
