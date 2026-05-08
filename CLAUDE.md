# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

JBox is a macOS-only Core Audio routing utility. C++ engine + C ABI + Swift wrapper + SwiftUI app. See `README.md` for scope and principles, `docs/spec.md` for design, `docs/plan.md` for phased implementation state and deviations.

Two companion docs sit alongside `plan.md` and an agent should know they exist:

- `docs/followups.md` — pending implementation work that's been deferred from the main path. Hardware-gated production wiring (real `CoreAudioBackend` HAL listeners, real `MacosPowerEventSource`) and non-critical perf refinements live here. Each entry carries Problem / What-to-do / Research-needed / Pitfalls / Acceptance / References. Read this *before* picking up any "deferred follow-up" item referenced from `plan.md`.
- `docs/refactoring-backlog.md` — refactoring of existing working code that we want to do but have deliberately not folded into feature work. Currently carries the `jbox_error_code_t` / `last_error` naming smell (R1) and the `@Observable` × subscript-write asymmetry watch-item (R3). R2 (live-Core-Audio test-skip pattern) is resolved as of 2026-05-08; entry retained for history. Read this *before* renaming anything in those families, adding a new variant to `jbox_error_code_t`, or adding a new periodic tick that mutates `@Observable` state via subscript paths in `EngineStore`.

When you defer something during feature work, file it in the right doc instead of leaving a TODO comment in code or a one-line mention in `plan.md`. The depth — open questions, pitfalls, acceptance plan — is the point.

## Tooling constraints (hard)

- **Swift Package Manager only** — there is no `.xcodeproj`. Do not propose Xcode IDE workflows. `Package.swift` is the single manifest.
- **Xcode.app must be installed** (for `XCTest` and `Testing` frameworks that `swift test` needs) but the IDE is never used.
- **No paid Apple Developer Program.** Binaries are **ad-hoc signed** (`codesign --sign -`) with Hardened Runtime. Do not add Developer ID signing or notarization without explicit go-ahead.
- **macOS 15+** (`.macOS(.v15)` in `Package.swift`).
- **Audio-input entitlement is mandatory.** `scripts/bundle_app.sh` emits `Jbox.entitlements` claiming `com.apple.security.device.audio-input` and has a post-sign grep guard. Under Hardened Runtime, missing this entitlement silently silences every mic IOProc (no log, no error) — it is the root cause of the Slice A silent-mic bug. Never strip the guard.

## Device & HAL ownership policy (hard)

Phase 7.6 ripped out hog mode + aggressive buffer-shrink after they stalled IOProc scheduling on aggregates and crashed a co-resident DAW. Re-introducing any of the patterns below requires explicit per-operation user opt-in, recorded as a new `docs/plan.md § Phase 7.6` deviation. "It seemed safe" / "it was easy" / "another fix needed it" are not sufficient — the cascade was hog-eviction-side and we no longer ship the path that triggers it.

- **No `kAudioDevicePropertyHogMode`.** No hog claim, no hog release, no `claimExclusive` / `releaseExclusive` on `IDeviceBackend`. The interface and both backends were stripped of these symbols intentionally.
- **The only HAL property write JBox issues is the v11 per-route `IDeviceBackend::setBufferFrameSize(uid, frames)`.** Fired once per device at `RouteManager::attemptStart`, gated on a non-`nil` / non-zero `RouteConfig.bufferFrames`, no hog claim, no exclusive ownership. For aggregate UIDs the call walks `kAudioAggregateDevicePropertyActiveSubDeviceList` and writes to each member directly. macOS resolves `max-across-clients`; the latency pill reports the resolved value, never the preference. See `docs/spec.md § 2.7` ("Per-route HAL buffer-frame-size preference").
- **Do not add other HAL property writes** — sample rate, IOMode, default device, stream format, hog, anything. Reading is fine. If a feature appears to require a new write, surface the trade-off to the user first; Preamble Core Design Principle #4 in `docs/spec.md` ("Do not step on other apps") is the contract.
- **`DeviceIOMux` is an IOProc multiplexer, not a buffer coordinator.** `attachInput` / `attachOutput` take `(key, callback, user_data)`. The fields `non_sharing_attached_`, `last_requested_frames_`, `exclusive_claimed_`, `requested_buffer_frames`, `updateBufferRequest`, `currentMinBufferRequest` are gone and stay gone.
- **Do not re-introduce a "share device" toggle, `BufferSizePolicy` enum, "Routing defaults" preference, or `SharingPill` view.** All four fronted hog-mode behaviour. JBox is share-only by construction.
- **Reactions to device hot-plug (sub-phase 7.6.4) and sleep/wake (sub-phase 7.6.5) never claim hog or fan out buffer writes.** The reaction is: stop affected routes, transition to WAITING, retry on reappearance / wake. That is sufficient.

## Commands

Always prefer `make` over invoking scripts or `swift` directly — the Makefile wraps the canonical pipeline.

```sh
make                 # list targets
make verify          # full pipeline (same as CI): RT-scan + Release build + Swift tests + C++ tests + TSan
make swift-test      # Swift Testing only
make cxx-test        # C++ tests only, with per-test durations
make cxx-test-tsan   # C++ tests under ThreadSanitizer
make rt-scan         # RT-safety static scanner on Sources/JboxEngineC/rt/
make app             # build Jbox.app (no DMG)
make run             # build + bundle + open build/Jbox.app
make dmg             # build the distributable DMG
make clean           # wipe .build/ build/ test-results/ .swiftpm/
```

Targeted C++ test runs:
```sh
swift run JboxEngineCxxTests '[ring_buffer]'      # run one Catch2 tag
swift run JboxEngineCxxTests --list-tests         # enumerate
swift run JboxEngineCxxTests 'Specific test name' # single case by name
```

Targeted Swift test runs:
```sh
swift test --filter MeterLevelTests               # one suite
swift test --filter "MeterLevelTests/zero"        # one case
```

CLI (headless engine): `swift run JboxEngineCLI --list-devices` / `--route <src>@1,2-><dst>@5,6`.

## Architecture — the big picture

Four layers, each with its own thread-safety contract:

1. **`Sources/JboxEngineC/rt/`** — RT-safe primitives. Used from Core Audio IOProc callbacks. **No allocations, no locks, no syscalls, no STL containers that allocate, no exceptions.** `scripts/rt_safety_scan.sh` statically greps for banned symbols; it is a CI gate. Contents: `ring_buffer.hpp` (lock-free SPSC), `atomic_meter.hpp`, `rt_log_queue.hpp`, `audio_converter_wrapper.{hpp,cpp}`, `rt_log_codes.hpp`.
2. **`Sources/JboxEngineC/control/`** — control-thread C++. Owns lifecycle: route add/start/stop/remove, device enumeration, drift sampling, log draining. Can allocate. Reaches into `rt/` for the RT primitives; the RT primitives never reach up here.
3. **`Sources/JboxEngineC/include/jbox_engine.h`** — the stable **public C ABI**. `JBOX_ENGINE_ABI_VERSION` is a `uint32_t`. Additive symbols may land with a MINOR bump; signatures of existing entries are frozen. See `docs/spec.md §§ 3.x` for the ABI contract.
4. **`Sources/JboxEngineSwift/`** — Swift wrapper over the C ABI (`JboxEngine`, `EngineStore`, `MeterPeaks`, `MeterLevel`, `PeakHoldTracker`, `JboxLog`). `@MainActor`-isolated. Plus `Sources/JboxApp/` (SwiftUI) and `Sources/JboxEngineCLI/` (headless CLI).

**Threading cheatsheet** — main thread reads atomics and writes commands; Core Audio IOProc threads (one per active device) do the RT work; a background control-thread `DispatchQueue` does allocations, device enumeration, drift sampling at ~100 Hz. See `docs/spec.md § 1.3`.

**Multi-route device sharing** uses `DeviceIOMux` (RCU-style active-route list) so multiple routes can share an IOProc on the same device — a single HAL callback fans out to per-route state.

**Drift correction** happens on the control thread (`DriftSampler` at 100 Hz) → atomic `target_input_rate` → RT thread applies via `AudioConverterWrapper::setInputRate`, gated by `shouldApplyRate` in `rate_deadband.hpp` (1 ppm threshold). The deadband is load-bearing: every `setInputRate` call flushes Apple's polyphase filter and costs ~16 extra input frames — without the deadband, sub-ppm PI noise caused audible clicks on real hardware.

## Conventions that matter

- **TDD is the norm.** Pure logic is covered by tests before implementation. See `MeterLevelTests.swift`, `PeakHoldTrackerTests.swift`, `rate_deadband_test.cpp` for the pattern. The `superpowers:test-driven-development` and `superpowers:systematic-debugging` skills describe the exact discipline.
- **Docs are the living source of truth.** Commit doc changes together with the code that motivates them — not in commit messages alone. `docs/spec.md` is the authoritative design; `docs/plan.md` tracks phase status with per-phase "deviations" sections where ad-hoc decisions are recorded.
- **Memorable fix patterns live in `docs/plan.md` Phase deviations**, not in commit messages. New deviations belong there.
- **C++ tests use Catch2 v3 (amalgamated, vendored in `ThirdParty/Catch2/`)** — not GoogleTest. Swift tests use **Swift Testing** (`@Suite`, `@Test`, `#expect`) — not XCTest.
- **`os_log` subsystem is `com.jbox.app`** with categories `app | engine | ui | bridge`. RT producers push numeric event codes into `RtLogQueue`; `LogDrainer` forwards each drained event to a composite sink — the `os_log` subsystem AND a per-process rotating file at `~/Library/Logs/Jbox/<process>.log` (`Jbox.log` for the .app, `JboxEngineCLI.log` for the headless CLI), 5 MiB × 3 size-rotated, fail-silent on file-side I/O errors so the os_log destination keeps running. Composition is wired at the bridge layer (`bridge_api.cpp::jbox_engine_create`); `LogDrainer` itself keeps its single-`Sink` contract.
- **Never `git push --force` to `master`.** Destructive git operations require explicit user approval (see the root system prompt).

## Pre-commit checklist

Before asking the user to review, the assistant must confirm each of the following. Treat unchecked items as blockers, not suggestions.

1. **`make verify` is green.** 184+ C++ cases, full Swift suite, RT-safety scan clean, TSan clean. A single failure means do not propose committing.
2. **New / changed behavior has a test.**
   - Pure logic (C++ or Swift) → unit test in the corresponding `Tests/...` suite, written **before** the implementation where the TDD skill applies.
   - Bug fixes → a regression test that would have caught the bug. Reference the symptom and the root cause in the test's top-of-file comment, not just "fixes X".
   - UI-only changes that can't be unit-tested → say so explicitly in the commit message and rely on the user's manual pass.
3. **Test review.** Re-read the newly added / modified tests end-to-end. Check:
   - Test names describe **behavior**, not implementation.
   - Assertions exercise the real public contract; no test-only methods leaked into production types.
   - No accidental "tautology" tests that pass trivially against the stub (run them once against a stub to confirm they actually fail).
   - Edge cases: zero, negative, empty, unknown id, boundary values, concurrent / multi-instance independence where the state is per-key.
4. **Documentation review.**
   - `docs/plan.md` — the current phase's checklist is up to date for any task that crossed `[ ]` → `[x]`. New "deviations" are recorded in the phase's deviations section with rationale + commit hash.
   - `docs/spec.md` — any contract that changed (ABI signatures, RT invariants, spec § references cited by tests or code) reflects the new behavior.
   - `docs/followups.md` — when you defer concrete implementation work (hardware-gated, off-main-path, or research-needed), add an `F<n>` entry rather than leaving a TODO. Update an existing entry when its status shifts (e.g., research surfaces an answer, blocker clears).
   - `docs/refactoring-backlog.md` — when you spot a smell that's bigger than the current slice can absorb, add an `R<n>` entry instead of folding a hasty rename into the feature work.
   - `README.md` — only if user-facing commands / prerequisites / status changed.
   - `CLAUDE.md` (this file) — only if a non-obvious constraint changed that a future Claude instance needs to know.
5. **Commit message mentions the "why", not just the "what".** Match the style of recent commits (`git log --oneline -10`). Include a line about test coverage added, and flag any deferred follow-up.
6. **Destructive / user-visible actions are opt-in.** Do not `git push`, `git tag`, create releases, delete branches, or force-update remotes unless the user explicitly authorizes that specific action in the current turn.

Treat the pre-commit checklist as a blocking stop sign: run through it even when the change looks trivial.
