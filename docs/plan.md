# JBox — Implementation Plan

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
- [Phase 7 — Persistence + launch-at-login](#phase-7--persistence--launch-at-login)
- [Phase 7.5 — Device sharing (hog-mode opt-out)](#phase-75--device-sharing-hog-mode-opt-out)
- [Phase 7.6 — Self-routing reliability](#phase-76--self-routing-reliability)
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
| 6     | SwiftUI UI                            | User can add / edit / delete / start / stop routes through the GUI.                   | ✅ All in-scope items landed (first slice + channel-label pickers + meter Slices A & B + post-Slice-B refinements: fan-out, edit routes, latency pill + diagnostics, tier modes, VoiceOver on `MeterPanel` + MenuBarExtra + three-tab Preferences + SwiftUI previews + single-instance `Window` scene). XCUITest flows **deferred** under the SPM-only constraint — see Phase 6 deviations + "UI tests (minimal):" for the gap write-up and the recommended path when revisited. |
| 6+    | Logging pipeline                      | `os_log`-visible events for engine lifecycle, route mutations, and RT dropouts.       | ✅ Drainer + Swift `Logger` wrappers + edge-triggered RT producers (coverage closed in `7f778d2`); rotating file sink shipped under Phase 8 in `b873d5d`. |
| 7     | Persistence + launch-at-login         | Relaunching the app restores configured routes; opt-in launch-at-login.                | ✅ Persistence + launch-at-login slices landed. Routes + preferences round-trip through `state.json`; the General Preferences "Launch at login" toggle is wired to `SMAppService.mainApp` via `LaunchAtLoginController` with one-time explanatory note (latched in `StoredPreferences.hasShownLaunchAtLoginNote`), `requiresApproval` callout + System Settings deep link, refresh-on-launch reconciliation. Scenes (and the sidebar shell that hosted them) **deferred to a future release** — see § 4.10 in `docs/spec.md` and the "After v1.0.0 — deferred work" entry below; v1 carries no schema reservation, the feature returns as a `v1 → v2` migration. |
| 7.5   | Device sharing (hog-mode opt-out)     | ~~Per-route and global "share device with other apps" preference; lock-glyph indicator when hog mode is active.~~ | ⛔ **Superseded and reverted by Phase 7.6 simplification.** The hog-mode + buffer-shrink machinery this phase opted out of has been removed entirely from the engine. Share is now the only mode. |
| 7.6   | Self-routing reliability              | Drop hog mode + HAL buffer-shrink. Users dial buffer in their interface software; JBox respects it. Plus a per-route SD-style buffer preference (no hog), device hot-plug, sleep/wake, error-state UX. | 🚧 Big simplification landed (cuts ~2400 lines + Phase 7.5's ABI/UI surface). Re-added a no-hog `setBufferFrameSize` (ABI v10 → v11 additive) after Superior Drummer demonstrated the cascade was hog-eviction-side, not property-write-side. **7.6.3 robust teardown + 7.6.4 hot-plug listeners + 7.6.5 sleep/wake + 7.6.6 aggregate-loss & stall watchdog + 7.6.7 targeted listeners & ERROR-trap fix all landed engine-side (ABI v11 → v12 → v13 → v15 additive: `JBOX_ERR_DEVICE_GONE` + `JBOX_ERR_SYSTEM_SUSPENDED` + `JBOX_ERR_DEVICE_STALLED`).** Production wiring of 7.6.4's `CoreAudioBackend` HAL listeners (F1) + 7.6.5's `MacosPowerEventSource` (F2) deferred to hardware-tested follow-ups. |
| 8     | Packaging and installation            | `Jbox.app` runs from `/Applications` on a clean user account.                          | ✅ Done. Bundling lane (`bundle_app.sh` + `build_release.sh` + `package_unsigned_release.sh` + `run_app.sh`) shipped; rotating file sink (`os_log` + `~/Library/Logs/Jbox/<process>.log`, 5 MiB × 3 rotation, fail-silent on I/O errors) landed; fresh-user smoke test on a clean macOS user account passed (2026-05-01). |
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
- [x] Add `LICENSE` placeholder ("all rights reserved; license decision pending"). Per project owner decision, a permissive license will be chosen later; change is a single-file replacement. **Resolved 2026-05-08 in `b82e9d1`** — replaced with Apache License, Version 2.0.

Scripts:
- [x] `scripts/rt_safety_scan.sh` — scans `Sources/JboxEngineC/rt/` for banned symbols (allocators, locks, dispatch calls, non-RT-safe logging, smart pointer construction). Bash-3.2 compatible (macOS default shell). Clean pass on an empty `rt/`.
- [x] `scripts/bundle_app.sh` — creates `build/Jbox.app`, generates `Info.plist` and an `Jbox.entitlements` plist, copies the executable, ad-hoc signs with Hardened Runtime **and the `com.apple.security.device.audio-input` entitlement attached**. Post-sign check asserts the entitlement is present and fails the script otherwise. (The entitlement was missing from the original Phase 1 version — see Phase 6 deviation note for the silent-mic bug it caused and how it was found.)
- [x] `scripts/build_release.sh` — wraps `swift build -c release` + `bundle_app.sh`. Works end-to-end.
- [x] `scripts/package_unsigned_release.sh` — started life as a placeholder stub in Phase 1. Real implementation (DMG with `.app`, `Applications` symlink, uninstaller, `READ-THIS-FIRST.txt`) landed in Phase 5 timeframe under `64c6fdd` so the release workflow could ship an alpha DMG. See Phase 8 tasks below for the remaining rotating-file-sink work; the DMG packaging itself is complete.
- [x] `scripts/run_app.sh` — builds, bundles, and launches the `.app` locally via `open`.

CI:
- [x] `.github/workflows/ci.yml` configured for GitHub Actions `macos-15` runner.
- [x] CI runs: `swift build -c release`, `swift test`, `scripts/rt_safety_scan.sh`.
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
- [x] Register a listener for `kAudioHardwarePropertyDevices` changes; post events to the control thread for re-enumeration. **Landed in Phase 7.6.4 + F1 / 7.6.7** — `CoreAudioBackend` registers HAL property listeners against the system object + each enumerated device + each aggregate via `AudioObjectAddPropertyListener` (function-pointer variant + `std::shared_mutex`); see `docs/followups.md` § F1 for the production wiring write-up.

Engine — route layer:
- [x] `RouteManager` (commit #5) — add / remove / start / stop route operations (control thread).
- [x] Route start sequence: resolve device handles, allocate `RingBuffer`, register IOProcs, transition to `running`.
- [x] Route stop sequence: close IOProcs via backend, release ring buffer storage.
- [x] Input IOProc copies selected channels from device input buffer into the route's ring buffer.
- [x] Output IOProc reads from ring buffer into selected channels of device output buffer. **No resampling yet** — assume matching sample rates.
- [x] RCU-style active-route lists (IOProc multiplexing behind a single registered callback). **Landed in Phase 5 commit `a83c4b5`** — `DeviceIOMux` publishes a `std::atomic<const RouteList*>` with sequence-counter-based quiescence on the control thread (sleep-based grace period was replaced with acq/rel sequence counters in commit #4 so ThreadSanitizer sees the synchronisation).

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
- [~] **Manual hardware acceptance test — deferred to Phase 9 hardware session.** Procedure (tick boxes as they pass):
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
- [~] 30-minute soak test on real devices. Procedure drafted in `docs/testing/soak.md` (2026-05-01); execution deferred to Phase 9 hardware session.
- [~] Sample-rate mismatch test: explicitly set source and destination devices to different rates via Audio MIDI Setup, run a route, verify audio is correct. Deferred to Phase 9 hardware session.

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
- [~] Real-hardware sanity test: three concurrent routes across two or more devices. **Deferred to Phase 9 hardware session.**

Phase 5 summary of deviations:
- **Quiescence vs. sleep-based grace period.** The initial Phase 5 design used `std::this_thread::sleep_for(1.5 × buffer_period)` to wait for in-flight RT iterations to exit before dropping the old list (as suggested in docs/spec.md § 2.3). That is correct in production but ThreadSanitizer flagged the subsequent delete as racing with the RT trampoline's iterator — TSan does not model time-based synchronisation. Replaced with a pair of `std::atomic<std::uint64_t>` enter/exit sequence counters per direction; the control thread snapshots enter-seq after the atomic store and yield-spins until exit-seq catches up. Same semantics, TSan-visible happens-before, no knob to tune. The `grace_period_seconds` constructor parameter was dropped.
- **Hot-plug listener still deferred.** docs/plan.md Phase 3 flagged `kAudioHardwarePropertyDevices` change listeners as "deferred to Phase 5 (paired with multi-route device sharing)". The Phase 5 task list itself did not re-list it; the multi-route work did not require it, and adding it now would mix concerns. Tracked as the next deferred item on the Phase 5 follow-up list above. Routes that lose a device still sit in `WAITING`/`ERROR` until the user re-issues `startRoute` after a manual `enumerateDevices()` refresh, as in Phase 3.

---

## Phase 6 — SwiftUI UI

**Status:** 🚧 First slice landed (4 commits: `7e3e3f8`..`3704846`), followed by the logging pipeline slice (`7684034`), channel-label pickers in the add-route sheet (`ddb1e7d`), and a coverage-closure pass (`7f778d2`). Meter Slices A + B, the post-Slice-B refinements (including fan-out, edit routes, latency pill + diagnostics panel, and tiered latency mode), the MenuBarExtra scene, and the three-tab Preferences window have all landed since. The app has a working main window, add-route + edit-route sheets with per-channel label pickers, row-level start/stop/remove actions, live 4 Hz status polling, expanded meter panels, a menu bar extra with a dynamic icon + popover, and a full Settings window with General / Audio / Advanced tabs (appearance picker, buffer-size policy, resampler quality preset, engine-diagnostics toggle). You can add → start → stop → remove → rename → reconfigure routes entirely from the GUI, against the real Core Audio engine. XCUITest flows are explicitly deferred under the SPM-only constraint (see deviation below). Scenes — and the sidebar shell originally planned to host them — have been deferred to a future release; see Phase 7's deviation entry and `docs/spec.md § 4.10` for the design record. The bridge API picked up one additive symbol (`jbox_engine_enumerate_device_channels` / `jbox_channel_list_free`) in Slice A, then further additive symbols for meters, latency components, rename, the supported-buffer-frame-size range, and the engine-wide resampler-quality preset (ABI v8) — every change MINOR per [spec.md § 1.6](./spec.md#16-versioning-of-the-bridge-api).

A **logging pipeline slice** also landed alongside the UI first-slice (unplanned but necessary for user-visible diagnostics): `LogDrainer` now drains `DefaultRtLogQueue` into `os_log` under subsystem `com.jbox.app`, RT callbacks push edge-triggered events on underrun / overrun / channel-mismatch, control-side code paths log route lifecycle and errors directly. Swift uses `Logger` wrappers (`JboxLog`) sharing the same subsystem. The rotating file sink in `~/Library/Logs/Jbox/` described in [spec.md § 2.9](./spec.md#29-rt-safe-logging) is **not yet implemented** — deferred to Phase 8 alongside packaging. See Phase 8 tasks below.

**Goal.** Build the v1 SwiftUI UI against the stable bridge API. No engine changes should be required to make the UI work.

**Entry criteria.** Phase 5 complete. Bridge API stable (no breaking changes planned during Phase 6).

**Exit criteria.**
- [x] User can add / edit / delete routes through the main window route editor. Add/delete landed in the first slice; rename + mapping edit landed in post-Slice-B refinement #2 (inline rename via double-click name, pencil-button → `EditRouteSheet` for everything else; running routes are restarted automatically after a reconfig via `EngineStore.replaceRoute`).
- [x] User can start / stop routes; status (running / stopped / waiting / error) reflects correctly. Row-level Start/Stop buttons and `StatusGlyph` render all five states; live polling keeps them up to date.
- [x] Per-channel signal indication — **Slice A (signal-present dots)**: landed. See the Meters task block below for the landing commit hashes.
- [x] Per-channel bar meters with color thresholds — **Slice B (full meters)**: landed. Dots stay as the glanceable collapsed summary; a chevron per route toggles the expanded `MeterPanel` with `Canvas`-drawn vertical bars, dB gridlines, and decaying peak-hold ticks. See the Meters task block below.
- [x] Menu bar extra shows overall state and exposes toggles for each route. `MenuBarExtra` scene (window style) with a dynamic icon driven by `EngineStore.overallState` (idle → outline route glyph, running → filled, attention → red triangle). Popover shows a per-route row with status glyph + Start/Stop, bulk Start All / Stop All, and Open JBox / Preferences / Quit actions. Engine ownership moved up from `AppRootView` to a new `AppState` held by `JboxApp`, so the window and the menu bar share one store.
- [x] Preferences window exposes buffer-size policy, resampler quality, and appearance. `PreferencesView` is a three-tab `TabView` (General / Audio / Advanced). General carries an appearance picker wired to every scene's `.preferredColorScheme()`; Audio carries a buffer-size policy picker (seeds `AddRouteSheet`'s Performance-mode default for new routes) and a resampler quality picker pushed through to the engine via ABI v8 `jbox_engine_set_resampler_quality`; Advanced keeps the diagnostics toggle and gains an Open Logs Folder button. Launch-at-login and meters-in-menu-bar are disabled placeholders (Phase 7); Export / Import / Reset Configuration are disabled placeholders that wait on `state.json` (Phase 7). See the Preferences task block below for the landing detail.
- [x] UI works without Xcode IDE (builds via `swift build`); SwiftUI previews work when opened in Xcode. The entire first slice was built and launched from the command line via `swift run JboxApp`.

**Tasks.**

Swift wrapper over bridge:
- [x] `JboxEngineSwift/JboxEngine.swift` — ergonomic Swift types (`Device`, `Route`, `RouteStatus`, `ChannelEdge`, `RouteState`) wrapping the C structs. Promoted to Equatable/Hashable/Sendable in `phase6 #1`.
- [x] Device enumeration + route state as an `@Observable` (`EngineStore`, Phase 6 #1). Refresh is currently user-driven (toolbar button + on-launch); hot-plug auto-refresh still waits on the Phase 5 follow-up listener for `kAudioHardwarePropertyDevices`.
- [x] Route status polling via a SwiftUI-bound timer (`.task` on `RouteListView`, ~4 Hz, Phase 6 #4).

Main window:
- [x] `NavigationSplitView` layout: sidebar + detail list of routes with empty-state fallback. (Phase 7 later stripped the sidebar when Scenes was deferred — see the Phase 7 deviation entry below and `docs/spec.md § 4.1` for the current shape.)
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
- [ ] Scene editor sheet. **Deferred to a future release** — see Phase 7 deviation below and `docs/spec.md § 4.10`.
- [x] Preferences window (`Settings` scene) with three tabs. **Landed.** `PreferencesView` is a `TabView` with one subview per tab (`GeneralPreferencesView`, `AudioPreferencesView`, `AdvancedPreferencesView`). Keys live in a central `JboxPreferences` enum (`appearanceKey`, `bufferSizePolicyKey`, `resamplerQualityKey`, `showDiagnosticsKey`) persisted in `NSUserDefaults`; Phase 7 can migrate them onto `AppState.preferences` without touching the view code. Typed value types (`AppearanceMode`, `BufferSizePolicy`) live in `JboxEngineSwift/Preferences.swift` so they're unit-testable without importing SwiftUI. The engine side picked up an additive ABI bump: ABI v7 → v8 with `jbox_engine_set_resampler_quality` + `jbox_engine_resampler_quality`, backed by a `jbox::rt::ResamplerQuality` enum + a new parameter on `AudioConverterWrapper`'s constructor (`Mastering` = today's `_Complexity_Mastering` + `Quality_Max`, `HighQuality` = `_Complexity_Normal` + `Quality_High`). The engine applies the preset at `attemptStart` when constructing each route's converter; changing the preference affects newly-started routes only — the footer copy tells users this explicitly. Tests: 6 Catch2 cases under `[resampler_quality]` (bridge getter/setter round-trip, NULL guards, unknown-value clamp to Mastering, both presets build and pass unity + 44.1→48 SRC cleanly); 18 Swift Testing cases under `PreferencesTests` covering typed-enum round-trips + defaults + lossy `.explicitOverride(0)` round-trip + associated-value-distinct equality + the live-engine bridge round-trip. UI side is not unit-tested (SwiftUI `@AppStorage` + SwiftUI views) but the engine wiring is.

Menu bar extra:
- [x] `MenuBarExtra` scene with dynamic icon. The icon is built as a precomposed `NSImage` by `MenuBarIconRenderer` — `MenuBarExtra`'s label only renders simple leaf views (a SwiftUI `Canvas` just doesn't appear), so the compositing happens in AppKit instead. The base glyph is three horizontal tracks between two columns of dots (an echo of the app icon) drawn with `NSBezierPath` in `NSColor.labelColor`; that dynamic color resolves to the menu bar's text color at draw time, so light/dark adaptation works without marking the image template (which would tint the colored status dot monochrome). A small status dot composites on top in the bottom-right corner — absent when idle, green when running, red when attention, with a `windowBackgroundColor` halo for contrast. Accessibility label reads "JBox, N routes running" / "all routes stopped" / "attention needed" so VoiceOver users get the same summary.
- [x] Popover content: per-route toggles, Start All / Stop All, menu items. `MenuBarContent` renders a header ("N routes running"), a row per route with `StatusGlyph` + Start/Stop button, Start All / Stop All buttons wired to `EngineStore.startAll()` / `stopAll()`, and Open JBox / Preferences… / Quit actions. A 2 Hz `.task` on the menu bar keeps the icon live while the main window is closed. The view style is `.window` so the whole popover behaves like a native-feeling palette.
- [x] **Open JBox raises the existing window** instead of spawning a new one. `MenuBarContent.openOrRaiseMainWindow()` walks `NSApp.windows`, filters out panels (the menu bar popover) and non-matching titles (Settings), and calls `makeKeyAndOrderFront` + `deminiaturize` on the surviving match. Falls through to `openWindow(id:)` only when no window is present.
- [x] **Prevent multiple main-window instances.** The main scene declaration in `JboxApp.swift` swapped from `WindowGroup("JBox", id: …)` to SwiftUI's single-instance `Window("JBox", id: …)` (macOS 13+). The framework now enforces uniqueness directly: `Cmd+N` no longer surfaces in the File menu (SwiftUI suppresses the auto-generated "New Window" item for `Window` scenes), and any programmatic `openWindow(id: "main")` raises the existing instance instead of spawning a duplicate. The AppKit-side `MenuBarContent.openOrRaiseMainWindow()` walk over `NSApp.windows` survives unchanged — it now serves as a deminiaturize-and-front-raise convenience rather than a workaround for SwiftUI's `WindowGroup` behaviour.

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
  - [x] `RouteRow` gains a chevron to toggle expansion; collapsed rows keep today's `SignalDotRow`, expanded rows swap it for the `MeterPanel`. Expansion state lives in `@State private var expandedRoutes: Set<UInt32>` on `RouteListView`.
  - [x] Color thresholds match [spec.md § 4.5](./spec.md#45-meters): gray below -60 dBFS, green below -6, yellow to -3, red above. Bar height is a second, color-independent cue for accessibility.
  - [x] VoiceOver-friendly labels on the expanded panel. `MeterPanel` now carries a `.accessibilityElement(children: .combine)` + `.accessibilityLabel(...)` pair fed by `MeterAccessibilityLabel.summary(source:destination:)` in `JboxEngineSwift` — the helper lives next to `MeterLevel` so the label-formatting logic is unit-testable without pulling in SwiftUI. Linear peak samples convert to dBFS; values at or below `MeterLevel.floorDb` are reported as `silent` so listeners get a clean token instead of `minus infinity`. See `MeterAccessibilityLabelTests` (7 cases pinning empty arrays, all-silent, sub-floor, audible peaks, over-full-scale, 1-based channel indices, side independence).

**Post-Slice-B refinements.** Five queued items captured from the post-Slice-B usage pass. Each is small enough to land in its own commit; none of them invalidate Slice A or Slice B.

1. **Fan-out mapping (1:N).** **Landed.**
   - [x] Engine: `ChannelMapper::validate()` no longer rejects duplicate `src`. The `kDuplicateSource` enum entry was removed outright (internal C++ only; `JBOX_ERR_MAPPING_INVALID` remains the public error code). Duplicate `dst` is still rejected — fan-in / summing stays deferred per [spec.md Appendix A](./spec.md#appendix-a--deferred--out-of-scope). No hot-path change: the scratch-copy / converter / metering loops already iterate per output slot, so a fan-out edge is "just another output channel" whose scratch cell happens to hold the same source sample.
   - [x] Tests: the existing `[channel_mapper]` cases flipped — "duplicate source is rejected" → "duplicate source is allowed (fan-out)"; the mixed-duplicates case now reports `kDuplicateDestination` since src-dup no longer errors; new fan-out shape case (1:3). `[route_manager][fan_out][integration]` asserts end-to-end that one src channel mapped to two dst channels produces the same sample on both. `EngineStoreTests.addRouteDuplicateDst` replaces the old duplicate-src case.
   - [x] UI: `AddRouteSheet`'s client-side validator drops the `seenSrc` check; keeps the `seenDst` rejection with the copy "destination channel … is already in use." Doc comment at the top of the file updated.
   - [x] Spec already updated: § 1.1, § 2.5 (converter output slot semantics), § 3.1 mapping invariants, § 4.3 editor validation, Appendix A.

2. **Edit existing routes.** **Landed.**
   - [x] Engine: `jbox_engine_rename_route(engine, id, new_name)` — additive symbol, ABI v6 → v7 (MINOR). Non-disruptive in every state; the call just mutates `RouteRecord::name`, which the RT path never touches. `RouteManager::renameRoute` + a test-only `routeName(id)` C++ getter for round-trip observation. Mapping / device / latency-mode / buffer-frames edits go through the Swift layer — the engine's `RouteRecord::mapping` is immutable after `addRoute`, so `EngineStore.replaceRoute(id:with:)` orchestrates stop → `addRoute` (new id) → `removeRoute` (old) → `startRoute` (if the old route was running), and best-effort restarts the old route if the mid-sequence `addRoute` fails.
   - [x] UI: `EditRouteSheet` (sibling to `AddRouteSheet`). The mapping editor was extracted into `ChannelMappingEditor` + `ChannelMappingValidator` so both sheets share it. Row affordances: double-click the route name for inline rename (Return commits via the non-disruptive engine API, Escape reverts); a pencil button in the action cluster opens the edit sheet. The sheet's apply button reads **Apply and restart** when the edit requires a reconfig on an active route, otherwise **Apply**. `⌘E` / `⌘R` shortcuts are deferred — they need a selection model the route list does not yet have; adding them later is additive.
   - [x] Tests: `[route_manager][rename]` (3 cases) pin round-trip on STOPPED / RUNNING / WAITING plus unknown-id rejection. `[bridge_api][rename]` pins the C-bridge forwarding, NULL clear, and the NULL-engine / unknown-id error paths. `EngineStoreTests` gains 4 cases covering rename id preservation + name-clear, unknown-id lastError surfacing, the rename fast path in `replaceRoute`, and the new-id issuance on mapping edits.

3. **Computed per-route latency.** **Landed.**
   - [x] Backend: `BackendDeviceInfo` gained `{input,output}_device_latency_frames` and `{input,output}_safety_offset_frames`. `CoreAudioBackend::enumerate()` populates each scope from `kAudioDevicePropertyLatency` / `kAudioDevicePropertySafetyOffset`; `SimulatedBackend` pass-throughs the values the test harness sets on the struct. (`kAudioStreamPropertyLatency` folding is deferred — we take the device-level number for now; most drivers report one or the other, and the pill is indicative per spec.md § 2.12.)
   - [x] Engine: pure helper `jbox::control::estimateLatencyMicroseconds` (new `control/latency_estimate.{hpp,cpp}`) computes the sum per [spec.md § 2.12](./spec.md#212-estimated-per-route-latency), split at the SRC/DST-rate boundary so routes that bridge different device rates compose correctly. `RouteManager::attemptStart` fills the components once (using `AudioConverterWrapper::primeLeadingFrames` for the SRC prime count and `RingBuffer::usableCapacityFrames() / 2` for the drift-sampler setpoint) and caches the total; `pollStatus` returns it through the new `jbox_route_status_t::estimated_latency_us` field. ABI bump 1 → 2 (MINOR; field is appended).
   - [x] Swift: `RouteStatus.estimatedLatencyUs`. New `LatencyFormatter.pillText(microseconds:)` renders buckets (`<1 ms`, `~N.N ms`, `~N ms`, `~N.N s`) and returns `nil` for 0 so the UI can hide the pill.
   - [x] UI: `RouteRow` shows a faint `LatencyPill` next to the counters whenever the engine reports a non-zero estimate.
   - [x] Expanded-panel component breakdown landed alongside #4 — the gridded latency breakdown (src HAL / safety / buffer / ring / SRC prime / dst buffer / safety / HAL, plus total) ships inside `DiagnosticsBlock` at the bottom of `MeterPanel`, visible only when the engine-diagnostics toggle is on.
   - [x] Tests: eight Catch2 cases pinning the estimator (same-rate sum, split rates, zero-rate guard, negative-rate guard, ring-dominates, monotonicity). An end-to-end `[route_manager][latency]` case drives `SimulatedBackend` with known HAL values + asserts the pill is > the HAL-lower-bound, larger on a bigger-buffer route, and zeroed after stop. Six Swift Testing cases cover `LatencyFormatter` bucket transitions. `pollStatus` fill-through is covered by the existing `estimated_latency_us == 0` assertion on STOPPED routes.

4. **Advanced / engine-diagnostics toggle.** **Landed.**
   - [x] Engine: ABI v3 → v4 (MINOR). New `jbox_route_latency_components_t` + `jbox_engine_poll_route_latency_components()` surface the per-component breakdown the engine already caches on `RouteRecord`. `RouteManager::pollLatencyComponents` returns zeros on stopped/unknown routes.
   - [x] Swift: `LatencyComponents` public struct + `Engine.pollLatencyComponents(id:)`. `EngineStore.latencyComponents[routeId]` cache refreshed on every `pollStatuses()` pass. `LatencyFormatter.breakdownLabel(frames:rate:)` renders per-row text.
   - [x] UI: new `Settings` scene with an Advanced tab, `@AppStorage("com.jbox.showDiagnostics")` bool (default off; persisted in NSUserDefaults until Phase 7 rolls a proper `AppState.preferences`). `RouteRow` no longer shows the `frames / consumed · u<K>` counter line unconditionally — those now live in a `DiagnosticsBlock` at the bottom of `MeterPanel`, visible only when the toggle is on. The block has two columns: engine counters (produced / consumed / underruns / overruns) and a gridded latency breakdown (src HAL / safety / buffer / ring / SRC prime / dst buffer / safety / HAL, plus total).
   - [x] Tests: four new Swift Testing cases for `breakdownLabel` (zero/sub-ms/mid-ms/integer-ms bands); `[route_manager][latency][components]` Catch2 case asserts zeros on stopped, known HAL fields survive the round-trip on running, `total_us` matches the status's `estimated_latency_us`, zeroes again on stop.
   - [x] Spec: § 4.6 Advanced tab was already aligned; § 2.12 now cross-references the new API entry point.

5. **VoiceOver label on the expanded meter panel** (carried forward from Slice B). **Landed.**
   - [x] Composite accessibility label on `MeterPanel` summarising "per-channel peak dBFS on source; per-channel peak dBFS on dest", so VoiceOver users get the same information the colour + height cues give sighted users. Implemented as a pure helper `MeterAccessibilityLabel.summary(source:destination:)` in `JboxEngineSwift` (mirroring `MeterLevel`'s "live in the engine module so SwiftUI-free tests can reach it" convention) wired into `MeterPanel.body` via `.accessibilityElement(children: .combine)`. Sub-floor and zero peaks collapse to the token `silent`; audible peaks render as rounded `-N dBFS`. Tested by `MeterAccessibilityLabelTests` (7 cases).

6. **Tiered latency mode.** Started as a simple "Low latency" toggle (ABI v3); expanded in a follow-up to a three-tier preset (ABI v5) after the Low tier's pill was still too large for real-time drum monitoring on the user's Roland V31 → UA Apollo rig. See [spec.md § 2.3](./spec.md#23-route-lifecycle) and [§ 2.7](./spec.md#27-per-device-coordination-when-routes-share-a-device).
   - [x] Engine: ABI field `low_latency` → `latency_mode` (v5). `RouteManager` picks between `kRingSafe` (8× / 4096 floor, setpoint ring/2), `kRingLowLatency` (3× / 512 floor, ring/2), and `kRingPerformance` (2× / 256 floor, **setpoint ring/4**). The new `target_fill_fraction` on the preset struct is read at `attemptStart`, cached into `LatencyComponents::ring_target_fill_frames`, and consumed by `drift_sampler.cpp`'s `ringTargetFill` — one source of truth per route. Existing V31-rationale comment preserved; expanded to explain the drum-monitoring motivation for Performance.
   - [x] Swift: `RouteConfig.lowLatency: Bool` → `RouteConfig.latencyMode: LatencyMode` (`.off` / `.low` / `.performance`), enum backed by `UInt32` mapping directly into the C field.
   - [x] UI: `AddRouteSheet`'s toggle became a three-way Picker; the footer copy switches per tier to describe the trade-off, with an explicit "underruns are expected" line for Performance.
   - [x] Tests: `[route_manager][latency_mode]` Catch2 case asserts strictly monotonic pills across the three tiers (Off > Low > Performance), with quantitative bounds on the gaps (≥ 30 000 µs Off→Low, ≥ 3 000 µs Low→Performance), and that the Performance breakdown's `ring_target_fill_frames` is under 40 % of the ring (confirming ring/4 setpoint, not ring/2). `EngineStoreTests.addRouteLatencyMode` round-trips the enum through the Swift bridge.
   - [ ] Follow-up: device-class-driven auto-sizing (USB vs PCIe vs built-in) with a soak-and-tighten scheme. Tracked in Phase 7 or later.
   - ⛔ *The follow-ups and check-marks below describe the hog-mode + per-route buffer-shrink mechanism that was ripped out by Phase 7.6 and replaced by the no-hog v11 `setBufferFrameSize` path. Kept here as historical context for the test names and the reasoning that motivated each landing — not as the current contract. The current contract lives in `CLAUDE.md` "Device & HAL ownership policy", `docs/spec.md § 2.7` "Per-route HAL buffer-frame-size preference", and the Phase 7.6 deviations below. Do **not** treat any of the bullets in this block as still-active code paths.*
   - ~~[ ] Follow-up: request 32-frame HAL buffers in Performance mode (vs the current 64).~~ *Obsolete.* Premise (mux's binary `low_latency` flag) no longer exists post-7.6. The user controls per-route buffer via `RouteConfig.bufferFrames` (v11); finer-grained engine-driven shrink is not re-opening.
   - [x] **Core Audio hog-mode exclusive ownership for the duplex fast path.** A device held open by another app keeps its buffer frame size at the larger of every client's preference (Core Audio's max-across-clients policy) — so `requestBufferFrameSize(64)` against a device another process is holding at a larger size is silently clamped. To match how DAWs force small buffers, `IDeviceBackend` gained `claimExclusive` / `releaseExclusive` (Core Audio `kAudioDevicePropertyHogMode` on the real backend). The fast path attempts to claim exclusive ownership before the buffer-shrink request; on stop it restores the buffer then releases hog mode in that order. Claim failure is non-fatal (we fall back to the shared-client path). Tested via `[route_manager][duplex][exclusive]` which asserts `isExclusive(uid)` flips true on start, false on stop, and the buffer is restored either way.
   - [x] **Drift-sampler setpoint refreshed from post-shrink buffers.** Cross-device Performance-mode pill was honest at ~11 ms but still above the target — because the drift-sampler setpoint (`ring_target_fill_frames`) was derived from the ring's `usableCapacityFrames`, and the ring was sized pre-shrink. The setpoint is what determines steady-state ring residency, so a pre-shrink-sized setpoint inflated both the pill and the real latency. Fixed by recomputing the setpoint in the post-attach refresh block using the post-shrink buffer sizes + the selected tier's `multiplier` and `target_fill_fraction`; ring capacity stays pre-shrink-sized (harmless burst-overflow headroom). Cross-device Performance drops from ~11 ms pill to ~6–7 ms. Trade-off surfaced in the docs: drain headroom == setpoint, so Performance's 1.3 ms setpoint clicks on source stalls > 1.3 ms; users who click fall back to Low (5.3 ms setpoint headroom). Tests: `[route_manager][latency_mode][mux][ring]` asserts the setpoint is ≤ 128 frames after the post-shrink refresh on a route that started with 512-frame devices, and that the pill falls under 8 000 µs.
   - [x] **Cross-device (mux) path: per-route buffer target + hog mode.** The non-duplex path (when source and destination devices differ) goes through `DeviceIOMux` and was previously using a hard-coded 64-frame target with no exclusive claim. That meant the user's per-route `bufferFrames` override was silently ignored on cross-device routes, and on a device another app was holding at a larger size Core Audio's max-across-clients policy clamped our request back up. Fixed: `DeviceIOMux::attachInput/attachOutput` now take `uint32_t requested_buffer_frames` instead of a bool; the mux tracks the min across non-zero entries, claims `IDeviceBackend::claimExclusive` on the 0→1 transition, and releases on the 1→0. The claim failure path is non-fatal (route keeps working on the shared-client path). When a new route attaches with a smaller request than the current min, the mux re-issues the shrink to the new value. Tests flipped: `[device_io_mux][low_latency]` now covers `isExclusive(uid)` lifecycle and the smallest-wins behavior; the no-opinion mixed-in scenario coexists cleanly.
   - [x] **AudioBufferList layout fix for aggregate devices.** On real hardware an aggregate device presents the IOProc with a *multi-buffer* list where each buffer carries one member's channel group in its own interleaved format (e.g. `mBuffers[0]` = 4 channels, `mBuffers[1]` = 2 channels for a 6-channel aggregate). The pre-Phase-6 duplex trampoline assumed either a single fully-interleaved buffer or one-channel-per-buffer planar; the aggregate layout matched neither and was read as "one sample per buffer stride", producing silent output on the fast path even though the pill showed ~4 ms. Helpers `readInputInterleaved` / `writeOutputFromInterleaved` extracted to `control/audio_buffer_interleave.{hpp,cpp}` and rewritten to walk each buffer's `mNumberChannels` independently. Unit-tested directly in `[audio_buffer_interleave][aggregate]` by building synthetic `AudioBufferList`s in the test — the simulated backend's `deliverBuffer` only exercises a flat interleaved layout, so without the direct helper tests the aggregate case would have stayed uncovered.
   - [x] **User-chosen buffer frame size per route.** Replaces the hard-coded 64-frame Performance target with a per-route override. `jbox_route_config_t` gained `buffer_frames` (ABI v5 → v6, MINOR, appended). New `jbox_engine_supported_buffer_frame_size_range(uid, min, max)` surfaces the HAL-reported range (intersected across aggregate sub-devices) so the UI can populate a picker restricted to values the device actually accepts. `AddRouteSheet` shows a buffer-size picker when Performance is selected; options are filtered against the source + destination devices' ranges. `RouteConfig.bufferFrames: UInt32?` (nil = use tier default). Tests: a Catch2 case pinning the override lands on the aggregate; a backend test pinning that aggregate range-intersection works; the existing duplex suite untouched (default path unchanged).
   - [x] **Aggregate-device fan-out for hog-mode and buffer-size changes.** Hogging an aggregate device alone doesn't evict clients from its member devices — the member devices are where the actual HAL buffer size lives, so the shrink is ignored when a member is being held larger elsewhere. Both `claimExclusive` and `requestBufferFrameSize` now walk `kAudioAggregateDevicePropertyActiveSubDeviceList` and apply to every active member before the aggregate itself; `claimExclusive` snapshots each member's pre-claim buffer frame size into per-UID state so `releaseExclusive` can restore each member to its own original (not the aggregate's). `SimulatedBackend` gained `addAggregateDevice(info, sub_uids)` so the same contract is tested in CI. The `LatencyComponents::src_buffer_frames` reflects the post-change aggregate value so users see what the HAL actually honored. Tested via `[route_manager][duplex][aggregate]` (a 3-device scenario where sub-input starts at 256, sub-output at 512, aggregate at 512 — after start all three are at 64, after stop each is back to its own original).
   - [x] **Direct-monitor fast path for same-device Performance routes.** When `source_uid == dest_uid` and `latency_mode == 2`, `attemptStart` skips the ring, converter, scratch buffers, and mux entirely and opens a single duplex IOProc via `IDeviceBackend::openDuplexCallback`. `duplexIOProcCallback` copies input → output channels inline (O(channels × frames)), updating both source and dest peak meters in one pass. Fan-out is supported (duplicate src edges replicate per output slot, like the non-fast-path). Exclusive: refuses to attach if any other IOProc already targets the UID, and refuses to start if a mux exists for the UID. Rationale: the user's drum-monitoring scenario (Roland V31 → UA Apollo aggregate) sat at ~30 ms on the cross-device path; the ring target fill and two separate IOProcs were what kept it there. Fast path collapses latency to HAL + one buffer period. See [spec.md § 2.7 "Direct-monitor fast path"](./spec.md#27-per-device-coordination-when-routes-share-a-device). Tests: `[route_manager][duplex][integration]` asserts end-to-end routing through a SimulatedBackend duplex device, correctness of fan-out over the fast path, and that the pill drops ring + converter + dst-buffer contributions to zero. Follow-up (landed in the same phase item): the duplex branch issues its own buffer-frame-size request (the mux-side refcount doesn't apply because the fast path bypasses the mux) — tests `[route_manager][duplex][buffer]` pin the shrink-on-start + restore-on-stop behavior and the "already small" no-op case; the pill's `src_buffer_frames` reflects the post-change HAL-honored value.
   - [x] Device buffer-size control. `IDeviceBackend` gained `currentBufferFrameSize` + `requestBufferFrameSize`; `DeviceIOMux` refcounts low-latency attachments across both directions, snapshots the original buffer size on the 0→1 edge, asks the backend to shrink to a 64-frame target on first attach, and restores on last detach. `CoreAudioBackend::requestBufferFrameSize` clamps requests into `kAudioDevicePropertyBufferFrameSizeRange`. Tests: three `[device_io_mux][low_latency]` Catch2 cases covering single-route shrink-and-restore, two-LL-routes share-one-shrink, and input+output on the same device sharing a single refcount. (Performance mode continues to use this same 64-frame request via the binary mux flag — see the follow-up above.)

UI tests (minimal):
- [x] Swift Testing cases for `EngineStore` against the live Core Audio engine (`Tests/JboxEngineTests/EngineStoreTests.swift`, Phase 6 #1).
- [x] SwiftUI preview providers for route row, route editor, and the main route-list view. `RouteListView.swift`, `MeterBar.swift`, `AddRouteSheet.swift`, and `EditRouteSheet.swift` ship `#Preview` blocks (16 in total) covering the populated route list, the empty-list `ContentUnavailableView`, the engine-error alert, every `RouteRow` state (running collapsed, running expanded, stopped, waiting, starting, error), `SignalDotRow` with and without peaks, `MeterPanel` at 2 and 8 channels, the `ChannelBar` colour zones, and the Add / Edit sheets pre-populated with a stub device list. Plumbing: a debug-only `EngineStore.preview(...)` factory inside `EngineStore.swift` (gated by `#if DEBUG`; only the same-file factory can write the `private(set)` observable arrays) snaps stub state onto a freshly-constructed real `Engine` (no routes started, so the engine sits idle). Stub data lives in a new `PreviewFixtures` enum in `JboxEngineSwift` so the C-bridge `JBOX_OK` / `JBOX_ERR_*` constants are reachable without forcing `JboxApp` to depend on `JboxEngineC`.
- [ ] **XCUITest event-injection flows — deferred to a post-Phase-6 follow-up. The placeholder file `Tests/JboxAppTests/PlaceholderTests.swift` stays as the slot the future cases land in.** Not abandoned; the gap is documented here so a future session can pick it up cleanly without re-investigating the constraint.

  *Goal when revisited.* Drive the running `Jbox.app` through `XCUIApplication`-style event injection for at least three flows: (a) launch → empty-list → tap "+" → fill `AddRouteSheet` → submit → row appears in the route list; (b) tap **Start** on an existing stopped route → `StatusGlyph` becomes the running variant within ~1 s and `LatencyPill` populates; (c) open the menu-bar extra → tap **Open JBox** → main window front-raises (covers the `Window` single-instance behaviour we just landed). A scene-switch flow can be added when Scenes lands as a deferred future feature (see "After v1.0.0" + `docs/spec.md § 4.10`).

  *Why it's hard under the current constraint.* The Apple-blessed XCUITest path is `xcodebuild test -destination ... -testPlan ...` against an `.xcodeproj` plus an `.xctestplan`. Both directly violate the **SPM-only / no Xcode IDE** rule in `CLAUDE.md` "Tooling constraints". `swift test` is a CLI test runner; it does not natively launch `.app` bundles or wire the XCUITest private framework into a regular SPM `.testTarget`. The lower-level `xctest` runner can in principle execute UI test bundles by hand (loading the bundle into a host process that constructs `XCUIApplication(url:)` against `build/Jbox.app`), but the configuration / signing / host-app linking is undocumented territory under SPM and brittle in practice — exactly the "fragile path nobody can maintain" shape the SPM-only rule was put in place to avoid.

  *Recommended path when revisited (in order of decreasing cleanliness).*
  1. **Allow a single generated `.xcodeproj` artefact for the UI test target only.** This is a policy decision, not a tooling decision. `Package.swift` stays the single authoritative manifest for `JboxApp`, `JboxEngineSwift`, etc.; the `.xcodeproj` is treated as a build artefact (regenerated on demand, gitignored, not the editable source of truth) used purely so `xcodebuild test` can drive an XCUITest target. CI runs `xcodebuild test -scheme JboxAppUITests`; humans never open the project in the IDE. This is the only path that follows the Apple-blessed grain; everything else swims upstream against the framework. Trigger to revisit: when "is the button click reaching the action" becomes the main remaining gap between unit tests and shipping confidence (currently it is not — see "What's already covering the surface" below).
  2. **Build `Jbox.app` via `bundle_app.sh`, then drive it from a hand-rolled SPM `.testTarget` that links `XCTest` + the private UI testing framework and constructs `XCUIApplication(url:)` against the bundle.** No `.xcodeproj`, but requires reverse-engineering the linker invocation `xcodebuild` performs for UI test targets. Expect macOS-version sensitivity. Use only if option 1 is rejected on policy grounds.
  3. **Vendor a third-party SwiftUI introspection library (e.g. `ViewInspector`)** and assert against the SwiftUI view tree at the Swift Testing layer using the `PreviewFixtures` stores. Catches "view broke" + "wrong identifier rendered" but cannot drive the AppKit event loop, so it doesn't actually answer "did the button click reach the action." Lower coverage than options 1–2; only worth doing if 1 and 2 are both blocked.

  *What's already covering the same surface today.* The XCUITest gap is narrower than it looks — much of what an XCUITest would assert is already pinned by other layers:
  - **Action semantics:** `Tests/JboxEngineTests/EngineStoreTests.swift` exercises every action a UI test would invoke (`addRoute` / `startRoute` / `stopRoute` / `replaceRoute` / `removeRoute` / `renameRoute`) against the live engine, including the error paths an alert would surface.
  - **Persistence round-trip:** `Tests/JboxEngineTests/PersistedStateTests.swift` and `StateStoreTests.swift` cover `state.json` round-trip + the launch-time restoration path.
  - **VoiceOver labels:** `Tests/JboxEngineTests/MeterAccessibilityLabelTests.swift` (5618eaf) pins the labels XCUITest would assert against, at the helper level.
  - **View rendering:** the `#Preview` blocks added in `dbcc5c9` are compile-time smoke tests for every key view (`RouteRow` × all states, `MeterPanel` × multiple channel counts, `AddRouteSheet`, `EditRouteSheet`, the populated / empty / engine-error `RouteListView`). A breakage that would crash a UI test usually breaks a preview first.
  - **Manual smoke pass:** `make run` builds + bundles + opens the app; the project owner runs through the golden flows during phase reviews. Phase 9's manual-hardware acceptance is where the un-mocked end-to-end path is signed off in practice.

  The remaining gap, narrowly, is *"did this specific button click flow through SwiftUI's event loop and reach the action handler"*. That gap exists, but the cost of closing it under the SPM-only constraint is high enough that it's the right call to wait for either (a) the project owner to approve option 1's policy carve-out for a generated `.xcodeproj`, or (b) the surface to grow large enough that manual smoke + unit coverage stops being credible.

Phase 6 first-slice summary of deviations:
- **Scene editor moved to Phase 7.** The original plan had scenes UI in Phase 6 and scene activation logic / persistence in Phase 7. A scene editor that cannot round-trip through disk is confusing to use and wastes test surface on a UI that would be rewritten once persistence lands. Consolidating into Phase 7 is cleaner.
- **UI verification gap.** The first-slice commits were built and smoke-launched from the command line (`swift run JboxApp` stays up). The actual rendering — main-window layout, sheet presentation, stepper clamping, toolbar buttons — has **not** been interactively verified. A human pass on the running app is needed; bugs surfaced there will be addressed in follow-up commits rather than retroactively edited into the first-slice commits.
- **Ring-buffer sizing was too tight for USB burst delivery.** With the mic-entitlement fix in place and live signal finally audible, the first extended V31 → Apollo test produced u1043 underruns in under a minute with clearly audible pops (`frames_produced` slightly ahead of `frames_consumed`, underruns climbing steadily, no drift runaway). Root cause: Phase 4's `max_buffer × 4` with a 256-frame floor (~5 ms) was tuned against the synchronous simulated backend and didn't account for USB class-compliant sources (V31, many others) delivering in bursts with multi-ms gaps. Fixed in `route_manager.cpp` by bumping to `max_buffer × 8` with a 4096-frame floor (~85 ms at 48 k) — still well below any human-perceptible routing latency. New C++ integration test (`route_manager_test.cpp`, tag `[ring_sizing]`) simulates a bursty USB source: 8 back-to-back source deliveries followed by 8 drain deliveries, asserts overrun + underrun counts stay at 0; fails loudly under the old 4×/256 sizing. The existing edge-triggered-overrun logging test's flood size was bumped from 1024 → 8192 frames so it still forces an overrun under the new 4096-floor ring. Spec § 2.3 updated with the new formula and rationale.
- **Silent-mic bug uncovered by Slice A dots.** Slice A's first real-hardware test revealed that a running mic route advanced `frames_produced` but every source-side peak stayed at 0. Root cause was not in the new meter code: `scripts/bundle_app.sh` had shipped since Phase 1 with Hardened Runtime (`--options runtime`) but no entitlements attached. Under Hardened Runtime, Core Audio silences input buffers when `com.apple.security.device.audio-input` is not claimed — IOProc callbacks still fire, `frames_produced` still advances, and nothing in the logs indicates the silencing. Before Slice A there was no user-visible signal indicator, so the bug had been dormant. Fix: emit a minimal `Jbox.entitlements` plist inline and pass `--entitlements` to `codesign`; add a post-sign grep to fail the script if the entitlement is not present. See the Phase 1 deviation note above, [docs/spec.md § 1.5](./spec.md#15-platform-and-entitlement-decisions), and § 5.2.
- **Logging pipeline landed unplanned.** The UI first slice surfaced that no `os_log` events were being emitted anywhere — only the `RtLogQueue` primitive (Phase 2) existed, with no producers and no drainer. An Option-B slice landed on top of Phase 6: `control::LogDrainer` owns the queue and a consumer thread, forwards events via a pluggable `Sink` (default `os_log`), `RouteManager` plumbs the queue pointer into each `RouteRecord`, RT callbacks push **edge-triggered** events on the first underrun / overrun / channel-mismatch after each (re)start (edge flags reset in `attemptStart`). Control-thread paths push `kLogRouteStarted` / `kLogRouteWaiting` / `kLogRouteStopped`. Bridge entry points (`jbox_engine_create`, `jbox_engine_destroy`, `jbox_engine_add_route`) call `os_log` directly for startup and accept/reject decisions. The Swift side adds `JboxLog` (`app` / `engine` / `ui` categories, subsystem `com.jbox.app`) and wires notices through `EngineStore` and `AppRootView`. Test hook: `jbox::internal::setLogSink` swaps the os_log sink for a capture sink; the test forward-decl defaults `spawn_log_drainer=false` so existing tests run unchanged. New test: `Tests/JboxEngineCxxTests/log_drainer_test.cpp` (5 cases, TSan-clean). The rotating-file sink piece of [spec.md § 2.9](./spec.md#29-rt-safe-logging) is **still pending** — scheduled for Phase 8 (see below).
- **XCUITest deferred under the SPM-only constraint.** The Phase 6 follow-up "a couple of XCUITest flows in `JboxAppTests`" was evaluated and intentionally deferred during the 2026-04 Phase 6 close-out pass. The Apple-blessed XCUITest path requires `xcodebuild test` against an `.xcodeproj` + `.xctestplan`; both violate the SPM-only / no Xcode IDE rule (`CLAUDE.md` "Tooling constraints"). The lower-level `xctest`-runner-against-a-built-app-bundle path is undocumented under SPM and brittle. Decision: keep `Tests/JboxAppTests/PlaceholderTests.swift` as the slot the future cases will land in, document the gap fully under "UI tests (minimal):" above (constraint, blocked path, recommended approach when revisited, what already covers the surface), and revisit when either the project policy on a generated `.xcodeproj` artefact relaxes or the surface grows past what `EngineStoreTests` + SwiftUI `#Preview` smoke + `make run` manual passes can credibly cover. **No** XCUITest infrastructure was landed in this phase; closing the deviation is the explicit decision *not* to land it under the current constraint.
- **setInputRate flush storm → audible clicks.** After the ring-sizing bump above, a real-hardware V31 → Apollo session still produced audible clicks on dynamic content and a slow-climbing underrun counter (u321 after ~1 minute). Root cause sits in the drift-correction path, not in ring sizing: `DriftSampler` recomputes `target_input_rate = nominal * (1 + ppm * 1e-6)` every 10 ms, and with the Phase 4 conservative gains (`kp=1e-6`, `ki=1e-8`) the per-tick PI output adjusts the rate by ~1e-7 ppm — infinitesimally small in practice, but always a distinct float, so the legacy naive `target != last_applied_rate` comparison in `route_manager.cpp` triggered `AudioConverterWrapper::setInputRate` on every RT callback. Each `setInputRate` call flushes Apple's polyphase filter state; a new characterization test (`audio_converter_wrapper_test.cpp`, tag `[hypothesis]`) drives the real Apple converter and quantifies the cost at ~16 extra input frames per call — ~1600 frames/s continuously drained from the ring at the 100 Hz tick rate, which both explains the audible discontinuities on transients and the slow ring drain that manifested as climbing underruns. Fix: new pure `shouldApplyRate(proposed, last_applied, nominal)` decision function in `Sources/JboxEngineC/control/rate_deadband.hpp` with a 1 ppm threshold (48 mHz at 48 k — roughly 10× below the audible rate-error threshold), wired into `route_manager.cpp`'s output IOProc. Tests: 9 unit cases in `rate_deadband_test.cpp` covering cold-start, sub/supra-threshold gating, a PI-noise tick sequence (0 applies expected), and a 5 ppm ramp; plus a companion end-to-end `[hypothesis]` case that mirrors the real apply-or-skip logic against a live Apple converter and asserts the deadband restores baseline input consumption. Side-effect documented in the same commit: `drift_integration_test.cpp` scenario 1 (`+50 ppm source`) had been passing at a 512-frame excursion band only because the flush storm itself was bounding ring fill (drift_tracker.cpp:14-16 calls this out); with the flush storm gone and Phase 4 gains unchanged, open-loop drift accumulation of ~744 frames at 310 s is now visible, so the band is relaxed to 1024 frames with a comment pointing at the real-hardware gain-tuning task still deferred per Phase 4 exit criteria. spec §§ 2.5–2.6 updated.

- **Drag-to-reorder route strips (2026-05-01).** *Goal:* let the user
  arrange route strips in the main window's order of choice and have
  that order persist across launches. *Choices:* SwiftUI's native
  `List.onMove` modifier on the `ForEach` in `RouteListView` over a
  custom `DropDelegate` (no payoff for v1; native gives the macOS
  reorder cursor, drop-line indicator, and `RouteRow` identity
  stability for free). Single new `@MainActor` mutation
  `EngineStore.moveRoute(from:to:)` mirrors `addRoute` / `removeRoute`
  shape — `before != after` id-list short-circuit on no-op moves
  (empty `IndexSet`, same-position drops, drop-immediately-past-self)
  avoids spurious `state.json` snapshots. Manual `Array.move`
  reimplementation because Swift's `MutableCollection.move(fromOffsets:toOffset:)`
  is SwiftUI-only and `EngineStoreSwift` imports only `Foundation` +
  `Observation` — index-adjustment math (decrement `destination` for
  every removal preceding it) matches SwiftUI's documented `onMove`
  semantics. Persistence is automatic: `[StoredRoute]` was already
  an ordered JSON array and `restoreRoutes` already rehydrates in
  array order. No engine, ABI, or schema changes (ABI stays at v14,
  `currentSchemaVersion` stays at 1). Eight
  `EngineStoreTests.moveRoute*` Swift Testing cases pin: single-row
  down/up, contiguous-multi-row IndexSet preserving relative order,
  non-contiguous multi-index `IndexSet([0, 3]) → 2` straddling the
  destination (defensive against future refactors of the manual
  index-adjustment math), two identity variants (`→ N`, `→ N+1`),
  empty IndexSet, single-row list, exactly-once `onRoutesChanged`
  firing per non-trivial move. Manual smoke covers the gesture (no
  automated UI-test infrastructure per CLAUDE.md). *Diff:* +171 LOC
  tests + 34 LOC engine-store + 3 LOC view + doc updates. Shipped as a single squashed commit on
  `feature/route-reorder` (squashed from per-task TDD slices during
  the post-review amend pass).

- **`@Observable` × `List.onMove` drag cancellation (2026-05-01,
  surfaced during the drag-to-reorder smoke pass).** *Symptom:* a
  drag started on a route strip would cancel mid-gesture — the
  drop indicator would briefly appear, then the row would snap
  back. *Root cause (subtler than first diagnosed):* Apple's
  `@Observable` macro is asymmetric on equal-value writes.
  *Direct* property-setter writes (`self.meters = next`) get a
  willSet short-circuit when the new value equals the old. But
  *subscript-through-collection* writes (`routes[i].status = …`,
  `latencyComponents[id] = …`) go through the `_modify` accessor,
  which fires the observation registrar's willSet unconditionally
  — value-equality is NOT compared on that path. With three idle
  routes the `pollStatuses` 4 Hz tick produced 12 unconditional
  fires/sec; `NSTableView` (the AppKit class behind SwiftUI
  `List` on macOS) treats every "data source did change" signal
  as a reason to invalidate the proposed drop, cancelling the
  in-flight gesture. *Fix:* diff-before-write guards at the two
  subscript-write sites in `EngineStore.swift` — `pollStatuses`
  (covers both `routes[i].status` and `latencyComponents[id]`)
  and the `refreshStatus` one-liner — gated on `Equatable`
  comparison of `RouteStatus` / `LatencyComponents`. The
  `pollMeters` direct-setter path is left without a guard —
  Observation already shorts equal-value writes there, and adding
  a redundant guard would be misleading dead code. *Regression
  coverage:* `EngineStoreTests.pollStatusesIsQuietOnNoChange`
  pins the `pollStatuses` subscript-write guard (verified to fail
  if either the `status != routes[i].status` or
  `components != latencyComponents[id]` arm is removed);
  `pollMetersIsQuietOnNoChange` pins the framework contract for
  direct-setter shorts (so that a future Apple change to
  Observation that drops the short-circuit fails the test loudly
  and we know to add a guard at that site).
  `refreshStatusIsQuietOnNoChange` independently pins the
  `refreshStatus` arm by driving an idempotent `stopRoute(id)` on
  an already-stopped route (the engine returns `JBOX_OK`, the
  store's success arm calls `refreshStatus`, and the guard must
  skip the equal-value subscript write) — verified to fail if the
  `status != routes[idx].status` clause is removed. The latent
  asymmetry generalises across `EngineStore` and is filed as
  `R3` in `docs/refactoring-backlog.md` so future contributors
  adding new periodic tick paths know to mirror the guard.
  *Watch-out for future refactors:* the `if … != …` guards on the
  subscript paths are NOT a perf micro-optimization — removing
  them silently regresses drag cancellation under any audio.
  Inline comments at both sites call this out and reference the
  asymmetry above. **Superseded the same day by the next deviation
  for *running* routes** — the full-`Equatable` guard turned out to
  hold only on idle routes, see below.

- **`@Observable` × subscript-write asymmetry, second order — counter
  ticks slip past full-equality diff (2026-05-01, surfaced in user
  smoke right after the previous fix).** *Symptom:* the drag-to-reorder
  gesture worked perfectly when *all* routes were stopped, but as soon
  as any route was running the drop indicator flickered during the
  drag and the row often snapped back on release. *Root cause:* the
  previous fix's diff-before-write predicate was full-`Equatable` on
  `RouteStatus`. `RouteStatus` carries four monotonic counter fields
  (`framesProduced` / `framesConsumed` / `underrunCount` /
  `overrunCount`) which tick on every `pollStatuses` pass for any
  route in `.running`. The full-equality compare therefore detected
  a difference *every* poll on running routes, the
  `routes[i].status = …` subscript-write fired, and the same
  `NSTableView` data-source-did-change cascade described above
  cancelled the in-flight drop. The previous diff guard was
  load-bearing only for *idle* routes; it did nothing for the case
  the user was actually dragging through. *Fix:* split `RouteStatus`
  into two channels at the `EngineStore` boundary. Stable fields
  (`state`, `lastError`, `estimatedLatencyUs`) drive the
  `routes[i].status` array-subscript write via a new exposed-`static`
  predicate `EngineStore.statusFieldsAreObservablyEqual(_:_:)`;
  monotonic counters publish into a new
  `routeCounters: [UInt32: RouteCounters]` dict via a *direct* setter
  (`self.routeCounters = next`), which `@Observable` short-circuits
  on equality and which — even when it fires — invalidates only
  observers of `routeCounters`, never the `routes` ForEach. The
  expanded `MeterPanel.DiagnosticsBlock` reads counters from the
  new dict; the values inside `routes[i].status` are now stale by
  design and callers that need live numbers must read the dict.
  `removeRoute` / `replaceRoute` prune the new dict alongside the
  existing `meters` / `latencyComponents` cleanup. *Regression
  coverage:* four pure-logic
  `EngineStoreTests.statusFieldsAreObservablyEqual…` cases pin the
  predicate (counter-only changes are no-ops; state, lastError, and
  estimatedLatencyUs changes each invalidate); two integration cases
  (`pollStatusesPublishesRouteCounters`,
  `removeRoutePrunesRouteCounters`) pin the new dict's lifecycle;
  and `pollStatusesIsQuietOnRoutesWhenRunningCountersTick` is a
  live-Core-Audio regression — it brings a real route to `.running`,
  verifies counters actually advance between polls, and asserts that
  `withObservationTracking` on `routes` does not fire across the next
  `pollStatuses` tick. The test skips via `Issue.record` on hosts
  that can't bring the route up or where the IOProc is silenced
  (no usable hardware / Hardened-Runtime sandbox without the
  `audio-input` entitlement); on a developer machine with audio it
  exercises the bug path end-to-end. The existing
  `pollStatusesIsQuietOnNoChange` / `refreshStatusIsQuietOnNoChange`
  / `pollMetersIsQuietOnNoChange` cases continue to pin the no-change
  paths and the framework-contract direct-setter short-circuit.
  *Watch-out for future refactors:* never put a high-frequency-ticking
  field into a value held by `routes[i].…` and gated on full
  `Equatable` — it will defeat any subscript-write guard. New volatile
  per-route data should follow `routeCounters`'s direct-setter dict
  pattern. `R3` in `docs/refactoring-backlog.md` carries the broader
  watch-item; this fix resolves its primary realisation.

---

## Phase 7 — Persistence + launch-at-login

**Status:** ✅ Both slices landed. Persistence: Codable layer + `StateStore` + `AppState` wiring + live route / preferences round-trip. Launch-at-login: `LaunchAtLoginService` protocol over `SMAppService.mainApp` (production wrapper) + a `FakeLaunchAtLoginService` test fixture; `LaunchAtLoginController` orchestrates the toggle, the one-time explanatory note (latched via `StoredPreferences.hasShownLaunchAtLoginNote`), the `requiresApproval` callout, and the live-status reconciliation; `AppState` builds the controller post-load and snapshots its persisted booleans through `onPersistableChange` → `state.json`. Scenes — and the sidebar shell that hosted them — were **deferred to a future release**; see the deviation note + the "After v1.0.0 — deferred work" entry. No scene scaffolding survives in v1; when the feature returns it lands as a single slice including a `v1 → v2` schema migration.

**Goal.** Make configured state durable across relaunches. Offer opt-in launch-at-login.

**Entry criteria.** Phase 6 complete.

**Exit criteria.**
- [x] Relaunching the app restores all configured routes.
- [~] Opt-in launch-at-login works: when toggled on, the app registers as a login item; after macOS login, JBox starts automatically and appears in the menu bar (routes remain stopped per the design). **Code complete** (controller + `SMAppService.mainApp` wrapper + persistence latch landed); the real log-out / log-back-in smoke is deferred to Phase 9 hardware session.
- [x] Schema migration infrastructure in place even though only v1 exists. `StoredAppState.currentSchemaVersion` + `StateStore.LoadError.schemaTooNew` refuse forward-schema files; a future v2 adds a ladder of decode-time migrators.
- [x] `state.json` is atomically written, debounced, backed up.

**Tasks.**

Persistence:
- [x] `Sources/JboxEngineSwift/Persistence/StoredAppState.swift` — `Codable` value types: `StoredAppState`, `StoredRoute`, `StoredPreferences`. `BufferSizePolicy` picks up a custom single-value Codable so the on-disk shape matches the `@AppStorage` `storedRaw` representation (0 = useDeviceSetting; N = override frames). Existing `ChannelEdge` / `DeviceReference` / `AppearanceMode` / `Engine.ResamplerQuality` / `LatencyMode` gain `Codable` conformance. Swift Testing cases cover round-trips, missing-key defaults, extra-key tolerance, BufferSizePolicy's tag-free encoding, schemaVersion decoding, fan-out mapping preservation, multi-route ordering, and `lastQuittedAt` round-trips. (Phase 7 originally also introduced `StoredScene` + `StoredSceneActivationMode` + `StoredAppState.scenes` because Scenes was scheduled for the same phase. When Scenes was deferred those types were removed entirely — see deviation entry below.)
- [x] `Sources/JboxEngineSwift/Persistence/StateStore.swift` — debounced atomic writer. `save()` serialises onto a private serial queue; the debounce window coalesces bursts into one write. Atomic sequence is `state.json.tmp` → rename existing `state.json` → `state.json.bak` → promote `.tmp` over `state.json`. `load()` falls back to `.bak` when `state.json` is missing (crash-between-rename resilience), ignores a stray `.tmp` scratch artefact, throws `fileCorrupt` on malformed JSON, and throws `schemaTooNew` when the file's `schemaVersion` outranks the app's support — whether the mismatch lives in `state.json` or the backup fallback. `flush()` is synchronous and safe from any actor. `defaultDirectory()` resolves `~/Library/Application Support/JBox` (overridable via `JBOX_STATE_DIR` for dev isolation). 15 Swift Testing cases pin the file-layout invariants using temp directories.
- [x] Load-on-launch + save-on-change wiring in `AppState` (Sources/JboxApp/JboxApp.swift). On first launch, migrates any `@AppStorage` keys that were the pre-Phase-7 source of truth into `StoredPreferences`; on subsequent launches, pushes the loaded preferences back into `UserDefaults` so the `@AppStorage` views observe them on first paint. Routes restore via `EngineStore.addRoute(_:persistId:createdAt:)` — the new overload threads the persisted UUID + createdAt through, so routes keep their durable identity across runs. Mutations trigger a debounced save via a new `EngineStore.onRoutesChanged` callback (route add / remove / rename / replace). Preference edits ride `UserDefaults.didChangeNotification`. Scene-phase `.background` transitions call `AppState.flush()` so a mutation parked in the debounce window is written before the app exits.

Launch-at-login:
- [x] Implement via `SMAppService.mainApp.register()` / `.unregister()` (macOS 13+).
- [x] Preferences toggle calls into this.
- [x] First-time enabling shows an explanatory note.

Testing:
- [x] Persistence round-trip tests (write, read, verify equality) — `PersistedStateTests.swift` (27 cases) + `StateStoreTests.swift` (15 cases).
- [x] Forward-compat test: decode a v1 JSON with missing post-v1 keys, confirm defaults are applied; decode a v1 JSON with extra keys, confirm they're ignored.
- [ ] Migration test for a real v2 schema (placeholder until a v2 field forces a bump).

Phase 7 launch-at-login slice summary of deviations:

- **Service / controller split, with a `LaunchAtLoginService` protocol seam.** `SMAppService.mainApp` is a system framework that mutates real LaunchServices state — exercising it from unit tests would toggle the runner's actual login items. Introduced an injectable `LaunchAtLoginService` protocol (status read + register/unregister) so a `FakeLaunchAtLoginService` in the test target drives every code path under `LaunchAtLoginController`. Production `SMAppServiceLaunchAtLogin` is a trivial 1:1 enum map + try/catch wrapping; intentionally untested in CI, manual smoke covers it. 33 controller cases + 5 fake-service cases pin the contract.
- **`LaunchAtLoginController` uses Swift's `@Observable` macro, not `ObservableObject + @Published`.** First cut used `ObservableObject` and `@Published` — looked correct in isolation, but the SwiftUI `Toggle` reaches the controller via `appState.launchAtLogin?.isEnabled`, where `appState` is `@Observable` (the macro) and the controller hangs off it as a stored property. `@Observable` only tracks property *assignments* on the parent; nested `@Published` flips inside an `ObservableObject` child are invisible unless the view declares `@ObservedObject` directly — which it doesn't, since the controller is reached through the parent. The Toggle would therefore not snap back when `setEnabled(true)` rejected the flip (e.g., `.requiresApproval` outcome). Fixed by switching the controller to `@Observable` + marking `service` and `onPersistableChange` `@ObservationIgnored`. Matches `EngineStore` and `AppState` exactly; one less observation framework in the repo.
- **`acknowledgeFirstTimeNote()` guard-logic bug caught in self-review (regression test added).** First cut had `guard pendingFirstTimeNote || !hasShownFirstTimeNote else { return }` — proceeds when EITHER pending OR latch-not-set. The dangerous case: a fresh user (`pending=false, latch=false`) calls acknowledge (e.g., a spurious binding fire on view rebuild) → guard succeeds → latch flips to true → user has silently consumed their first-time-note allowance without ever seeing the note. Original test suite missed it because every "no-op when not armed" case was set up with `hasShownFirstTimeNote: true`. Fixed to `guard pendingFirstTimeNote else { return }` and added regression case `acknowledgeNoOpDoesNotConsumeAllowance` covering `pending=false, latch=false`.
- **`hasShownLaunchAtLoginNote` is a one-way latch on `StoredPreferences`, not a session-scoped flag.** First draft considered keeping the "have we shown the note?" state purely in-memory on `LaunchAtLoginController`, since the alert is one-time-per-session anyway. That misses the *across-relaunch* requirement: a user who toggled on, dismissed the note, quit, relaunched, toggled off, and toggled back on would see the note a second time — which contradicts spec § 4.6's "first-time enabling shows an explanatory note" (singular). Added `StoredPreferences.hasShownLaunchAtLoginNote: Bool` (additive, default false, decodeIfPresent) + an `onPersistableChange` callback on the controller that `AppState` wires to `scheduleSave()`. Acknowledging the note flips the latch, persists, and the latch never resets. 4 persistence cases pin the field's defaults / round-trip / missing-key / extra-key tolerance.
- **`refresh()` reconciles, doesn't remediate — `lastError` survives a refresh.** `AppState.load()` calls `LaunchAtLoginController.refresh()` once after construction so an out-of-band toggle in System Settings between sessions is picked up. Tempting to also clear `lastError` on refresh ("the error must be stale by now"), but that would mask a real registration failure: if the user enabled, register threw, and the user then re-opens Preferences, the error message must still be visible — it explains why the toggle is in its current state. `lastError` clears only on the next *successful* operation. Pinned by `LaunchAtLoginControllerTests` F4.
- **`onPersistableChange` fires on persisted-field changes only.** `lastError` is in-memory; firing the persistence callback on a `lastError`-only mutation would queue a no-op save (`StoredPreferences.Equatable` would short-circuit it but only after a redundant snapshot). The callback gates on actual `isEnabled` / `hasShownFirstTimeNote` flips — pinned by `LaunchAtLoginControllerTests` I1, with a control case that provokes a `lastError`-only change and asserts the counter doesn't tick.
- **`AppState.snapshotPreferences()` preserves the launch-at-login fields when re-reading from `UserDefaults`.** Pre-existing preferences are mirrored through `@AppStorage` and re-snapshotted on `UserDefaults.didChangeNotification`. Adding launch-at-login to that pipeline would have meant either (a) bridging the controller's state through `UserDefaults` (extra plumbing for no benefit; the controller already owns the truth), or (b) overwriting `persisted.preferences.launchAtLogin` / `hasShownLaunchAtLoginNote` to whatever `UserDefaults` thinks (zero, since they're not stored there). Fixed by merging: `snapshotPreferences()` reads everything else from `UserDefaults` but copies the launch-at-login fields forward from the in-memory `persisted.preferences` snapshot. The controller's `onPersistableChange` is the only writer for those two fields.
- **spec.md § 7.6 line 419 ("no `SMAppService` registration") was about *privileged* helpers, not `mainApp.register()`.** That sentence was written in the context of installer / virtual-driver paths and bundled the entire `SMAppService` API with `notarization` + `privileged installer helper`. `SMAppService.mainApp.register()` is a different code path: it works ad-hoc-signed, requires no Developer ID, and is the documented post-macOS-13 replacement for `LSSharedFileList`. Disambiguated the spec line to call out `loginItem(_:)` / `agent(_:)` / `daemon(_:)` as the still-out-of-scope privileged registrations, and added a parenthetical pointing at § 4.6's mainApp use.
- **No SwiftUI XCUITest / ViewInspector coverage.** SPM-only constraint (per CLAUDE.md). The controller's published flags are the testable seam for the alert + requiresApproval callout; the SwiftUI bindings are conventional. Manual smoke on a real install (drag `Jbox.app` to `/Applications`, enable, log out, log back in, observe app launch) is the acceptance gate for the production path.
- **Refresh-on-Preferences-tab-appear, via `.task` on `GeneralPreferencesView`'s body.** Without it, the requiresApproval round-trip has a stale-state hole: user clicks "Open Login Items…", flips approval in System Settings, returns to JBox — without a refresh, the controller keeps reporting the pre-approval state until next app launch. `.task` fires once each time the view appears (Settings scene appears/disappears each time the user opens / closes Preferences), so the `LaunchAtLoginController.refresh()` call is naturally scoped.
- **`AppState.load()` ordering: assign `self.launchAtLogin` BEFORE wiring `onPersistableChange` and BEFORE calling `refresh()` + an explicit `snapshotLaunchAtLogin()`.** Two adjacent issues caught in self-review: (a) if the callback fired during `refresh()` (e.g., service.status drifts between init read and refresh read), the closure read `self.launchAtLogin` and observed `nil` — silently dropping the snapshot. Fixed by assigning before the refresh. (b) When `state.json`'s `launchAtLogin` mirror is stale at boot (user disabled the login item via System Settings while JBox was closed → controller's `isEnabled` reads false but persisted bool reads true), refresh() alone doesn't help because it only fires the callback when `isEnabled` *changes during the refresh*, not when the persisted mirror was already wrong on entry. Fixed by adding an explicit `snapshotLaunchAtLogin()` call after refresh; its `guard fresh != persisted.preferences else { return }` makes it a no-op when the values are already in sync.
- *Diff:* +6 source files (3 production: `LaunchAtLogin.swift`, `SMAppServiceLaunchAtLogin.swift`, persistence field; 3 test: `LaunchAtLoginServiceTests.swift`, `LaunchAtLoginControllerTests.swift`, `FakeLaunchAtLoginService.swift`) + JboxApp.swift wiring (controller construction + `snapshotLaunchAtLogin` + the General-tab UI: live toggle, alert, requiresApproval callout with System Settings deep link, error callout, `.task` refresh). 42 new Swift Testing cases this slice (5 fake-service + 34 controller + 3 persistence). `make verify` green: 288 Swift tests / 34 suites + 298 C++ Catch2 cases (regular + TSan).

Phase 7 persistence-slice summary of deviations:
- **`StoredRoute` carries `latencyMode` + `bufferFrames`.** spec § 3.1.3 predates Phase 6's tiered latency modes; without persisting these two fields, every relaunch would snap routes back to the `.off` tier + tier-default buffer size, erasing Performance-mode choices the user deliberately made. Both fields are optional on decode (defaults: `.off`, `nil`) so pre-Phase-7 files still load cleanly. The spec's § 3.1.3 snippet will catch up in the next spec pass.
- **`StoredPreferences.showDiagnostics` is a sixth field.** spec § 3.1.4 lists five preferences; the Advanced-tab diagnostics toggle landed mid-Phase-6 without an entry, and without persisting it the user's choice resets on relaunch. Added as an additive field; missing-key decode returns `false` as the default.
- **`BufferSizePolicy` Codable is single-value, not tagged.** Auto-synthesised Codable on an enum with associated values produces a tagged object (`{"useDeviceSetting": {}}` / `{"explicitOverride": {"frames": 256}}`). Custom implementation encodes a single `UInt32` matching the existing `storedRaw` representation — cleaner diffs, forward-compatible with the `@AppStorage` migration path that reads `Int` directly.
- **Preferences still ride `@AppStorage` views, synced into `StoredPreferences`.** The minimal-touch approach: SwiftUI views keep their `@AppStorage` bindings; `AppState` observes `UserDefaults.didChangeNotification`, snapshots into `StoredPreferences`, and writes `state.json`. Reverse direction (loaded preferences → UserDefaults) primes the bindings on launch. A future slice can rewrite the preferences views to bind directly to an `@Observable` preferences model — not worth the view-layer churn during the persistence slice.
- **No migration ladder yet.** `currentSchemaVersion` stays at `1`; every post-v1 field has been added as an additive optional so no migration is needed. A `v1 → v2` slot in `StateStore.load()` will be introduced the first time a field needs a schema bump.
- **First-run saves immediately.** When `state.json` is missing, `AppState.load()` flushes a fresh file as soon as it migrates `@AppStorage` values, so the user sees the file on disk without waiting for a mutation. Simplifies debugging (the file's existence confirms JBox has been launched at least once) and sidesteps a subtle race where the engine init could fail and leave no file behind.
- **Scenes (and the sidebar that hosted them) deferred to a future release.** Honest review of v1's monitoring topology surfaced that the user's actual workflow has stable routes that are rarely toggled — so the per-route Start/Stop in the route list and the bulk Start All / Stop All in the menu-bar popover already cover the day-to-day case without the extra concept. The Phase 6 sidebar shell wrapped a single non-actionable "All Routes" item awaiting Scenes; both go away together. Concretely: stripped `NavigationSplitView` from `Sources/JboxApp/RouteListView.swift` (now a single-pane list under the existing toolbar), removed the disabled "Scene · None yet" row from `Sources/JboxApp/MenuBarContent.swift` (along with its supporting Divider), updated `docs/spec.md § 1.1 / § 3.1 (renumbered) / § 3.2 / § 3.5 / § 4.1 / § 4.2 / § 4.4 / § 4.6 / § 4.7 / § 4.9 / Appendix A`, and added a dedicated `docs/spec.md § 4.10 — Future feature: Scenes (with sidebar)` carrying the full preserved design (data model, activation modes, sidebar layout, scene editor, menu-bar slot, key flows, persistence, tests). *Initial cut left `StoredScene` + `StoredSceneActivationMode` + `StoredAppState.scenes` in place as a "forward-compat reservation" so the feature could return without a schema bump.* The user pushed back — speculative scaffolding for an unbuilt feature reads as half-finished work to anyone reading the codebase later, even at ~65 LOC, and the migration ladder is the v1 → vN mechanism designed exactly for this. Second cut removed the type, the field, the `StoredSceneCodableTests` suite (3 cases), and the `"scenes": []` literals from the JSON-shape pin tests. v1 carries zero scene scaffolding. When the feature returns: bump `StoredAppState.currentSchemaVersion` from `1` to `2`, add a `migrate_v1_to_v2` to `StateStore.load()` that initialises `scenes: []`, ship the type + field + activation logic + UI as a single coherent slice. § 4.10 remains the design record. `make verify` clean on both cuts.

---

## Phase 7.5 — Device sharing (hog-mode opt-out)

**Status:** ⛔ **Superseded and reverted by Phase 7.6 simplification.** The Phase 7.5 implementation landed (commits `8f9cb20`, `af5d6eb`) and worked as designed, but the underlying hog-mode + buffer-shrink machinery it opted *out* of turned out to be the wrong tool for the v1 monitoring topology — it interacted poorly with macOS aggregate devices (silent IOProc-scheduler stalls; property-write cascades that destabilised co-resident clients). Phase 7.6 removed the entire hog/buffer-shrink path from the engine, which makes the Phase 7.5 opt-out moot — share is now the only mode. The narrative below is preserved as historical context for anyone reading the git log; the implementation is gone.

**Goal.** Give users an opt-out from JBox's default hog-mode policy so other apps (Music, Safari, Zoom, …) can keep using a device while JBox has a route on it. Default behaviour preserves today's flow — if the user never touches the new preference, nothing changes.

**Entry criteria.** Phase 6 complete (the engine already has `claimExclusive` / `releaseExclusive`, the duplex fast-path, and the mux buffer-size refcount). Phase 7 persistence slice complete (the new preference rides `StoredPreferences` without needing a schema bump).

**Exit criteria.**
- [x] A per-route checkbox "Share device with other apps" in the route editor does what it says: the route skips `claimExclusive` and tolerates whatever HAL buffer size the shared-client path ends up giving.
- [x] A global "Share devices with other apps by default" toggle in Preferences governs newly-created routes; existing routes keep their stored value.
- [x] Performance-tier routes flagged `share=true` are silently demoted to Low tier, and the UI surfaces the demotion so the user isn't silently punished.
- [x] When two routes share a source device with mismatched flags, the hog-mode claim is driven by the non-sharing route and released only when *that* route detaches — and the route row's lock glyph names which route is holding the device.
- [x] `make verify` green on the combined diff.

**Tasks.**

Engine / ABI:
- [x] `jbox_engine.h` ABI v8 → v9 (MINOR, additive): appended `uint8_t share_device` to `jbox_route_config_t`; appended `uint32_t status_flags` to `jbox_route_status_t`; defined `JBOX_ROUTE_STATUS_SHARE_DOWNGRADE`.
- [x] `RouteRecord` gained `share_device: bool` + a runtime `share_downgraded: bool` sibling set during `attemptStart`.
- [x] `route_manager.cpp`: gated the duplex fast-path `claimExclusive` on `!r.share_device`; threaded the flag into `DeviceIOMux::attachInput/attachOutput`; in `attemptStart`, when `latency_mode == kPerformance && share_device` demoted to Low and raised `share_downgraded`; `pollStatus` populates `status_flags`.
- [x] `DeviceIOMux::attachInput/attachOutput` grew a `share_device: bool` parameter; the mux maintains a `non_sharing_attached` counter *parallel* to the overall attach count. `claimExclusive` fires when both `non_sharing_attached > 0` and `currentMinBufferRequest > 0` (preserving the pre-Phase-7.5 invariant that a no-opinion Off-tier route never hogs the device). Buffer-size-min negotiation stays tied to the overall refcount, so sharing routes still participate in the buffer min.
- [x] `bridge_api.cpp::convertRouteConfig` copies the new field; `jbox_engine_poll_route_status` fills in `status_flags` from `r.share_downgraded`.

Swift wrapper + persistence:
- [x] `RouteConfig.shareDevices: Bool` (default `false`); `Engine.addRoute` threads it through as the new C field.
- [x] `RouteStatus.statusFlags: UInt32` + convenience `shareDowngraded: Bool` derived from the `JBOX_ROUTE_STATUS_SHARE_DOWNGRADE` bit.
- [x] `StoredRoute.shareDevices: Bool?` — `nil` means "inherit the global default"; missing key on decode yields `nil` so pre-Phase-7.5 state files still load.
- [x] `StoredPreferences.shareDevicesByDefault: Bool` (default `false`); additive Codable change, no migration.
- [x] `AppState` resolves `effective = route.shareDevices ?? preferences.shareDevicesByDefault` at `restoreRoutes` time. `snapshotRoutes` preserves the `Bool?` shape unless the user explicitly diverges from the default. Engine never sees `nil`.
- [x] `JboxPreferences.shareDevicesByDefaultKey = "com.jbox.shareDevicesByDefault"` and bridged through `readPreferencesFromDefaults` / `writePreferencesIntoDefaults`.

UI:
- [x] `AddRouteSheet` + `EditRouteSheet`: "Share device with other apps" checkbox below the latency-tier picker. When checked, the Performance tier option is relabelled "Performance — unavailable when sharing" and an inline helper explains. If Performance was selected, the tier snaps to Low on toggle.
- [x] `PreferencesView` "Audio" tab gains a "Routing defaults" section with the global toggle + helper copy.
- [x] `RouteRow`: tiny lock glyph next to the latency pill when the route's source device is currently hog-held, with a tooltip naming the route holding hog mode (derived from `persisted.routes` + live status, no extra engine accessor needed).
- [x] Downgrade indicator on the row: a `SharingPill` companion to `LatencyPill`, rendered only when `route.status.shareDowngraded` is set. Same pill geometry as the latency pill so the two sit in one visual rhythm at the right edge of the row; tinted orange (foreground + 15 % orange background fill) to draw attention. Icon + "Shared · Low" label on-pill; the longer remediation copy lives in the `.help(...)` tooltip with `.contentShape(RoundedRectangle(...))` extending the hover hit region to the full pill. See the UX-iteration deviation below for how we arrived here.

Tests:
- [x] Catch2 `[route_manager][share_device]` (4 cases): share-path skips `claimExclusive` on the duplex fast path; Performance + share demotes to Low + sets `SHARE_DOWNGRADE`; regression — share=false preserves today's exclusive behaviour; `SHARE_DOWNGRADE` survives a stop + start cycle (guards the effective-mode-local-not-mutating invariant below).
- [x] Catch2 `[device_io_mux][share_device]` (3 cases): share-only attach never claims; mixed attachments keep hog tied to the non-sharing route (non-sharing first, then sharing first — symmetry both ways); release fires on non-sharing detach regardless of sharing-route lifecycle.
- [x] Swift Testing: `StoredPreferences.shareDevicesByDefault` round-trips + decodes to `false` on missing key; `StoredRoute.shareDevices` round-trips as `Bool?` with both `true` and `false` pinned, and decodes to `nil` from a pre-Phase-7.5 JSON blob.

Phase 7.5 summary of deviations:
- **Mux hog gate is AND, not just non_sharing > 0.** The original mini-plan suggested driving hog-mode purely from the `non_sharing_attached_` refcount. That would have regressed the pre-Phase-7.5 invariant "no-opinion Off-tier routes never claim exclusive" — a route with `requested_buffer_frames = 0 && share_device = false` would start hogging the device with no latency win. Fixed by gating on both: `exclusive_claimed iff non_sharing_attached_ > 0 AND currentMinBufferRequest > 0`. The single-route `[device_io_mux][low_latency]` regression test was the one that caught this; keeping the AND means the hog claim still fires for everything that actually wants a buffer size.
- **Share mode skips the buffer-size request entirely.** Earlier draft of `updateBufferRequest` still issued `requestBufferFrameSize(uid, target)` without holding hog, on the theory that the HAL's shared-client max-across-clients policy would either honour it (when JBox was the only client) or clamp it upward (when another app held the device). That theory fails on aggregate devices: `CoreAudioBackend::requestBufferFrameSize` on an aggregate UID fans `setBufferFramesOnID` out across every active sub-device, and when those per-sub-device calls happen *without* hog mode on a running aggregate, Core Audio's internal scheduler stalls — the IOProc stops firing, the route sits in `RUNNING` with silence, and no error surfaces. Caught on a real V31 → Apollo-aggregate route during the Phase 7.5 manual test. Fixed by gating the request on `exclusive_claimed_`: share-mode attachments leave the device at its current buffer size, which is the mode's contract (coexist with other apps at whatever they're using). Pinned by an assertion in the `[device_io_mux][share_device]` regression test that `bufferSizeRequests()` stays empty for share-only attach/detach cycles.
- **Demotion uses a local `effective_mode`, not a mutation of `r.latency_mode`.** An earlier draft mutated the record's `latency_mode` from 2 to 1 when `share_device` forced the demotion. That was invisible after the first start — but on the *second* start (after a stop), the gating check `r.latency_mode == 2` no longer matched, so `share_downgraded` silently cleared and `pollStatus` stopped surfacing `SHARE_DOWNGRADE`. Caught during the review pass. Fixed by computing a stack-local `effective_mode` at the top of `attemptStart` and using it for the fast-path check, ring-sizing switch, and mux-target predicate — `r.latency_mode` stays equal to the user's stored choice across the route's lifetime. Pinned by a new `[route_manager][share_device]` case that cycles start / stop three times and asserts the flag on every poll.
- **`snapshotRoutes` distinguishes "route was already persisted" from "brand-new UI add" via dictionary key *presence*, not the stored value.** The subtlety: a brand-new route with `shareDevices = false && defaultShare = false` has the same effective bool as a legacy (pre-Phase-7.5) route with `shareDevices = nil && defaultShare = false`. If we collapsed those into the same "matches default, inherit" branch, a brand-new route the user explicitly saved as false would silently flip to true when the user later changed the default. Fixed with `[UUID: Bool?]` + `if let priorStored = priorByPersistId[id]` key-presence check: only previously-persisted routes get to preserve the `nil` inherit-sentinel; new routes always pin a concrete `Bool` so later default changes don't retroactively alter them.
- **Downgrade indicator landed on its third iteration.** v1 was a bare orange `arrow.down.circle` glyph next to the latency pill — the user flagged it as unexplained (the glyph alone doesn't name the effect, and the tooltip required hover). v2 replaced the glyph with a secondary-coloured caption line ("Sharing device — Performance downgraded to Low") under the mapping summary — removed the hover requirement but was, per manual testing, too quiet to catch on scan. v3 (landed) is a `SharingPill` companion to `LatencyPill`: same pill geometry, orange foreground + 15 % orange fill (warning palette, deliberately loud), on-pill label "Shared · Low", full remediation copy in the tooltip. Two gotchas baked in: (a) a `.contentShape(RoundedRectangle(...))` modifier extends the hover hit region across the pill's padding — without it, SwiftUI's `.help(...)` only fires over the icon + text glyphs themselves, leaving the padded border unresponsive to the tooltip; (b) matching the latency pill's shape keeps the two in one visual rhythm so they don't compete with the Start/Stop/edit/trash cluster to the right.
- **Lock glyph is derivable from both source *and* destination UIDs.** A first draft only checked `r.config.source.uid == uid`, missing the case where route X has `dest = B` with share=false and route Y has `source = B` — Y's source would be hog-held because of X, but Y would show no lock. Hog mode is per-device at the HAL (not per-direction), so the UI check is now `source.uid == uid || destination.uid == uid`. Still derivable from `store.routes` alone, no ABI addition.

---

## Phase 7.6 — Self-routing reliability

**Status:** 🚧 The big simplification landed; **7.6.3 robust teardown landed (2026-04-27); 7.6.4 engine-side hot-plug listeners + auto-recovery and 7.6.5 engine-side sleep/wake handling both landed (2026-04-28)**. The `CoreAudioBackend` HAL property listener wiring for 7.6.4 and the `MacosPowerEventSource` for 7.6.5 are deferred to hardware-tested follow-ups — CI exercises the simulator path for both. Re-scoped twice in quick succession during 2026-04: first from an in-house-driver plan to BlackHole (after the macOS 13+ HAL-sandbox codesign wall on ad-hoc bundles), then from BlackHole detection to "drop hog mode entirely" (after manual hardware testing showed the hog/buffer-shrink machinery was the source of the v1 use case's problems, not a missing virtual driver). The own-driver path archived earlier (branch `archive/phase7.6-own-driver`) stays archived.

**Goal.** Make JBox's self-routing path durable and predictable. Two buckets:

1. **Drop hog mode and the aggressive HAL buffer-shrink path.** *(Landed; subsequently a single no-hog `setBufferFrameSize` write was re-introduced in ABI v11 — see the v11 deviation below and `CLAUDE.md` "Device & HAL ownership policy" for the durable contract.)* The previous plan tried to control device buffer sizes via `kAudioDevicePropertyHogMode` + `kAudioDevicePropertyBufferFrameSize` writes. Both interacted poorly with macOS aggregate devices: silent IOProc-scheduler stalls when fan-out wrote properties without authority; `HALS_PlugIn::HostInterface_PropertiesChanged: the object is not valid` cascades that crashed co-resident DAWs sharing the aggregate. The cure was worse than the disease — multiple control-flow bandages chasing one feature that v1's monitoring topology doesn't need. The fix: users dial their interface buffer in their interface software (UA Console, RME TotalMix, MOTU CueMix, Audio MIDI Setup), JBox respects whatever it finds — and the optional per-route `RouteConfig.bufferFrames` preference (v11) is the *only* HAL property write the engine ever issues, with no hog claim attached. Phase 7.5's "share device with other apps" opt-out is therefore moot; share is the only mode and the toggle is gone.
2. **Reliability follow-ups.** *(Pending.)* Device hot-plug, sleep/wake, robust teardown. These still apply on top of the simplified engine.

**Entry criteria.** Phase 7.5 (now reverted; see [§ Phase 7.5](#phase-75--device-sharing-hog-mode-opt-out)) was the entry signal. The simplification commit removes Phase 7.5's machinery wholesale.

**Exit criteria.**

- [x] **7.6.1.** "Engine error" alert never re-presents after the user dismisses it. Regression test in `EngineStoreTests`.
- [x] **7.6.simplification.** Hog mode / buffer-shrink / share-device opt-out removed from the engine, the C ABI (v9 → v10 MAJOR), the Swift wrapper, the persistence layer, the UI (toggle + lock glyph + SharingPill), and the test suite. Routes share devices by default — same behaviour Phase 7.5 made opt-in, now uniform. `RouteConfig.shareDevices`, `RouteStatus.statusFlags`, `JBOX_ROUTE_STATUS_SHARE_DOWNGRADE`, `IDeviceBackend::claimExclusive` / `releaseExclusive` / `requestBufferFrameSize` are gone.
- [x] **7.6.3.** `closeCallback` and `releaseRouteResources` check return codes; failed teardown leaves the in-memory record alive so the next stop/dispose retries instead of silently leaking. New `[sim_backend][teardown_failure]` + `[route_manager][teardown_failure]` cases. (Strictly about IOProc destruction — the hog-release path is gone.) See the 7.6.3 deviation below for the bool-return contract, the destructor-side best-effort retry on the mux, and the dispose-time retry on `removeRoute`.
- [x] **7.6.4 (engine).** `DeviceChangeWatcher` + `IDeviceBackend::setDeviceChangeListener` interface land. Engine spawns a 10 Hz consumer thread that drains the watcher and feeds events to `RouteManager::handleDeviceChanges`: kDeviceIsNotAlive on a UID a running route depends on transitions the route to WAITING with `last_error = JBOX_ERR_DEVICE_GONE`; kDeviceListChanged / kAggregateMembersChanged refresh the device manager and retry every WAITING route (auto-recovery on reappearance). New ABI v12 (additive) with `JBOX_ERR_DEVICE_GONE`. CoreAudioBackend's HAL property listener registration is **deferred to a follow-up commit after manual hardware testing** — the simulator path is what CI exercises today. See the 7.6.4 deviation below + `docs/refactoring-backlog.md` § R1 for the open `last_error` naming question.
- [x] **7.6.5 (engine).** `PowerStateWatcher` + `IPowerEventSource` interface land. `kWillSleep` events fire a synchronous handler (wired by Engine to `RouteManager::prepareForSleep`) and the watcher acks regardless of what the handler does. `kPoweredOn` events queue and drain on the existing 10 Hz consumer thread, which then calls `RouteManager::recoverFromWake` (primes per-route `wake_retries_remaining = 3` + `wake_next_retry_at = now`) followed by `tickWakeRetries(now)` every tick to fire due retries with linear backoff (200ms × attempt index → cumulative +0ms / +200ms / +600ms). After 3 failed attempts the route stays `WAITING + JBOX_ERR_SYSTEM_SUSPENDED`; 7.6.4's device-change watcher remains the long-tail recovery mechanism. New ABI v13 (additive) with `JBOX_ERR_SYSTEM_SUSPENDED`. The production `MacosPowerEventSource` (`IORegisterForSystemPower` wiring) is **deferred to a hardware-tested follow-up**, same model as 7.6.4's CoreAudioBackend HAL listener.
- [x] `make verify` green on the combined diff. Verified throughout 7.6.x landings; the pre-commit checklist enforces it on every commit.
- [~] Manual hardware acceptance on a real aggregate device confirms (a) first start always produces audio at the user's chosen interface buffer, (b) hot-unplugging a sub-device transitions the route to WAITING and replugging recovers, (c) sleep/wake cycle restarts the route within a few seconds, (d) any engine error popup is dismissed in one click and never re-presents. **Deferred to Phase 9 hardware session.**

### What the simplification removed

**Engine C++:**
- `IDeviceBackend::claimExclusive` / `releaseExclusive` / `requestBufferFrameSize` — gone from the interface.
- `CoreAudioBackend`: `hogDeviceID`, `unhogDeviceID`, `setBufferFramesOnID`, `getActiveSubDevices`, `exclusive_state_`.
- `SimulatedBackend`: `exclusive_claimed` per-slot, `exclusive_state_`, `isExclusive`, `bufferSizeRequests`. New `setBufferFrameSize(uid, frames)` test seam mirrors the user updating the buffer in their interface software.
- `RouteManager`: `r.share_device`, `r.share_downgraded`, `r.duplex_exclusive_claimed`, the Performance-on-share silent demotion, the buffer-shrink call site in the duplex fast path.
- `DeviceIOMux`: `non_sharing_attached_`, `last_requested_frames_`, `exclusive_claimed_`, `updateBufferRequest`, `currentMinBufferRequest`, the `share_device` parameter on `attachInput` / `attachOutput`, the `requested_buffer_frames` field on `InputEntry` / `OutputEntry`.

**C ABI (v9 → v10 MAJOR break):**
- `share_device` field removed from `jbox_route_config_t`.
- `status_flags` field removed from `jbox_route_status_t`.
- `JBOX_ROUTE_STATUS_SHARE_DOWNGRADE` constant removed.
- `buffer_frames` field removed from `jbox_route_config_t` — **subsequently re-added in v11 (additive, MINOR)** with no-hog SD-style semantics; see deviation above.
- `jbox_engine_supported_buffer_frame_size_range` entry point removed (the only consumer was the now-deleted per-route Performance buffer picker).

**Swift wrapper + persistence:**
- `RouteConfig.shareDevices` removed; `RouteConfig.bufferFrames` removed and **subsequently re-added in v11** with no-hog semantics.
- `RouteStatus.statusFlags` and `shareDowngraded` removed.
- `Engine.supportedBufferFrameSizeRange(forDeviceUid:)` and `EngineStore.bufferFrameRange(forDeviceUid:)` removed (no UI consumer).
- `StoredRoute.shareDevices` removed; `StoredRoute.bufferFrames` removed and **subsequently re-added** with `decodeIfPresent`. `StoredPreferences.bufferSizePolicy` and `StoredPreferences.shareDevicesByDefault` removed; `BufferSizePolicy` enum + its custom Codable removed entirely. Old `state.json` files still decode cleanly because `JSONDecoder` ignores unknown keys and surviving fields use `decodeIfPresent` with defaults — the dropped fields are silently discarded.
- `JboxPreferences.shareDevicesByDefaultKey` and `JboxPreferences.bufferSizePolicyKey` removed; their `readPreferencesFromDefaults` / `writePreferencesIntoDefaults` plumbing is gone.

**SwiftUI:**
- "Share device with other apps" checkbox in `AddRouteSheet` / `EditRouteSheet` removed, along with the Performance-unavailable-when-sharing helper copy and the `onChange` snap-to-Low handler.
- Per-route "Buffer size" picker on the Performance tier in `AddRouteSheet` / `EditRouteSheet` was briefly removed and **re-added** with the v11 mechanism. The picker now offers `16 / 32 / 64 / 128 / 256 / 512 / 1024 / 2048` plus a "No preference" default; tier-mode footer copy is updated to explain the `max-across-clients` semantic.
- "Routing defaults" section + Audio-tab "Buffer-size policy" picker in `PreferencesView` removed; replaced with an informational footer explaining that buffer is set per-route on the route sheet (not globally).
- `SharingPill` view removed; `RouteRow`'s lock glyph and `hogHoldingRoute()` / `hogHolderTooltip()` helpers removed.

**Tests:**
- Removed Catch2 cases: every `[route_manager][duplex][buffer]`, `[route_manager][duplex][exclusive]`, `[route_manager][duplex][aggregate]`, `[route_manager][share_device]`, `[device_io_mux][share_device]`, `[device_io_mux][low_latency]`, the three `[device_backend][buffer]` `supportedBufferFrameSizeRange` cases, and the post-shrink mux tests.
- Added Catch2 cases for v11: `[route_manager][duplex][buffer_frames]` (single-device override, no-override no-write, aggregate fan-out, cross-device both-sides write). Each pins that exactly one `setBufferFrameSize` per device fires, the simulated backend records the write, and the device's buffer lands at the requested value with no hog claim.
- Removed Swift Testing cases: `shareDevicesByDefaultRoundTrip`, `shareDevicesRoundTrip`, `shareDevicesMissingIsNil`, the `BufferSizePolicy` round-trip suite. Added a Phase-7.5/Phase-6-shape decode-with-discard regression pinning persistence-compat. Re-added `bufferFramesRoundTrip` covering both `StoredRoute.bufferFrames` round-trip and the missing-key-decodes-to-nil case.
- After the cut + the v11 re-add: 221 C++ Catch2 cases (was 270+; +5 new buffer-related regressions), 145 Swift Testing cases. `make verify` green.

### What survived

- **Latency tiers (Off / Low / Performance).** Now strictly *ring-sizing* presets for cross-device routes; for self-routes (src == dst) the duplex fast path still skips the ring + converter. The HAL buffer is whatever the user has set in their interface software; JBox respects it.
- **`DeviceIOMux`** for multi-route fan-out on a single device (input + output trampolines, RCU-style dispatch).
- **Drift sampler / `AudioConverterWrapper`** for cross-device routes with independent clocks.
- **The alert-dismiss fix from sub-phase 7.6.1.**

### Pending sub-phases (unchanged in spirit)

#### Sub-phase 7.6.3 — Robust teardown ✅

**Goal.** Stop teardown from masking failures. When a destroy call fails on a degraded / hot-unplugged device, the in-memory bookkeeping must record that so the next opportunity retries — instead of nulling the pointer and silently leaking the kernel-side resource.

- [x] `IDeviceBackend::closeCallback` returns `bool` (interface change; both `CoreAudioBackend` and `SimulatedBackend` updated). `true` = closed / already gone (caller may clear its handle). `false` = destroy refused (caller MUST keep its IOProcId for retry; the backend retains the slot bookkeeping). New RT log code `kLogTeardownFailure = 104` (value_a = backend status placeholder, value_b = IOProcId).
- [x] `core_audio_backend.cpp` `closeCallback` — checks `AudioDeviceDestroyIOProcID`'s `OSStatus`. On non-`noErr` it leaves `rec.ca_proc_id` set + the `ioprocs_` entry in the map and returns `false`. (`AudioDeviceStop` failures are absorbed silently — Apple documents stop as idempotent and "already stopped" returns are benign on the destroy path.) The log push lives at the *caller* layer for now (RouteManager / DeviceIOMux own the queue handle); plumbing the `OSStatus` into `value_a` is a small follow-up that lands when manual hardware testing demands the richer breadcrumb.
- [x] `route_manager.cpp` `releaseRouteResources` — duplex teardown reads the bool; on `false` it preserves `r.duplex_ioproc_id`, pushes `kLogTeardownFailure` with the route id + IOProcId, and continues with `stopDevice` / ring / converter / scratch / mux release unconditionally ("state fully drained except for the IOProc handle").
- [x] `route_manager.cpp` `attemptStart` (duplex fast path) retries the residual close before opening fresh: a stuck `r.duplex_ioproc_id` gets one more `closeCallback` attempt and, on success, `openDuplexCallback` proceeds. Persistent refusal returns `JBOX_ERR_DEVICE_BUSY` with a fresh log.
- [x] `route_manager.cpp` `removeRoute` makes one final best-effort `closeCallback` attempt on any residual `duplex_ioproc_id` before erasing the route record. The route is going away — this is its terminal dispose. Persistent refusal here logs and accepts the kernel-side leak; 7.6.4's HAL listeners are the durable recovery mechanism.
- [x] `device_io_mux.cpp` — last-detach close paths (`detachInput`, `detachOutput`) check the bool: on `false` they preserve `input_ioproc_id_` / `output_ioproc_id_`, push `kLogTeardownFailure` (route_id 0; the mux is per-device), and skip `maybeStopDevice` for that direction (the slot is still attached at the kernel level). Mux destructor makes one best-effort retry; remaining failures log loudly. `DeviceIOMux` constructor gains a borrowed `jbox::rt::DefaultRtLogQueue*` (threaded through `RouteManager::getOrCreateMux`).
- [x] `SimulatedBackend` test seams (test-fixture surface): `setNextCloseCallbacksFailing(count)` (global budget), `setCloseCallbackFailing(id, count)` (per-id budget; checked first), `isCallbackOpen(id)`, `hasInputCallback(uid)`, `hasOutputCallback(uid)`, `hasDuplexCallback(uid)`. Failure-injected closes return `false` and preserve callback bookkeeping exactly like a real `AudioDeviceDestroyIOProcID` non-`noErr` would.
- [x] Catch2 cases: 4× `[sim_backend][teardown_failure]` (verify the fixture seam) + 5× `[route_manager][teardown_failure]` (duplex log emission, duplex retry-on-next-startRoute, removeRoute completes-with-log, mux log emission, mux persistent-failure leaves slot populated). 238 cases pass; `make verify` green on the combined diff.

#### Sub-phase 7.6.4 — HAL property listeners + auto-recovery ✅ (engine; HAL plumbing pending)

**Goal.** Notice device topology changes the moment they happen, instead of relying on the user to click Refresh.

- [x] `IDeviceBackend::setDeviceChangeListener(cb, user_data)` interface — both `CoreAudioBackend` (currently a stub: stores the callback; HAL listener wiring pending) and `SimulatedBackend` (full impl + the `simulateDeviceRemoval` / `simulateDeviceReappearance` / `simulateAggregateMembersChanged` test seams) implement it.
- [x] `DeviceChangeEvent` struct on the C++ side: `kDeviceListChanged` / `kDeviceIsNotAlive` / `kAggregateMembersChanged` + `uid`. Backend reports — debouncing / dedup is the listener's responsibility.
- [x] New `DeviceChangeWatcher` (`Sources/JboxEngineC/control/device_change_watcher.{hpp,cpp}`) registers itself on the backend, captures events into a `std::mutex`-protected `std::deque`, and exposes `drain()` for the control thread. Mutex (rather than SPSC) because events are sparse — sample-rate cascades hit a handful per second; hot-plug + sleep/wake cycles a handful per minute. Lock contention is a non-issue at that rate.
- [x] `RouteManager::handleDeviceChanges(events)` — two-pass reaction: first apply every `kDeviceIsNotAlive` (force-stop + WAITING + `JBOX_ERR_DEVICE_GONE`), then collapse all list-changed / members-changed events into a single `dm_.refresh()` followed by `attemptStart` on every WAITING route. Idempotent on repeats (already-WAITING route ignored), correct under bursty cascades (one refresh per drain regardless of N events).
- [x] `Engine` owns the watcher + spawns a 10 Hz consumer thread (`hotPlugThreadLoop`) that calls `tickHotPlug()` → `watcher_.drain()` + `rm_.handleDeviceChanges(events)`. Same `spawn_sampler_thread` opt-out flag the drift sampler uses, so tests can drive `tickHotPlug()` synchronously.
- [x] `JBOX_ERR_DEVICE_GONE` (ABI v11 → v12 additive) carries the device-loss origin on `r.last_error`. Initial-WAITING (route started before its devices appeared) keeps `last_error = JBOX_OK`, so a future UI can differentiate "waiting on first plug-in" from "yanked, recovering". The naming smell on the `last_error` field + the underlying `jbox_error_code_t` type is logged in `docs/refactoring-backlog.md` § R1 for a focused later sprint.
- [x] Catch2 cases land:
  - 3 `[device_change_watcher]` (drain mechanics)
  - 3 `[sim_backend][device_change]` (simulator seams fire the right kinds in the right order)
  - 6 `[route_manager][device_loss]` (loss on src / dst / unrelated; idempotent; auto-recovery via list-changed; multi-route shared device)
- [x] **F1 (engine half) landed (2026-04-28):** real `CoreAudioBackend` HAL listener registration via `AudioObjectAddPropertyListener` against the system object + each enumerated device + each aggregate, with a pure `core_audio_hal_translation.hpp` helper translating callbacks into `DeviceChangeEvent`s and `enumerate()` reconciling the per-device listener set on every refresh. See the Phase 7.6 deviation below for the design choices (function-pointer variant over block API, shared-mutex over dispatch queue, no-lock-during-Apple-calls rule). Manual hardware acceptance per [`docs/followups.md` § F1](./followups.md#f1--production-hal-property-listener-registration-in-coreaudiobackend) is the user's gate; CI exercises only the simulator path.
- [ ] **Pending follow-up — F3 in [`docs/followups.md`](./followups.md#f3--devicechangewatcher-event-debounce):** ~200ms timer-based debounce inside the watcher's drain. Non-critical perf refinement; add when real-hardware traces (post-F1) show a sample-rate cascade actually thrashes.

#### Sub-phase 7.6.5 — Sleep/wake handling ✅ (engine; macOS event source pending)

**Goal.** Survive a sleep/wake cycle without the user touching anything.

- [x] `IPowerEventSource` interface (`PowerStateEvent` with `kWillSleep` / `kPoweredOn`, `setPowerStateListener(cb, user_data)` virtual, `acknowledgeSleep()` virtual). Production wiring (`IORegisterForSystemPower` + the synchronous `IOAllowPowerChange` ack call) lands in a separate hardware-tested commit; `SimulatedPowerEventSource` ships now with `simulateWillSleep` / `simulatePoweredOn` test seams + an `ackCount()` introspection counter.
- [x] New `PowerStateWatcher` (`Sources/JboxEngineC/control/power_state_watcher.{hpp,cpp}`) splits the listener: `kWillSleep` invokes a caller-registered `setSleepHandler` synchronously and then calls `source.acknowledgeSleep()` regardless (so missing handler == still acks); `kPoweredOn` is queued onto a `std::mutex`-protected `std::deque` for the engine's tick to drain via `drain()`. The split keeps the synchronous-ack contract clean — production code wires the sleep handler to `RouteManager::prepareForSleep`; the wake-recovery loop pops kPoweredOn events off the queue.
- [x] `RouteManager::prepareForSleep()` — synchronous teardown of every RUNNING route + transition to `WAITING` with `last_error = JBOX_ERR_SYSTEM_SUSPENDED`. STOPPED / pre-existing-WAITING / ERROR routes left untouched.
- [x] `RouteManager::recoverFromWake()` — primes `wake_retries_remaining = 3` + `wake_next_retry_at = now` on every WAITING+SYSTEM_SUSPENDED route. Does not attempt to start anything itself.
- [x] `RouteManager::tickWakeRetries(now)` — fires due retries. Linear backoff: after attempt N fires (N ∈ {1,2}), wait `N × 200ms` before attempt N+1. With budget=3 + base=200ms attempts land at +0ms / +200ms / +600ms cumulative from priming. After 3 failed attempts the route stays in `WAITING + SYSTEM_SUSPENDED`; the existing 7.6.4 device-change listener picks up devices that come back later still. `now` is parameterised so tests drive the cadence deterministically (same idiom as `DriftSampler::tickAll(dt_seconds)`).
- [x] `Engine` constructor takes an optional `std::unique_ptr<IPowerEventSource>`; nullptr disables sleep/wake handling end to end. When non-null, Engine creates a `PowerStateWatcher`, wires its sleep handler to `rm_.prepareForSleep()`, and the existing 10 Hz consumer thread drives `tickPower()` after `tickHotPlug()` per iteration. `~Engine` clears the sleep handler explicitly before `rm_` is destroyed so a late kWillSleep can't fire on a half-destroyed RouteManager.
- [x] `JBOX_ERR_SYSTEM_SUSPENDED = 9` (ABI v12 → v13 additive) carries the sleep origin on `r.last_error`. The UI can render distinct affordances for "yanked, recovering" (DEVICE_GONE, 7.6.4) vs "system was just asleep, retrying" (SYSTEM_SUSPENDED, 7.6.5) vs "first plug-in pending" (JBOX_OK).
- [x] Catch2 cases land:
  - 3 `[sim_power]` (the simulator's simulate-* + acknowledgeSleep counter)
  - 3 `[power_state_watcher]` (kWillSleep invokes handler + acks; missing handler still acks; kPoweredOn drains via drain())
  - 6 `[route_manager][sleep_wake]` (prepareForSleep on running; left alone for non-running; first attempt success; bounded retries on missing devices; device-appears-between-attempts wins on the right tick; non-participants untouched)
- [ ] **Pending follow-up — F2 in [`docs/followups.md`](./followups.md#f2--production-macospowereventsource-wrapping-ioregisterforsystempower):** real `MacosPowerEventSource` wrapping `IORegisterForSystemPower`. Without this, 7.6.5's recovery path is dead in production. Hardware-gated; surface area + dispatch-queue research + IOPM ack pitfalls + acceptance plan all written up under § F2.

#### Sub-phase 7.6.6 — Aggregate-loss detection + stall watchdog ✅ (engine; UI)

- [x] **Aggregate-aware UID matching.** `BackendDeviceInfo` gains `is_aggregate` + `aggregate_member_uids`, populated by both backends during `enumerate()`. `RouteRecord::watched_uids` carries `source_uid` + `dest_uid` + each aggregate's active sub-device UIDs; `attemptStart` populates it on RUNNING transition; `RouteManager::handleDeviceChanges` walks the set on `kDeviceIsNotAlive`. Closes the real-world bug where a USB interface that was a member of an aggregate was unplugged and the route stayed RUNNING. Multi-day app sessions surfaced this; the simulator path 7.6.4 originally shipped didn't exercise aggregate composition.
- [x] **`kAggregateMembersChanged` re-expansion.** When the aggregate that backs a running route loses one of its active sub-devices and macOS only fires the aggregate-level signal (no per-member `kDeviceIsNotAlive`), `handleDeviceChanges` re-expands `watched_uids` against the refreshed `dm_` and tears the route down when a previously-watched member is gone. Aggregates that grow or reshape without losing a member don't disturb running routes.
- [x] **Stall watchdog (`JBOX_ERR_DEVICE_STALLED`, ABI v14 → v15 additive).** Per-route counters `last_seen_frames_produced` / `last_seen_frames_consumed` / `stall_ticks`; `RouteManager::tickStallWatchdog(now)` increments `stall_ticks` while both counters stay frozen and the route is `RUNNING`. At 5 ticks (500 ms on the 10 Hz hot-plug cadence) the route is torn down and transitions to `WAITING + JBOX_ERR_DEVICE_STALLED`. Independent safety net for any "silent death" failure mode the HAL listeners didn't surface (another app preempts the device, an aggregate IOProc freezes without an IsAlive=0).
- [x] **Periodic WAITING-route retry.** `RouteManager::retryWaitingRoutes()` driven from `Engine::hotPlugThreadLoop` at 1 Hz (every 10th hot-plug tick). Refreshes `dm_` and calls `attemptStart` on each `WAITING` route. Generalises recovery so routes come back automatically when devices return, regardless of whether the HAL fires `kDeviceListChanged` / `kAggregateMembersChanged` — Apple's HAL is unreliable for "device powered off and back on" because the audio object often persists at the HAL layer; only the sample flow stops and resumes. `attemptStart` fast-fails on still-missing devices so the cost is trivial. Coexists with `tickWakeRetries`'s bounded retry schedule for `SYSTEM_SUSPENDED` routes (both call `attemptStart`, which is idempotent).
- [x] **UI: diagnostic text on WAITING rows.** `routeRowErrorText` (new) maps `(state, last_error)` to a row caption; `RouteListView` consumes it. WAITING rows whose `last_error` is `DEVICE_GONE` / `DEVICE_STALLED` / `SYSTEM_SUSPENDED` now show a red caption distinguishing them from "first plug-in pending" (last_error == JBOX_OK).
- [x] **Tests.** 14 new Catch2 cases (6 `[aggregate_loss]`, 4 `[stall]`, 3 `[waiting_retry]`, 1 new `[sleep_wake]` for the wake-retry state-gate) on the simulator path + 3 new `[device_manager][aggregate_members]` helper cases + 7 Swift Testing cases for `routeRowErrorText`. Total `make verify` impact: +24 cases, all green. The skip-set in `handleDeviceChanges` (Phase 7.6.6 in-flight fix), the stall-watchdog reset across stop+start (`resetStallWatchdog` at `attemptStart`'s RUNNING transition), the periodic `retryWaitingRoutes` recovery path, and the `tickWakeRetries` state-gate that prevents wake-retry from corrupting a route promoted by another path each have dedicated regression cases so future refactors can't silently strip them.

- [x] **UI: chevron icon reflects effective expand state.** `RouteRow`'s disclosure chevron now uses `(expanded && canExpand) ? "chevron.down" : "chevron.right"` — when the route flips to a non-RUNNING state and the meter view disappears, the chevron flips to `right` to match. The user's `expanded` intent is preserved underneath: when the route returns to RUNNING the chevron flips back to `down` and meters reappear without an extra click.

**Manual hardware acceptance (user's gate, mirrors F1).** Build an aggregate in AMS containing two physical interfaces, start a route on the aggregate, yank one interface. Within ~1 s the route should flip to orange-clock + "Device disconnected — waiting for it to return." Re-plug → orange clock disappears, route returns to RUNNING. (Two further hardware acceptance scenarios — `kAggregateMembersChanged`-only sub-device removal where macOS doesn't fire a per-member `IsNotAlive`, and an aggregate sample-rate cascade — are exercised by simulator regression tests; the user's hardware gate is the yank-and-replug pass above.)

#### Sub-phase 7.6.7 — Targeted listeners + ERROR-trap fix ✅ (engine)

- [x] **ERROR-trap fix in `attemptStart`.** Five failure paths used to park the route in `ERROR` for device-side conditions that are routinely transient on a recovery retry: channel-mismatch when the aggregate parent's `input_channel_count` / `output_channel_count` shrinks because a sub-device is missing; duplex-path mux conflict; duplex-path retry-close refusal; duplex-path `openDuplexCallback` failure; regular-path source/destination mux-attach refusal. Pre-7.6.7 the route was stuck in `ERROR` and `retryWaitingRoutes()` (which only iterates `WAITING`) never tried again — the user had to click Start manually. Now: those five paths transition to `WAITING + DEVICE_BUSY` (or `WAITING + DEVICE_GONE` for the channel-mismatch case) so the periodic retry / next event recovers automatically. Genuine config errors — channel mismatch on a route that has *never* reached `RUNNING` (the `ever_started` flag is false) — still go to `ERROR + MAPPING_INVALID`; silently waiting on a typo would hide it from the user.
- [x] **Targeted HAL listeners.** New `IDeviceBackend::setWatchedUids(std::vector<std::string>)` (default no-op). `RouteManager::publishWatchedUids()` computes the union of `{source_uid, dest_uid}` ∪ aggregate members across every route NOT in `STOPPED` or `ERROR`, and pushes the set to the backend after every state-changing entry point (`addRoute` is a no-op since STOPPED routes are excluded, `removeRoute` / `startRoute` / `stopRoute` / `handleDeviceChanges` / `retryWaitingRoutes` after a refresh). `CoreAudioBackend::reconcilePerDeviceListeners` filters its install/remove diff against the set so `kAudioDevicePropertyDeviceIsAlive` and `kAudioAggregateDevicePropertyActiveSubDeviceList` listeners are only registered on UIDs the user actually cares about. Pre-7.6.7 we registered listeners on every enumerated device — wasted listener traffic + reconcile cost on a system inventory that may include many devices nobody is routing through. The system-wide `kAudioHardwarePropertyDevices` listener stays unconditionally registered (it is what surfaces a previously-unwatched UID returning to the system) so newly-needed devices still get picked up immediately.
- [x] **Tests.** 8 new Catch2 cases: `[error_trap]` quadruple covering (a) previously-running route with shrunken channel count → `WAITING` + recovery, (b) never-started route with bad mapping → `ERROR` (counter-test), (c) duplex-close transient → `WAITING` + recovery, (d) duplex-mux-conflict (non-duplex sibling holds the slot) → `WAITING` + recovery once the sibling stops; `[watched_uids]` quadruple covering (e) STOPPED routes don't pollute the set, (f) aggregate routes include parent + member UIDs, (g) ERROR routes are excluded, (h) `handleDeviceChanges` republishes after `kAggregateMembersChanged` so newly-active sub-device UIDs land in the listener filter. Total `make verify` impact: +8 cases, all green.

Phase 7.6 summary of deviations:

- **7.6.7 design — targeted listeners + ERROR-trap fix (2026-05-02).** *Goal:* close the second real-world recovery gap — a route torn down for stall/aggregate-loss not auto-recovering when the device returned, and the per-device HAL listener load growing with the entire system inventory rather than the routing set. *Choices made:*
  - **`ever_started` flag on `RouteRecord` over a more elaborate "is this device aggregate-and-degraded?" check.** Channel-mismatch in `attemptStart` is ambiguous on its own — a never-started route with a typo and a previously-running route on a degraded aggregate both look the same. The flag splits them cleanly: if the route has reached `RUNNING` once, we trust the original mapping was valid against the full device, so a current shortfall is a transient. Costs one bool per route; latched, never reset.
  - **WAITING + DEVICE_BUSY for every device-side transient in `attemptStart`, not just channel mismatch.** Five paths (mux conflict, retry-close refusal, `openDuplexCallback` failure, src/dst attach refusals) all map to the same recoverable bucket. The semantic line is: device-side conditions go to WAITING (retryable); programming/allocation-side failures (`AudioConverterWrapper` ctor throwing) keep the strict ERROR. Avoids carrying a per-path "is this really fatal?" classification.
  - **Backend-stored watched-UID set with idempotent comparison.** `IDeviceBackend::setWatchedUids` is the single seam; `CoreAudioBackend` stores the set and reconciles only when it changes. RouteManager publishes liberally (after every state-changing entry point) and the backend de-dupes — no need to track "did anything actually change?" at the call sites. Same shape as the existing `setDeviceChangeListener` plumbing.
  - **Excluded `STOPPED` and `ERROR` from the watched set; included everything else.** STOPPED routes aren't running so listener events for their UIDs are wasted. ERROR routes need user intervention before any recovery is meaningful, so listening on their UIDs is also wasted. WAITING + STARTING + RUNNING all need monitoring: WAITING for "device returned" reinvocation, STARTING/RUNNING for "device disappeared" detection.
  - **System-wide `kAudioHardwarePropertyDevices` listener stays unconditionally registered.** It's a single global listener (cheap), and it is what fires when a device the user starts caring about *next* shows up. Filtering it would require predicting future watched-UID sets. The per-device filter only applies to the `kAudioDevicePropertyDeviceIsAlive` and `kAudioAggregateDevicePropertyActiveSubDeviceList` listeners that actually grow with the inventory.
  - **`parkInWaitingTransient(r, code)` private helper extracted from the five reclassified attemptStart paths.** Each path collapsed to one line: set state, set last_error, push the WAITING log event, return JBOX_OK. Also covers the channel-mismatch DEVICE_GONE case. Five repetitions of the same four-line block went away; future tweaks to the WAITING-transient handshake live in one place.
  - **`tickWakeRetries` republishes when it ran a refresh.** The wake-retry budget can promote a route to RUNNING (set unchanged) or push it to ERROR via attemptStart's converter-ctor edge (set shrinks). Without a republish at the tick's tail the watched set would be stale; the periodic 1 Hz retry would notice eventually but listener load would bloat in the meantime. Same dedupe-on-no-change as every other publish point.
  - *Diff:* +~140 LOC engine + tests + docs. 8 new Catch2 cases (4 `[error_trap]` + 4 `[watched_uids]`). `make verify` green; 332 → 340 cases.

- **7.6.6 design — aggregate-aware UID matching + stall watchdog (2026-05-02).** *Goal:* close the real-world bug where a route on an aggregate stayed RUNNING when one of the aggregate's sub-devices was unplugged. *Choices made:*
  - **`watched_uids` on `RouteRecord` over a reverse lookup on `DeviceManager`.** A flat set per route, populated at `attemptStart`, lives where the matcher already iterates. Avoids growing `DeviceManager`'s API surface (previous brainstorming option 1B). Trade-off: the cached set goes stale if AMS reconfigures the aggregate while the route is running, so `handleDeviceChanges` re-expands on every `kAggregateMembersChanged`.
  - **Stall watchdog as an independent safety net rather than relying solely on HAL signals.** Frames-not-advancing is the most direct signal possible — the watchdog catches "silent death" cases that no HAL property listener fires for (another app temporarily preempts the IOProc, an aggregate's internal scheduling goes wrong, an undocumented Apple edge). Initial threshold of 1.5 s tuned down to **500 ms** after real-hardware acceptance: users perceived "stuck on green" for the full 1.5 s after powering an aggregate's sub-device off, which felt sluggish. 500 ms still dominates a typical 64-frame @ 48 kHz round-trip (~1.3 ms) by ~400×, and the "either-side advance resets" rule keeps false-positive risk negligible.
  - **Periodic WAITING-route retry over relying purely on HAL events.** Real-hardware acceptance also surfaced that the HAL doesn't fire `kDeviceListChanged` or `kAggregateMembersChanged` reliably on "device powered off and back on" — Apple keeps the audio object resident at the HAL layer; only the sample flow stops and resumes. Without periodic retry, routes torn down for `DEVICE_STALLED` (or quirky `DEVICE_GONE`) sat in `WAITING` forever even after audio came back. Added `RouteManager::retryWaitingRoutes()` driven from `Engine::hotPlugThreadLoop` at 1 Hz. `attemptStart` fast-fails on still-missing devices so the steady-state cost is one `enumerate()` per second per affected engine. Generalises recovery; obviates the need to detect the comeback via any specific HAL event.
  - **`tickWakeRetries` state-gate.** The new 1 Hz periodic retry can promote a `WAITING + SYSTEM_SUSPENDED` route to `RUNNING` inside the wake-retry budget window (3 attempts at +0 / +200 / +600 ms). Without a gate, the next due wake-retry would call `attemptStart` on a RUNNING route — which already has its muxes / ring / converter attached, so re-running the start sequence would corrupt that state. Added an `r.state != JBOX_ROUTE_STATE_WAITING` check at the top of `tickWakeRetries`'s inner loop that zeros `wake_retries_remaining` and skips. Also closes a pre-existing latent race (any path that promoted a SYSTEM_SUSPENDED route — e.g., `handleDeviceChanges` retry or a manual stop+start — could have triggered the same corruption); 7.6.6 surfaced it because the new periodic retry made the window much more reachable.
  - **No `kAudioDevicePropertyDeviceIsRunningSomewhere` tiebreaker.** Brainstormed and dropped — the watchdog as-is is conservative enough (both counters must freeze together) and the property is global, not per-IOProc. Capture as a follow-up if real-hardware testing reveals false positives.
  - **Implementation surfaced one interaction the design didn't anticipate.** Without a skip-set in the WAITING-retry pass, a route torn down for member loss would be immediately retried by the existing WAITING-route loop and re-promoted to RUNNING (because `attemptStart`'s only check is `findByUid != nullptr`, which the aggregate UID still satisfies). Added `member_loss_torn_down` set scoped to a single `handleDeviceChanges` invocation; subsequent invocations (e.g., once the device reappears and `kDeviceListChanged` fires) use a fresh empty set, so legitimate recovery still flows through.
  - **Design's `anyWatchedUidMissing` helper inlined as an ad-hoc loop in `handleDeviceChanges`.** The design doc named the helper at § 1; the shipped code inlines the equivalent loop directly in the post-refresh re-expansion phase. Behaviour is identical for the single call site that exists today. Capturing as a deviation so a future caller doesn't re-derive the loop and so the named-helper extraction can be picked up as a focused refactoring slice when there's a second consumer.
  - *Diff:* +14 commits / +~600 LOC engine + tests + docs. `make verify` green.

- **F1 design — function-pointer HAL listeners + shared_mutex for cross-thread state + no-lock-during-Apple-calls rule (2026-04-28).** *Goal:* land the production `CoreAudioBackend` HAL property listener wiring that 7.6.4's reaction layer needs, so devices that come and go fire `DeviceChangeEvent`s in production (not just in the simulator). *Choices made along the way:*
  - **Function-pointer `AudioObjectAddPropertyListener` over the block-API + dispatch-queue variant.** F1's research called the block API "probably right" because it gives a known thread context. The shipped code uses the function-pointer variant and a `std::shared_mutex` to serialise the read side of cross-thread state instead. Reasoning: the block API would have pulled in a `dispatch_queue_t` we own, plus its lifetime + targeting plumbing, for the same correctness the mutex gets us. The shared-mutex pattern is uniform with how the rest of the engine handles cross-thread state (no dispatch queues anywhere else in the engine). If F3's traces show actual contention on the mutex's shared side, revisit.
  - **Two state buckets, with different ownership rules.** `device_change_cb_` + `device_change_user_` + `id_to_uid_` are cross-thread (HAL callback reads, control thread writes) and live behind `callback_state_mutex_`. `system_listener_installed_` + `per_device_listeners_` are mutated by `setDeviceChangeListener`, `enumerate()`, and `~CoreAudioBackend()` and ride on the engine's documented single-control-thread model (`engine.hpp:8`). Splitting the buckets is what lets the no-lock-during-Apple-calls rule (next bullet) work without losing track of installed listeners. F1 introduces no new cross-thread access on the second bucket — the existing `device_ids_` / `started_` / `ioprocs_` fields ship under the same assumption.
  - **The control thread NEVER holds `callback_state_mutex_` during an Apple `AudioObjectAdd/RemovePropertyListener` call.** Apple's Remove blocks until in-flight callbacks complete; in-flight callbacks acquire the shared lock briefly. If the control thread held the exclusive lock during Remove, a callback firing on another HAL thread would block on the shared lock; Remove would block on the callback; hung. The shipped pattern is "snapshot under lock, drop lock, call Apple, optionally publish under lock again." Encoded in the helper structure: `removeAllListeners()` + `installSystemListener()` + `reconcilePerDeviceListeners()` are all control-thread, all lock-free at call time, and the cross-thread state is published before the Apple call is made.
  - **Diff-style reconciliation in `enumerate()` over remove-all-then-add-all.** `enumerate()` runs every UI device-list refresh + after every `dm_.refresh()` triggered by a list-changed event. Tear-down-and-rebuild on each call would mean N Apple add/remove pairs per UI tick on a stable system. The shipped code computes the AudioObjectID delta and only fires Add/Remove for changed entries — steady-state cost is one `AudioObjectGetPropertyDataSize` per enumerated device (the aggregate-detection probe inside `reconcilePerDeviceListeners`). Idempotent, matches the engine's "remove all listeners on `setDeviceChangeListener(nullptr)`" path.
  - **No "shutting down" flag.** F1's research suggested adding a flag the callback checks before touching state. Skipped — clearing `device_change_cb_` under the exclusive lock in the destructor (then calling `removeAllListeners()` outside the lock) achieves the same shutdown-safety with one fewer atomic. The destructor's Apple Remove blocks on in-flight callbacks; those callbacks read `device_change_cb_` under the shared lock, observe null, and bail.
  - **HAL listener body is a free function (`halPropertyListenerCallback`) that forwards to a member.** Apple's listener signature takes a `void* refCon`; we pass `this`. The static thunk does no work other than forwarding. This keeps the C-callable signature off the public class surface.
  - **`isAggregateDevice` probes via `AudioObjectGetPropertyDataSize` on `kAudioAggregateDevicePropertyActiveSubDeviceList`.** Non-aggregate devices return an error or zero size; aggregates return >0 even with empty member lists. No string-comparison or magic-property heuristics. Cheaper than walking all subselectors.
  - **Pure translator for the callback body's logic.** `core_audio_hal_translation.{hpp,cpp}` exposes `translateHalPropertyChange(selector, uid, is_alive_readback) → optional<DeviceChangeEvent>`. The actual HAL plumbing (registering listeners, calling the callback) is genuinely hard to test without real hardware; the translation logic is pure. 6 Catch2 cases under `[core_audio][hal_translation]` cover every selector branch, including the `IsAlive==1` no-event case, the empty-uid `IsAlive==0` case (production reverse-lookup-miss path), and unrelated-selector hardening.
  - **HAL listener lifecycle integration tests as a CI safety net.** 4 cases under `[core_audio][hal_listener_lifecycle]` exercise the real Apple Add/Remove sequence on the macOS runner's actual device list — register, re-register, enumerate-with-listener-active, destruct-with-listener-active. CI cannot fire callbacks (no hot-plug), so the assertions are weaker than the translator group: they catch typos, mismatched Add/Remove arguments, and destructor-cleanup crashes. The end-to-end "real device disappears → route transitions to WAITING" behavior remains gated on the manual hardware acceptance pass.
  - **Manual hardware acceptance is the user's gate.** Three tests — yank a sub-device of a running aggregate; yank a USB interface entirely; change sample rate via Audio MIDI Setup. All written up in `docs/followups.md` § F1. CI cannot exercise this; the deferred-status entry under 7.6.4's pending follow-ups is now ticked, but the manual acceptance gate remains until the user runs the three tests.
  - *Diff:* +480 LOC engine + tests + docs. 272 Catch2 cases (was 262; +10 new — 6 translator + 4 lifecycle integration). `make verify` green; the pre-existing `[multi_route][stress]` flake (intermittent, observed on baseline master without F1 changes) is captured as `docs/followups.md` § F4.

- **7.6.5 design — synchronous-ack split, bounded retries with linear backoff, optional power source on Engine (2026-04-28).** *Goal:* survive a sleep/wake cycle without user intervention. *Choices made along the way:*
  - **One listener interface, two distinct call shapes via PowerStateWatcher.** A single generic `setPowerStateListener` on `IPowerEventSource` keeps the abstraction tight; the watcher then *splits* it: `setSleepHandler(std::function<void()>)` for the synchronous kWillSleep callback (caller's job is to do its work + return; the watcher always acks the source afterward), and `drain()` for asynchronous kPoweredOn events. Forcing every consumer to handle the synchronous-ack contract at every call site would mean boilerplate and missed acks; pushing kWillSleep onto a queue would mean the system stalls waiting for an ack the tick can't deliver in time. The split keeps both concerns local to the right layer.
  - **Engine wires `setSleepHandler` to `rm_.prepareForSleep()` via a captured `this` lambda.** Cleared explicitly in `~Engine()` before rm_ is destroyed — the `PowerStateWatcher` destructor unregisters from the source on its own, but member-destruction order in C++ runs after rm_ for power_watcher_, so a late kWillSleep on the source's thread between rm_ destruction and watcher destruction would call into a half-destroyed RouteManager. The explicit clear plugs that gap.
  - **Bounded retries with linear backoff over passive-listener-driven recovery.** Project owner asked for "the standard pattern". Implementation: per-route `wake_retries_remaining` (uint8_t, 0 unless recovery is in flight) + `wake_next_retry_at` (steady_clock::time_point); `recoverFromWake` primes them; `tickWakeRetries(now)` fires due retries. With budget=3 + base=200ms, attempts cumulative at +0ms / +200ms / +600ms from wake. After exhaustion the route stays `WAITING + SYSTEM_SUSPENDED` and 7.6.4's device-change watcher takes over for the long tail. Tests inject `now` via parameter (no `sleep_for` flake).
  - **Optional power source on Engine, mirroring `spawn_sampler_thread`.** A nullptr `power_source` constructor argument disables sleep/wake handling end-to-end (no PowerStateWatcher created, `tickPower()` is a no-op). Same opt-out shape `DriftSampler` uses. Tests not exercising sleep/wake construct Engine without a source; tests that need it pass a `SimulatedPowerEventSource`. Production will eventually pass a `MacosPowerEventSource`.
  - **`JBOX_ERR_SYSTEM_SUSPENDED` distinct from `JBOX_ERR_DEVICE_GONE`.** Three semantically different "WAITING" reasons now: initial-WAITING (`JBOX_OK`) for "user started a route before its devices appeared", `JBOX_ERR_DEVICE_GONE` for "yanked / hot-unplugged", `JBOX_ERR_SYSTEM_SUSPENDED` for "system was just asleep". A future UI surface (Phase 6 follow-up) renders distinct affordances for each. The `last_error` / `jbox_error_code_t` naming smell still applies — see `docs/refactoring-backlog.md` § R1.
  - **`MacosPowerEventSource` deferred.** Same shape as 7.6.4's deferred CoreAudioBackend HAL listeners: the abstraction + simulator + reaction layer ship now; the macOS-specific `IORegisterForSystemPower` + `IOAllowPowerChange` wiring lands in a separate hardware-tested commit. CI can't usefully exercise the IOPM dispatch queue path.
  - *Diff:* +695 LOC (engine + tests + new files). 262 Catch2 cases (was 250; +12 new). `make verify` green.

- **7.6.4 design — host-driven 10 Hz tick over a separate-thread alternative; idempotent reactions in lieu of timer-based debounce; CoreAudioBackend HAL listener wiring deferred (2026-04-28).** *Goal:* notice device topology changes the moment they happen and recover affected routes without user intervention. *Choices made along the way:*
  - **Mutex-protected `std::deque` over a lock-free SPSC queue** in `DeviceChangeWatcher`. Hot-plug events are sparse — sample-rate cascades produce a handful per second, hot-plug + sleep/wake events a handful per minute. Mutex contention at that rate is invisible; SPSC's lock-free wins are dwarfed by its complexity (fixed-size buffer, manual UID copy into a fixed-size char array). The `RtLogQueue` lock-free SPSC is the right tool for RT-callback producers; `DeviceChangeWatcher`'s producer thread is HAL's listener pool, which can comfortably take a mutex.
  - **Idempotent reactions over a 200ms timer-based debounce.** The plan's checklist entry called for ~200ms coalescing inside the watcher. Implementing that requires an injectable clock for tests (otherwise tests become timing-dependent flakes). The simpler approach: make the *reaction* layer idempotent — already-WAITING routes ignore subsequent kDeviceIsNotAlive on the same UID, bursts of kDeviceListChanged collapse to one `dm_.refresh()` per drain. Correctness is the same; the only thing missing is the perf refinement of avoiding N redundant refreshes during a cascade. Added as a "pending follow-up" — defers until real-hardware traces actually show it's worth the complexity.
  - **One internal hot-plug consumer thread, ticking at 10 Hz.** Two alternatives considered: piggyback on the drift sampler's 100 Hz thread (would require coupling DriftSampler to Engine or to a callback hook — awkward), or expose `Engine::tickHotPlug()` and have the Swift host drive it from a Combine timer (clean but ties responsiveness to the host's cadence and adds a Swift-side wiring step). The dedicated thread mirrors `DriftSampler`'s pattern exactly, gates on the same `spawn_sampler_thread` flag tests use, and keeps the drift loop unentangled. The cost is one extra thread per Engine instance, sleeping 99% of the time.
  - **`JBOX_ERR_DEVICE_GONE` set on `r.last_error` for the device-loss path; initial-WAITING keeps `JBOX_OK`.** Differentiates "first plug-in pending" from "yanked, recovering" — the engine signal a future UI improvement needs to render distinct affordances (a "plug device in" hint vs. a "device removed, waiting for it to come back" badge). The field naming smell (`last_error == JBOX_OK` is a contradiction; `jbox_error_code_t` carries non-error variants) is captured in `docs/refactoring-backlog.md` § R1 for a focused later sprint — folding the rename into 7.6.4 was deferred so the feature work could land without an ABI rename absorbing the slice.
  - **CoreAudioBackend HAL property listener wiring deferred.** The interface is wired end to end (`setDeviceChangeListener` stores the callback), but the actual `AudioObjectAddPropertyListener` calls on `kAudioHardwarePropertyDevices` / `kAudioDevicePropertyDeviceIsAlive` / `kAudioAggregateDevicePropertyActiveSubDeviceList` haven't landed yet. The callback fires from a HAL-internal thread which is hard to exercise from CI; a manual hardware test pass is the right gate before merging that path. Until that lands, production behaviour is "engine doesn't know when devices come and go" — which is exactly what we shipped pre-7.6.4 anyway, no regression.
  - **Reaction layer tests bypass the watcher.** The 6 `[route_manager][device_loss]` cases call `rm.handleDeviceChanges(events)` directly with synthetic event vectors (after mutating the backend's device list to match what a real listener would see). The 3 `[device_change_watcher]` cases test the queue mechanics in isolation. The 3 `[sim_backend][device_change]` cases pin the simulator's simulate-* seams. No engine-level integration test exercises Engine + Watcher + RouteManager together — the path is mostly forwarding (`drain` + `handleDeviceChanges`) and the HAL listener side is the genuinely-hard-to-test piece. Manual hardware acceptance covers the integration end-to-end.
  - *Diff:* +680 LOC (engine + tests + new files). 250 Catch2 cases (was 238; +12 new). `make verify` green.

- **7.6.3 design — bool-return contract on `IDeviceBackend::closeCallback` + caller-side log emission + dispose-time retry (2026-04-27).** *Goal:* surface refused IOProc destroys instead of silently nulling pointers and leaking kernel-side resources. *Choices made along the way:*
  - **Bool over status enum.** The actually-distinguishable outcomes are "closed/already-gone" (caller may clear handle) and "destroy refused" (caller must keep handle for retry). Both reduce to a binary signal. The richer diagnostic — the macOS `OSStatus` from `AudioDeviceDestroyIOProcID` — belongs in the log payload, not the return type. Today the log's `value_a` is 0 (engine-layer push without backend-side status passing); plumbing `OSStatus` through is a follow-up that lands when manual hardware testing actually demands it.
  - **Caller pushes `kLogTeardownFailure`.** `RouteManager` and `DeviceIOMux` already own the borrowed log queue; `CoreAudioBackend` does not. Pushing at the caller layer avoids forcing a queue dependency into the backend (and into `SimulatedBackend`'s test fixture); the trade-off is the lost `OSStatus` in `value_a`, which is acceptable until real-hardware diagnostics need it.
  - **Retry surfaces.** Three retry hooks land instead of one because each path's natural "next opportunity" is different. (1) `RouteManager::attemptStart`'s duplex fast path retries the residual close before opening fresh — without this, the next `startRoute` would fail with `DEVICE_BUSY` until the user removed and recreated the route. (2) `RouteManager::removeRoute` makes one final best-effort attempt before erasing — the route is being disposed, so this is its terminal dispose path and there is no future "next opportunity" for the route record itself. (3) `DeviceIOMux`'s destructor makes a best-effort retry — the mux's lifetime ends when both directions go idle, so the destructor IS the next opportunity. None of these loop; the bound is exactly one retry per natural opportunity, and 7.6.4's HAL listeners pick up persistent failures the next time the device topology shifts.
  - **`route_id = 0` on mux-emitted logs.** A mux is a per-device resource shared across routes — there is no single owning route to attribute the failure to. Operators correlate via `value_b` (the IOProcId, which they can match against the `kLogRouteStarted` event that opened it).
  - **Test fixture seam on `SimulatedBackend`, not on production types.** `setNextCloseCallbacksFailing` + `setCloseCallbackFailing` + `isCallbackOpen` + `hasInputCallback` / `hasOutputCallback` / `hasDuplexCallback` all live on `SimulatedBackend` (which is itself a test fixture). No `friend` declarations, no `#ifdef JBOX_TEST_INTROSPECTION` plumbing, no test-only methods on `RouteManager` / `DeviceIOMux`. Tests assert behavioural consequences (slot state, log emission, restart success) rather than peeking at private fields.
  - **Persistent-failure surface deliberately tested.** A budget=2 case (`mux teardown leaves slot populated when retries exhaust the budget`) locks in that two consecutive failures (first-attempt + destructor retry) leave the kernel-side slot occupied with two `kLogTeardownFailure` events. This is the leak that 7.6.4's `DeviceChangeWatcher` is designed to detect and recover from — the contract is "log loudly, accept the leak, hand off to the listener layer".
  - *Diff:* +250 LOC C++, +85 LOC test code. 238 Catch2 cases (was 229; +9 new teardown_failure cases). `make verify` green.

- **Re-introduce a no-hog `setBufferFrameSize` after the strip went too far (2026-04-26).** *Symptom:* with the entire HAL buffer-frame-size mechanism gone (the v10 strip), the project owner's drum-monitoring rig couldn't get below ~14 ms. UA Console doesn't expose a buffer-size control on Apollo; macOS's default for the user's V31 is ~256 frames; nothing on the box was setting either device's buffer below the default, so the latency pill was honestly reporting whatever the devices happened to be at and that wasn't usable. *Empirical disproof of the v10 cascade theory:* Superior Drummer is shown to write `kAudioDevicePropertyBufferFrameSize` on Apollo with *no hog claim* and *not* trigger the `HALS_PlugIn::HostInterface_PropertiesChanged: the object is not valid` cascade that the v10 strip was prophylaxis against. The cascade that crashed Luna was almost certainly hog-mode-eviction-side (Luna's HAL plugin emitting state-changed about objects being torn down as it lost the device), not property-write-side. *Fix:* re-introduce a single backend method `IDeviceBackend::setBufferFrameSize(uid, frames)` that maps to one `AudioObjectSetPropertyData` per device — with no hog claim, no exclusive ownership, no aggregate-driver fan-out (for an aggregate UID it enumerates the active sub-device list and writes to each member directly, the same shape SD uses). macOS resolves the actual buffer with `max-across-clients`, so co-resident apps with a bigger ask keep the device at their value while they run. Re-add `RouteConfig.bufferFrames` (Swift) / `buffer_frames` on `jbox_route_config_t` (ABI v10 → v11 additive MINOR) / `StoredRoute.bufferFrames` (decodeIfPresent, optional). UI restores the per-route Buffer Size picker on the Performance tier with honest copy: *"The Buffer size is a preference: macOS resolves the actual value as the max across all active clients, so another app asking for a bigger buffer will pull the buffer up while it's running."* No `BufferSizePolicy` enum, no global preference, no hog mode, no exclusive lock, no share toggle, no SharingPill, no partial-hog gating, no `claimExclusive` / `releaseExclusive`. ~250 lines of new code (interface method + two impls + property write helper + RouteRecord field + attemptStart wiring on both fast path and mux path + ABI struct field + Swift wrapper threading + persistence field + UI pickers + 4 Catch2 cases + 1 Swift Testing case). *Regression coverage:* `[route_manager][duplex][buffer_frames]` (single device + override, aggregate fan-out, no-override no-write, cross-device both-sides write); `bufferFramesRoundTrip` Swift Testing case pinning persistence + missing-key-decodes-to-nil. *What survives from the strip's reasoning:* still no hog mode, still no share toggle, still no exclusive lock, still no policy enum. The strip removed too much; this re-adds the *single* mechanism Superior Drummer's existence proves is safe.
- **Drop hog mode entirely (2026-04-25).** *Symptom:* every attempt to fix the bugs the v1 monitoring topology surfaced added new control-flow scaffolding around the same one feature — JBox-managed HAL buffer sizes via `kAudioDevicePropertyHogMode` + `kAudioDevicePropertyBufferFrameSize`. The fixes themselves chased each other: gating the buffer-shrink on partial hog (commit `c4557e7`) tripled monitoring latency from ~4.6 ms to ~14 ms; selective fan-out (`f945c39`, `2aabef1`) regressed in subtle aggregate-list-timing edge cases; removing the outer gate (`00a2dd6`) restored the latency but didn't make the underlying mechanism any less fragile. Each commit added more state, more flags, more conditional paths. *Root cause:* the v1 monitoring topology (aggregate device + hardware mixer at the destination) doesn't need JBox-managed buffer sizes at all. The user's interface software (UA Console, RME TotalMix, etc.) already exposes a buffer setting — that's the right control surface, not JBox's hog claim. The hog-mode + buffer-shrink machinery existed for a topology v1 doesn't ship. *Fix:* delete the entire feature. ABI v9 → v10 MAJOR break (no external consumers; both sides of the bridge are owned in this repo). Persistence: nominally breaking, mostly silent — `StoredRoute.shareDevices` and `StoredPreferences.shareDevicesByDefault` are gone from the type, but `JSONDecoder` ignores unknown keys and the surviving fields use `decodeIfPresent` with defaults, so a `state.json` written by a Phase 7.5 build still decodes cleanly into the new shape (the share fields are silently dropped). `currentSchemaVersion` stays at `1`. The four buffer-shrink-fix commits (`c4557e7`, `f945c39`, `2aabef1`, `00a2dd6`) are not preserved in master history — the branch was reset to `23ed419` (alert fix + plan rewrite) before this simplification commit landed. *Regression test:* none specific. Each removal target had its own tests; those are gone too. The remaining 219 Catch2 cases + 155 Swift Testing cases are the safety net for what survived.
- **Pivot from BlackHole detection to self-routing reliability (2026-04-25).** *Symptom:* manual hardware validation on a real aggregate device combining two interfaces with one hardware-mixer-equipped destination showed the v1 multi-source-monitoring use case (live hardware source + media apps both reaching the destination's monitor outs through its hardware mixer) **already worked** with no virtual driver. *Fix:* re-scoped Phase 7.6 from "Virtual output via BlackHole" to fixing the bugs the same hardware test surfaced. *Subsequent fix:* see "Drop hog mode entirely" deviation above.
- **Earlier pivot from in-house HAL plugin to BlackHole (2026-04-23).** *Symptom:* ad-hoc-signed `JboxVirtualDriver.driver` (commits `0aa5cfe`..`8438da3` on the archive branch) installed into `/Library/Audio/Plug-Ins/HAL/` on macOS 26.4.1 fails to load; `coreaudiod` emits `HALS_RemotePlugInRegistrar.mm:418 Throwing Exception: Remote driver service was unable to load plug-in: JboxVirtualDriver.driver`, followed by `xpc_error=[159]`. *Root cause:* macOS 13+'s out-of-process HAL plugin loader enforces a real Apple-issued team identity that ad-hoc signatures cannot satisfy; the project's "no paid Apple Developer Program" constraint rules out the only fix. *Fix:* archived the HAL-plugin path at branch `archive/phase7.6-own-driver`. Initially rebuilt Phase 7.6 on top of BlackHole; subsequent re-scopes (above) dropped that path too.

**Forecast risks for the pending sub-phases (7.6.3 / 7.6.4 / 7.6.5).**

- **Sleep/wake auto-restart UX.** Aggressive auto-restart against a USB interface that hasn't fully re-enumerated post-wake can spin against a still-resetting device. Mitigation: bounded retry count, fall back to WAITING, surface a single (now-non-looping) toast.
- **Aggregate sub-device listeners are noisy during sample-rate cascades.** Debouncing at 200 ms in `DeviceChangeWatcher` should cover the worst-case observed cascade duration.

**Explicitly out of scope for Phase 7.6:**

- Shipping our own HAL plugin or DriverKit DEXT. Archived.
- BlackHole-specific detection or any virtual-output transport awareness. After the simplification, BlackHole works as an ordinary HAL-backed Core Audio device — JBox doesn't need to know.
- Privileged installer helper or `.pkg` distribution. The existing `.dmg` lane is sufficient.
- Re-introducing JBox-managed buffer sizes. The user's interface software is the control surface.

---

## Phase 7.7 — Per-route gain + mixer-strip UI ✅

**Goal.** Add a per-route VCA-style fader, per-channel trims, route-wide and per-channel mute, and a console-style mixer-strip UI with DAW-standard meter scale and driver-published channel labels. See `docs/spec.md` § 2.14 (engine) and § 4.5 (UI).

**Status.** ✅ Complete. ABI v14 (per-route master gain, per-channel trims, mute), `MixerStripColumn` UI, and persistence shipped on `master` (commits including `82924cf`, `6f6e3a4`, `7333d7d`, `00e3c56`).

- [x] T1: `Sources/JboxEngineC/rt/gain_smoother.hpp` — RT-safe block-rate one-pole IIR + 16 Catch2 cases (commit `69f7e6d`).
- [x] T2: `Sources/JboxEngineSwift/FaderTaper.swift` — pure dB ↔ position taper + 28 Swift Testing cases (commit `02be324`).
- [x] T3: `RouteRecord` + C++ `RouteConfig` gain state plumbing (commit `922f071`).
- [x] T4: `addRoute` + `attemptStart` wire dB → linear and configure smoothers (commit `c40413b`).
- [x] T5: `outputIOProcCallback` applies smoothed `master * trim[ch]` + 6 Catch2 cases (commit `a4ab7ac`).
- [x] T6: `duplexIOProcCallback` mirrors the gain wiring + 3 Catch2 cases (commit `e99c8f3`).
- [x] T7: ABI v13 → v14 header bump, fields, setter declarations, history backfill (commit `17a7997`).
- [x] T8: `bridge_api.cpp` setters + Engine / RouteManager pass-throughs + Swift call-site patch + 1 Catch2 case (commit `5e16df6`).
- [x] T9: Swift `Route` model gains `masterGainDb` / `trimDbs` / `muted` (commit `1dbced0`).
- [x] T10: `StoredRoute` persists the new fields additively + 6 Swift Testing cases (commit `8e253f4`).
- [x] T11: `EngineStore.setMasterGainDb` / `setChannelTrimDb` / `setRouteMuted` + Option A non-finite policy + 8 Swift Testing cases (commit `ad51c30`).
- [x] T12: `MeterLevel.dawScaleMarks` + 3 Swift Testing cases (commit `1778abc`).
- [x] T13: `FaderSlider` widget — vertical, dB-bound, console-style cap (commit `f62de39`, polished in `efa01ca`).
- [x] T14: `ChannelStripColumn` widget (commit `a47a9c1`, later folded into `MixerStripColumn` in T18).
- [x] T15: `MasterFaderStrip` widget (commit `12726ba`, later renamed to `VCAFaderStrip` then folded into `MixerStripColumn`).
- [x] T16: `MeterPanel` rewrite as mixer-strip layout (commit `61594b2`, layout iterated through `147a882` → `04617c2` → `a0c40d6` → `816eb52`).
- [x] T17: `make verify` green; manual smoke confirmed by project owner.
- [x] T18: this entry + spec.md § 2.14 / § 4.5 updates.

Phase 7.7 summary of deviations:

- **Task 1 — mute-decay test relaxed.** Plan asserted `< 1e-6` at 50 ms with τ = 10 ms; mathematically impossible (`exp(-5) ≈ 6.7e-3` is the floor for an ideal one-pole). Replaced with `< 1e-2` (-40 dB, perceptually inaudible) at 50 ms plus a strict `< 1e-6` at 200 ms (≈ 20 τ). NaN / ±inf rejection in `step()` added on top of the spec'd surface.
- **Task 4 — defensive `kAtomicMeterMaxChannels` guard in addRoute.** `ChannelMapper::validate()` does not bound mapping size against the `std::array<…, 64>` storage in `RouteRecord`; the new guard prevents an out-of-bounds write on a 65-edge mapping and was placed before the partial-`RouteRecord` allocation in the `addRoute` body.
- **Task 7 — build is red between Task 7 and Task 8 in the Swift wrapper, not in `bridge_api.cpp`.** Plan predicted an undefined-symbol link error; in practice Swift's auto-generated memberwise init for `jbox_route_config_t` requires the new fields at the call site so the failure surfaces earlier in the Swift compile. Task 8's prompt was extended to patch the Swift call site too.
- **Task 11 — Option A non-finite handling.** EngineStore's gain setters reject NaN (no-op) and clamp `-Float.infinity` to `FaderTaper.minFiniteDb` (-60 dB) before storing. Keeps `StateStore`'s default JSONEncoder (which throws on non-finite floats) safe without configuring a `nonConformingFloatEncodingStrategy`. True silence flows through the route-wide MUTE button (`Route.muted`) or the per-channel MUTE button (`Route.channelMuted`) — the trim setter is never the silence path.
- **Manual-smoke iterations (post-T16).** Four polish commits after the initial mixer-strip rewrite: `147a882` fixed an accumulating-translation drag bug + 2.5× sensitivity divisor; `04617c2` unified column layout via flex-height bar zones; `efa01ca` renamed Master → VCA in the UI (engine ABI keeps `master_gain_db`), gave the cap a console-style shape, and dropped the sensitivity divisor for 1:1 cursor tracking; `a0c40d6` added per-channel MUTE for column-height parity; `816eb52` collapsed `ChannelStripColumn` + `VCAFaderStrip` into one `MixerStripColumn` widget AND made per-channel mute a separate `Route.channelMuted: [Bool]` flag (with its own EngineStore setter + StoredRoute persistence + restore-on-launch replay) so toggling mute leaves the trim fader where the user put it — matching the route-wide VCA mute behavior.
- **Per-channel mute is UI-only.** The engine ABI has no per-channel mute field. The Swift wrapper translates a per-channel mute toggle into a `setRouteChannelTrimDb(... -∞)` engine call while preserving `Route.trimDbs[i]` (the user's intended trim) in the model for restore on un-mute. A real engine-side `target_channel_muted[i]` would force a v15 ABI bump; the Swift-wrapper approach gets the same audible result without one.
- **Post-launch panel refinement.** After Phase 7.7 manual smoke, the `MeterPanel` was iterated again to split into two outlined sections (`MeterSectionFrame`: SOURCE on the left, DESTINATION on the right with the VCA pinned far-right), with a per-section dB scale, a horizontal **mirror** layout for `.sourceMeter` strips (meter occupies the slot where `.channel`'s fader sits — opposite-side meters across the panel), **equal-base section widths** (DESTINATION = SOURCE + `vcaSlotWidth`), centered section titles, and a default window size sized for a stereo route (`920×680`). Layout math + dimensions were extracted to `Sources/JboxEngineSwift/MixerLayout.swift` (`MixerStripDimensions`, `MixerPanelLayout`) and pinned by `MixerLayoutTests` (17 cases) so the invariants survive future commits. `docs/spec.md § 4.5` is the authoritative description; design doc § 4.8 carries the narrative.

---

## Phase 8 — Packaging and installation ✅

**Status:** ✅ Complete. Bundling lane + rotating file sink landed; fresh-user smoke test on a clean macOS user account passed (2026-05-01, project owner).

**Goal.** Produce a real distributable `.app` bundle. Make the installation story clear.

**Entry criteria.** Phase 7 complete.

**Exit criteria.**
- `scripts/bundle_app.sh` produces a valid `Jbox.app` that runs when dragged to `/Applications`.
- `scripts/build_release.sh` runs the full build, bundles, ad-hoc-signs, and leaves a ready-to-use `.app`.
- `scripts/package_unsigned_release.sh` produces a `.dmg` containing the app, an `Applications` symlink, an `Uninstall Jbox.command`, and a `READ-THIS-FIRST.txt`. **(Already landed early in `64c6fdd`; leave as-is unless the layout needs revisiting.)**
- Smoke test on a clean macOS user account: unzip → drag to Applications → right-click → Open → Gatekeeper dialog → Open → app launches → microphone permission dialog → grant → app works.

**Tasks.**

Scripts:
- [x] `scripts/bundle_app.sh` — full implementation. Creates `build/Jbox.app/Contents/{MacOS,Resources}`; copies the release-mode `JboxApp` *and* `JboxEngineCLI` binaries into `Contents/MacOS/`; slices `assets/jbox-icon.png` into `Jbox.icns` via `sips` + `iconutil`; emits `Info.plist` (bundle id `dev.sha1n.Jbox`, `LSMinimumSystemVersion=15.0`, `NSMicrophoneUsageDescription`); emits `Jbox.entitlements` with `com.apple.security.device.audio-input`; ad-hoc signs with Hardened Runtime; post-sign greps the entitlement and fails the build if it's missing (Slice A silent-mic guard, see `CLAUDE.md`).
- [x] `scripts/build_release.sh` — convenience wrapper: `swift build -c release` then `bundle_app.sh`.
- [x] `scripts/package_unsigned_release.sh` — builds `JBox-<version>.dmg` containing the `.app`, an `Applications` symlink, an `Uninstall Jbox.command`, and a `READ-THIS-FIRST.txt`. *(Landed early in `64c6fdd` so the release workflow could ship an alpha DMG.)*
- [x] `scripts/run_app.sh` — build + bundle + `open build/Jbox.app`.

Assets:
- [x] `Jbox.icns` is generated at bundle time from `assets/jbox-icon.png` (no committed `.icns`; `bundle_app.sh` slices the source PNG via `sips`/`iconutil`).
- [x] `Info.plist` is rendered inline in `bundle_app.sh` via heredoc (no separate `Info.plist.in` template — single substitution site keeps version/build number/bundle id/microphone usage description discoverable in one file).

Documentation:
- [x] Update `README.md` quick-start section if any install / build steps changed during implementation. *(Reviewed 2026-05-01: quick-start lines 57–150 still match the current scripts/Makefile — no drift to fix. Status section dating from before Phase 7.7 is a separate Phase 9 final-polish item.)*
- [x] `READ-THIS-FIRST.txt` is rendered inline in `package_unsigned_release.sh` (no committed template under `Sources/JboxApp/Resources/`; the user-facing copy is auditable in the script that ships it).

Logging — rotating file sink (completes [spec.md § 2.9](./spec.md#29-rt-safe-logging)):
- [x] New `RotatingFileSink` (`Sources/JboxEngineC/control/rotating_file_sink.{hpp,cpp}`) installed alongside the existing `defaultOsLogSink` via a free `compositeSink(std::vector<Sink>)` helper. Composition lives at the bridge layer (`bridge_api.cpp::jbox_engine_create`) so tests using `createEngineWithBackend` keep the simpler os_log-only setup. Every drained event reaches both destinations.
- [x] Size-based rotation. Defaults: 5 MiB per file, `keep_count=3` (live + 2 rotated). Pre-write rotation: a write that would push the live file over the cap rotates first into `<base>.1.log` / `<base>.2.log` (oldest dropped) and then writes the event into a fresh live file.
- [x] Lazy parent-directory creation on first write (via `std::filesystem::create_directories`). On any I/O failure (permission denied, disk full, rename refused) the sink flips `healthy_=false` and silently no-ops further events; the os_log arm of the composite is unaffected — "fall back to os_log-only" is the published contract.
- [x] On-disk line format: ISO-8601 UTC timestamp + the existing `defaultOsLogSink` body, e.g. `2026-05-01T12:34:56.789Z  jbox evt=route_started route=42 a=2 b=4 ts=12345`. Wall-clock prefix added because file lines have no Console.app sidecar to supply it; the body is identical so file output and `log show` output align grep-by-grep.
- [x] 17 Catch2 cases under `[rotating_file_sink]` / `[composite_sink]` / `[default_jbox_log_path]` / `[log_code_name]`. Tests use per-case `std::filesystem::temp_directory_path()` scratch dirs (no `~/Library/Logs/Jbox/` writes from CI).
- [x] **Per-process file with a shared directory.** `~/Library/Logs/Jbox/Jbox.log` from the .app, `~/Library/Logs/Jbox/JboxEngineCLI.log` from the CLI. The plan's hint was "likely yes (share)"; the deviation below explains why per-process won.

Testing:
- [x] Fresh-user smoke test: create a new macOS user account, download the `.dmg`, follow the `READ-THIS-FIRST.txt`, verify the app runs and a test route works. *(Project-owner pass on 2026-05-01.)*

Phase 8 summary of deviations:

- **Per-process log files over a single shared file (2026-05-01).** *Goal:* close the rotating-file-sink TODO from Phase 6+. *Choices made along the way:*
  - **Per-process file (`<process>.log`) over a shared `jbox.log`.** Plan suggested "likely yes (share)" with "noisier but clearer" as the per-process trade-off. Reasoning that flipped the decision: rotation under `std::filesystem::rename` is racy across processes — two writers hitting the cap simultaneously would step on each other's renames. With per-process files there's no rename contention; the directory itself is shared so the uninstaller's existing `rm -rf $HOME/Library/Logs/JBox` line still removes everything. "Noisier" reduces to "two filenames" since the .app and CLI are rarely run simultaneously by the same user.
  - **Composition lives at the bridge, not inside `LogDrainer`.** `LogDrainer` keeps its single-`Sink` contract; a free `compositeSink(std::vector<Sink>)` builds the multiplexer. Result: no churn in `LogDrainer`'s threading + shutdown-flush invariants, and tests that construct `LogDrainer` directly with a capture sink stay one-line. The file sink is non-copyable (owns an `std::ofstream`); the composite captures it via `std::shared_ptr<RotatingFileSink>` so the resulting `std::function`-shaped sink is copyable.
  - **ISO-8601 UTC prefix on each line.** `defaultOsLogSink`'s line body has no wall-clock — `os_log` infrastructure injects it at view time. A file line lives without that sidecar, so the sink prepends `2026-05-01T12:34:56.789Z  ` (millisecond precision) and keeps the rest of the body identical. "Plain text matching the defaultOsLogSink format" stays true grep-by-grep on the body.
  - **`logCodeName` extracted to `log_drainer.hpp`.** The existing `defaultOsLogSink` had a private switch covering 9 of the 10 log codes — `kLogTeardownFailure = 104` (added in 7.6.3) was missing. Pulling the helper out to a public header consolidates the switch, fixes the missing case, and gives the file sink the same string output without duplicating the table.
  - **Pre-write rotation over post-write rotation.** A write that *would* push the live file over the cap rotates first; the post-rotation live file then takes the event. Cleaner cap honour-ing (live file ≤ cap at rest) than the alternative ("write, then rotate if too big") which leaves the rotated file briefly over-cap. Tests pin: live `liveBytes() <= max_bytes_per_file` after every operation.
  - **`getprogname()` for the per-process basename.** POSIX, no allocation, returns the short program name. The bridge falls back to `"JBox"` if it returns null/empty so the sink never crashes on a degenerate process state.
  - *Diff:* engine impl (~190 LOC across `rotating_file_sink.{hpp,cpp}`) + bridge wiring + tests + docs. 315 Catch2 cases (was 298; +17 new — 9 RotatingFileSink + 3 compositeSink + 2 logCodeName + 3 defaultJboxLogPath). `make verify` green.

---

## Phase 9 — Release hardening and device-level testing

**Status:** 🚧 Partial. **Cut `v0.1.0-alpha` on 2026-05-09 ahead of full Phase 9 completion** — first public pre-1.0 release, see deviations below. CI release pipeline (`release.yml`, `bundle_app.sh`, `package_unsigned_release.sh`) shipped earlier in Phase 8 and is exercised by this tag. Real-hardware acceptance procedures and lint gates remain open and are tracked under `docs/followups.md` § F5–F6 toward `v0.2.0+` and `v1.0.0`.

**Goal.** Prove the release is ready for real use; set up release gates; cut v1.0.0.

**Entry criteria.** Phase 8 complete.

**Exit criteria.**
- All release-gate tests (see [spec.md § 5.6](./spec.md#56-release-gates)) pass.
- `v1.0.0` tag exists; GitHub Releases draft populated with `Jbox-1.0.0.dmg` (ad-hoc signed, not notarized).
- `docs/testing/soak.md`, `docs/testing/latency.md`, `docs/testing/stress.md` document the real-hardware test procedures so future releases are reproducible.

**Tasks.**

Real-hardware tests (documented and run):
- [ ] **Soak test.** Run at least one representative route for ≥ 30 minutes on real hardware. Verify zero dropouts in logs, drift tracker in band, no clipping, meter values plausible. *Procedure drafted in [`docs/testing/soak.md`](./testing/soak.md) on 2026-05-01 — CLI canonical, GUI variant, acceptance criteria, recording template, failure triage. Hardware run is the remaining half of this task.*
- [ ] **Latency measurement.** Loopback test: patch a destination output back into a source input via cable; inject a test pulse; measure round-trip latency. Confirm within ±1 ms of theoretical expectation. Document procedure in `docs/testing/latency.md`.
- [ ] **Stress / disconnect test.** Start and stop routes rapidly. Unplug and replug devices. Verify graceful recovery, no crashes, no stuck routes. Document procedure in `docs/testing/stress.md`.

CI release pipeline:
- [ ] GitHub Actions workflow triggered by tag `v*.*.*`:
  - Full build in release mode.
  - Run all automated tests.
  - Run `build_release.sh` and `package_unsigned_release.sh`.
  - Upload `.dmg` as a draft pre-release asset. **(Already landed in `64c6fdd` / `release.yml`.)**
- [ ] Release checklist in `.github/ISSUE_TEMPLATE/release.md` covering the release-gate items from the spec.

Code quality gates:
- [ ] **`clang-tidy` + `swiftlint` wired into the build + CI.** Promised in the original Phase 1 / Phase 2 task lists and never landed; the gates that did land (`scripts/rt_safety_scan.sh`, ThreadSanitizer, the Catch2 + Swift Testing suites, compiler warnings) cover most of what they would have, but a real lint pass is conventional for a release gate. Sub-tasks:
  - [ ] `bear -- swift build` (or equivalent) wired into a Makefile target so `compile_commands.json` is reproducible on a fresh clone and in CI.
  - [ ] `.clang-tidy` config focused on `Sources/JboxEngineC/control/` (the `rt/` subtree stays under `rt_safety_scan.sh`'s narrower contract). Tuned check set with zero false positives on the current tree before flipping CI to error-level.
  - [ ] `.swiftlint.yml` config covering `Sources/JboxEngineSwift/` and `Sources/JboxApp/`. Tuned rule set with zero false positives.
  - [ ] `make lint` Makefile target invoking both linters.
  - [ ] GitHub Actions step in `ci.yml` that runs `make lint` and fails on warnings.
  - [ ] Any pre-existing findings either fixed or explicitly suppressed at the call site with justification.

Final polish:
- [ ] Review and update all three docs (`README.md`, `spec.md`, `plan.md`) for anything that changed during implementation.
- [ ] Scan all source files for stale TODOs / FIXMEs; address or file follow-up issues.
- [x] Verify `LICENSE` has been decided and committed. **Done 2026-05-08 in `b82e9d1`** — Apache License, Version 2.0.
- [ ] Smoke test the release `.dmg` from GitHub on a fresh user account.

Tagging:
- [x] Tag `v0.1.0-alpha` on `master` — first public pre-1.0 release. **Done 2026-05-09.** GitHub Releases draft populated by `release.yml`; published after manual smoke-test of the CI-built DMG.
- [ ] Tag `v1.0.0` on `master`.
- [ ] Promote the draft release to published on GitHub.

Phase 9 summary of deviations:

- **Cut `v0.1.0-alpha` ahead of full Phase 9 completion (2026-05-09).** Rather than block the first user-visible build on the soak / latency / stress hardware-acceptance pass and the clang-tidy / swiftlint wiring, the first public release ships as an explicit pre-1.0 alpha. Rationale: phases 1–8 are complete (drift-corrected multi-route engine, mixer-strip UI with per-route gain + mute, persistence, launch-at-login, ad-hoc-signed DMG with audio-input entitlement guard); the project owner manually verified routing on real hardware before tagging; and `make verify` is green on the release commit. The two known user-visible limitations — F1 production hot-plug hardware acceptance still pending and F2 sleep/wake recovery not yet implemented — are surfaced verbatim in the GitHub Release body's "Known limitations" section so early users can route around them. Phase 9 items still open: F5 (clang-tidy + swiftlint CI gates), F6 (real-hardware soak / latency / stress procedure runs per `docs/testing/`). When both close, plus F1 hardware acceptance, plus F2 implementation, the gate to `v1.0.0` is met.

---

## After v1.0.0 — deferred work

The items in [spec.md § Appendix A](./spec.md#appendix-a--deferred--out-of-scope) are candidates for future versions. Pick up whichever is most valuable at the time. Likely ordering based on current intuition (subject to revision):

1. **Fan-out** — cheap extension of the v1 model; clearly useful ("send one source to both speakers and headphones").
2. **Developer ID signing + notarization** — if distribution audience grows and the one-time Gatekeeper step becomes a real pain.
3. **Per-route gain / trim** — useful once the mixer boundary is carefully drawn.
4. **Auto-update via Sparkle** — useful once notarization is in place.
5. **Fan-in / summing** — requires explicit design work because it crosses into mixer territory.
6. **Scenes (with sidebar)** — full design preserved at [spec.md § 4.10](./spec.md#410-future-feature--scenes-with-sidebar). Trigger to revisit: a real user workflow that swaps between distinct route configurations often enough to make per-route Start/Stop feel like friction. Nothing is wired up in v1; the slice ships as one coherent change.

   **⚠️ Do NOT skip the brainstorming step.** The project owner has explicitly said they want to rethink the UX before implementation. Before writing any code (no types, no migrations, no views), run a brainstorming session with the project owner — use the `superpowers:brainstorming` skill if available — covering at minimum: concrete user stories that motivate scenes vs. ad-hoc Start/Stop, what happens when a route in an active scene is manually toggled (does the active-scene indicator clear? show "modified"?), can a route belong to multiple scenes and what does that mean for activation, what feedback does the user get when activating a scene with offline devices, the empty-state model, and the upgrade UX for an existing v1 user who suddenly sees a sidebar. Lock the UX *first*, then revise the bullet list below before implementing.

   Rough phase shape *as currently sketched* (assume the brainstorm will rewrite this — exact scope revisited at implementation time):
   - [ ] **Schema bump + migration** — bump `StoredAppState.currentSchemaVersion` from `1` to `2`. Introduce `StoredScene` + `StoredSceneActivationMode` value types and a `scenes: [StoredScene]` field on `StoredAppState`. Add a `migrate_v1_to_v2` entry to `StateStore.load()`'s migration ladder that initialises `scenes: []` for state files written by v1. One Swift Testing migration test pinning the v1 → v2 ladder and Codable round-trips for the new types.
   - [ ] **Application-layer activation engine** — given a `Scene`, compute the set of routes to start / stop and issue the corresponding `Engine.startRoute` / `Engine.stopRoute` commands. Exclusive vs. additive modes (or whatever the brainstorm settles on). Engine remains scene-unaware. Swift Testing cases against a mocked engine pin the activation contract.
   - [ ] **Sidebar UI shell** — re-introduce `NavigationSplitView` in `RouteListView.swift` (or whatever shape the brainstorm settles on). Bump main-window minimum width to accommodate the splitter if the sidebar shape survives.
   - [ ] **Scene editor sheet** — shape TBD post-brainstorm.
   - [ ] **Menu-bar Scene row** — shape TBD post-brainstorm.
   - [ ] **Persistence wiring** — add scene mutations (create / edit / delete / mode change) to `EngineStore.onRoutesChanged` / `AppState` save triggers; spec § 3.2 already lists the trigger format.
   - [ ] **Smoke tests** — SwiftUI `#Preview` blocks for whatever views the brainstorm produces.
7. **Virtual Core Audio device (JBox as a selectable input / output)** — larger post-v1 feature; see [spec.md § 2.15](./spec.md#215-deferred-to-future-versions). Rough phase shape (exact scope revisited at implementation time):
   - [ ] HAL plugin skeleton — a userspace `.driver` bundle that registers a virtual device with configurable channel count and sample rate, and exposes an in-process transport (shared-memory ring or Mach port) for samples. Separate SPM target; ad-hoc signed with its own entitlements.
   - [ ] Engine-side contract — teach `RouteManager` / `DeviceIOMux` to recognise the virtual device UID and bypass the HAL `AudioDeviceIOProc` path, feeding samples directly through an SPSC ring that mirrors the existing ring-buffer primitive. Drift correction is unnecessary for this leg (both ends share the host's clock domain), so the route's PI loop degenerates to a rate-identity passthrough when either endpoint is virtual.
   - [ ] Installer path — the HAL plugin directory (`/Library/Audio/Plug-Ins/HAL/`) is system-owned and requires admin authorization to write. Ship a small privileged helper (SMJobBless or the modern equivalent at the time) invoked from the app on first run; make sure uninstall removes the plugin cleanly.
   - [ ] Packaging — likely a `.pkg` rather than `.dmg` lane, since the HAL plugin has to be copied into a system directory. Reassess the "no paid Apple Developer Program" constraint at this point: ad-hoc-signed HAL plugins may work for local install but probably require user-driven Security & Privacy approval each install, which is a worse UX than the v1 Gatekeeper one-time step. Keep the ad-hoc lane available; offer Developer-ID-signed `.pkg` only if the distribution audience demands it.
   - [ ] Testing strategy — the HAL plugin itself needs to be tested against a real Core Audio host (no easy way to stub the HAL's other side); add a Swift test that opens the virtual device from an `AVAudioEngine` client and verifies round-trip samples via a routed path. C++ tests cover the engine-side virtual-endpoint handling with a simulated backend.
   - [ ] Interaction with the engine: independent. After the Phase 7.6 simplification the engine no longer claims hog mode and only writes `kAudioDevicePropertyBufferFrameSize` per-route when the user opts in via `RouteConfig.bufferFrames` (ABI v11) — the virtual device behaves as an ordinary Core Audio device on JBox's read-only side, and the user's interface software remains the buffer control surface for the real destination.

Each of these gets its own short spec update and its own mini-plan at the time of implementation.
