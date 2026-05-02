import SwiftUI
import JboxEngineSwift

/// Main window: route list. Owns the "Add Route" toolbar button and
/// drives a ~4 Hz polling loop that refreshes route state while the
/// view is on screen.
struct RouteListView: View {
    let store: EngineStore
    @State private var showingAddSheet = false
    /// When non-nil, the edit sheet is presented for that route.
    @State private var editingRoute: Route? = nil

    @State private var expandedRoutes: Set<UInt32> = []

    /// How often to re-poll route statuses while the view is visible.
    /// 4 Hz is plenty for state transitions and counters.
    private static let pollInterval: Duration = .milliseconds(250)

    /// Meter polling cadence — ~30 Hz. Spec §4.5 lands on 30 Hz for the
    /// full bar meters in Slice B; we match it now so Slice A's dots
    /// feel live under transient signal and we don't have to retune
    /// when bars arrive.
    private static let meterInterval: Duration = .milliseconds(33)

    var body: some View {
        detailContent
            .navigationTitle("Jbox")
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
            .sheet(item: $editingRoute) { route in
                EditRouteSheet(route: route, store: store) {
                    editingRoute = nil
                }
            }
            .alert(
                "Engine error",
                isPresented: .init(
                    get: { store.lastError != nil },
                    set: { if !$0 { store.clearLastError() } }
                )
            ) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(store.lastError ?? "")
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
                    RouteRow(
                        route: route,
                        store: store,
                        expanded: expandedRoutes.contains(route.id),
                        onToggleExpanded: { toggleExpansion(route.id) },
                        onEditRequested: { editingRoute = route }
                    )
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                    .padding(.vertical, 4)
                }
                .onMove { offsets, destination in
                    store.moveRoute(from: offsets, to: destination)
                }
            }
            .listStyle(.inset)
        }
    }

    private func toggleExpansion(_ id: UInt32) {
        if expandedRoutes.contains(id) {
            expandedRoutes.remove(id)
        } else {
            expandedRoutes.insert(id)
        }
    }
}

// MARK: - Row

struct RouteRow: View {
    let route: Route
    let store: EngineStore
    let expanded: Bool
    let onToggleExpanded: () -> Void
    let onEditRequested: () -> Void

    /// Inline-rename state: when true, the display name becomes a
    /// TextField bound to `renameDraft`. Enter commits through the
    /// engine's non-disruptive rename API; Escape reverts.
    @State private var isRenaming = false
    @State private var renameDraft = ""
    @FocusState private var renameFieldFocused: Bool

    private var canExpand: Bool {
        route.status.state == .running
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 12) {
                Button(action: onToggleExpanded) {
                    // "Down" only when the row is BOTH expanded AND
                    // currently expandable. When the route flips to a
                    // non-RUNNING state the meter view disappears, so
                    // a down-pointing chevron would lie about the row's
                    // visual state. The user's `expanded` intent is
                    // preserved underneath: when the route returns to
                    // RUNNING the chevron flips back to down + meters
                    // reappear without an extra click.
                    let showOpenChevron = expanded && canExpand
                    Image(systemName: showOpenChevron ? "chevron.down" : "chevron.right")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(canExpand ? .secondary : .tertiary)
                        .frame(width: 28, height: 28)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!canExpand)
                .help(canExpand
                      ? (expanded ? "Collapse meter view" : "Expand meter view")
                      : "Meters are available while the route is running")

                StatusGlyph(state: route.status.state)

                VStack(alignment: .leading, spacing: 2) {
                    nameView
                    Text(mappingSummary)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if route.status.state == .running && !expanded {
                        SignalDotRow(peaks: store.meters[route.id])
                    }
                    if let errorText = errorText {
                        Text(errorText)
                            .font(.caption2)
                            .foregroundStyle(.red)
                    }
                }

                Spacer()

                if let latencyText = LatencyFormatter.pillText(
                    microseconds: route.status.estimatedLatencyUs) {
                    LatencyPill(text: latencyText)
                }

                if route.status.state == .running || route.status.state == .starting {
                    Button("Stop") { store.stopRoute(route.id) }
                } else {
                    Button("Start") { store.startRoute(route.id) }
                        .disabled(route.status.state == .starting)
                }

                Button {
                    onEditRequested()
                } label: {
                    Image(systemName: "pencil")
                }
                .buttonStyle(.borderless)
                .help("Edit route")

                Button(role: .destructive) {
                    store.removeRoute(route.id)
                } label: {
                    Image(systemName: "trash")
                }
                .buttonStyle(.borderless)
                .help("Remove route")
            }

            if expanded, route.status.state == .running,
               let peaks = store.meters[route.id] {
                Divider()
                    .padding(.top, 4)
                MeterPanel(route: route, store: store, peaks: peaks)
                    .padding(.bottom, 4)
                    .transition(.opacity.combined(with: .move(edge: .top)))
            }
        }
        .padding(.vertical, 12)
        .padding(.horizontal, 14)
        .background(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color(nsColor: .textBackgroundColor).opacity(0.35))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.secondary.opacity(0.25), lineWidth: 1)
        )
        .animation(.easeInOut(duration: 0.15), value: expanded)
    }

    private var mappingSummary: String {
        let n = route.config.mapping.count
        let channels = n == 1 ? "1 channel" : "\(n) channels"
        return "\(channels) — \(route.config.source.lastKnownName) → \(route.config.destination.lastKnownName)"
    }

    private var errorText: String? {
        routeRowErrorText(state: route.status.state,
                          lastError: route.status.lastError)
    }

    @ViewBuilder
    private var nameView: some View {
        if isRenaming {
            TextField("Route name", text: $renameDraft)
                .textFieldStyle(.roundedBorder)
                .font(.headline)
                .focused($renameFieldFocused)
                .onSubmit(commitRename)
                .onExitCommand(perform: cancelRename)
                .onChange(of: renameFieldFocused) { _, isFocused in
                    // Commit on defocus (standard Finder-style rename).
                    // Escape already flipped `isRenaming` to false so the
                    // stale focus change from the field being torn down
                    // is a no-op on that path.
                    if !isFocused && isRenaming {
                        commitRename()
                    }
                }
                .frame(maxWidth: 360)
        } else {
            Text(route.config.displayName)
                .font(.headline)
                .contentShape(Rectangle())
                .onTapGesture(count: 2, perform: startRename)
                .help("Double-click to rename")
        }
    }

    private func startRename() {
        // Seed the draft with the user's custom name, not displayName.
        // If `config.name` is nil we start with an empty field so the
        // user types a fresh label instead of editing the auto-generated
        // "source → destination" fallback.
        renameDraft = route.config.name ?? ""
        isRenaming = true
        DispatchQueue.main.async {
            renameFieldFocused = true
        }
    }

    private func commitRename() {
        let trimmed = renameDraft.trimmingCharacters(in: .whitespaces)
        // `""` clears the custom name and lets `displayName` fall back
        // to the auto "source → destination" label. Skip the engine
        // round-trip when nothing actually changed.
        let current = route.config.name ?? ""
        if trimmed != current {
            store.renameRoute(route.id, to: trimmed)
        }
        isRenaming = false
    }

    private func cancelRename() {
        isRenaming = false
    }
}

/// Faint rounded-rect pill showing the approximate end-to-end route
/// latency (spec.md § 2.12). The formatter already renders "~" and the
/// unit; this view is the visual wrapper only.
struct LatencyPill: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.caption2.monospacedDigit())
            .foregroundStyle(.secondary)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(Color.secondary.opacity(0.12))
            )
            .help("Estimated end-to-end latency (HAL + safety offset + "
                  + "buffers + ring + SRC). Indicative; some drivers "
                  + "under-report. See docs/spec.md § 2.12.")
            .accessibilityLabel("Estimated latency \(text)")
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

// MARK: - Previews

#if DEBUG
#Preview("RouteListView — populated") {
    RouteListView(store: PreviewFixtures.sampleStore())
        .frame(width: 900, height: 520)
}

#Preview("RouteListView — empty") {
    RouteListView(store: PreviewFixtures.emptyStore())
        .frame(width: 900, height: 360)
}

#Preview("RouteListView — engine error") {
    routeListErrorPreview()
}

@MainActor
private func routeListErrorPreview() -> some View {
    let running = PreviewFixtures.runningRoute()
    let store = EngineStore.preview(
        routes: [running, PreviewFixtures.stoppedRoute()],
        devices: PreviewFixtures.devices,
        meters: [running.id: PreviewFixtures.meters(channels: running.config.mapping.count)],
        latencyComponents: [running.id: PreviewFixtures.latency(for: running)],
        lastError: "Couldn't open destination device.")
    return RouteListView(store: store)
        .frame(width: 900, height: 520)
}

#Preview("RouteRow — running, collapsed") {
    routeRowPreview(route: PreviewFixtures.runningRoute(),
                    expanded: false,
                    withMeters: true)
}

#Preview("RouteRow — running, expanded") {
    routeRowPreview(route: PreviewFixtures.runningRoute(),
                    expanded: true,
                    withMeters: true)
}

#Preview("RouteRow — stopped") {
    routeRowPreview(route: PreviewFixtures.stoppedRoute(),
                    expanded: false,
                    withMeters: false)
}

#Preview("RouteRow — waiting") {
    routeRowPreview(route: PreviewFixtures.waitingRoute(),
                    expanded: false,
                    withMeters: false)
}

#Preview("RouteRow — starting") {
    routeRowPreview(route: PreviewFixtures.startingRoute(),
                    expanded: false,
                    withMeters: false)
}

#Preview("RouteRow — error") {
    routeRowPreview(route: PreviewFixtures.errorRoute(),
                    expanded: false,
                    withMeters: false)
}

#Preview("SignalDotRow — mixed signal") {
    SignalDotRow(peaks: PreviewFixtures.meters(channels: 4))
        .padding()
}

#Preview("SignalDotRow — no signal yet") {
    SignalDotRow(peaks: nil)
        .padding()
}

@MainActor
private func routeRowPreview(route: Route,
                             expanded: Bool,
                             withMeters: Bool) -> some View {
    let meters: [UInt32: MeterPeaks] = withMeters
        ? [route.id: PreviewFixtures.meters(channels: route.config.mapping.count)]
        : [:]
    let store = EngineStore.preview(
        routes: [route],
        devices: PreviewFixtures.devices,
        meters: meters,
        latencyComponents: withMeters
            ? [route.id: PreviewFixtures.latency(for: route)]
            : [:])
    return RouteRow(
        route: route,
        store: store,
        expanded: expanded,
        onToggleExpanded: {},
        onEditRequested: {})
        .padding()
        .frame(width: 800)
}
#endif
