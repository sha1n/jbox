import SwiftUI
import JboxEngineSwift

/// Main window: sidebar + route list. Owns the "Add Route" toolbar
/// button and drives a ~4 Hz polling loop that refreshes route state
/// while the view is on screen.
struct RouteListView: View {
    let store: EngineStore
    @State private var showingAddSheet = false

    /// How often to re-poll route statuses while the view is visible.
    /// 4 Hz is plenty for state transitions and counters.
    private static let pollInterval: Duration = .milliseconds(250)

    /// Meter polling cadence — ~30 Hz. Spec §4.5 lands on 30 Hz for the
    /// full bar meters in Slice B; we match it now so Slice A's dots
    /// feel live under transient signal and we don't have to retune
    /// when bars arrive.
    private static let meterInterval: Duration = .milliseconds(33)

    var body: some View {
        NavigationSplitView {
            List {
                Label("All Routes", systemImage: "arrow.triangle.turn.up.right.circle")
            }
            .navigationTitle("Jbox")
        } detail: {
            detailContent
                .task {
                    while !Task.isCancelled {
                        if !store.routes.isEmpty {
                            store.pollStatuses()
                        }
                        try? await Task.sleep(for: Self.pollInterval)
                    }
                }
                .task {
                    // Separate, faster loop for meters — at ~30 Hz this
                    // is one atomic-exchange-per-channel per running
                    // route, cheap even with many routes.
                    while !Task.isCancelled {
                        if store.routes.contains(where: { $0.status.state == .running }) {
                            store.pollMeters()
                        } else if !store.meters.isEmpty {
                            // Clear stale peaks when nothing is running
                            // so the UI doesn't hang on an old snapshot.
                            store.pollMeters()
                        }
                        try? await Task.sleep(for: Self.meterInterval)
                    }
                }
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
                if route.status.state == .running {
                    SignalDotRow(peaks: store.meters[route.id])
                }
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

/// Phase 6 Slice A signal indicator: one small dot per mapped channel
/// on each side of the route. Filled when the last polled peak for
/// that channel exceeded `MeterPeaks.signalThreshold`, outline when
/// silent. Absent peaks (route not yet polled) render as outlines —
/// nothing to show, but the channel slots are preserved so the row
/// height doesn't twitch mid-poll. Bar meters with dB thresholds land
/// in Slice B; see [spec.md § 4.5](../../docs/spec.md#45-meters).
struct SignalDotRow: View {
    let peaks: MeterPeaks?

    var body: some View {
        HStack(spacing: 6) {
            DotGroup(label: "in",  values: peaks?.source      ?? [])
            Image(systemName: "arrow.right")
                .font(.caption2)
                .foregroundStyle(.tertiary)
            DotGroup(label: "out", values: peaks?.destination ?? [])
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(accessibilityLabel)
    }

    private var accessibilityLabel: String {
        let src = peaks?.source ?? []
        let dst = peaks?.destination ?? []
        let srcActive = src.filter { $0 > MeterPeaks.signalThreshold }.count
        let dstActive = dst.filter { $0 > MeterPeaks.signalThreshold }.count
        return "Source signal on \(srcActive) of \(src.count); destination signal on \(dstActive) of \(dst.count)"
    }
}

private struct DotGroup: View {
    let label: String
    let values: [Float]

    var body: some View {
        HStack(spacing: 4) {
            Text(label)
                .font(.caption2)
                .foregroundStyle(.tertiary)
            if values.isEmpty {
                // Keep a sliver of space so the row doesn't collapse
                // vertically while the first poll is in flight.
                Text("—")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            } else {
                HStack(spacing: 3) {
                    ForEach(Array(values.enumerated()), id: \.offset) { _, v in
                        Dot(active: v > MeterPeaks.signalThreshold)
                    }
                }
            }
        }
    }
}

private struct Dot: View {
    let active: Bool

    var body: some View {
        Image(systemName: active ? "circle.fill" : "circle")
            .font(.system(size: 7))
            .foregroundStyle(active ? Color.green : Color.secondary.opacity(0.5))
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
