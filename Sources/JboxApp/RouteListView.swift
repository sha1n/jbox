import SwiftUI
import JboxEngineSwift

/// Main window: sidebar + route list. Owns the "Add Route" toolbar
/// button; the sheet it presents is a placeholder in this commit and
/// gets a real editor in phase6 #3.
struct RouteListView: View {
    let store: EngineStore
    @State private var showingAddSheet = false

    var body: some View {
        NavigationSplitView {
            List {
                Label("All Routes", systemImage: "arrow.triangle.turn.up.right.circle")
            }
            .navigationTitle("Jbox")
        } detail: {
            detailContent
                .toolbar {
                    ToolbarItem(placement: .primaryAction) {
                        Button {
                            showingAddSheet = true
                        } label: {
                            Label("Add Route", systemImage: "plus")
                        }
                        .help("Create a new route")
                    }
                    ToolbarItem(placement: .automatic) {
                        Button {
                            store.refreshDevices()
                        } label: {
                            Label("Refresh devices", systemImage: "arrow.clockwise")
                        }
                        .help("Re-enumerate audio devices")
                    }
                }
                .sheet(isPresented: $showingAddSheet) {
                    AddRouteSheet(store: store) {
                        showingAddSheet = false
                    }
                }
                .alert(
                    "Engine error",
                    isPresented: .init(
                        get: { store.lastError != nil },
                        set: { if !$0 { /* no dismiss hook yet */ } }
                    )
                ) {
                    Button("OK", role: .cancel) {}
                } message: {
                    Text(store.lastError ?? "")
                }
        }
    }

    @ViewBuilder
    private var detailContent: some View {
        if store.routes.isEmpty {
            ContentUnavailableView {
                Label("No routes yet",
                      systemImage: "arrow.triangle.turn.up.right.circle")
            } description: {
                Text("Click + to send audio from one device's channels to another's.")
            }
        } else {
            List {
                ForEach(store.routes) { route in
                    RouteRow(route: route, store: store)
                        .padding(.vertical, 2)
                }
            }
            .listStyle(.inset)
        }
    }
}

// MARK: - Row

struct RouteRow: View {
    let route: Route
    let store: EngineStore

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            StatusGlyph(state: route.status.state)

            VStack(alignment: .leading, spacing: 2) {
                Text(route.config.displayName)
                    .font(.headline)
                Text(mappingSummary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if let errorText = errorText {
                    Text(errorText)
                        .font(.caption2)
                        .foregroundStyle(.red)
                }
            }

            Spacer()

            counterLine
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .help("frames produced / consumed / underruns")

            if route.status.state == .running || route.status.state == .starting {
                Button("Stop") { store.stopRoute(route.id) }
            } else {
                Button("Start") { store.startRoute(route.id) }
                    .disabled(route.status.state == .starting)
            }

            Button(role: .destructive) {
                store.removeRoute(route.id)
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .help("Remove route")
        }
    }

    private var mappingSummary: String {
        let n = route.config.mapping.count
        let channels = n == 1 ? "1 channel" : "\(n) channels"
        return "\(channels) — \(route.config.source.lastKnownName) → \(route.config.destination.lastKnownName)"
    }

    private var errorText: String? {
        guard route.status.state == .error else { return nil }
        return String(cString: jbox_error_code_name(route.status.lastError))
    }

    private var counterLine: some View {
        Text("\(route.status.framesProduced) / \(route.status.framesConsumed) · u\(route.status.underrunCount)")
    }
}

struct StatusGlyph: View {
    let state: RouteState

    var body: some View {
        Image(systemName: symbol)
            .foregroundStyle(color)
            .imageScale(.large)
            .help(label)
            .accessibilityLabel(label)
    }

    private var symbol: String {
        switch state {
        case .running:  return "circle.fill"
        case .starting: return "arrow.triangle.2.circlepath"
        case .waiting:  return "clock"
        case .stopped:  return "circle"
        case .error:    return "exclamationmark.triangle.fill"
        }
    }

    private var color: Color {
        switch state {
        case .running:  return .green
        case .starting: return .yellow
        case .waiting:  return .orange
        case .stopped:  return .secondary
        case .error:    return .red
        }
    }

    private var label: String {
        switch state {
        case .running:  return "Running"
        case .starting: return "Starting"
        case .waiting:  return "Waiting for device"
        case .stopped:  return "Stopped"
        case .error:    return "Error"
        }
    }
}
