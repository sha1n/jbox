import Foundation
import Testing
@testable import JboxEngineSwift

// Tests for the on-disk `StateStore` (docs/spec.md § 3.2).
// Each test uses an isolated temp directory so tests don't leak state
// and can run in parallel.

@Suite("StateStore")
struct StateStoreTests {

    private static func makeTempDir() -> URL {
        let base = FileManager.default.temporaryDirectory
            .appendingPathComponent("jbox-state-store-tests-\(UUID().uuidString)",
                                    isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    private static func cleanUp(_ dir: URL) {
        try? FileManager.default.removeItem(at: dir)
    }

    private static let tinyDebounce: Double = 0.0  // synchronous for tests

    // MARK: load() from empty

    @Test("load from empty directory returns nil (first run)")
    func loadEmptyReturnsNil() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        #expect(try store.load() == nil)
    }

    @Test("init creates the directory if it does not exist")
    func initCreatesDirectory() throws {
        let parent = Self.makeTempDir(); defer { Self.cleanUp(parent) }
        let dir = parent.appendingPathComponent("nested/Jbox", isDirectory: true)
        #expect(!FileManager.default.fileExists(atPath: dir.path))
        _ = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        var isDir: ObjCBool = false
        let exists = FileManager.default.fileExists(atPath: dir.path, isDirectory: &isDir)
        #expect(exists)
        #expect(isDir.boolValue)
    }

    // MARK: save()/load() round-trip

    @Test("save then load round-trips state")
    func saveLoadRoundTrip() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)

        let original = StoredAppState(
            routes: [StoredRoute(
                id: UUID(), name: "test", isAutoName: false,
                sourceDevice: DeviceReference(uid: "s", lastKnownName: "S"),
                destDevice:   DeviceReference(uid: "d", lastKnownName: "D"),
                mapping: [ChannelEdge(src: 0, dst: 0)],
                createdAt: Date(timeIntervalSince1970: 1_700_000_000),
                modifiedAt: Date(timeIntervalSince1970: 1_700_000_100),
                latencyMode: .performance, bufferFrames: 64)],
            preferences: StoredPreferences(resamplerQuality: .highQuality))

        store.save(original)
        store.flush()

        let loaded = try store.load()
        #expect(loaded == original)
    }

    @Test("save writes pretty-printed JSON diff-friendly for humans")
    func savedFileIsPretty() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        store.save(StoredAppState())
        store.flush()

        let data = try Data(contentsOf: store.stateFileURL)
        let text = String(decoding: data, as: UTF8.self)
        #expect(text.contains("\n"))
        // Spec-mandated 2-space indent; pretty-printer output uses it.
        #expect(text.contains("  "))
    }

    @Test("save leaves no .tmp file behind")
    func saveAtomic() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        store.save(StoredAppState())
        store.flush()

        let tmp = store.stateFileURL.appendingPathExtension("tmp")
        #expect(!FileManager.default.fileExists(atPath: tmp.path))
    }

    // MARK: backups

    @Test("first save does not create a backup")
    func firstSaveNoBackup() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        store.save(StoredAppState())
        store.flush()
        #expect(!FileManager.default.fileExists(atPath: store.backupFileURL.path))
    }

    @Test("second save moves previous content into state.json.bak")
    func secondSaveBacksUpPrevious() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)

        let first = StoredAppState(
            preferences: StoredPreferences(appearance: .light))
        let second = StoredAppState(
            preferences: StoredPreferences(appearance: .dark))

        store.save(first); store.flush()
        store.save(second); store.flush()

        #expect(FileManager.default.fileExists(atPath: store.backupFileURL.path))

        // Decode the backup directly and confirm it carries `first`.
        let decoder = JSONDecoder(); decoder.dateDecodingStrategy = .iso8601
        let bakData = try Data(contentsOf: store.backupFileURL)
        let bak = try decoder.decode(StoredAppState.self, from: bakData)
        #expect(bak == first)

        // And state.json carries `second`.
        #expect(try store.load() == second)
    }

    // MARK: schema version

    @Test("load throws schemaTooNew when file schema > currentSchemaVersion")
    func loadRejectsFutureSchema() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)

        let json = #"""
        {
          "schemaVersion": \#(StoredAppState.currentSchemaVersion + 1),
          "routes": [],
          "scenes": [],
          "preferences": {}
        }
        """#
        try Data(json.utf8).write(to: store.stateFileURL)

        do {
            _ = try store.load()
            Issue.record("expected schemaTooNew")
        } catch let StateStore.LoadError.schemaTooNew(fileVersion, supportedVersion) {
            #expect(fileVersion == StoredAppState.currentSchemaVersion + 1)
            #expect(supportedVersion == StoredAppState.currentSchemaVersion)
        }
    }

    @Test("load throws fileCorrupt when the JSON is malformed")
    func loadRejectsCorruptJson() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        try Data("this is not json".utf8).write(to: store.stateFileURL)
        do {
            _ = try store.load()
            Issue.record("expected fileCorrupt")
        } catch StateStore.LoadError.fileCorrupt {
            // expected
        }
    }

    // MARK: backup recovery

    @Test("load falls back to .bak when state.json is missing")
    func loadRecoversFromBak() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)

        // Seed a .bak; no state.json. Mirrors a crash between the
        // main-rename and tmp-promotion steps.
        let seed = StoredAppState(preferences: StoredPreferences(appearance: .dark))
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(seed).write(to: store.backupFileURL)

        let loaded = try store.load()
        #expect(loaded == seed)
    }

    // MARK: debounce

    @Test("debounce coalesces a burst of saves into a single write")
    func debounceCoalesces() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: 0.1)

        // Save three different states in rapid succession. Only the
        // last one should hit disk after the debounce window.
        let first = StoredAppState(preferences: StoredPreferences(appearance: .light))
        let middle = StoredAppState(preferences: StoredPreferences(appearance: .system))
        let last = StoredAppState(preferences: StoredPreferences(appearance: .dark))

        store.save(first)
        store.save(middle)
        store.save(last)

        // Before the debounce elapses, nothing is on disk.
        #expect(!FileManager.default.fileExists(atPath: store.stateFileURL.path))

        store.flush()  // forces the pending write through synchronously.

        #expect(try store.load() == last)
        #expect(!FileManager.default.fileExists(atPath: store.backupFileURL.path),
                "debounced writes should only produce one on-disk artefact")
    }

    @Test("flush is a no-op when nothing is pending")
    func flushWithNothingPending() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        store.flush()  // should not throw or write
        #expect(!FileManager.default.fileExists(atPath: store.stateFileURL.path))
    }

    // MARK: - Crash-recovery corner cases

    @Test("load ignores a stray .tmp file (no state.json, no .bak)")
    func loadIgnoresStrayTmp() throws {
        // Mirrors a crash mid-`Data.write` — the scratch .tmp was
        // created but never renamed over state.json. The next launch
        // should treat that as a first-run shape, not load the partial
        // content we can't verify the integrity of.
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        try Data("{}".utf8).write(to: store.tempFileURL)
        #expect(try store.load() == nil)
    }

    @Test("primary state.json wins when both it and .bak are present")
    func primaryWinsOverBackup() throws {
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        let primary = StoredAppState(
            preferences: StoredPreferences(appearance: .dark))
        let backup = StoredAppState(
            preferences: StoredPreferences(appearance: .light))

        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        try enc.encode(primary).write(to: store.stateFileURL)
        try enc.encode(backup).write(to: store.backupFileURL)

        #expect(try store.load() == primary)
    }

    @Test(".bak with a too-new schema still surfaces schemaTooNew on fallback")
    func backupFallbackHonoursSchemaGuard() throws {
        // The fallback path isn't an escape hatch for loading
        // otherwise-rejected files — the schema guard must fire
        // whichever file ends up being read.
        let dir = Self.makeTempDir(); defer { Self.cleanUp(dir) }
        let store = try StateStore(directory: dir, debounceSeconds: Self.tinyDebounce)
        let json = #"""
        {
          "schemaVersion": \#(StoredAppState.currentSchemaVersion + 1),
          "routes": [], "scenes": [], "preferences": {}
        }
        """#
        try Data(json.utf8).write(to: store.backupFileURL)
        do {
            _ = try store.load()
            Issue.record("expected schemaTooNew from .bak fallback")
        } catch StateStore.LoadError.schemaTooNew {
            // expected
        }
    }
}
