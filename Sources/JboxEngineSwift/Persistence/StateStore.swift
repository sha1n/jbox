import Foundation

/// Durable store for `StoredAppState` backed by a JSON file on disk
/// (docs/spec.md § 3.2). Thread-safe: public entry points marshal
/// work onto a private serial queue so concurrent `save()` calls from
/// different actors are serialised without locking the caller.
///
/// Debounce semantics: successive `save()` calls within the debounce
/// window are coalesced — only the most-recently-queued state is
/// written when the timer fires. `flush()` forces the pending write
/// through synchronously (used on shutdown so a mutation parked in
/// the debounce window is not lost).
///
/// File layout inside `directory`:
///   - `state.json`      — current state, pretty-printed JSON.
///   - `state.json.bak`  — previous state, one generation.
///   - `state.json.tmp`  — atomic-write scratch file; never persists.
public final class StateStore: @unchecked Sendable {

    public enum LoadError: Error, Equatable {
        /// The file existed but failed JSON decoding.
        case fileCorrupt(String)
        /// The file's schemaVersion is greater than this build knows.
        /// The user likely downgraded the app; refuse to load to avoid
        /// silently dropping fields we cannot represent.
        case schemaTooNew(fileVersion: Int, supportedVersion: Int)
    }

    public let directory: URL
    public let debounceSeconds: Double

    public var stateFileURL:  URL { directory.appendingPathComponent("state.json") }
    public var backupFileURL: URL { directory.appendingPathComponent("state.json.bak") }
    public var tempFileURL:   URL { directory.appendingPathComponent("state.json.tmp") }

    private let queue: DispatchQueue
    private var pendingState: StoredAppState?
    private var pendingItem: DispatchWorkItem?
    private let queueSpecificKey = DispatchSpecificKey<Void>()

    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    public init(directory: URL, debounceSeconds: Double = 0.5) throws {
        self.directory = directory
        self.debounceSeconds = debounceSeconds
        self.queue = DispatchQueue(label: "com.jbox.state-store")
        self.queue.setSpecific(key: queueSpecificKey, value: ())

        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        self.encoder = enc

        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        self.decoder = dec

        try FileManager.default.createDirectory(
            at: directory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o755])
    }

    // MARK: - Load

    /// Returns `nil` on first run (no state.json, no state.json.bak).
    /// Falls back to the backup when state.json is missing but the
    /// backup is present (mirrors a crash after the main-rename step
    /// but before the tmp-promotion step). Throws `LoadError` on
    /// malformed JSON or a forward-schema mismatch.
    public func load() throws -> StoredAppState? {
        let fm = FileManager.default
        let primaryExists = fm.fileExists(atPath: stateFileURL.path)
        let backupExists  = fm.fileExists(atPath: backupFileURL.path)

        if primaryExists {
            return try decodeFile(at: stateFileURL)
        }
        if backupExists {
            return try decodeFile(at: backupFileURL)
        }
        return nil
    }

    private func decodeFile(at url: URL) throws -> StoredAppState {
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            throw LoadError.fileCorrupt("read failed: \(error)")
        }

        // Peek at schemaVersion first — if it's ahead of this build,
        // we cannot safely decode fields we don't know about.
        if let versionProbe = try? decoder.decode(SchemaProbe.self, from: data),
           versionProbe.schemaVersion > StoredAppState.currentSchemaVersion {
            throw LoadError.schemaTooNew(
                fileVersion: versionProbe.schemaVersion,
                supportedVersion: StoredAppState.currentSchemaVersion)
        }

        do {
            return try decoder.decode(StoredAppState.self, from: data)
        } catch {
            throw LoadError.fileCorrupt(String(describing: error))
        }
    }

    private struct SchemaProbe: Decodable {
        let schemaVersion: Int
    }

    // MARK: - Save (debounced)

    /// Queue a save of `state`. Multiple calls within `debounceSeconds`
    /// coalesce into a single on-disk write carrying the most recent
    /// state. Synchronous when `debounceSeconds == 0` (useful in tests).
    public func save(_ state: StoredAppState) {
        queue.async { [weak self] in
            guard let self else { return }
            self.pendingState = state
            self.pendingItem?.cancel()

            if self.debounceSeconds <= 0 {
                self.writePendingLocked()
                return
            }

            let work = DispatchWorkItem { [weak self] in
                self?.writePendingLocked()
            }
            self.pendingItem = work
            self.queue.asyncAfter(
                deadline: .now() + self.debounceSeconds,
                execute: work)
        }
    }

    /// Force any pending debounced save through synchronously. Called
    /// from `applicationWillTerminate`-style hooks so a mutation
    /// parked inside the debounce window still hits disk.
    public func flush() {
        let onQueue = DispatchQueue.getSpecific(key: queueSpecificKey) != nil
        let work = {
            self.pendingItem?.cancel()
            self.pendingItem = nil
            if self.pendingState != nil {
                self.writePendingLocked()
            }
        }
        if onQueue { work() } else { queue.sync(execute: work) }
    }

    // MARK: - Atomic write (runs on `queue`)

    private func writePendingLocked() {
        guard let state = pendingState else { return }
        pendingState = nil
        pendingItem = nil

        let fm = FileManager.default
        let data: Data
        do {
            data = try encoder.encode(state)
        } catch {
            // Encode failures bubble up via the os_log sink — we can't
            // surface a throw from a background queue.
            JboxLog.app.error("state encode failed: \(String(describing: error), privacy: .public)")
            return
        }

        do {
            // Step 1: write new content to .tmp next to state.json and fsync
            // so the rename-over step swaps complete data, not a partial
            // buffer sitting in the page cache.
            try data.write(to: tempFileURL, options: [.atomic])

            // Step 2: if state.json already exists, move it into .bak
            // (replacing any previous .bak). Keeps a single-generation
            // backup as documented in spec § 3.2.
            if fm.fileExists(atPath: stateFileURL.path) {
                if fm.fileExists(atPath: backupFileURL.path) {
                    try fm.removeItem(at: backupFileURL)
                }
                try fm.moveItem(at: stateFileURL, to: backupFileURL)
            }

            // Step 3: promote .tmp over state.json (atomic on same volume).
            try fm.moveItem(at: tempFileURL, to: stateFileURL)
        } catch {
            JboxLog.app.error("state write failed: \(String(describing: error), privacy: .public)")
            // Best-effort cleanup so the next save starts clean.
            try? fm.removeItem(at: tempFileURL)
        }
    }
}

// MARK: - Default location

public extension StateStore {
    /// Resolve the spec-mandated default persistence directory
    /// (`~/Library/Application Support/Jbox`). Honors a `JBOX_STATE_DIR`
    /// environment variable so the dev workflow can isolate state away
    /// from the installed-app directory during testing.
    static func defaultDirectory() -> URL {
        if let override = ProcessInfo.processInfo.environment["JBOX_STATE_DIR"],
           !override.isEmpty {
            return URL(fileURLWithPath: override, isDirectory: true)
        }
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask).first
            ?? FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("Jbox", isDirectory: true)
    }
}
