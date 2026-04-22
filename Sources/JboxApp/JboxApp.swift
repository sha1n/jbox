import SwiftUI
import AppKit
import JboxEngineSwift

@main
struct JboxApp: App {
    /// Single shared app model that owns the `EngineStore` and the
    /// init-error slot so both the main window and the menu bar extra
    /// see the same engine instance. Lives for the app's lifetime.
    @State private var appState = AppState()

    /// Window-group identifier used by the menu bar's "Open Jbox"
    /// action to reopen the main window after the user has closed it.
    private static let mainWindowId = "main"

    var body: some Scene {
        WindowGroup("Jbox", id: Self.mainWindowId) {
            AppRootView(appState: appState)
        }
        .windowResizability(.contentMinSize)

        Settings {
            PreferencesView()
        }

        MenuBarExtra {
            MenuBarScene(appState: appState, mainWindowId: Self.mainWindowId)
        } label: {
            MenuBarIconLabel(store: appState.store)
        }
        .menuBarExtraStyle(.window)
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

/// Owner for the `EngineStore` + the init-error slot. Shared between
/// the main window and the menu bar extra via SwiftUI's `@State`
/// propagation — both scenes see the same reference, so either one
/// can drive `load()` the first time it appears.
@MainActor
@Observable
final class AppState {
    var store: EngineStore?
    var initError: String?

    func load() {
        guard store == nil, initError == nil else { return }
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
        .frame(minWidth: 820, minHeight: 520)
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
        guard let store else { return "Jbox, starting" }
        switch store.overallState {
        case .idle:      return "Jbox, all routes stopped"
        case .running:
            let n = store.runningRouteCount
            if n == 0 { return "Jbox, routes starting" }
            return n == 1 ? "Jbox, 1 route running" : "Jbox, \(n) routes running"
        case .attention: return "Jbox, attention needed"
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
