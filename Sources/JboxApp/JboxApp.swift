import SwiftUI
import JboxEngineSwift

@main
struct JboxApp: App {
    var body: some Scene {
        WindowGroup("Jbox") {
            ContentView()
        }
        .windowResizability(.contentMinSize)
    }
}

struct ContentView: View {
    var body: some View {
        VStack(spacing: 16) {
            Text("Jbox")
                .font(.largeTitle)
            Text("Phase 1 scaffolding — UI lands in Phase 6")
                .foregroundStyle(.secondary)
            Text("Engine ABI version: \(JboxEngine.abiVersion)")
                .font(.footnote)
                .foregroundStyle(.tertiary)
        }
        .frame(minWidth: 800, minHeight: 500)
        .padding()
    }
}
