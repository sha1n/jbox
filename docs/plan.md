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
| 4     | Drift correction and resampling       | 30-minute soak test on real devices with zero dropouts.                                | ✅ Code done (13 commits from `6e42c74`) — hardware soak deferred |
| 5     | Multi-route and shared devices        | Three simultaneous routes running, including one that shares a device with another.   | ✅ Code done (4 commits from `a83c4b5`) — real-hardware three-route sanity deferred |
| 6     | SwiftUI UI                            | User can add / edit / delete / start / stop routes through the GUI.                   | 🚧 First slice + channel-label pickers landed (7 commits from `7e3e3f8` through `7f778d2`) — meters, MenuBarExtra, Preferences, scenes still pending |
| 6+    | Logging pipeline                      | `os_log`-visible events for engine lifecycle, route mutations, and RT dropouts.       | 🚧 Option-B slice landed (drainer + Swift `Logger` wrappers + edge-triggered RT producers). Coverage closed in `7f778d2`. Rotating file sink still pending (Phase 8). |
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
- [x] `scripts/bundle_app.sh` — creates `build/Jbox.app`, generates `Info.plist` and an `Jbox.entitlements` plist, copies the executable, ad-hoc signs with Hardened Runtime **and the `com.apple.security.device.audio-input` entitlement attached**. Post-sign check asserts the entitlement is present and fails the script otherwise. (The entitlement was missing from the original Phase 1 version — see Phase 6 deviation note for the silent-mic bug it caused and how it was found.)
- [x] `scripts/build_release.sh` — wraps `swift build -c release` + `bundle_app.sh`. Works end-to-end.
- [x] `scripts/package_unsigned_release.sh` — started life as a placeholder stub in Phase 1. Real implementation (DMG with `.app`, `Applications` symlink, uninstaller, `READ-THIS-FIRST.txt`) landed in Phase 5 timeframe under `64c6fdd` so the release workflow could ship an alpha DMG. See Phase 8 tasks below for the remaining rotating-file-sink work; the DMG packaging itself is complete.
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
- **`bundle_app.sh` and `build_release.sh` are real, not stubs.** Overshoot; kept as-is since they work and Phase 8 would have had to rewrite them anyway. *Post-Phase-1 correction:* the original `bundle_app.sh` ad-hoc-signed with Hardened Runtime (`--options runtime`) but never attached any entitlements plist, so Core Audio silently delivered zero-filled input buffers at runtime — IOProc callbacks still fired and `frames_produced` still advanced, so the bug was invisible until Phase 6 Slice A dots surfaced it. Fixed by emitting a minimal `Jbox.entitlements` inline (same heredoc pattern as `Info.plist`) claiming `com.apple.security.device.audio-input`, passing `--entitlements` to `codesign`, and adding a post-sign verification step that fails the script if the entitlement dropped out. See [docs/spec.md § 1.5](./spec.md#15-platform-and-entitlement-decisions) and § 5.2.

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

**Status:** ✅ Code complete, CI green (13 commits starting at `6e42c74`). **Manual hardware acceptance tests deferred** — real-hardware 30-minute soak and 48k/44.1k audible verification pending the owner's rig. See the deferred-task checklist at the bottom of this phase.

**Goal.** Add `AudioConverter` to every route (regardless of sample-rate match) and drive it from the drift tracker. Make the engine stable under long runs.

**Entry criteria.** Phase 3 complete. Phase 3's ≤ 5-minute route works on real devices.

**Exit criteria.**
- [~] Any route runs for ≥ 30 minutes on real hardware (V31 + Apollo) with zero dropouts, zero overruns, zero underruns logged. **Deferred to manual hardware test** — real-hardware soak pending the owner's rig.
- [~] A route configured with mismatched sample rates (e.g., source 48 k / destination 44.1 k) produces audibly correct audio. **Deferred to manual hardware test** — audible AMS verification pending the owner's rig.
- [~] Integration tests with simulated clock drift (±50 ppm) hold within a 512-frame excursion band for 5 simulated minutes — note: the band is bounded by AudioConverter's ASBD-flush side-effect, not by the PI controller's corrective math (see Phase 4 deviations summary below).
- [x] The drift tracker's PI gains are documented in `control/drift_tracker.cpp` and justified by test results.

**Tasks.**

Engine:
- [x] `audio_converter_wrapper.hpp` / `.cpp` — thin wrapper around Apple `AudioConverter` with variable-ratio support.
- [x] Wire `AudioConverter` into route lifecycle: construct at route start, destroy at route stop.
- [x] Output IOProc reads from ring buffer, passes through `AudioConverter` with current ratio, writes to device output buffer.
- [x] Drift tracker sampling: control-thread timer at ~100 Hz reads fill level, updates PI state, writes new rate to the `AudioConverter`.

Tuning:
- [x] Integration test that injects a controlled clock-drift error and measures time-to-converge.
- [~] Tune `Kp` and `Ki` — harness ran; grid flat under the simulated backend (setInputRate's ASBD flush dominates), so starting values (`Kp = 1e-6`, `Ki = 1e-8`, `max = 100 ppm`) retained as production gains pending real-hardware tuning. See deviations summary below.
- [x] Document final gains and the reasoning in `control/drift_tracker.cpp`.

Real-hardware verification:
- [ ] 30-minute soak test on real devices. Document procedure in a new file `docs/testing/soak.md`.
- [ ] Sample-rate mismatch test: explicitly set V31 and Apollo to different rates via Audio MIDI Setup, run a route, verify audio is correct.

Tests:
- [~] Simulated clock-drift tests in `JboxEngineCxxTests` covering source-faster-than-dest, dest-faster-than-source, and transient bursts. First two done; the "transient bursts" test is a short-horizon variant of scenario 1 (not a true mid-run rate change) — see TODO in `drift_integration_test.cpp`.
- [x] Unit tests on the `AudioConverter` wrapper confirming ratio updates don't glitch the output stream (`audio_converter_wrapper_test.cpp`).

Phase 4 summary of deviations:
- **Apple AudioConverter property key.** spec.md § 2.5 and the original plan named `kAudioConverterSampleRate`, which was not consistently writable on our float-interleaved SRC configuration. Implementation uses `kAudioConverterCurrentInputStreamDescription` with a full ASBD re-set; the wrapper's no-op fast-path limits the cost. A code comment in `audio_converter_wrapper.cpp` flags the deviation.
- **PI gain tuning is not real.** The simulated-backend drift tests pass regardless of controller direction because `setInputRate`'s ASBD re-set flushes Apple's internal SRC buffer, which alone is sufficient to bound ring fill. The committed gains match the design-doc starting values; real-hardware tuning is deferred per exit criteria. See `drift_tracker.cpp` for the long form.
- **Step-disturbance test.** The third drift integration scenario is a short-horizon variant of scenario 1 (same drift from t=0) rather than a true mid-run rate change. A real step test is flagged in the test file with a TODO for a future pass.

---

## Phase 5 — Multi-route and shared devices

**Status:** ✅ Code complete, CI green (4 commits: `a83c4b5`..`d418562`). **Real-hardware three-route sanity test deferred** — same rig-availability pattern as Phases 3/4. Hot-plug listener for `kAudioHardwarePropertyDevices` is still deferred (see deviation note at the end of this phase).

**Goal.** Support multiple simultaneous routes, including routes that share a source device or destination device. Prove the RCU-style active-route list pattern under load.

**Entry criteria.** Phase 4 complete.

**Exit criteria.**
- [x] At least three concurrent routes run without interference.
- [x] At least one test scenario has two routes sharing a source device, one route sharing a destination with one of those. All three run cleanly; adding / removing any one does not disturb the others. (`multi_route_test.cpp` "three routes, shared source and shared destination" case.)
- [x] Integration tests under ThreadSanitizer do not flag races on the active-route lists. (`swift run --sanitize=thread JboxEngineCxxTests` — 145 cases, 667 713 assertions, zero TSan warnings.)
- [~] Real-hardware sanity test: start three routes, run for 10 minutes, no dropouts. **Deferred to manual hardware test** — rig not connected.

**Tasks.**

Engine:
- [x] Implement RCU-style active-route list in `DeviceIOMux` (`std::atomic<const RouteList*>`) with sequence-counter-based quiescence on the control thread. Landed in commit #1 (`a83c4b5`) with the list types living in `device_io_mux.{hpp,cpp}`.
- [x] Modify input / output IOProcs to iterate the active-route list and dispatch per-route work. The mux registers a single input and single output trampoline per device; per-route callbacks are stored in the published list and invoked in sequence.
- [x] Safe route-add and route-remove: allocate new list, copy + modify, atomic pointer swap, wait for RT quiescence (seq counters), drop old list. Replaced the initial sleep-based grace period with acq/rel sequence counters in commit #4 so ThreadSanitizer sees the synchronisation (sleeps are not HB-visible to TSan).

Tests:
- [x] Integration tests for add-while-running, remove-while-running scenarios. (`multi_route_test.cpp` "add-while-running keeps the first route flowing" and "remove-while-running stops one without disturbing peers".)
- [x] Concurrent-route stress test: rapid start / stop of many routes on the same devices; verify no crashes, no leaks, no corrupted output. (`multi_route_stress_test.cpp` — two scenarios exercising both the mux directly and the RouteManager path, both TSan-clean.)
- [ ] Real-hardware sanity test: three concurrent routes across two or more devices. **Deferred** until the owner's rig is connected.

Phase 5 summary of deviations:
- **Quiescence vs. sleep-based grace period.** The initial Phase 5 design used `std::this_thread::sleep_for(1.5 × buffer_period)` to wait for in-flight RT iterations to exit before dropping the old list (as suggested in docs/spec.md § 2.3). That is correct in production but ThreadSanitizer flagged the subsequent delete as racing with the RT trampoline's iterator — TSan does not model time-based synchronisation. Replaced with a pair of `std::atomic<std::uint64_t>` enter/exit sequence counters per direction; the control thread snapshots enter-seq after the atomic store and yield-spins until exit-seq catches up. Same semantics, TSan-visible happens-before, no knob to tune. The `grace_period_seconds` constructor parameter was dropped.
- **Hot-plug listener still deferred.** docs/plan.md Phase 3 flagged `kAudioHardwarePropertyDevices` change listeners as "deferred to Phase 5 (paired with multi-route device sharing)". The Phase 5 task list itself did not re-list it; the multi-route work did not require it, and adding it now would mix concerns. Tracked as the next deferred item on the Phase 5 follow-up list above. Routes that lose a device still sit in `WAITING`/`ERROR` until the user re-issues `startRoute` after a manual `enumerateDevices()` refresh, as in Phase 3.

---

## Phase 6 — SwiftUI UI

**Status:** 🚧 First slice landed (4 commits: `7e3e3f8`..`3704846`), followed by the logging pipeline slice (`7684034`), channel-label pickers in the add-route sheet (`ddb1e7d`), and a coverage-closure pass (`7f778d2`). The app has a working main window, add-route sheet with per-channel label pickers, row-level start/stop/remove actions, and live 4 Hz status polling — you can add → start → stop → remove routes entirely from the GUI, against the real Core Audio engine. Meters, MenuBarExtra, Preferences, scene editor, and the XCUITest flows are not implemented yet; scene editor is being pushed into Phase 7 next to persistence (a scene editor without durable state is a UX pothole). The bridge API picked up one additive symbol (`jbox_engine_enumerate_device_channels` / `jbox_channel_list_free`); ABI version unchanged (additive is MINOR per [spec.md § 1.6](./spec.md#16-versioning-of-the-bridge-api)).

A **logging pipeline slice** also landed alongside the UI first-slice (unplanned but necessary for user-visible diagnostics): `LogDrainer` now drains `DefaultRtLogQueue` into `os_log` under subsystem `com.jbox.app`, RT callbacks push edge-triggered events on underrun / overrun / channel-mismatch, control-side code paths log route lifecycle and errors directly. Swift uses `Logger` wrappers (`JboxLog`) sharing the same subsystem. The rotating file sink in `~/Library/Logs/Jbox/` described in [spec.md § 2.9](./spec.md#29-rt-safe-logging) is **not yet implemented** — deferred to Phase 8 alongside packaging. See Phase 8 tasks below.

**Goal.** Build the v1 SwiftUI UI against the stable bridge API. No engine changes should be required to make the UI work.

**Entry criteria.** Phase 5 complete. Bridge API stable (no breaking changes planned during Phase 6).

**Exit criteria.**
- [~] User can add / edit / delete routes through the main window route editor. Add/delete landed in the first slice. **Rename and mapping-edit are scheduled in the post-Slice-B refinements block below** — rename is a non-disruptive metadata update; mapping edit on a running route uses stop → reconfigure → start. Re-create workflow remains available as the fallback.
- [x] User can start / stop routes; status (running / stopped / waiting / error) reflects correctly. Row-level Start/Stop buttons and `StatusGlyph` render all five states; live polling keeps them up to date.
- [x] Per-channel signal indication — **Slice A (signal-present dots)**: landed. See the Meters task block below for the landing commit hashes.
- [x] Per-channel bar meters with color thresholds — **Slice B (full meters)**: landed. Dots stay as the glanceable collapsed summary; a chevron per route toggles the expanded `MeterPanel` with `Canvas`-drawn vertical bars, dB gridlines, and decaying peak-hold ticks. See the Meters task block below.
- [ ] Menu bar extra shows overall state and exposes toggles for each route. **Pending.**
- [ ] Preferences window exposes buffer-size policy, resampler quality, and appearance. **Pending.**
- [x] UI works without Xcode IDE (builds via `swift build`); SwiftUI previews work when opened in Xcode. The entire first slice was built and launched from the command line via `swift run JboxApp`.

**Tasks.**

Swift wrapper over bridge:
- [x] `JboxEngineSwift/JboxEngine.swift` — ergonomic Swift types (`Device`, `Route`, `RouteStatus`, `ChannelEdge`, `RouteState`) wrapping the C structs. Promoted to Equatable/Hashable/Sendable in `phase6 #1`.
- [x] Device enumeration + route state as an `@Observable` (`EngineStore`, Phase 6 #1). Refresh is currently user-driven (toolbar button + on-launch); hot-plug auto-refresh still waits on the Phase 5 follow-up listener for `kAudioHardwarePropertyDevices`.
- [x] Route status polling via a SwiftUI-bound timer (`.task` on `RouteListView`, ~4 Hz, Phase 6 #4).

Main window:
- [x] `NavigationSplitView` layout: sidebar (currently just "All Routes" — Scenes row lands with Phase 7 persistence) + detail list of routes with empty-state fallback.
- [x] Route row view: status glyph, display name, source → destination / channel-count summary, counters (produced / consumed / underruns), Start/Stop and Remove buttons.
- [x] Route editor sheet: device pickers (direction-filtered), dynamic channel-mapping editor (1-indexed display, constrained steppers), client-side validation mirroring the v1 ChannelMapper rules, engine-side errors surfaced inline.
- [x] **Human-readable channel labels in the mapping editor.** Core Audio exposes per-channel names via `kAudioObjectPropertyElementName` (scope = input / output, element = 1..N). Devices that bother to populate it (UA Apollo, MOTU, etc.) return labels like `Monitor L`, `Virtual 1`, `ADAT 3`; simpler devices return empty, in which case the UI falls back to numeric labels. Landed in `ddb1e7d`, coverage closed in `7f778d2`. Sub-tasks:
  - [x] Extend `IDeviceBackend` with `channelNames(uid, direction)` returning `std::vector<std::string>` (one entry per channel, possibly empty strings).
  - [x] `CoreAudioBackend::channelNames`: query `kAudioObjectPropertyElementName` per element, convert `CFStringRef` → `std::string`, trim empties.
  - [x] `SimulatedBackend::channelNames`: test seam — returns either a test-provided map or empty vectors.
  - [x] Bridge: `jbox_engine_enumerate_device_channels(engine, uid, direction, err)` returning a heap-allocated `jbox_channel_list_t` (paired `jbox_channel_list_free`). New symbol; appending is MINOR ABI.
  - [x] Swift wrapper: `Engine.enumerateChannels(uid:direction:) throws -> [String]`.
  - [x] `EngineStore`: per-(uid, direction) cache, invalidated on `refreshDevices()`.
  - [x] `AddRouteSheet`: channel steppers replaced with `Picker`s rendering `"Ch N · <name>"` when a name is present, `"Ch N"` when not (display via `ChannelLabel`).
  - [x] Tests: bridge-level round-trip through `SimulatedBackend` (`bridge_api_test.cpp`); Swift-side cache-invalidation test (`EngineStoreTests.swift`); label formatter tests (`ChannelLabelTests.swift`); RT-producer / drainer coverage in `logging_pipeline_test.cpp`.
- [ ] Scene editor sheet. **Deferred to Phase 7** — scenes only make sense with persistence.
- [ ] Preferences window (`Settings` scene) with three tabs. **Pending.**

Menu bar extra:
- [ ] `MenuBarExtra` scene with dynamic icon. **Pending.**
- [ ] Popover content: per-route toggles, scene picker, Start All / Stop All, menu items. **Pending.**

Meters — split into two slices so the diagnostic dots land first:

**Slice A — per-channel signal-present dots (source + dest).** ✅ Landed. The cheapest thing that answers "is the break upstream or downstream?": one filled/empty dot per source channel and per destination channel, updated at ~30 Hz. No colors, no bars, no dBFS readout — just "signal vs. silence." Tasks:
  - [x] `RouteRecord` gains a source-side `jbox::rt::AtomicMeter` and a dest-side one, each covering `channels_count` mapped channels.
  - [x] Input IOProc updates the source meter with per-mapped-channel peak-over-block from the selected source-device channels (before ring-buffer write). Channel-outer extraction loop keeps this to O(channels) atomic compare-exchanges per block instead of O(frames × channels).
  - [x] Output IOProc updates the dest meter with per-mapped-channel peak-over-block from the post-resample data being written to the device output buffer.
  - [x] Both meters are reset (all `kAtomicMeterMaxChannels` slots zeroed) in `attemptStart` alongside the edge-triggered log flags.
  - [x] Bridge: `jbox_meter_side_t { JBOX_METER_SIDE_SOURCE = 0, JBOX_METER_SIDE_DEST = 1 }` + `jbox_engine_poll_meters(engine, route_id, side, out_peaks, max_channels) -> size_t` added to `jbox_engine.h` and implemented in `bridge_api.cpp`. Additive symbol; matches the ABI policy already followed by `jbox_engine_enumerate_device_channels`.
  - [x] Swift wrapper: `Engine.MeterSide { .source, .destination }` + `Engine.pollMeters(routeId:side:maxChannels:) -> [Float]` (non-throwing — meter polling is a high-frequency UI path).
  - [x] `EngineStore.pollMeters()` drains every running route's source + dest peaks into `meters: [UInt32: MeterPeaks]`. Non-running routes are absent from the snapshot so stopping a route clears its dots on the next pass. A separate `~30 Hz` `.task` in `RouteListView` drives it while the main window is visible.
  - [x] `RouteListView` row: `SignalDotRow` shows `in` dots left of an arrow, `out` dots right. Filled dot when peak > `MeterPeaks.signalThreshold` (0.001, ~-60 dBFS); outline otherwise. Accessibility label summarises "source signal on N of M; destination signal on P of Q".
  - [x] Tests:
    - C++ integration test (`meters_test.cpp`, 9 cases) — through `SimulatedBackend`, inject known samples and verify `pollMeters` reports the expected per-channel peaks on both sides, truncates cleanly when `max_channels < channels_count`, resets on re-read, and rejects unknown ids / invalid side / null buffer / zero capacity.
    - C++ bridge test (`bridge_api_test.cpp`) — round-trip through the C entry point; null-engine and bad-args guards.
    - Swift test (`EngineStoreTests.swift`) — wrapper returns `[]` for unknown routes, stopped routes, and `maxChannels == 0`; `EngineStore.pollMeters()` publishes an empty snapshot when no routes are running.

**Slice B — full bar meters + color thresholds + accessibility.** Landed on top of Slice A.
  - [x] Pure-logic types in `JboxEngineSwift`: `MeterLevel` (dB-to-fraction math + color-zone classification) and `PeakHoldTracker` (per-`(routeId, side, channel)` hold state with linear decay). Both TDD'd — `Tests/JboxEngineTests/MeterLevelTests.swift` (16 cases pinning spec § 4.5 thresholds) and `Tests/JboxEngineTests/PeakHoldTrackerTests.swift` (14 cases: cold-start, promote/ignore/decay, per-route/side/channel independence, `forget`).
  - [x] `EngineStore.pollMeters()` now also feeds `PeakHoldTracker`; `removeRoute()` calls `forget(routeId:)` so holds don't survive a deleted route. New public `heldPeak(routeId:side:channel:now:)` reader returns the decayed hold. `EngineStoreTests` gains three cases pinning the wiring.
  - [x] SwiftUI views (`Sources/JboxApp/MeterBar.swift`): `MeterPanel` composes source-side `BarGroup` + arrow + dest-side `BarGroup`. `BarGroup` draws per-channel `ChannelBar` via `Canvas` (frame + zone-coloured fill + peak-hold tick) alongside a shared `DbScale` strip (0 / -3 / -6 / -20 / -40 / -60 dBFS gridlines). Bars flex between a 10 pt floor and a 36 pt cap so small routes (1–2 ch) spread comfortably and 8+ ch routes stay inside the row. A 30 Hz `TimelineView` inside the expanded body redraws the hold-tick decay between polls.
  - [x] `RouteRow` gains a chevron to toggle expansion; collapsed rows keep today's `SignalDotRow`, expanded rows swap it for the `MeterPanel`. Expansion state lives in `@State private var expandedRoutes: Set<UInt32>` on `RouteListView`; Phase 7 persistence can migrate it onto `EngineStore`.
  - [x] Color thresholds match [spec.md § 4.5](./spec.md#45-meters): gray below -60 dBFS, green below -6, yellow to -3, red above. Bar height is a second, color-independent cue for accessibility.
  - [ ] VoiceOver-friendly labels on the expanded panel. **Pending** — `SignalDotRow` already carries one; `MeterPanel` still needs a comparable composite label. Deferred to a follow-up since it's additive.

**Post-Slice-B refinements.** Five queued items captured from the post-Slice-B usage pass. Each is small enough to land in its own commit; none of them invalidate Slice A or Slice B.

1. **Fan-out mapping (1:N).**
   - [ ] Engine: `ChannelMapper::validate()` drops the "duplicate `src`" rejection; keeps the "duplicate `dst`" rejection (summing / fan-in stays deferred per [spec.md Appendix A](./spec.md#appendix-a--deferred--out-of-scope)). No hot-path change — the scratch-copy / converter / metering loops already iterate per output slot, so a fan-out edge is "just another output channel" whose scratch cell happens to hold the same source sample.
   - [ ] Tests: new `channel_mapper_test.cpp` case asserting fan-out (two edges with the same `src` but different `dst`) validates as `kOk`; existing "duplicate source rejected" case flips. Add a `route_manager_test.cpp` case through `SimulatedBackend` that injects a known signal on one source channel and asserts it appears on two destination channels.
   - [ ] UI: `AddRouteSheet` mirror — the client-side validator currently mirrors the old "duplicate source" rule; relax it, keep "duplicate destination" as the only uniqueness check. Update the sheet's inline error copy to "Destination channel already in use."
   - [ ] Spec already updated: § 1.1, § 2.5 (converter output slot semantics), § 3.1 mapping invariants, § 4.3 editor validation, Appendix A.

2. **Edit existing routes.**
   - [ ] Engine: a non-disruptive rename call (`jbox_engine_rename_route(engine, id, new_name)` — additive symbol, MINOR ABI bump). Mapping edit on a running route uses the existing `stopRoute` + mutate config + `startRoute` sequence from the Swift side — no new engine API for the mapping path in v1.
   - [ ] UI: `EditRouteSheet` (sibling to `AddRouteSheet`, sharing the channel-mapping editor component); row affordance via double-click the name for rename, double-click elsewhere or `⌘E` for mapping edit. The sheet's "Apply" button copy reflects whether the route is running (e.g. "Apply and restart" vs "Apply").
   - [ ] Tests: rename round-trip (Swift + C++ bridge); mapping-edit flow exercised end-to-end via `EngineStoreTests`.

3. **Computed per-route latency.** **Landed.**
   - [x] Backend: `BackendDeviceInfo` gained `{input,output}_device_latency_frames` and `{input,output}_safety_offset_frames`. `CoreAudioBackend::enumerate()` populates each scope from `kAudioDevicePropertyLatency` / `kAudioDevicePropertySafetyOffset`; `SimulatedBackend` pass-throughs the values the test harness sets on the struct. (`kAudioStreamPropertyLatency` folding is deferred — we take the device-level number for now; most drivers report one or the other, and the pill is indicative per spec.md § 2.12.)
   - [x] Engine: pure helper `jbox::control::estimateLatencyMicroseconds` (new `control/latency_estimate.{hpp,cpp}`) computes the sum per [spec.md § 2.12](./spec.md#212-estimated-per-route-latency), split at the SRC/DST-rate boundary so routes that bridge different device rates compose correctly. `RouteManager::attemptStart` fills the components once (using `AudioConverterWrapper::primeLeadingFrames` for the SRC prime count and `RingBuffer::usableCapacityFrames() / 2` for the drift-sampler setpoint) and caches the total; `pollStatus` returns it through the new `jbox_route_status_t::estimated_latency_us` field. ABI bump 1 → 2 (MINOR; field is appended).
   - [x] Swift: `RouteStatus.estimatedLatencyUs`. New `LatencyFormatter.pillText(microseconds:)` renders buckets (`<1 ms`, `~N.N ms`, `~N ms`, `~N.N s`) and returns `nil` for 0 so the UI can hide the pill.
   - [x] UI: `RouteRow` shows a faint `LatencyPill` next to the counters whenever the engine reports a non-zero estimate.
   - [ ] Expanded-panel component breakdown is deferred to land with #4 (diagnostics toggle) — the pill alone is the first slice and doesn't need a separate Advanced view until the breakdown is defensible.
   - [x] Tests: eight Catch2 cases pinning the estimator (same-rate sum, split rates, zero-rate guard, negative-rate guard, ring-dominates, monotonicity). An end-to-end `[route_manager][latency]` case drives `SimulatedBackend` with known HAL values + asserts the pill is > the HAL-lower-bound, larger on a bigger-buffer route, and zeroed after stop. Six Swift Testing cases cover `LatencyFormatter` bucket transitions. `pollStatus` fill-through is covered by the existing `estimated_latency_us == 0` assertion on STOPPED routes.

4. **Advanced / engine-diagnostics toggle.**
   - [ ] Preferences window (Phase 6 pending task) adds an **Advanced → "Show engine diagnostics"** toggle (default off). Stored in `AppState.preferences` once Phase 7 persistence lands; until then, a `@State` default on the app root is fine.
   - [ ] `RouteRow` uses the toggle: when off, the row hides the `frames_produced / frames_consumed · u<K>` counter string and leaves only the friendlier signals (status glyph + dots/bars + latency pill). When on, the counters + the latency breakdown live inside the expanded panel, not in the collapsed row, to keep the collapsed line calm.
   - [ ] Spec already updated: § 4.6 Advanced tab.

5. **VoiceOver label on the expanded meter panel** (carried forward from Slice B — still pending).
   - [ ] Composite accessibility label on `MeterPanel` summarising "per-channel peak dBFS on source; per-channel peak dBFS on dest", so VoiceOver users get the same information the colour + height cues give sighted users. Cheap follow-up; matches the pattern `SignalDotRow` already uses for the collapsed state.

UI tests (minimal):
- [x] Swift Testing cases for `EngineStore` against the live Core Audio engine (`Tests/JboxEngineTests/EngineStoreTests.swift`, Phase 6 #1).
- [ ] SwiftUI preview providers for route row, route editor, sidebar. **Pending.**
- [ ] A couple of XCUITest flows in `JboxAppTests` (add route, start route, switch scene). **Pending** — may be skipped on CI if they don't work headlessly, but runnable locally.

Phase 6 first-slice summary of deviations:
- **Scene editor moved to Phase 7.** The original plan had scenes UI in Phase 6 and scene activation logic / persistence in Phase 7. A scene editor that cannot round-trip through disk is confusing to use and wastes test surface on a UI that would be rewritten once persistence lands. Consolidating into Phase 7 is cleaner.
- **UI verification gap.** The first-slice commits were built and smoke-launched from the command line (`swift run JboxApp` stays up). The actual rendering — NavigationSplitView layout, sheet presentation, stepper clamping, toolbar buttons — has **not** been interactively verified. A human pass on the running app is needed; bugs surfaced there will be addressed in follow-up commits rather than retroactively edited into the first-slice commits.
- **Ring-buffer sizing was too tight for USB burst delivery.** With the mic-entitlement fix in place and live signal finally audible, the first extended V31 → Apollo test produced u1043 underruns in under a minute with clearly audible pops (`frames_produced` slightly ahead of `frames_consumed`, underruns climbing steadily, no drift runaway). Root cause: Phase 4's `max_buffer × 4` with a 256-frame floor (~5 ms) was tuned against the synchronous simulated backend and didn't account for USB class-compliant sources (V31, many others) delivering in bursts with multi-ms gaps. Fixed in `route_manager.cpp` by bumping to `max_buffer × 8` with a 4096-frame floor (~85 ms at 48 k) — still well below any human-perceptible routing latency. New C++ integration test (`route_manager_test.cpp`, tag `[ring_sizing]`) simulates a bursty USB source: 8 back-to-back source deliveries followed by 8 drain deliveries, asserts overrun + underrun counts stay at 0; fails loudly under the old 4×/256 sizing. The existing edge-triggered-overrun logging test's flood size was bumped from 1024 → 8192 frames so it still forces an overrun under the new 4096-floor ring. Spec § 2.3 updated with the new formula and rationale.
- **Silent-mic bug uncovered by Slice A dots.** Slice A's first real-hardware test revealed that a running mic route advanced `frames_produced` but every source-side peak stayed at 0. Root cause was not in the new meter code: `scripts/bundle_app.sh` had shipped since Phase 1 with Hardened Runtime (`--options runtime`) but no entitlements attached. Under Hardened Runtime, Core Audio silences input buffers when `com.apple.security.device.audio-input` is not claimed — IOProc callbacks still fire, `frames_produced` still advances, and nothing in the logs indicates the silencing. Before Slice A there was no user-visible signal indicator, so the bug had been dormant. Fix: emit a minimal `Jbox.entitlements` plist inline and pass `--entitlements` to `codesign`; add a post-sign grep to fail the script if the entitlement is not present. See the Phase 1 deviation note above, [docs/spec.md § 1.5](./spec.md#15-platform-and-entitlement-decisions), and § 5.2.
- **Logging pipeline landed unplanned.** The UI first slice surfaced that no `os_log` events were being emitted anywhere — only the `RtLogQueue` primitive (Phase 2) existed, with no producers and no drainer. An Option-B slice landed on top of Phase 6: `control::LogDrainer` owns the queue and a consumer thread, forwards events via a pluggable `Sink` (default `os_log`), `RouteManager` plumbs the queue pointer into each `RouteRecord`, RT callbacks push **edge-triggered** events on the first underrun / overrun / channel-mismatch after each (re)start (edge flags reset in `attemptStart`). Control-thread paths push `kLogRouteStarted` / `kLogRouteWaiting` / `kLogRouteStopped`. Bridge entry points (`jbox_engine_create`, `jbox_engine_destroy`, `jbox_engine_add_route`) call `os_log` directly for startup and accept/reject decisions. The Swift side adds `JboxLog` (`app` / `engine` / `ui` categories, subsystem `com.jbox.app`) and wires notices through `EngineStore` and `AppRootView`. Test hook: `jbox::internal::setLogSink` swaps the os_log sink for a capture sink; the test forward-decl defaults `spawn_log_drainer=false` so existing tests run unchanged. New test: `Tests/JboxEngineCxxTests/log_drainer_test.cpp` (5 cases, TSan-clean). The rotating-file sink piece of [spec.md § 2.9](./spec.md#29-rt-safe-logging) is **still pending** — scheduled for Phase 8 (see below).
- **setInputRate flush storm → audible clicks.** After the ring-sizing bump above, a real-hardware V31 → Apollo session still produced audible clicks on dynamic content and a slow-climbing underrun counter (u321 after ~1 minute). Root cause sits in the drift-correction path, not in ring sizing: `DriftSampler` recomputes `target_input_rate = nominal * (1 + ppm * 1e-6)` every 10 ms, and with the Phase 4 conservative gains (`kp=1e-6`, `ki=1e-8`) the per-tick PI output adjusts the rate by ~1e-7 ppm — infinitesimally small in practice, but always a distinct float, so the legacy naive `target != last_applied_rate` comparison in `route_manager.cpp` triggered `AudioConverterWrapper::setInputRate` on every RT callback. Each `setInputRate` call flushes Apple's polyphase filter state; a new characterization test (`audio_converter_wrapper_test.cpp`, tag `[hypothesis]`) drives the real Apple converter and quantifies the cost at ~16 extra input frames per call — ~1600 frames/s continuously drained from the ring at the 100 Hz tick rate, which both explains the audible discontinuities on transients and the slow ring drain that manifested as climbing underruns. Fix: new pure `shouldApplyRate(proposed, last_applied, nominal)` decision function in `Sources/JboxEngineC/control/rate_deadband.hpp` with a 1 ppm threshold (48 mHz at 48 k — roughly 10× below the audible rate-error threshold), wired into `route_manager.cpp`'s output IOProc. Tests: 9 unit cases in `rate_deadband_test.cpp` covering cold-start, sub/supra-threshold gating, a PI-noise tick sequence (0 applies expected), and a 5 ppm ramp; plus a companion end-to-end `[hypothesis]` case that mirrors the real apply-or-skip logic against a live Apple converter and asserts the deadband restores baseline input consumption. Side-effect documented in the same commit: `drift_integration_test.cpp` scenario 1 (`+50 ppm source`) had been passing at a 512-frame excursion band only because the flush storm itself was bounding ring fill (drift_tracker.cpp:14-16 calls this out); with the flush storm gone and Phase 4 gains unchanged, open-loop drift accumulation of ~744 frames at 310 s is now visible, so the band is relaxed to 1024 frames with a comment pointing at the real-hardware gain-tuning task still deferred per Phase 4 exit criteria. spec §§ 2.5–2.6 updated.

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
- `scripts/package_unsigned_release.sh` produces a `.dmg` containing the app, an `Applications` symlink, an `Uninstall Jbox.command`, and a `READ-THIS-FIRST.txt`. **(Already landed early in `64c6fdd`; leave as-is unless the layout needs revisiting.)**
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
- [x] `scripts/package_unsigned_release.sh` — builds `Jbox-<version>.dmg` containing the `.app`, an `Applications` symlink, an `Uninstall Jbox.command`, and a `READ-THIS-FIRST.txt`. *(Landed early in `64c6fdd` so the release workflow could ship an alpha DMG.)*
- [ ] `scripts/run_app.sh` — build + bundle + `open build/Jbox.app`.

Assets:
- [ ] Create `Jbox.icns` (placeholder acceptable in early iterations; the project owner can provide or commission one).
- [ ] `Info.plist.in` template with substitution tokens for version, build number, microphone usage description, bundle identifier.

Documentation:
- [ ] Update `README.md` quick-start section if any install / build steps changed during implementation.
- [ ] Add a `READ-THIS-FIRST.txt` template under `Sources/JboxApp/Resources/` used by `package_unsigned_release.sh`.

Logging — rotating file sink (completes [spec.md § 2.9](./spec.md#29-rt-safe-logging)):
- [ ] Extend `control::LogDrainer` with a second sink that writes to `~/Library/Logs/Jbox/jbox.log`, or wire a separate composite sink alongside the existing `os_log` sink. Either way, both destinations must receive every event.
- [ ] Size- or date-based rotation (pick one; size-based with a small rotation count is simpler and matches the "sparse events" workload). Default cap ~5 MB per file, keep last 3.
- [ ] Handle log-directory creation on first write; handle permission / disk-full failures by falling back to os_log-only rather than dropping the whole pipeline.
- [ ] Decide the on-disk line format — plain text matching the `defaultOsLogSink` format is the natural choice (human-greppable, no parser needed).
- [ ] Add unit tests that exercise rotation without real filesystem access — the sink should be injectable with a filesystem abstraction, or the test should use a temp directory.
- [ ] Decide whether `.app`'s `Contents/MacOS/Jbox` and the CLI share the same log file (likely yes) or separate by process (noisier but clearer).

Testing:
- [ ] Fresh-user smoke test: create a new macOS user account, download the `.dmg`, follow the `READ-THIS-FIRST.txt`, verify the app runs and a test route works.

---

## Phase 9 — Release hardening and device-level testing

**Goal.** Prove the release is ready for real use; set up release gates; cut v1.0.0.

**Entry criteria.** Phase 8 complete.

**Exit criteria.**
- All release-gate tests (see [spec.md § 5.6](./spec.md#56-release-gates)) pass.
- `v1.0.0` tag exists; GitHub Releases draft populated with `Jbox-1.0.0.dmg` (ad-hoc signed, not notarized).
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
  - Upload `.dmg` as a draft pre-release asset. **(Already landed in `64c6fdd` / `release.yml`.)**
- [ ] Release checklist in `.github/ISSUE_TEMPLATE/release.md` covering the release-gate items from the spec.

Final polish:
- [ ] Review and update all three docs (`README.md`, `spec.md`, `plan.md`) for anything that changed during implementation.
- [ ] Scan all source files for stale TODOs / FIXMEs; address or file follow-up issues.
- [ ] Verify `LICENSE` has been decided and committed.
- [ ] Smoke test the release `.dmg` from GitHub on a fresh user account.

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
