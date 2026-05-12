# Plan — Diagnostic Collection Bundle

**Status:** ⏳ Pending — proposal awaiting project-owner review.
**Created:** 2026-05-12.
**Motivating session:** [`docs/2026-05-12-f1-hardware-acceptance-debug-session.md`](2026-05-12-f1-hardware-acceptance-debug-session.md).
**Reference layout:** matches the per-phase shape used in [`docs/plan.md`](plan.md) (Status / Goal / Entry / Exit / Tasks / Deviations).

---

## Table of Contents

- [Motivation](#motivation)
- [Resolved decisions](#resolved-decisions)
- [Bundle shape (proposed)](#bundle-shape-proposed)
- [Data-element source map](#data-element-source-map)
- [Phase A — Engine data surface (C ABI v15 → v16)](#phase-a--engine-data-surface-c-abi-v15--v16)
- [Phase B — Swift bundler library](#phase-b--swift-bundler-library)
- [Phase C — App UI integration](#phase-c--app-ui-integration)
- [Phase D — CLI parity](#phase-d--cli-parity)
- [Phase E — Documentation](#phase-e--documentation)
- [Out of scope](#out-of-scope)
- [References](#references)

---

## Motivation

The 2026-05-12 F1 hardware-acceptance session (`docs/2026-05-12-f1-hardware-acceptance-debug-session.md`) used about three hours mostly on data-collection plumbing rather than reasoning:

- Route-diagnostics counters (`frames_produced` / `frames_consumed` / underrun / overrun, the per-component latency breakdown) update at 4 Hz inside the route panel. Reading them is currently `Cmd+Shift+4` plus eyeballing a frozen screenshot.
- `~/Library/Logs/Jbox/Jbox.log*` had to be located, sized, and grepped by hand (`grep -nE 'evt=(overrun|underrun|route_waiting|route_started)' …`).
- `log show --predicate 'subsystem == "com.jbox.app"' --info --debug --last 1h` had to be typed verbatim — the user has to know the predicate.
- The full enumerated device list (UID, sample rate, buffer frames, channel counts) is not exposed anywhere outside the AddRoute picker.

"The thing I wished I had a button for" is a one-click artifact that captures everything above plus its provenance (app version, engine ABI, macOS version, hardware model, ISO-8601 timestamp).

---

## Resolved decisions

All six open questions resolved 2026-05-12 by project-owner review. Captured here so a future implementer doesn't have to reconstruct the choices.

1. **Bundle container.** ✅ Zip via `Process` → `/usr/bin/zip`. Effort is small (~50 lines for the driver + a Swift Testing case on the argv); one-file attachment is worth it.
2. **Default output location.** ✅ Always show `NSSavePanel`, pre-filled with `~/Desktop/jbox-diagnostics-<ISO8601>.zip`. No silent-Desktop variant; one prompt-and-go path.
3. **Engine API shape.** ✅ Omnibus call (`jbox_engine_poll_diagnostics_snapshot`). New per-device HAL-latency-frames surface is needed regardless of approach (it currently only exists inside per-route composition), so we're adding new ABI surface anyway — bundling it into one atomic-snapshot call is the cleaner choice.
4. **CLI parity.** ❌ Deferred to a future follow-up (call it `F8` when filed). The Preferences-tab button covers the headline use case; CLI parity is a nice-to-have for power users, not a v1-essential. Phase D below remains in the doc as a reference for whoever picks it up later, but the v1 implementation stops at Phase C.
5. **`log show` subprocess.** ❌ File sink only for v1. Rotating sink at `~/Library/Logs/Jbox/Jbox.log{,.1,.2}` is sufficient for engine events.
6. **User-notes input.** ✅ Ship the optional single-line text area; included only when non-empty.

---

## Bundle shape (proposed)

Top-level directory inside the zip: `jbox-diagnostics-<ISO8601>/`. The zip itself is `jbox-diagnostics-<ISO8601>.zip`. All filenames use the same ISO-8601 timestamp (UTC, second precision, colons replaced with `-` to keep them filesystem-safe on macOS / Windows / Linux: `2026-05-12T07-48-43Z`).

```
jbox-diagnostics-2026-05-12T07-48-43Z/
├── README.txt                  ← what this bundle is, what each file means, JBox version, timestamp
├── system.txt                  ← macOS version, kernel build, hw.model, hw.machine, sysctl identifiers
├── engine.txt                  ← engine ABI version, build id, current resampler preset
├── devices.txt                 ← all enumerated devices: UID, name, IO direction, channel counts,
│                                  nominal SR, current HAL buffer frames, HAL latency + safety offset
├── routes.txt                  ← per-route textual snapshot: id, persistId, source UID, dest UID,
│                                  mapping, latency tier, RouteState, last_error,
│                                  frames_produced/consumed, underrun/overrun counters,
│                                  full latency-component breakdown
├── routes.json                 ← same data as routes.txt but machine-readable for future tooling
│                                  (parser-friendly; the text file is the human-friendly view)
├── logs/
│   ├── Jbox.log                ← live rotating file (may be empty for the .app variant if no events
│   │                              have drained since launch)
│   ├── Jbox.1.log              ← rotated file 1 (if present)
│   └── Jbox.2.log              ← rotated file 2 (if present)
└── notes.txt                   ← optional; present only when the user typed something
```

`README.txt` is the manifest: it names every file above and tells the reader to inspect the bundle before sending it. No JSON manifest for v1; the text file is enough at this scale.

**Rationale — why a zip:**
- One-file email / Slack / GitHub-issue attachment.
- macOS Finder unzips with a double-click, so a recipient who only has Finder still sees the layout above.
- `/usr/bin/zip` is in every macOS 15 install, runs in JBox's sandbox-off environment without entitlement changes, and produces the same artifact as the macOS Archive Utility.

**Rationale — why two route files (`routes.txt` + `routes.json`):**
- The text file is for the human filing the bug — column-aligned, readable cold.
- The JSON file is for a future debugger who wants to grep across multiple bundles (e.g., "how often does `last_error == JBOX_ERR_DEVICE_STALLED` appear when `latency_mode == 2`?"). It uses the same fields; no separate schema work.

---

## Data-element source map

| Bundle file        | Field                          | Source                                                                                       | New surface? |
|--------------------|--------------------------------|----------------------------------------------------------------------------------------------|--------------|
| `engine.txt`       | App version                    | `Bundle.main.infoDictionary["CFBundleShortVersionString"]`                                   | no           |
| `engine.txt`       | Engine ABI version             | `JboxEngine.abiVersion` (existing)                                                           | no           |
| `engine.txt`       | Resampler quality              | `engine.resamplerQuality` (existing, ABI v8)                                                 | no           |
| `system.txt`       | macOS version                  | `ProcessInfo.processInfo.operatingSystemVersionString`                                       | no           |
| `system.txt`       | Hardware model                 | `sysctlbyname("hw.model")` + `hw.machine` + `kern.osversion`                                 | no           |
| `devices.txt`      | UID / name / channel counts / SR / buffer frames | `jbox_engine_enumerate_devices` (existing, ABI v1)                         | no           |
| `devices.txt`      | HAL latency + safety offset    | Currently only computed inside `RouteManager::pollLatencyComponents` per *route*; not exposed per device | **yes — see Phase A** |
| `routes.txt`/`.json` | id, persistId, source/dest UID, mapping | `EngineStore.routes` (existing)                                                  | no           |
| `routes.txt`/`.json` | RouteState + last_error      | `jbox_engine_poll_route_status` (existing)                                                   | no           |
| `routes.txt`/`.json` | frame counters / underrun / overrun | `jbox_engine_poll_route_status` (existing)                                            | no           |
| `routes.txt`/`.json` | full latency components       | `jbox_engine_poll_route_latency_components` (existing, ABI v4)                              | no           |
| `routes.txt`/`.json` | latency tier, buffer-frames preference | `RouteConfig` (existing Swift-side)                                                | no           |
| `routes.txt`/`.json` | masterGainDb, trimDbs, muted, channelMuted | `Route` (existing)                                                            | no           |
| `logs/`            | Rotating file contents         | `~/Library/Logs/Jbox/Jbox.log{,.1,.2}` directly via `FileManager` copy                       | no           |
| `notes.txt`        | User free-form text            | Pre-collect sheet text area                                                                  | yes (UI)     |

Only one element (per-device HAL latency / safety offset) is not already reachable through the existing C ABI. The plan adds an omnibus call that bundles a strict superset of existing data so the Swift side gets a single consistent snapshot in one ABI hop.

---

## Phase A — Engine data surface (C ABI v15 → v16)

**Status:** ⏳ Pending.

**Goal.** Add a single ABI v16 omnibus call that returns a snapshot of every enumerated device's HAL parameters (including per-device latency frames + safety-offset frames) and every known route's full diagnostic state in one heap-allocated structure. Bump the ABI to 16. No existing signatures change.

**Entry criteria.** Resolved decisions in place (see top of doc).

**Exit criteria.**
- `JBOX_ENGINE_ABI_VERSION` is `16u`.
- `jbox_engine_poll_diagnostics_snapshot` + `jbox_diagnostics_snapshot_free` declared in `jbox_engine.h`, defined in `bridge_api.cpp`, with engine-side composition in `engine.cpp` / `route_manager.cpp` / `device_manager.cpp`.
- Catch2 cases cover: empty engine (no routes, no devices); engine with one stopped route; engine with one running route (smoke under the simulated backend); engine with two routes sharing a source device; ABI-version field equals `16`; free function tolerates `NULL`.
- `scripts/rt_safety_scan.sh` clean (the new code is control-thread-only).
- `make verify` green.

**Proposed surface — additive only.** New header lines:

```c
/* ABI history continued:
 *  16  MINOR — added jbox_diagnostics_snapshot_t,
 *              jbox_engine_poll_diagnostics_snapshot,
 *              jbox_diagnostics_snapshot_free. Per-device snapshot
 *              extends jbox_device_info_t with HAL latency frames and
 *              safety offset frames; per-route snapshot is the
 *              concatenation of the existing status + latency
 *              component structs plus the route's source/dest UID.
 */
#define JBOX_ENGINE_ABI_VERSION 16u

/* Heap-allocated; caller frees via jbox_diagnostics_snapshot_free. */
typedef struct {
    char     uid[JBOX_UID_MAX_LEN];
    char     name[JBOX_NAME_MAX_LEN];
    uint32_t direction;                 /* bitmask of jbox_device_direction_t */
    uint32_t input_channel_count;
    uint32_t output_channel_count;
    double   nominal_sample_rate;
    uint32_t buffer_frame_size;         /* current HAL value, max-across-clients */
    /* NEW in v16: HAL-side latency contributions per direction. 0 when
     * the device does not report one (some USB drivers under-report). */
    uint32_t input_hal_latency_frames;
    uint32_t input_safety_offset_frames;
    uint32_t output_hal_latency_frames;
    uint32_t output_safety_offset_frames;
} jbox_diagnostic_device_t;

typedef struct {
    jbox_route_id_t                 route_id;
    char                            source_uid[JBOX_UID_MAX_LEN];
    char                            dest_uid[JBOX_UID_MAX_LEN];
    uint32_t                        latency_mode;     /* mirror of jbox_route_config_t */
    uint32_t                        buffer_frames;    /* mirror of route preference */
    jbox_route_status_t             status;           /* existing */
    jbox_route_latency_components_t latency;          /* existing */
} jbox_diagnostic_route_t;

typedef struct {
    uint32_t                  abi_version;     /* JBOX_ENGINE_ABI_VERSION */
    jbox_resampler_quality_t  resampler_quality;
    uint64_t                  capture_ts_us;   /* CLOCK_REALTIME at snapshot */
    jbox_diagnostic_device_t* devices;
    size_t                    device_count;
    jbox_diagnostic_route_t*  routes;
    size_t                    route_count;
} jbox_diagnostics_snapshot_t;

jbox_diagnostics_snapshot_t* jbox_engine_poll_diagnostics_snapshot(
    jbox_engine_t* engine, jbox_error_t* err);

void jbox_diagnostics_snapshot_free(jbox_diagnostics_snapshot_t* snap);
```

**Engine-side composition.**
- `Engine::pollDiagnosticsSnapshot` (new) calls `DeviceManager::enumerate()` for the device side and `RouteManager::collectDiagnosticRoutes()` for the route side. Both operations are already control-thread-safe; the omnibus call serializes via the existing engine-level mutex.
- `DeviceManager` already reads `kAudioDevicePropertyLatency` + `kAudioDevicePropertySafetyOffset` per device when computing route latency (see `route_manager.cpp` ~1108–1121 referenced in the F1 session doc). Lift that read into `BackendDeviceInfo` so the device list carries it natively — this is where the only new read happens. Reads only; no HAL writes (binding § "Device & HAL ownership policy" in `CLAUDE.md`).
- `RouteManager::collectDiagnosticRoutes(out_vector)` iterates `routes_`, calls the existing `pollRouteStatus` and `pollLatencyComponents` paths per route, and copies in the source/dest UIDs from `RouteRecord`.

**Tasks.**

- [ ] T1 — write Catch2 cases first (`Tests/JboxEngineCxxTests/diagnostics_snapshot_test.cpp`):
  - empty engine → `device_count == 0`, `route_count == 0`, `abi_version == 16`.
  - one stopped route → `route_count == 1`, `status.state == STOPPED`, `status.frames_produced == 0`.
  - one running route under simulated backend → counters non-zero, `latency.total_us` non-zero.
  - two routes sharing a source device → both routes appear, both reference the same `source_uid`, device list contains the source UID exactly once.
  - `jbox_diagnostics_snapshot_free(NULL)` is a no-op.
  - `jbox_diagnostics_snapshot_free(non-null)` is idempotent against the heap pattern used in `jbox_device_list_free`.
- [ ] T2 — extend `BackendDeviceInfo` with `input_hal_latency_frames` / `output_hal_latency_frames` / `*_safety_offset_frames`; populate during `enumerate()` in `core_audio_backend.cpp` (lifted from the per-route path) and in `simulated_backend.cpp` (zero-filled is fine, plus add one test variant that sets non-zero values).
- [ ] T3 — declare the new types in `jbox_engine.h` with ABI history comment; bump `JBOX_ENGINE_ABI_VERSION` to `16u`.
- [ ] T4 — implement `Engine::pollDiagnosticsSnapshot` + `RouteManager::collectDiagnosticRoutes`. Allocations are `std::malloc` / `std::calloc` matching the existing `jbox_device_list_t` allocator so `jbox_diagnostics_snapshot_free` can use plain `std::free`.
- [ ] T5 — implement `jbox_engine_poll_diagnostics_snapshot` / `jbox_diagnostics_snapshot_free` in `bridge_api.cpp`, mirroring the error-and-free conventions of `jbox_engine_enumerate_devices` / `jbox_device_list_free`. `os_log` an `evt=diagnostics_snapshot` line at notice priority (subsystem `com.jbox.app`, category `bridge`) on every call so a future debugger can see who pressed the button when.
- [ ] T6 — `JboxEngine.swift` adds the Swift-side wrapper: `Engine.pollDiagnosticsSnapshot() throws -> DiagnosticsSnapshot`. The Swift struct mirrors the C surface but uses Swift-idiomatic types (`String`, `[DiagnosticDevice]`, `[DiagnosticRoute]`). Swift Testing case in `Tests/JboxEngineTests/EngineDiagnosticsSnapshotTests.swift`: smoke against the bridge using a stub-backed engine if one exists, or skip-with-reason when no engine can be spun up in CI.
- [ ] T7 — RT-safety scan pass (the new code is control-thread, but lift the scan check to be sure no symbol leaked into `rt/`).

**Phase A deviations.** *(empty)*

---

## Phase B — Swift bundler library

**Status:** ⏳ Pending.

**Goal.** Pure-logic bundler in `Sources/JboxEngineSwift/` that takes a `DiagnosticsSnapshot` (Phase A) plus environment metadata and produces every text file in the bundle. Add the zip-bundling helper that drives `/usr/bin/zip`. All formatters TDD-first.

**Entry criteria.** Phase A merged and ABI v16 released.

**Exit criteria.**
- New file `Sources/JboxEngineSwift/Diagnostics/DiagnosticsBundle.swift` (and friends) compiles into `JboxEngineSwift`.
- `Tests/JboxEngineTests/DiagnosticsBundleTests.swift` covers:
  - `routes.txt` formatter pins output across all `RouteState` values, both with and without running counters.
  - `routes.json` formatter round-trips through `JSONDecoder` and re-encodes identically.
  - `devices.txt` formatter prints "—" (em dash) for zero HAL-latency frames, the numeric frame count otherwise.
  - `system.txt` formatter contains the literal app version string passed in; does not contain `$HOME`, the user's full name, or any env-var that could be PII.
  - `README.txt` formatter names every file the bundle contains (zero-orphan check: every file the writer plans to emit must appear in README).
  - The zip-driving helper composes the right `/usr/bin/zip` argv and uses a temp directory under `FileManager.default.temporaryDirectory` so the test suite never writes to `~/Desktop`.
  - Zero PII smoke test: assert no part of any formatter's output contains `getlogin()`, `NSUserName()`, `$HOME` literal, or `state.json` path string.
- `make swift-test` green.

**Proposed module layout.**

```
Sources/JboxEngineSwift/Diagnostics/
├── DiagnosticsBundle.swift          ← top-level `collect(into: URL) throws -> URL`
├── DiagnosticsSnapshot.swift        ← Swift mirror of Phase A's C struct
├── DiagnosticsEnvironment.swift     ← system + app metadata reader (sysctlbyname, ProcessInfo, Bundle)
├── DiagnosticsFormatters.swift      ← pure functions: snapshot → README/routes/devices/system/engine .txt
├── DiagnosticsJSON.swift            ← snapshot → routes.json via Codable
├── DiagnosticsLogCopier.swift       ← copies ~/Library/Logs/Jbox/Jbox.log{,.1,.2} into the bundle dir
└── DiagnosticsZipper.swift          ← drives /usr/bin/zip via Process; returns the zip URL
```

**Threading and isolation.**
- `DiagnosticsBundle.collect(into:)` is `@MainActor` for the snapshot-poll boundary (so it sees the same `EngineStore`-coherent view the UI does) but the file I/O sub-steps are `await`-pushed onto a background task. The whole call is `async throws` and returns the URL of the produced zip.

**Tasks.**

- [ ] T1 — write `DiagnosticsBundleTests.swift` first against the formatter signatures (TDD).
- [ ] T2 — implement `DiagnosticsSnapshot` as the Swift mirror of Phase A's struct (plain `Sendable` value types).
- [ ] T3 — implement `DiagnosticsEnvironment` using `sysctlbyname("hw.model", …)` etc. + `ProcessInfo` + `Bundle.main`. Bash-out is forbidden (open question #3 implicitly resolved).
- [ ] T4 — implement `DiagnosticsFormatters`. Each formatter is a pure `(snapshot, env) -> String`. Vendor-neutral copy: never name V31, Apollo, UAD Console, etc. The formatter prints whatever the engine reports as `name` for a device — that is the only valid surface.
- [ ] T5 — implement `DiagnosticsJSON` with a stable JSON shape (top-level `{ "schema_version": 1, "captured_at": ..., "routes": [...], "devices": [...], "engine": {...} }`).
- [ ] T6 — implement `DiagnosticsLogCopier`. Copies via `FileManager.default.copyItem(at:to:)`; tolerates missing rotated files (`.1.log` / `.2.log` may not exist on a fresh user account); never reads outside `~/Library/Logs/Jbox/`.
- [ ] T7 — implement `DiagnosticsZipper`. Drives `/usr/bin/zip` via `Process` with `-r` from a parent temp directory; failure surfaces as a typed Swift error.
- [ ] T8 — implement `DiagnosticsBundle.collect(into:)` as the public entry point. Composes the above steps; cleans up the temp staging directory in a defer block on both success and failure.

**Phase B deviations.** *(empty)*

---

## Phase C — App UI integration

**Status:** ⏳ Pending.

**Goal.** Surface the "Collect Diagnostics" affordance in the main app. Wire the trigger to `DiagnosticsBundle.collect` from Phase B. No new persistence fields.

**Entry criteria.** Phase B merged.

**Exit criteria.**
- `Help ▸ Collect Diagnostics…` menu item appears, accessibility-labelled, with keyboard shortcut `⌘⇧D` (no current conflict — verified against `RouteListView.swift`'s toolbar and `JboxApp.swift`'s scene).
- The Advanced tab in Preferences (existing — `AdvancedPreferencesView`) gains a "Collect Diagnostics…" button alongside "Open Logs Folder…".
- Triggering the affordance shows a small sheet with: a one-paragraph explainer (vendor-neutral copy), an optional notes text-area, a primary "Collect" button, and a Cancel button.
- The "Collect" action presents an `NSSavePanel` pre-filled with `~/Desktop/jbox-diagnostics-<ISO8601>.zip` so the user can confirm or redirect the destination per save.
- Successful collection reveals the produced zip in Finder via `NSWorkspace.shared.activateFileViewerSelecting([url])` (analogous to the existing "Open Logs Folder…" behavior).
- Failure surfaces as a non-blocking inline message in the sheet ("Couldn't write the bundle: <reason>"); the sheet stays open so the user can retry.
- `make verify` green.
- Manual smoke test: open `make run`, trigger `⌘⇧D`, leave notes empty, click Collect, accept the prompted Desktop path, verify Finder reveals the resulting zip; unzip; spot-check `README.txt`, `routes.txt`, `logs/Jbox.log`.

**Tasks.**

- [ ] T1 — `Sources/JboxApp/DiagnosticsCollectSheet.swift` (new). SwiftUI sheet view; mirrors `AddRouteSheet.swift` for layout discipline. Contains the optional notes text area + primary action. Vendor-neutral copy.
- [ ] T2 — `JboxApp.swift`: add `.commands` block with a `CommandMenu("Help")` (or extend the existing one) hosting "Collect Diagnostics…" with `.keyboardShortcut("d", modifiers: [.command, .shift])`. The action presents the sheet via a `@State` flag held in `AppRootView`.
- [ ] T3 — `Sources/JboxApp/JboxApp.swift` `AdvancedPreferencesView`: add the second `Section` button "Collect Diagnostics…" pointing at the same action. Footer copy: "Bundles current route state + logs into a zip. You can inspect every file before sending."
- [ ] T4 — App-level wiring: on Collect, present `NSSavePanel` (`nameFieldStringValue` = `jbox-diagnostics-<ts>.zip`, `directoryURL` = `~/Desktop`); on confirm, call `DiagnosticsBundle.collect(into: url, engine: engineStore.engine, notes: notes)`. Wrapped in `Task { @MainActor in ... }` so cancellation propagates cleanly when the sheet is dismissed during the in-flight collect.
- [ ] T5 — Accessibility labels on every new control. VoiceOver smoke pass via `defaults read com.apple.universalaccess` while testing.
- [ ] T6 — Swift Testing case for the sheet's text-binding behaviour (notes empty → no `notes.txt` in bundle; notes non-empty → file present with verbatim contents). Routes through the Phase B formatter API; no XCUITest (per Phase 6 deferral).

**Phase C deviations.** *(empty)*

---

## Phase D — CLI parity

**Status:** ⏭️ Deferred (2026-05-12). Project-owner review concluded the Preferences-tab button covers the headline use case; CLI parity is a power-user nice-to-have, not v1-essential. This section remains for whoever picks up the follow-up (file as `F8` if/when it returns to the path).

**Goal.** A `--collect-diagnostics [<output-path>]` subcommand on `JboxEngineCLI`. Produces the same bundle as the app, drawing on the same Phase B library.

**Entry criteria.** Phase B merged. The follow-up that re-introduces Phase D would entail re-evaluating whether the CLI's headless surface still benefits, given how Phases B / C shipped.

**Exit criteria.**
- `swift run JboxEngineCLI --collect-diagnostics` writes `~/Desktop/jbox-diagnostics-<ts>.zip`.
- `swift run JboxEngineCLI --collect-diagnostics /tmp/out.zip` writes to the explicit path.
- The CLI bundle's `logs/` carries `JboxEngineCLI.log{,.1,.2}`, not `Jbox.log` — the per-process file naming established in Phase 8 is preserved.
- The CLI variant works without spinning up an audio engine if no routes / devices are needed beyond enumeration — i.e., a one-shot `Engine()` construction is sufficient. (`Engine()` already triggers a `jbox_engine_create` which is cheap and read-only.)
- `Tests/JboxEngineCLITests/CLIParsingTests.swift` covers the new flag.
- `make verify` green.

**Tasks.**

- [ ] T1 — `Sources/JboxEngineCLICore/CLIParsing.swift`: add `case collectDiagnostics(URL?)` to `CLICommand`; update `parseCLI` and `usage()`.
- [ ] T2 — `Tests/JboxEngineCLITests/CLIParsingTests.swift`: add cases for the new flag (with and without arg; with invalid path-shape).
- [ ] T3 — `Sources/JboxEngineCLI/main.swift`: dispatch `.collectDiagnostics` to a new `runCollectDiagnostics(_ output: URL?) throws` that calls `DiagnosticsBundle.collect`.
- [ ] T4 — Manual smoke: `make build && build/Jbox.app/Contents/MacOS/JboxEngineCLI --collect-diagnostics /tmp/foo.zip` → unzip → spot-check.

**Phase D deviations.** *(empty)*

---

## Phase E — Documentation

**Status:** ⏳ Pending.

**Goal.** User-facing and contributor-facing documentation reflects the new feature. No new architectural concepts in `spec.md`; this is a wholly additive client of the existing engine surface.

**Entry criteria.** Phase C merged (Phase D is deferred per resolved-decisions § 4 above).

**Exit criteria.**
- `README.md`: a new "Filing a bug report" subsection in the README points at the menu item / CLI flag and lists what the bundle contains.
- `docs/spec.md § 1.6`: ABI history extended with the v16 entry (already done implicitly in the header; mirror in the spec narrative).
- `docs/spec.md § 2.11`: new omnibus call mentioned in the "Representative signatures" list.
- `docs/testing/stress.md`: a new "if the test thrashes, run Collect Diagnostics" line in the procedure.
- `docs/plan.md` milestone table: a new row "10 — Diagnostic collection" (or whatever number is appropriate when this plan is promoted) with status emoji.

**Tasks.**

- [ ] T1 — `README.md` — add "Filing a bug report" subsection under the existing user-facing section.
- [ ] T2 — `docs/spec.md § 1.6` — append the v16 entry to the ABI history paragraph.
- [ ] T3 — `docs/spec.md § 2.11` — list `jbox_engine_poll_diagnostics_snapshot` in the representative signatures.
- [ ] T4 — `docs/testing/stress.md` — add the "before stopping the engine, run Collect Diagnostics" sentence to the stress procedure.
- [ ] T5 — `docs/plan.md` milestone table — add the row.
- [ ] T6 — `CLAUDE.md` — *no change needed.* The diagnostic-collection feature does not introduce any new RT-side path, new HAL write, new entitlement, or new persistence file, so the constraints written for future Claude instances are unchanged.

**Phase E deviations.** *(empty)*

---

## Out of scope

The following are intentionally not part of this feature. Listing them here so a future review doesn't have to reverse-engineer the boundary:

- **No automatic upload or telemetry.** The bundle is a local file; the user attaches it to whatever channel they choose. JBox has no servers, makes no network calls, asks for no `com.apple.security.network.client` entitlement.
- **No "Report a Bug" form.** The button writes a file; it doesn't open a mail composer, a GitHub issue, or a webview.
- **No scheduled / periodic collection.** This is a one-button affordance, not a continuous data sink.
- **No notification when a bundle is created.** Finder reveal is the entire feedback; no `UNUserNotificationCenter`, no `NSAlert`.
- **No bundling of `state.json`.** The user's persistence file lives at `~/Library/Application Support/Jbox/state.json` and contains the user's routes by device UID. Out of scope on PII grounds — UIDs are device-stable identifiers and could plausibly fingerprint a rig. Users can attach it manually if a bug actually depends on persistence shape.
- **No bundling of macOS Audio MIDI Setup state beyond what JBox enumerates.** JBox sees what Core Audio reports; AMS's own state file is the user's, not JBox's.
- **No `log show` subprocess.** The rotating file sink already mirrors every drained engine event. (Re-open question #5 if a future bug session demonstrates a need for system-side Core Audio events that don't reach JBox.)
- **No bundle-format versioning.** The schema is "what v1 ships." Per the "no speculative future code" rule, no `schema_version: 1` reservation for a v2; if a v2 ever lands, it bumps additively.
- **No new Preferences fields.** A "default save location" preference is *out* of scope — the `NSSavePanel` is shown every save with Desktop pre-filled, so per-save redirection is one click away.
- **No CLI subcommand in v1.** Phase D is deferred per resolved-decisions § 4. The Phase B library is engine-agnostic so a future follow-up can wire the CLI subcommand without revisiting Phase B's API shape.

---

## References

- `docs/2026-05-12-f1-hardware-acceptance-debug-session.md` — motivating debug session; data-collection pain that drives this feature.
- `docs/spec.md § 1.6` — ABI versioning rules (current ABI is v15; this feature lands as v16 additive).
- `docs/spec.md § 2.11` — engine public C API.
- `docs/spec.md § 2.12` — per-route latency estimation (the `jbox_route_latency_components_t` source of truth).
- `docs/spec.md § 2.9` — RT-safe logging + rotating file sink contract (`~/Library/Logs/Jbox/<process>.log`, 5 MiB × 3).
- `docs/spec.md § 4.6` — Preferences window shape (Advanced tab is where the secondary button lands).
- `docs/spec.md § 5.1` — testing strategy (Catch2 v3 for C++, Swift Testing for Swift, XCUITest deferred).
- `Sources/JboxEngineC/include/jbox_engine.h` — current ABI surface (v15).
- `Sources/JboxEngineC/control/route_manager.cpp` ~1108–1121 — per-route latency composition; per-device HAL latency reads live in this neighbourhood today (the only data the new ABI surface needs).
- `Sources/JboxEngineC/control/bridge_api.cpp` `jbox_engine_create` — rotating file sink composition (informs where the log files live).
- `Sources/JboxApp/RouteListView.swift` — current diagnostics panel host; reference point for vendor-neutral copy.
- `Sources/JboxApp/JboxApp.swift` `AdvancedPreferencesView` — sibling location for the new "Collect Diagnostics…" button.
- `Sources/JboxEngineCLI/main.swift` and `Sources/JboxEngineCLICore/CLIParsing.swift` — CLI-parity entry points (Phase D).
- `CLAUDE.md § "Device & HAL ownership policy"` — confirms the new HAL-property reads are allowed; no writes added.
