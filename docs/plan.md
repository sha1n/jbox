# Jbox — Implementation Plan

**Status:** Draft v1, companion to [spec.md](./spec.md).
**Approach:** incremental, phase-gated. Each phase produces a demonstrable outcome, and each phase has an explicit exit criterion. No phase begins until the previous phase's exit criterion is met. Phases are mostly linear; Phase 6 (UI) could partially overlap with Phase 5 once the bridge API stabilizes, but as a guideline, keep them sequential to avoid integration churn.

**Reference.** Every task in this document links back to the relevant section of [spec.md](./spec.md). If a task changes scope during implementation, update **both** documents in the same change.

---

## Table of Contents

- [Phase 0 — Specification (current)](#phase-0--specification-current)
- [Phase 1 — Foundation](#phase-1--foundation)
- [Phase 2 — Engine core primitives](#phase-2--engine-core-primitives)
- [Phase 3 — First working route](#phase-3--first-working-route)
- [Phase 4 — Drift correction and resampling](#phase-4--drift-correction-and-resampling)
- [Phase 5 — Multi-route and shared devices](#phase-5--multi-route-and-shared-devices)
- [Phase 6 — SwiftUI UI](#phase-6--swiftui-ui)
- [Phase 7 — Persistence, scenes, launch-at-login](#phase-7--persistence-scenes-launch-at-login)
- [Phase 8 — Packaging and installation](#phase-8--packaging-and-installation)
- [Phase 9 — Release hardening and device-level testing](#phase-9--release-hardening-and-device-level-testing)

---

## Milestone overview

| Phase | Title                                 | Demonstrable outcome                                                                 | Status |
|-------|---------------------------------------|--------------------------------------------------------------------------------------|--------|
| 0     | Specification                         | `docs/` committed; spec is the agreed source of truth.                                | ✅ Done (`c327e63`) |
| 1     | Foundation                            | `swift build` succeeds on an empty SPM skeleton. CI runs green.                       | ✅ Done (`59188ad`) |
| 2     | Engine core primitives                | Ring buffer, RT log queue, atomic meter, drift tracker — unit-tested and RT-safe.     | ✅ Done (`48ea5cb`) |
| 3     | First working route                   | Audio from V31 → Apollo Virtual outputs, end-to-end, with real devices.               | ✅ Code done (`6b74c59`) — manual hardware test deferred |
| 4     | Drift correction and resampling       | 30-minute soak test on real devices with zero dropouts.                                | ⏳ Pending |
| 5     | Multi-route and shared devices        | Three simultaneous routes running, including one that shares a device with another.   | ⏳ Pending |
| 6     | SwiftUI UI                            | User can add / edit / delete / start / stop routes through the GUI.                   | ⏳ Pending |
| 7     | Persistence, scenes, launch-at-login  | Relaunching the app restores configured routes and scenes.                             | ⏳ Pending |
| 8     | Packaging and installation            | `Jbox.app` runs from `/Applications` on a clean user account.                          | ⏳ Pending |
| 9     | Release hardening                     | v1.0.0 tagged and published to GitHub Releases.                                        | ⏳ Pending |

---

## Phase 0 — Specification

**Status:** ✅ Complete. Initial docs committed in `c327e63` and pushed to `origin/master`.

**Goal.** Produce the three docs — `README.md` at the repo root, `docs/spec.md`, `docs/plan.md` — committed to the repo, and approved by the project owner.

**Entry criteria.** An empty repository (only `.git`). A clear user vision of what's being built.

**Exit criteria.**
- `README.md` (repo root), `docs/spec.md`, `docs/plan.md` all exist.
- All three documents are internally consistent: same entity names, same scope, same deferred list.
- Spec self-review performed: no placeholders, no contradictions, no ambiguity.
- Project owner reviewed all three documents and approved.

**Tasks.**
- [x] Brainstorm the full v1 design with the project owner (architecture, engine, data model, UI, testing, distribution).
- [x] Write `docs/spec.md`.
- [x] Write `README.md` (repo root).
- [x] Write `docs/plan.md`.
- [x] Spec self-review pass (scan for "TBD", "TODO", placeholders, contradictions).
- [x] Project owner review of all three documents.
- [x] Initial commit on `master` with all three documents.

---

## Phase 1 — Foundation

**Status:** ✅ Complete. Scaffolding committed in `59188ad` and pushed; CI run #24624922185 green in 40 seconds.

**Goal.** Stand up the repository scaffolding so all subsequent work happens in a buildable, CI-checked project.

**Entry criteria.** Phase 0 complete. Docs approved.

**Exit criteria.**
- [x] `swift build` succeeds on the empty SPM skeleton.
- [x] `swift test` succeeds with the placeholder tests (3 suites, 4 tests, all passing under Swift Testing).
- [x] `scripts/rt_safety_scan.sh` runs and prints "clean" against the empty engine source tree.
- [x] GitHub Actions workflow runs on push to `master` and on pull requests; all checks green.
- [x] `.gitignore`, `LICENSE`, and the `scripts/` directory are in place. (`README.md` already exists from Phase 0.)

**Tasks.**

Repository scaffolding:
- [x] Create `Package.swift` at repo root declaring the target layout from [spec.md § 5.3](./spec.md#53-project-layout). Five targets plus three test targets; `cxxLanguageStandard: .cxx20`.
- [x] Create directory structure under `Sources/`: `JboxEngineC/include/`, `JboxEngineC/rt/`, `JboxEngineC/control/`, `JboxEngineSwift/`, `JboxEngineCLI/`, `JboxApp/`.
- [x] Add `jbox_engine.h` in `Sources/JboxEngineC/include/` with the ABI version macro and `jbox_engine_abi_version()` declaration.
- [x] Add `JboxApp.swift` — minimal SwiftUI App struct, single window showing the engine ABI version.
- [x] Add `main.swift` in `JboxEngineCLI/` that prints the ABI version.
- [x] Add `.gitignore` covering `.build/`, `.swiftpm/`, `.DS_Store`, `*.xcodeproj` / `*.xcworkspace`, `build/`.
- [x] Add `LICENSE` placeholder ("all rights reserved; license decision pending"). Per project owner decision, a permissive license will be chosen later; change is a single-file replacement.

Scripts:
- [x] `scripts/rt_safety_scan.sh` — scans `Sources/JboxEngineC/rt/` for banned symbols (allocators, locks, dispatch calls, non-RT-safe logging, smart pointer construction). Bash-3.2 compatible (macOS default shell). Clean pass on an empty `rt/`.
- [x] `scripts/bundle_app.sh` — **fully functional** (not a placeholder): creates `build/Jbox.app`, generates `Info.plist`, copies the executable, ad-hoc signs with Hardened Runtime. Overshot the original plan; good as-is.
- [x] `scripts/build_release.sh` — wraps `swift build -c release` + `bundle_app.sh`. Works end-to-end.
- [x] `scripts/package_unsigned_release.sh` — placeholder stub (exits zero with a notice). Full implementation in Phase 8.
- [x] `scripts/run_app.sh` — builds, bundles, and launches the `.app` locally via `open`.

CI:
- [x] `.github/workflows/ci.yml` configured for GitHub Actions `macos-15` runner.
- [x] CI runs: `swift build -c release`, `swift test`, `scripts/rt_safety_scan.sh`.
- [ ] `clang-tidy` and `swiftlint` steps included but permitted to warn without failing on Phase 1 (tighten in later phases). **Deferred to Phase 2** (no engine code yet to lint).
- [x] CI passes on `master` push. (Initial PR flow skipped for a solo repo; PR triggers are still wired up for future contributions.)

Testing harness:
- [x] Placeholder test targets (`JboxEngineTests`, `JboxEngineIntegrationTests`, `JboxAppTests`), each with passing stub tests. `JboxEngineTests` additionally verifies runtime ABI matches the header constant.
- [x] `swift test` runs all three and passes (3 suites / 4 tests under Swift Testing).

Deviations from the original Phase 1 plan worth noting:
- **Swift Testing instead of XCTest.** Original plan mentioned XCTest; discovered during verification that XCTest isn't shipped with Xcode Command Line Tools. Switched to Swift Testing (modern, bundled with Swift 6.x), which also surfaced that **Xcode.app must be installed** (for its frameworks) even though the IDE isn't used. Docs (`README.md`, `docs/spec.md`) updated in the same commit.
- **`bundle_app.sh` and `build_release.sh` are real, not stubs.** Overshoot; kept as-is since they work and Phase 8 would have had to rewrite them anyway.

---

## Phase 2 — Engine core primitives

**Status:** ✅ Complete. Scaffolded in `de8c4c5`; five primitives landed across commits `d6f7d68` (AtomicMeter), `c706eeb` (ChannelMapper), `11ef83b` (RtLogQueue + scanner fix), `f1f4107` (RingBuffer), `48ea5cb` (DriftTracker). CI green on all. C++ suite: 66 test cases, ~598k assertions, passes under ThreadSanitizer.

**Goal.** Implement the RT-safe primitives the engine is built from, with full unit-test coverage, **before** introducing Core Audio. This phase is entirely synthetic — no real audio devices yet.

**Phase 2 tooling decisions (locked in at Phase 2 kickoff):**
- **C++ test framework: Catch2 v3**, vendored as source in `ThirdParty/Catch2/` (amalgamated `.hpp` + `.cpp`). Rationale: mature, well-documented, works cleanly with SPM as a regular `.target`; vendoring keeps the repo dependency-free (no submodules, no package registry). Catch2's own `main()` provides the test runner — C++ tests are invoked via `swift run JboxEngineCxxTests` (debug by default for ThreadSanitizer coverage). Swift-side tests continue to use Swift Testing.
- **Execution style: per-primitive commits.** Order: `AtomicMeter` → `ChannelMapper` → `RtLogQueue` → `RingBuffer` (with concurrent stress) → `DriftTracker`. Each commit adds one primitive plus its unit tests, passes CI independently, and is individually revertible.
- **ThreadSanitizer** enabled for the `JboxEngineCxxTests` target in debug configuration via `.unsafeFlags(["-fsanitize=thread"])`. Release builds unaffected. CI runs the C++ tests in debug mode.

**Entry criteria.** Phase 1 complete.

**Exit criteria.**
- [x] `RingBuffer`, `AtomicMeter`, `RtLogQueue`, `DriftTracker`, `ChannelMapper` all implemented in `Sources/JboxEngineC/rt/` or `control/` as appropriate.
- [x] Each primitive has a dedicated unit-test file with concurrent stress tests where applicable.
- [x] ThreadSanitizer enabled for test builds; no data races detected.
- [x] `scripts/rt_safety_scan.sh` clean against `rt/`.
- [ ] `clang-tidy` warnings in the engine sources cleared or explicitly suppressed with justification. **Deferred to Phase 3** — set up a proper clang-tidy config once we have Core Audio integration code to lint against; lint value on the five synthetic primitives alone is low and would risk over-fitting rules.

**Tasks.**

Data structures:
- [x] `ring_buffer.hpp` — lock-free SPSC ring buffer in `Sources/JboxEngineC/rt/`.
  - Fixed-size at construction; no runtime resize.
  - Interleaved `float` samples.
  - `writeFrames(const float*, size_t frames)` and `readFrames(float*, size_t frames)`.
  - Returns count of frames actually written / read; underrun and overrun do **not** throw.
  - Atomic head / tail with appropriate memory ordering.
  - Deviation: header-only; the caller owns the backing storage (allows the class itself to stay allocation-free and live in `rt/`).
- [x] `atomic_meter.hpp` — `std::atomic<float>` per channel; update-max on write; atomic exchange-with-zero on read. Fixed 64-channel capacity (stack-allocated, no heap).
- [x] `rt_log_queue.hpp` — SPSC queue of fixed-size log records (numeric code, timestamp, payload). Templated on capacity; `DefaultRtLogQueue = RtLogQueue<1024>`.
- [x] `drift_tracker.hpp` — PI controller with anti-windup. Lives in `control/` (runs at ~100 Hz from the control thread). Deliberately decoupled from ring-buffer semantics: caller computes the error, tracker is a pure PI controller.
- [x] `channel_mapper.hpp` / `.cpp` — validates edge lists against v1 invariants (non-empty, non-negative indices, unique sources, unique destinations).

Tests:
- [x] `ring_buffer_test.cpp` — single-thread correctness, wrap-around, full- and empty-buffer behaviour; 200k-frame SPSC stress test (no separate `_stress_test.cpp` file — stress tests colocated for easier discovery).
- [x] `drift_tracker_test.cpp` — fresh state, proportional-only, integral-only, combined PI with arithmetic verification, anti-windup on saturation, zero/negative dt edge cases, realistic ppm-scale run with Phase 4 starting gains.
- [x] `channel_mapper_test.cpp` — accepts valid edge lists, rejects duplicates, rejects mismatched counts, precedence ordering between error conditions.
- [x] `atomic_meter_test.cpp` — update-max semantics, atomic-read-reset, channel independence, out-of-range safety, 8-thread concurrent writers, SPSC producer/consumer interleaving.
- [x] `rt_log_queue_test.cpp` — FIFO ordering, wrap-around, full/empty behaviour, 100k-message SPSC stress, slow-consumer drop test.

Quality gates:
- [x] ThreadSanitizer enabled via `swift run --sanitize=thread JboxEngineCxxTests`; all stress tests pass under race detection.
- [x] `rt_safety_scan.sh` clean (now with C++ comment stripping to eliminate false positives on explanatory text).
- [x] CI passes on every Phase 2 commit.

Deviations from the original Phase 2 plan worth noting:
- **Sanitizer flag delivery.** The original plan put `.unsafeFlags(["-fsanitize=thread"])` in Package.swift; that doesn't work because the flag is a clang-driver flag, not a linker flag. Resolution: sanitizer is invoked via SPM's `swift run --sanitize=thread` at the CLI (and in CI). Cleaner and the package stays configuration-agnostic.
- **`rt_safety_scan.sh` comment awareness.** Legitimate comments containing words like "no printf" or "new head" were tripping the scanner. Fixed during the RtLogQueue commit by adding a C++-comment stripper in awk before the banned-symbol grep. Line numbers in reports still reference the original source.
- **Ring buffer + log queue header-only.** Both live in `.hpp` only (no `.cpp`) because they are templated on capacity (log queue) or template-like in structure (ring buffer takes external storage). Simpler for callers and there was no implementation cost to paying.
- **`clang-tidy` deferred.** See exit-criteria note above.

---

## Phase 3 — First working route

**Status:** ✅ Code complete, CI green (8 commits: `7b7064a`..`6b74c59`). **Manual hardware acceptance test deferred** until the owner connects a V31 + Apollo (or equivalents). See the deferred-task checklist at the bottom of this phase.

**Goal.** Integrate Core Audio HAL into the engine. Prove the architecture end-to-end by making audio flow from V31 to Apollo Virtual outputs in real time. **Drift correction is deliberately out of scope for this phase** — over long runs the route may drift, which is acceptable here. The purpose is to validate Core Audio integration, IOProc registration, and the ring buffer bridge, not long-term stability.

**Entry criteria.** Phase 2 complete. A Roland V31 and a UA Apollo available for testing (or equivalent devices).

**Exit criteria.**
- [x] The engine enumerates connected Core Audio devices and exposes them via the bridge API.
- [~] Starting a route configured as V31 ch 1,2 → Apollo Virt 1,2 makes audio flow. **Deferred to manual test** — verified in CI/tests against SimulatedBackend with byte-exact sample-flow assertions; real-hardware verification pending.
- [~] Audible verification by the project owner using UA Console. **Deferred to manual test.**
- [~] ≤ 5 minutes of running without dropouts or glitches. **Deferred to manual test** (Phase 4 will establish the long-soak acceptance criterion once drift correction is in place).
- [x] `scripts/rt_safety_scan.sh` clean.

**Tasks.**

Engine — device layer:
- [x] Backend abstraction (`IDeviceBackend`) introduced so tests can drive the engine without real hardware.
  - [x] `SimulatedBackend` (commit #2) — deterministic, zero threads, no sleeps.
  - [x] `CoreAudioBackend` (commit #3) — real implementation via `AudioObjectGetPropertyData(kAudioHardwarePropertyDevices)`, `AudioDeviceCreateIOProcID`, `AudioDeviceStart` / `AudioDeviceStop`. Handles both interleaved and non-interleaved device formats.
- [x] `DeviceManager` (commit #4) — UID-keyed registry + enumeration cache.
- [ ] Register a listener for `kAudioHardwarePropertyDevices` changes; post events to the control thread for re-enumeration. **Deferred to Phase 5** (paired with multi-route device sharing).

Engine — route layer:
- [x] `RouteManager` (commit #5) — add / remove / start / stop route operations (control thread).
- [x] Route start sequence: resolve device handles, allocate `RingBuffer`, register IOProcs, transition to `running`.
- [x] Route stop sequence: close IOProcs via backend, release ring buffer storage.
- [x] Input IOProc copies selected channels from device input buffer into the route's ring buffer.
- [x] Output IOProc reads from ring buffer into selected channels of device output buffer. **No resampling yet** — assume matching sample rates.
- [ ] RCU-style active-route lists (IOProc multiplexing behind a single registered callback). **Deferred to Phase 5** — Phase 3 enforces one-route-per-direction-per-device via `JBOX_ERR_DEVICE_BUSY`.

Bridge API:
- [x] Implemented `jbox_engine_create`, `jbox_engine_destroy` (commit #6; Engine facade owns DeviceManager + RouteManager).
- [x] Implemented `jbox_engine_enumerate_devices` with heap-allocated snapshot and `jbox_device_list_free`.
- [x] Implemented `jbox_engine_add_route`, `jbox_engine_remove_route`, `jbox_engine_start_route`, `jbox_engine_stop_route`.
- [x] Implemented `jbox_engine_poll_route_status`.

CLI harness (commit #7):
- [x] `JboxEngineCLI --list-devices` — tabular device listing (direction / channel counts / sample rate / buffer / UID).
- [x] `JboxEngineCLI --route <src-uid>@<src-chs>-><dst-uid>@<dst-chs>` — creates and starts a route, polls status every second until SIGINT. Channels are 1-indexed on the CLI (converted to 0-indexed for the engine).

Tests:
- [x] Integration tests via `SimulatedBackend`: `RouteManager` (commit #5) and `Engine` / bridge API (commit #6) with byte-exact sample-flow assertions. Covers mapping validation, waiting-for-device, underrun counting, stop / remove semantics, device-sharing constraint, non-contiguous channel selection.
- [x] Swift wrapper tests against live Core Audio (commit #7 / #8): create, destroy, enumerate, error propagation.
- [ ] **Manual hardware acceptance test — deferred.** Procedure (tick boxes as they pass):
  - [ ] Connect a source device (ideally Roland V31) and a destination device (ideally UA Apollo).
  - [ ] Run `./scripts/verify.sh` and confirm everything still passes locally.
  - [ ] Run `swift run JboxEngineCLI --list-devices` and confirm both devices appear with the expected channel counts.
  - [ ] Run `swift run JboxEngineCLI --route '<v31-uid>@1,2-><apollo-uid>@5,6'` (substituting real UIDs and the desired virtual channels).
  - [ ] Play a note on the V31; verify UA Console shows signal on Virtual inputs 5/6 and that processed audio comes out of whatever bus you routed Virtual 5/6 to.
  - [ ] Let the route run for at least 5 minutes; note any audible clicks, dropouts, or obvious drift (some drift is expected pre-Phase 4).
  - [ ] Hit Ctrl-C; confirm the CLI shuts down cleanly, devices stop cleanly (no stuck "device in use" state on other apps), and the terminal returns promptly.

Phase 3 summary of deviations:
- **Backend abstraction introduced upfront** (not in the original spec). Pays for Phase 4's drift-tracking simulation needs in advance; exposes the engine cleanly for tests.
- **Device hot-plug listener deferred** to Phase 5 (couples naturally with shared-device work).
- **RCU multi-route lists deferred** to Phase 5; Phase 3 enforces one-route-per-direction-per-device.
- **`jbox::internal::createEngineWithBackend`** helper added (not public) so integration tests can substitute `SimulatedBackend` through the C bridge.

---

## Phase 4 — Drift correction and resampling

**Goal.** Add `AudioConverter` to every route (regardless of sample-rate match) and drive it from the drift tracker. Make the engine stable under long runs.

**Entry criteria.** Phase 3 complete. Phase 3's ≤ 5-minute route works on real devices.

**Exit criteria.**
- Any route runs for ≥ 30 minutes on real hardware (V31 + Apollo) with zero dropouts, zero overruns, zero underruns logged.
- A route configured with mismatched sample rates (e.g., source 48 k / destination 44.1 k) produces audibly correct audio.
- Integration tests with simulated clock drift (±50 ppm) converge within 10 seconds and hold within a target band for at least 5 simulated minutes.
- The drift tracker's PI gains are documented in `control/drift_tracker.cpp` and justified by test results.

**Tasks.**

Engine:
- [ ] `audio_converter_wrapper.hpp` / `.cpp` — thin wrapper around Apple `AudioConverter` with variable-ratio support.
- [ ] Wire `AudioConverter` into route lifecycle: construct at route start, destroy at route stop.
- [ ] Output IOProc reads from ring buffer, passes through `AudioConverter` with current ratio, writes to device output buffer.
- [ ] Drift tracker sampling: control-thread timer at ~100 Hz reads fill level, updates PI state, writes new rate to the `AudioConverter`.

Tuning:
- [ ] Integration test that injects a controlled clock-drift error and measures time-to-converge.
- [ ] Tune `Kp` and `Ki` so convergence is within 10 seconds and steady-state error stays below a threshold (define numeric threshold).
- [ ] Document final gains and the reasoning in `control/drift_tracker.cpp`.

Real-hardware verification:
- [ ] 30-minute soak test on real devices. Document procedure in a new file `docs/testing/soak.md`.
- [ ] Sample-rate mismatch test: explicitly set V31 and Apollo to different rates via Audio MIDI Setup, run a route, verify audio is correct.

Tests:
- [ ] Simulated clock-drift tests in `JboxEngineIntegrationTests` covering source-faster-than-dest, dest-faster-than-source, and transient bursts.
- [ ] Unit tests on the `AudioConverter` wrapper confirming ratio updates don't glitch the output stream.

---

## Phase 5 — Multi-route and shared devices

**Goal.** Support multiple simultaneous routes, including routes that share a source device or destination device. Prove the RCU-style active-route list pattern under load.

**Entry criteria.** Phase 4 complete.

**Exit criteria.**
- At least three concurrent routes run without interference.
- At least one test scenario has two routes sharing a source device, one route sharing a destination with one of those. All three run cleanly; adding / removing any one does not disturb the others.
- Integration tests under ThreadSanitizer do not flag races on the active-route lists.
- Real-hardware sanity test: start three routes, run for 10 minutes, no dropouts.

**Tasks.**

Engine:
- [ ] Implement RCU-style active-route list in `DeviceHandle` (`std::atomic<RouteList*>`), with deferred-free of old lists on the control thread.
- [ ] Modify input / output IOProcs to iterate the active-route list and dispatch per-route work.
- [ ] Safe route-add and route-remove: allocate new list, copy + modify, atomic pointer swap, schedule old list for deferred reclamation.

Tests:
- [ ] Integration tests for add-while-running, remove-while-running scenarios.
- [ ] Concurrent-route stress test: rapid start / stop of many routes on the same devices; verify no crashes, no leaks, no corrupted output.
- [ ] Real-hardware sanity test: three concurrent routes across V31 and Apollo.

---

## Phase 6 — SwiftUI UI

**Goal.** Build the v1 SwiftUI UI against the stable bridge API. No engine changes should be required to make the UI work.

**Entry criteria.** Phase 5 complete. Bridge API stable (no breaking changes planned during Phase 6).

**Exit criteria.**
- User can add / edit / delete routes through the main window route editor.
- User can start / stop routes; status (running / stopped / waiting / error) reflects correctly.
- Per-channel meters update live while routes run.
- Menu bar extra shows overall state and exposes toggles for each route.
- Preferences window exposes buffer-size policy, resampler quality, and appearance.
- UI works without Xcode IDE (builds via `swift build`); SwiftUI previews work when opened in Xcode.

**Tasks.**

Swift wrapper over bridge:
- [ ] `JboxEngineSwift/JboxEngine.swift` — ergonomic Swift types (`Device`, `Route`, `RouteStatus`, etc.) wrapping the C structs.
- [ ] Device enumeration as an `@Observable` (or `ObservableObject`) that republishes when Core Audio notifies the engine.
- [ ] Route status polling via a timer bound to SwiftUI.

Main window:
- [ ] `NavigationSplitView` layout: sidebar (All Routes + Scenes) + route list.
- [ ] Route row view with status glyph, name, source → destination summary, meters, start/stop button, `[⋯]` menu.
- [ ] Route editor sheet: device pickers, channel multi-select lists, mapping preview, validation.
- [ ] Scene editor sheet.
- [ ] Preferences window (`Settings` scene) with three tabs.

Menu bar extra:
- [ ] `MenuBarExtra` scene with dynamic icon.
- [ ] Popover content: per-route toggles, scene picker, Start All / Stop All, menu items.

Meters:
- [ ] SwiftUI `Canvas`-based meter view; single 30 Hz timer drives the whole app.
- [ ] Color thresholds and accessibility labels.

UI tests (minimal):
- [ ] SwiftUI preview providers for route row, route editor, sidebar.
- [ ] A couple of XCUITest flows in `JboxAppTests` (add route, start route, switch scene) — may be skipped on CI if they don't work headlessly, but runnable locally.

---

## Phase 7 — Persistence, scenes, launch-at-login

**Goal.** Make configured state durable across relaunches. Implement scenes. Offer opt-in launch-at-login.

**Entry criteria.** Phase 6 complete.

**Exit criteria.**
- Relaunching the app restores all configured routes and scenes exactly.
- Scenes activate correctly (exclusive and additive modes).
- Opt-in launch-at-login works: when toggled on, the app registers as a login item; after macOS login, Jbox starts automatically and appears in the menu bar (routes remain stopped per the design).
- Schema migration infrastructure in place even though only v1 exists (stub migrations compile and run).
- `state.json` is atomically written, debounced, backed up.

**Tasks.**

Persistence:
- [ ] `Persistence/AppState.swift` — `Codable` structs matching [spec.md § 3.1](./spec.md#31-entities).
- [ ] `Persistence/StateStore.swift` — debounced atomic writer, `.bak` backup, schema-version dispatch.
- [ ] Load-on-launch: read `state.json`, run migrations if needed, refuse to load newer-schema files with a user-visible error.

Scenes:
- [ ] Scene activation logic in the application layer (not the engine): given a scene, compute the set of routes to start / stop and issue the appropriate engine commands.
- [ ] Exclusive vs. additive activation modes.
- [ ] UI hook-up: sidebar click activates scene.

Launch-at-login:
- [ ] Implement via `SMAppService.mainApp.register()` / `.unregister()` (macOS 13+).
- [ ] Preferences toggle calls into this.
- [ ] First-time enabling shows an explanatory note.

Testing:
- [ ] Persistence round-trip tests (write, read, verify equality).
- [ ] Migration test: load a v1 JSON, serialize, confirm no data loss.
- [ ] Scene activation tests with mocked engine commands.

---

## Phase 8 — Packaging and installation

**Goal.** Produce a real distributable `.app` bundle. Make the installation story clear.

**Entry criteria.** Phase 7 complete.

**Exit criteria.**
- `scripts/bundle_app.sh` produces a valid `Jbox.app` that runs when dragged to `/Applications`.
- `scripts/build_release.sh` runs the full build, bundles, ad-hoc-signs, and leaves a ready-to-use `.app`.
- `scripts/package_unsigned_release.sh` produces a `.zip` containing the app and a `READ-THIS-FIRST.txt`.
- Smoke test on a clean macOS user account: unzip → drag to Applications → right-click → Open → Gatekeeper dialog → Open → app launches → microphone permission dialog → grant → app works.

**Tasks.**

Scripts:
- [ ] `scripts/bundle_app.sh` — full implementation:
  - Create `build/Jbox.app/Contents/{MacOS,Resources}`.
  - Copy `.build/release/JboxApp` → `Contents/MacOS/Jbox`.
  - Generate `Info.plist` from template with version / build number / bundle id / microphone usage description.
  - Copy `Jbox.icns` → `Contents/Resources/`.
  - `codesign --sign - --force --options runtime Jbox.app` (ad-hoc signing with Hardened Runtime).
- [ ] `scripts/build_release.sh` — convenience wrapper: `swift build -c release` + `bundle_app.sh`.
- [ ] `scripts/package_unsigned_release.sh` — zips `Jbox.app` plus `READ-THIS-FIRST.txt` into `Jbox-<version>.zip`.
- [ ] `scripts/run_app.sh` — build + bundle + `open build/Jbox.app`.

Assets:
- [ ] Create `Jbox.icns` (placeholder acceptable in early iterations; the project owner can provide or commission one).
- [ ] `Info.plist.in` template with substitution tokens for version, build number, microphone usage description, bundle identifier.

Documentation:
- [ ] Update `README.md` quick-start section if any install / build steps changed during implementation.
- [ ] Add a `READ-THIS-FIRST.txt` template under `Sources/JboxApp/Resources/` used by `package_unsigned_release.sh`.

Testing:
- [ ] Fresh-user smoke test: create a new macOS user account, download the `.zip`, follow the `READ-THIS-FIRST.txt`, verify the app runs and a test route works.

---

## Phase 9 — Release hardening and device-level testing

**Goal.** Prove the release is ready for real use; set up release gates; cut v1.0.0.

**Entry criteria.** Phase 8 complete.

**Exit criteria.**
- All release-gate tests (see [spec.md § 5.6](./spec.md#56-release-gates)) pass.
- `v1.0.0` tag exists; GitHub Releases draft populated with `Jbox-1.0.0.zip` and the `READ-THIS-FIRST.txt`.
- `docs/testing/soak.md`, `docs/testing/latency.md`, `docs/testing/stress.md` document the real-hardware test procedures so future releases are reproducible.

**Tasks.**

Real-hardware tests (documented and run):
- [ ] **Soak test.** Run at least one representative route for ≥ 30 minutes on real hardware. Verify zero dropouts in logs, drift tracker in band, no clipping, meter values plausible. Document procedure in `docs/testing/soak.md`.
- [ ] **Latency measurement.** Loopback test: patch a destination output back into a source input via cable; inject a test pulse; measure round-trip latency. Confirm within ±1 ms of theoretical expectation. Document procedure in `docs/testing/latency.md`.
- [ ] **Stress / disconnect test.** Start and stop routes rapidly. Unplug and replug devices. Verify graceful recovery, no crashes, no stuck routes. Document procedure in `docs/testing/stress.md`.

CI release pipeline:
- [ ] GitHub Actions workflow triggered by tag `v*.*.*`:
  - Full build in release mode.
  - Run all automated tests.
  - Run `build_release.sh` and `package_unsigned_release.sh`.
  - Upload `.zip` as a draft release asset.
- [ ] Release checklist in `.github/ISSUE_TEMPLATE/release.md` covering the release-gate items from the spec.

Final polish:
- [ ] Review and update all three docs (`README.md`, `spec.md`, `plan.md`) for anything that changed during implementation.
- [ ] Scan all source files for stale TODOs / FIXMEs; address or file follow-up issues.
- [ ] Verify `LICENSE` has been decided and committed.
- [ ] Smoke test the release `.zip` from GitHub on a fresh user account.

Tagging:
- [ ] Tag `v1.0.0` on `master`.
- [ ] Promote the draft release to published on GitHub.

---

## After v1.0.0 — deferred work

The items in [spec.md § Appendix A](./spec.md#appendix-a--deferred--out-of-scope) are candidates for future versions. Pick up whichever is most valuable at the time. Likely ordering based on current intuition (subject to revision):

1. **Fan-out** — cheap extension of the v1 model; clearly useful ("send one source to both speakers and headphones").
2. **Developer ID signing + notarization** — if distribution audience grows and the one-time Gatekeeper step becomes a real pain.
3. **Per-route gain / trim** — useful once the mixer boundary is carefully drawn.
4. **Auto-update via Sparkle** — useful once notarization is in place.
5. **Fan-in / summing** — requires explicit design work because it crosses into mixer territory.

Each of these gets its own short spec update and its own mini-plan at the time of implementation.
