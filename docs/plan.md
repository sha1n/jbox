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
| 2     | Engine core primitives                | Ring buffer, RT log queue, atomic meter, drift tracker — unit-tested and RT-safe.     | ⏳ Pending |
| 3     | First working route                   | Audio from V31 → Apollo Virtual outputs, end-to-end, with real devices.               | ⏳ Pending |
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

**Goal.** Implement the RT-safe primitives the engine is built from, with full unit-test coverage, **before** introducing Core Audio. This phase is entirely synthetic — no real audio devices yet.

**Entry criteria.** Phase 1 complete.

**Exit criteria.**
- `RingBuffer`, `AtomicMeter`, `RtLogQueue`, `DriftTracker`, `ChannelMapper` all implemented in `Sources/JboxEngineC/rt/` or `control/` as appropriate.
- Each primitive has a dedicated unit-test file with concurrent stress tests where applicable.
- ThreadSanitizer enabled for test builds; no data races detected.
- `scripts/rt_safety_scan.sh` clean against `rt/`.
- `clang-tidy` warnings in the engine sources cleared or explicitly suppressed with justification.

**Tasks.**

Data structures:
- [ ] `ring_buffer.hpp` / `.cpp` — lock-free SPSC ring buffer in `Sources/JboxEngineC/rt/`.
  - Fixed-size at construction; no runtime resize.
  - Interleaved `float` samples.
  - `write(const float*, size_t frames)` and `read(float*, size_t frames)`.
  - Returns count of frames actually written / read; underrun and overrun do **not** throw.
  - Atomic head / tail with appropriate memory ordering.
- [ ] `atomic_meter.hpp` — `std::atomic<float>` per channel; update-max on write; atomic exchange-with-zero on read.
- [ ] `rt_log_queue.hpp` / `.cpp` — SPSC queue of fixed-size log records (numeric code, timestamp, payload).
- [ ] `drift_tracker.hpp` / `.cpp` — PI controller over ring buffer fill level. Lives in `control/`, not `rt/`, because it's driven from the control thread. Exposes `adjustment_ppm() -> double`.
- [ ] `channel_mapper.hpp` / `.cpp` — validates edge lists against v1 invariants (unique sources, unique destinations, matching counts).

Tests:
- [ ] `ring_buffer_test.cpp` — single-thread correctness, wrap-around, full-buffer behavior, empty-buffer behavior.
- [ ] `ring_buffer_stress_test.cpp` — two-thread stress test running for ≥10 seconds with a producer and consumer at different speeds; verify totals balance and no corruption.
- [ ] `drift_tracker_test.cpp` — given synthetic fill-level time series, verify convergence within a target band.
- [ ] `channel_mapper_test.cpp` — accepts valid edge lists, rejects duplicates, rejects mismatched counts.
- [ ] `atomic_meter_test.cpp` — update-max semantics, atomic-read-reset.

Quality gates:
- [ ] ThreadSanitizer enabled for engine tests (`.unsafeFlags(["-fsanitize=thread"])` in the test target in `Package.swift` when appropriate).
- [ ] `rt_safety_scan.sh` still clean.
- [ ] CI passes.

---

## Phase 3 — First working route

**Goal.** Integrate Core Audio HAL into the engine. Prove the architecture end-to-end by making audio flow from V31 to Apollo Virtual outputs in real time. **Drift correction is deliberately out of scope for this phase** — over long runs the route may drift, which is acceptable here. The purpose is to validate Core Audio integration, IOProc registration, and the ring buffer bridge, not long-term stability.

**Entry criteria.** Phase 2 complete. A Roland V31 and a UA Apollo available for testing (or equivalent devices).

**Exit criteria.**
- The engine enumerates connected Core Audio devices and exposes them via the bridge API.
- Starting a route configured as V31 ch 1,2 → Apollo Virt 1,2 makes audio flow.
- Audible verification by the project owner using UA Console.
- ≤ 5 minutes of running without dropouts or glitches (longer soak deferred to Phase 4).
- `scripts/rt_safety_scan.sh` clean.

**Tasks.**

Engine — device layer:
- [ ] `device_manager.cpp` — enumerate devices via `AudioObjectGetPropertyData(kAudioHardwarePropertyDevices)`. For each device, read UID, name, sample rate, buffer frame size, channel counts.
- [ ] Register a listener for `kAudioHardwarePropertyDevices` changes; post events to the control thread for re-enumeration.
- [ ] `DeviceHandle` construction, input/output IOProc registration / unregistration, `AudioDeviceStart` / `AudioDeviceStop`.

Engine — route layer:
- [ ] `route_manager.cpp` — add / remove / start / stop route operations (control thread).
- [ ] Route start sequence: resolve device handles, allocate ring buffer, register IOProcs, transition to `running`.
- [ ] Route stop sequence: atomic removal from device active-route lists, RCU grace period, IOProc unregistration.
- [ ] Input IOProc copies selected channels from device input buffer into the route's ring buffer.
- [ ] Output IOProc reads from ring buffer into selected channels of device output buffer. **No resampling yet** — assume matching sample rates.

Bridge API:
- [ ] Implement `jbox_engine_create`, `jbox_engine_destroy`.
- [ ] Implement `jbox_engine_enumerate_devices` returning a snapshot.
- [ ] Implement `jbox_engine_add_route`, `jbox_engine_remove_route`, `jbox_engine_start_route`, `jbox_engine_stop_route`.
- [ ] Implement `jbox_engine_poll_route_status`.

CLI harness:
- [ ] Extend `JboxEngineCLI/main.swift` to accept `--list-devices` and `--route <src-uid>:<src-channels>-><dst-uid>:<dst-channels>`.
- [ ] `--route` command creates the engine, configures the route, starts it, runs until the user presses Ctrl-C.

Tests:
- [ ] Integration test using a simulation harness: stub device drivers (not Core Audio) that feed deterministic samples into the engine via the same IOProc pattern. Verify samples come out correctly.
- [ ] **Real-device manual test** — documented in this file as the Phase 3 acceptance procedure: connect V31 and Apollo, run the CLI to list devices, start a route V31 ch 1,2 → Apollo Virt 1,2, play a note on the V31, confirm sound in UA Console.

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
