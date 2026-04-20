import Foundation
import os

/// Shared subsystem identifier used by every Jbox `Logger` instance.
/// Matches the engine-side `os_log_create("com.jbox.app", ...)` calls
/// so the whole app surfaces under one predicate in Console.app / the
/// `log` CLI (`log stream --predicate 'subsystem == "com.jbox.app"'`).
public enum JboxLog {
    public static let subsystem = "com.jbox.app"

    /// Control-plane errors and lifecycle events originating on the
    /// Swift side of the bridge (EngineStore, App entry point, etc.).
    public static let app    = Logger(subsystem: subsystem, category: "app")

    /// Engine-bridge error paths called by the Swift wrapper.
    public static let engine = Logger(subsystem: subsystem, category: "engine")

    /// SwiftUI view-layer events worth narrating (sheet presentation,
    /// validation failures the user surfaced but didn't submit, etc.).
    public static let ui     = Logger(subsystem: subsystem, category: "ui")
}
