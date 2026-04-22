import SwiftUI
import AppKit
import JboxEngineSwift

/// Popover content for the menu bar extra (spec.md § 4.2). Displays
/// a one-line summary, a row per route with a start/stop toggle and
/// status glyph, a placeholder for the Phase 7 scene picker, and
/// bulk Start All / Stop All / Open Jbox / Preferences / Quit
/// actions. "No deep editing from the menu bar" — see spec.md § 4.2.
///
/// A lightweight status-poll task drives icon updates while the main
/// window is closed: the window-style MenuBarExtra keeps this view
/// alive for the app's lifetime, so the `.task` runs even when the
/// popover is not visible.
struct MenuBarContent: View {
    let store: EngineStore
    let mainWindowId: String

    @Environment(\.openWindow) private var openWindow
    @Environment(\.openSettings) private var openSettings

    /// Polling cadence for route status while the app is alive. Slower
    /// than the main window's 4 Hz — the menu bar icon only needs to
    /// catch state transitions, not animate counters. The two polling
    /// loops are idempotent: both hop to `@MainActor` and both just
    /// overwrite `routes[i].status` with the same fresh values, so the
    /// redundancy is free in correctness terms.
    private static let statusInterval: Duration = .milliseconds(500)

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
                .padding(.horizontal, 12)
                .padding(.top, 10)
                .padding(.bottom, 6)

            Divider()

            routeListOrEmpty
                .padding(.horizontal, 8)
                .padding(.vertical, 6)

            Divider()

            scenePickerPlaceholder
                .padding(.horizontal, 12)
                .padding(.vertical, 6)

            Divider()

            bulkActions
                .padding(.horizontal, 12)
                .padding(.vertical, 6)

            Divider()

            appActions
                .padding(.horizontal, 12)
                .padding(.top, 6)
                .padding(.bottom, 10)
        }
        .frame(width: 320)
        .task {
            while !Task.isCancelled {
                if !store.routes.isEmpty {
                    store.pollStatuses()
                }
                try? await Task.sleep(for: Self.statusInterval)
            }
        }
    }

    // MARK: Header

    private var header: some View {
        let count = store.runningRouteCount
        let summary: String = {
            switch store.overallState {
            case .idle:      return "All routes stopped"
            case .running:
                // `.running` covers `.running` and `.starting`; the
                // count counts only `.running`. If every "running"
                // route is actually still in `.starting`, display a
                // progress blurb instead of "0 routes running".
                if count == 0 { return "Starting…" }
                return count == 1 ? "1 route running" : "\(count) routes running"
            case .attention: return "Attention needed"
            }
        }()

        return HStack(spacing: 8) {
            Image(systemName: headerIconName(for: store.overallState))
                .foregroundStyle(headerIconColor(for: store.overallState))
            VStack(alignment: .leading, spacing: 0) {
                Text("Jbox")
                    .font(.headline)
                Text(summary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
    }

    private func headerIconName(for state: OverallState) -> String {
        switch state {
        case .idle:      return "circle"
        case .running:   return "circle.fill"
        case .attention: return "exclamationmark.triangle.fill"
        }
    }

    private func headerIconColor(for state: OverallState) -> Color {
        switch state {
        case .idle:      return .secondary
        case .running:   return .green
        case .attention: return .red
        }
    }

    // MARK: Route list

    @ViewBuilder
    private var routeListOrEmpty: some View {
        if store.routes.isEmpty {
            Text("No routes configured")
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .center)
                .padding(.vertical, 12)
        } else {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(store.routes) { route in
                    MenuBarRouteRow(route: route, store: store)
                }
            }
        }
    }

    // MARK: Scene picker placeholder

    /// Scenes land in Phase 7 (persistence + activation logic). The
    /// menu bar keeps a disabled placeholder so the layout and user
    /// model are stable when scenes arrive — the picker will drop
    /// into this slot and inherit its styling.
    private var scenePickerPlaceholder: some View {
        HStack {
            Text("Scene")
                .foregroundStyle(.secondary)
            Spacer()
            Text("None yet")
                .foregroundStyle(.tertiary)
        }
        .font(.callout)
        .help("Scenes land with persistence in Phase 7.")
    }

    // MARK: Bulk actions

    private var bulkActions: some View {
        HStack(spacing: 8) {
            Button {
                store.startAll()
            } label: {
                Text("Start All")
                    .frame(maxWidth: .infinity)
            }
            .disabled(!canStartAny)

            Button {
                store.stopAll()
            } label: {
                Text("Stop All")
                    .frame(maxWidth: .infinity)
            }
            .disabled(!canStopAny)
        }
        .controlSize(.regular)
    }

    private var canStartAny: Bool {
        store.routes.contains {
            $0.status.state == .stopped || $0.status.state == .error
        }
    }

    private var canStopAny: Bool {
        store.routes.contains {
            $0.status.state == .running ||
            $0.status.state == .starting ||
            $0.status.state == .waiting
        }
    }

    // MARK: App actions

    private var appActions: some View {
        VStack(alignment: .leading, spacing: 4) {
            Button {
                openOrRaiseMainWindow()
            } label: {
                menuRowLabel("Open Jbox")
            }
            .buttonStyle(.plain)

            Button {
                NSApp.activate(ignoringOtherApps: true)
                openSettings()
            } label: {
                menuRowLabel("Preferences…")
            }
            .buttonStyle(.plain)

            Button {
                NSApp.terminate(nil)
            } label: {
                menuRowLabel("Quit Jbox")
            }
            .buttonStyle(.plain)
        }
    }

    /// Front-raises an existing main window instead of opening a new
    /// one. `openWindow(id:)` on a default `WindowGroup` always creates
    /// a fresh instance, which means every menu-bar click spawns
    /// another copy. The AppKit side of the scene machinery is the only
    /// place we can cheaply ask "is a window already up?" — we filter
    /// `NSApp.windows` down to our titled main window (the menu bar
    /// popover is an `NSPanel` and is skipped; Settings carries a
    /// different title). If we find one, we deminiaturize it if needed
    /// and bring it key-and-front; only when nothing matches do we
    /// fall through to `openWindow(id:)`. Stopping SwiftUI from
    /// creating more than one instance *at all* (⌘N, repeated
    /// `openWindow(id:)` calls, etc.) is a separate follow-up.
    private func openOrRaiseMainWindow() {
        NSApp.activate(ignoringOtherApps: true)
        if let existing = NSApp.windows.first(where: isMainWindow) {
            if existing.isMiniaturized {
                existing.deminiaturize(nil)
            }
            existing.makeKeyAndOrderFront(nil)
            return
        }
        openWindow(id: mainWindowId)
    }

    private func isMainWindow(_ window: NSWindow) -> Bool {
        // Skip panels — the menu bar extra's popover is one, as are
        // various AppKit utility windows we don't want to target.
        guard !(window is NSPanel) else { return false }
        // `WindowGroup("Jbox", id: "main")` titles its instance "Jbox".
        // Settings window carries its own title; status popovers have
        // empty titles.
        return window.title == "Jbox"
    }

    private func menuRowLabel(_ title: String) -> some View {
        Text(title)
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
    }
}

/// One row per route in the menu bar popover. Shows a status glyph,
/// the route's display name, and a Start/Stop button. Row taps aren't
/// wired to anything (spec § 4.2 — "no deep editing from the menu
/// bar"); the button is the only control.
struct MenuBarRouteRow: View {
    let route: Route
    let store: EngineStore

    var body: some View {
        HStack(spacing: 8) {
            StatusGlyph(state: route.status.state)
                .imageScale(.small)
            VStack(alignment: .leading, spacing: 0) {
                Text(route.config.displayName)
                    .font(.callout)
                    .lineLimit(1)
                    .truncationMode(.tail)
                if let detail = detailText {
                    Text(detail)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            actionButton
                .controlSize(.small)
        }
        .padding(.vertical, 2)
        .padding(.horizontal, 4)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(accessibilityLabel)
    }

    private var detailText: String? {
        switch route.status.state {
        case .waiting: return "waiting for device"
        case .error:   return "error — see main window"
        default:       return nil
        }
    }

    @ViewBuilder
    private var actionButton: some View {
        switch route.status.state {
        case .running, .starting, .waiting:
            Button("Stop") { store.stopRoute(route.id) }
        case .stopped, .error:
            Button("Start") { store.startRoute(route.id) }
        }
    }

    private var accessibilityLabel: String {
        let stateWord: String
        switch route.status.state {
        case .running:  stateWord = "running"
        case .starting: stateWord = "starting"
        case .waiting:  stateWord = "waiting for device"
        case .stopped:  stateWord = "stopped"
        case .error:    stateWord = "error"
        }
        return "\(route.config.displayName), \(stateWord)"
    }
}
