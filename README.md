# JBox

[![CI](https://github.com/sha1n/jbox/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/sha1n/jbox/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/sha1n/jbox/branch/master/graph/badge.svg)](https://codecov.io/gh/sha1n/jbox)

**A native macOS audio routing utility.** JBox routes selected channels from one Core Audio device to selected channels on another, in real time, with low latency and robust drift correction between independent clocks.

---

## What JBox does

JBox is a **generic Core Audio routing tool**. It bridges any input-capable device to any output-capable device, with:

- **Arbitrary 1:N channel mapping** — pick any source channels, any destination channels, map them in any order. A single source channel may feed multiple destination channels (fan-out); multi-source summing (fan-in) is out of scope.
- **Multiple simultaneous routes** — more like a patchbay than a single-pair bridge.
- **Automatic drift correction** between devices with independent hardware clocks.
- **Automatic sample-rate conversion** when source and destination rates differ.
- **Graceful handling** of device disconnect / reconnect and missing-on-launch cases.

Unlike macOS Aggregate Devices — the built-in mechanism for combining multiple audio interfaces — JBox does not create a system-wide composite device. Routes are per-app, isolated, and dynamic. Other apps sharing the same hardware are unaffected.

---

## Who this is for

Anyone on macOS who wants to send audio from one device's outputs into another device's inputs (or virtual inputs), without building a macOS Aggregate Device.

A typical example: routing a hardware instrument or external sound module into specific virtual inputs on an audio interface, so the interface's DSP / console software can apply inserts, sends, or effects to that signal.

---

## What JBox is not

- **Not a mixer.** No summing, no gain, no mute. 1:N routing only — a source channel can feed many destinations (fan-out), but two sources cannot combine into one destination (fan-in / summing stays out of scope).
- **Not a DAW.** No timeline, no plugins, no recording, no MIDI.
- **Not a virtual audio driver.** JBox does not ship its own audio device. For the multi-source live-monitoring case (a hardware source + media apps reaching the same physical monitor outs), the recommended topology uses a macOS aggregate device plus the destination interface's hardware mixer — see [docs/spec.md § 2.13](./docs/spec.md#213-multi-source-low-latency-monitoring-topology). Users without a hardware-mixer-equipped interface can substitute a third-party loopback driver such as [BlackHole](https://github.com/ExistentialAudio/BlackHole) for the aggregate; JBox treats it as an ordinary Core Audio device and needs no driver-specific code.
- **Not a network audio tool.** No Dante, no AVB, no NDI, no IP streaming.

---

## Design principles

1. **Pro-audio routing semantics.** Channel numbering is 1-indexed in the UI (0-indexed internally). Devices are identified by Core Audio UID (stable across reboots). Each route is independent; lifecycle and errors are per-route.
2. **Top-performance real-time engine.** The audio-processing path is written in C++ with strict real-time discipline: no allocations, no locks, no syscalls on the audio thread. Engineered for near-zero added latency.
3. **UI is replaceable.** The engine is an independent C++ library exposed via a stable public C API. Today's SwiftUI UI is one implementation of that API; it can be rewritten or replaced without touching the engine.
4. **Do not step on other apps.** JBox runs as a shared Core Audio client on every route — it never claims hog mode and never evicts other clients. When a route asks for a smaller HAL buffer than the device is currently running at, JBox writes `kAudioDevicePropertyBufferFrameSize` once per affected device — exactly the way Superior Drummer and other low-latency apps do — and lets the macOS HAL resolve the actual buffer as the **max across all active clients** (so a co-resident DAW asking for a bigger buffer pulls the buffer up while it's running, and JBox's preference takes effect once that DAW lets go). The user can also dial the buffer in their interface software (UA Console, RME TotalMix, Audio MIDI Setup, etc.) and JBox respects whatever it finds. An earlier design tried to also claim hog mode and shrink the buffer aggressively from inside JBox — that path caused IOProc-scheduler stalls on aggregate devices and crashes in co-resident DAWs and was removed (see Phase 7.6 in `docs/plan.md`).
5. **Personal use first.** v1 runs with free Apple tooling and ad-hoc code signing. No paid Apple Developer Program required. Development works entirely from the command line — the Xcode IDE is never required (though Xcode.app must be installed for its frameworks; see Prerequisites). Distribution to others is possible via an unsigned `.dmg` lane with clear Gatekeeper instructions.

---

## Documentation

- **[docs/spec.md](./docs/spec.md)** — the authoritative technical specification. Five sections: system architecture, audio engine internals, data model and persistence, UI design, and testing / build / distribution. Read this for design decisions and their rationale.
- **[docs/plan.md](./docs/plan.md)** — phased implementation plan with concrete tasks. Nine phases from empty repo to v1.0.0 release. Read this to follow or contribute to the implementation; phase statuses are updated as work lands.
- **[docs/releases.md](./docs/releases.md)** — end-to-end release pipeline walk-through: the version synchronization map, the tag-driven flow, and how to redo a bad release.
- **This file** — orientation. Scope, principles, documentation map, quick-start.

---

## Quick-start (for the builder)

### Prerequisites

- macOS 15 (Sequoia) or later.
- **Xcode.app** installed (free — via the App Store, or via a direct `.xip` download from [developer.apple.com/download/all](https://developer.apple.com/download/all/) using a free Apple Developer account — **not** the paid $99/yr Developer Program). Xcode.app provides the `XCTest` and `Testing` frameworks that `swift test` requires; Command Line Tools alone are not sufficient.
- After first install, accept the Xcode license once: open `Xcode.app` and click "Agree," or run `sudo xcodebuild -license`.
- An editor of your choice. **Using the Xcode IDE is never required** — all development happens at the command line via `swift build` / `swift test`. Any editor with Swift LSP support works (VS Code with the Swift extension, Cursor, Nova, Vim with `sourcekit-lsp`, etc.).

### Build

```sh
# Clone
git clone <repo-url> jbox
cd jbox

# Build all targets (engine + app + CLI)
swift build -c release

# Wrap the built app executable into a .app bundle
./scripts/bundle_app.sh

# Run the app
open ./build/Jbox.app
```

The app is ad-hoc signed (`codesign --sign -`) during bundling. macOS will run it on your own Mac without Gatekeeper warnings once you approve access on first launch.

### Run the engine CLI (headless)

Useful for testing the engine without the GUI:

```sh
swift run JboxEngineCLI --list-devices
swift run JboxEngineCLI --route <src-uid>:<src-channels> -> <dst-uid>:<dst-channels>
```

See `Sources/JboxEngineCLI/main.swift` for full options.

### Make targets (recommended)

Common commands are wrapped in a [`Makefile`](./Makefile) at the repo root:

```sh
make            # show all targets (default)
make clean      # wipe .build/ build/ test-results/ .swiftpm/
make build      # produce the distributable DMG (JBox-<version>.dmg)
make test       # run the full test pipeline (same as CI)
make all        # clean + build + test

# Fine-grained:
make app        # build Jbox.app only (no DMG)
make run        # build, bundle, and launch
make cli        # build just JboxEngineCLI
make swift-test # Swift tests only (fast iteration)
make cxx-test   # C++ tests only (with per-test timings)
make rt-scan    # RT-safety static scanner only
```

The version used for `make build` defaults to `git describe`; override with `make VERSION=X.Y.Z build`. All output lands in git-ignored directories (`.build/`, `build/`, `test-results/`).

### Direct commands

Behind the Make targets, the canonical script is [`scripts/verify.sh`](./scripts/verify.sh) — the "is my tree green?" gate that mirrors CI exactly:

1. RT-safety static scan on `Sources/JboxEngineC/rt/`
2. Release build of all SPM targets
3. Swift-side tests (Swift Testing)
4. C++ engine tests via Catch2, with per-test timings
5. C++ engine tests under ThreadSanitizer

For targeted runs:

```sh
swift test                                       # Swift Testing tests
swift run JboxEngineCxxTests --durations yes     # C++ tests with timings
swift run --sanitize=thread JboxEngineCxxTests   # C++ tests under TSan
./scripts/rt_safety_scan.sh                      # static RT-safety check
```

Additional Catch2 flags for targeted runs:

```sh
swift run JboxEngineCxxTests --list-tests        # enumerate without running
swift run JboxEngineCxxTests '[ring_buffer]'     # run one tag only
swift run JboxEngineCxxTests --reporter compact  # one line per test
```

Device-level integration tests (soak, latency measurement) require real hardware and are documented in [docs/plan.md § Phase 9](./docs/plan.md#phase-9--release-hardening-and-device-level-testing).

### First launch

On first launch, macOS prompts for audio-device access. Grant it — JBox needs this to route audio between any device with a microphone-class designation (which includes every input-capable audio interface).

---

## Debugging and logs

JBox emits structured log events through the unified logging system (`os_log`) and additionally appends every event to a rotating per-process file under `~/Library/Logs/Jbox/` (`Jbox.log` for the .app, `JboxEngineCLI.log` for the headless CLI). The file is size-rotated at 5 MiB with up to 3 files retained (live + 2 rotated), and falls back to os_log-only on I/O errors (permission denied, disk full) rather than dropping the whole pipeline. The `log` command and Console.app still show everything in real time.

### Subsystem and categories

All JBox logging lives under a single subsystem so you can filter with one predicate:

```
subsystem: com.jbox.app
categories: app | engine | ui | bridge
```

- `app` — Swift app entry point (startup, engine init failures).
- `engine` — Swift side of the C bridge (device enumeration, route add / start / stop / remove + errors), and the real-time drainer output (underruns, overruns, channel mismatches, route lifecycle).
- `ui` — reserved for SwiftUI view-layer events (not heavily used yet).
- `bridge` — direct C-side calls around engine lifecycle and `add_route` accept / reject decisions.

### Live stream

Run JBox in one terminal, then tail its output in another:

```sh
# Everything JBox emits, live
log stream --predicate 'subsystem == "com.jbox.app"'

# Just the RT / engine channel
log stream --predicate 'subsystem == "com.jbox.app" AND category == "engine"'

# Include info-level messages too (default: notice and above)
log stream --level info --predicate 'subsystem == "com.jbox.app"'

# Include debug-level as well
log stream --level debug --predicate 'subsystem == "com.jbox.app"'
```

### Post-hoc queries

```sh
# Last five minutes of JBox activity
log show --predicate 'subsystem == "com.jbox.app"' --last 5m

# Same, but include info + debug (defaults hide these)
log show --predicate 'subsystem == "com.jbox.app"' --last 5m --info --debug
```

### Enabling info- and debug-level persistence

By default, `info` and `debug` messages are discarded almost immediately even if you request them in `log show`. To make them stick for live investigation:

```sh
# Turn persistence on for JBox
sudo log config --subsystem com.jbox.app --mode "level:debug,persist:debug"

# Verify
sudo log config --status --subsystem com.jbox.app

# Turn it back off when you're done (recommended — debug-level is chatty)
sudo log config --subsystem com.jbox.app --reset
```

### Event taxonomy

Real-time events produced by the engine carry a compact numeric payload:

| Event              | Payload (`value_a`, `value_b`)                              |
|--------------------|-------------------------------------------------------------|
| `underrun`         | `(total_underruns, 0)`                                      |
| `overrun`          | `(total_overruns, 0)`                                       |
| `channel_mismatch` | `(expected_channels, actual_channels)`                      |
| `route_started`    | `(source_channel_count, dest_channel_count)`                |
| `route_waiting`    | `(source_missing_flag, dest_missing_flag)`                  |
| `route_stopped`    | `(0, 0)`                                                    |
| `route_error`      | `(jbox_error_code, 0)`                                      |

Underrun / overrun / channel-mismatch events are **edge-triggered** — only the first occurrence after each (re)start is logged, to keep the queue sparse under sustained trouble. The running totals remain visible through `jbox_engine_poll_route_status`.

### Troubleshooting

- **App appears to start but no routes work:** check the log for `engine` events. Startup logs `engine created abi=<N>` from the `bridge` category; absence means the bundle is failing to load or crashing before engine-create.
- **Route stays in `waiting`:** look for `evt=route_waiting` — `value_a=1` means the source device UID was not found at start time, `value_b=1` means the destination was missing. Refresh devices (or reconnect) and retry.
- **Route goes silent intermittently:** look for `evt=underrun` or `evt=overrun`. The first occurrence after a start will log; stop and re-start the route to re-arm the edge trigger if you want to confirm the problem is still occurring.
- **Device shows up in one picker but not the other:** JBox's source list is Core Audio devices with **input streams** (audio flowing hardware → Mac). A device that only appears in the Mac's output direction — because the Mac *sends* to it but cannot *read* from it — is correctly excluded from the source picker. If you expected a device to be a source but don't see it, verify in **Audio MIDI Setup** that it exposes input streams.

---

## Releases

For a complete walk-through — including every place the version number appears, how they stay in sync, and troubleshooting — see [docs/releases.md](./docs/releases.md).

### Cutting a release

All releases are driven by pushing a Git tag. CI handles everything else.

**One-time setup per tag:**

```sh
# 1. Make sure master is green and you're up-to-date.
git checkout master
git pull
./scripts/verify.sh            # optional but recommended

# 2. Create an annotated tag. The tag name must start with 'v'.
git tag -a v0.0.1-alpha -m "v0.0.1-alpha: engine + CLI pre-release"

# 3. Push the tag.
git push origin v0.0.1-alpha
```

Pushing a `v*` tag triggers [`.github/workflows/release.yml`](.github/workflows/release.yml). On the macOS runner it:

1. Builds release mode (`swift build -c release`).
2. Runs `scripts/bundle_app.sh` to produce `build/Jbox.app` (ad-hoc signed, Hardened Runtime on, CLI bundled inside at `Contents/MacOS/JboxEngineCLI`).
3. Runs `scripts/package_unsigned_release.sh` to wrap everything into `build/JBox-<version>.dmg` (drag-to-install `.app` + uninstaller `.command` + README).
4. Creates or updates a GitHub Release for the tag **as a draft, pre-release**, attaching the DMG. Release notes are auto-generated from the commit history since the previous tag.

### Publishing the draft

The release is a draft by design — so you can sanity-check the DMG before it goes public.

1. Go to <https://github.com/sha1n/jbox/releases>.
2. Find the draft release for your tag and click it.
3. Download the attached `JBox-<version>.dmg`. Optionally mount it and run it through your usual pre-release checks.
4. Edit the release notes if you want to add anything beyond the auto-generated list.
5. Click **Publish release** (or **Save draft** to keep it unpublished).

### If something goes wrong

If the build fails or you want to redo a tag:

```sh
# Delete the local tag.
git tag -d v0.0.1-alpha

# Delete the remote tag.
git push origin :refs/tags/v0.0.1-alpha

# Also delete the draft release on GitHub (Releases page → Delete).

# Fix the issue, then re-tag and re-push.
git tag -a v0.0.1-alpha -m "v0.0.1-alpha: engine + CLI pre-release"
git push origin v0.0.1-alpha
```

Be careful with retagging a tag that's already been published — consumers may have pulled it. Use new tags for public reruns.

### Local dry-run

You can produce the DMG exactly as CI would, locally:

```sh
JBOX_VERSION=0.0.1-alpha ./scripts/build_release.sh
JBOX_VERSION=0.0.1-alpha ./scripts/package_unsigned_release.sh
# → build/Jbox-0.0.1-alpha.dmg
```

No tag, no push, no GitHub Release — just the DMG on disk for you to inspect.

### Versioning scheme

- `v0.x.y[-label]` — development pre-releases. The GUI app is a placeholder; the CLI is the useful part. Label examples: `alpha`, `beta`.
- `v1.0.0` — first stable release, post-Phase-6, with the real SwiftUI UI.
- Patch tags like `v1.0.1` are plain bugfixes; minor like `v1.1.0` are feature additions.

### Recipient experience

The DMG is **ad-hoc signed, not notarized** (no paid Apple Developer Program yet). Recipients on other Macs will hit a one-time Gatekeeper warning and need to right-click → Open the `.app` on first launch. The bundled `READ-THIS-FIRST.txt` walks them through it.

If that friction ever matters enough to solve, the next step is joining the paid Apple Developer Program ($99/year) and adding Developer ID signing + notarization to the release pipeline. No code changes required; just CI secrets and a `codesign` identity.

---

## Status

**Phase 6 — SwiftUI UI, all in-scope items shipped.** Phases 0–5 are code-complete; the C++ engine, Core Audio backend, drift correction + resampling, and the multi-route RCU dispatcher are all in place. The GUI supports add → start → stop → remove routes end-to-end against real Core Audio, with human-readable channel-label pickers in the add-route sheet, live 4 Hz route-status polling, 30 Hz meters, and an Advanced-only diagnostics panel. An `os_log`-based logging pipeline (with edge-triggered RT producers) is wired up under subsystem `com.jbox.app`.

Phase 6 refinements landed:
- **Fan-out mapping** (1:N) — one source channel can feed multiple destinations.
- **Computed per-route latency pill** — engine exposes the HAL + buffer + ring + SRC components through the C ABI; UI surfaces the total on the route row and the breakdown in the diagnostics panel.
- **Tiered latency modes** per route — Off (safe 8× ring, 4096 floor), Low (3× / 512 floor), Performance (2× / 256 floor + `ring/4` drift setpoint). The picker is per-route.
- **Direct-monitor fast path** for same-device Performance routes — bypasses the ring and SRC entirely, copies input → output in one duplex IOProc, aggregate-device aware.
- **HAL buffer size** — JBox respects whatever buffer the user has configured in their interface software; the route's latency pill reflects the honest end-to-end estimate. Tier presets pick ring sizing, not HAL buffer.
- **Edit existing routes** — non-disruptive inline rename on double-click (engine-side `jbox_engine_rename_route`, ABI v7), plus a pencil button that opens an edit sheet for full reconfigs (device / mapping / tier / buffer). Running routes are stopped, reconfigured, and restarted as a single action; the apply button reads "Apply and restart" when that will happen.
- **Menu bar extra** — a menu bar icon (a monochrome route glyph derived from the app icon, auto-tinted for light/dark mode) with a small colored status dot in the corner: absent when idle, green when running, red when any route needs attention. Clicking opens a window-style popover with a status summary, per-route Start/Stop toggles, bulk Start All / Stop All, and Open JBox / Preferences / Quit actions.
- **Preferences window (three tabs)** — General / Audio / Advanced. General carries an appearance picker (System / Light / Dark) wired to every scene via `.preferredColorScheme()` and a Launch-at-login toggle wired to `SMAppService.mainApp` (with a one-time explanatory note on first enable, and an "Open Login Items…" deep link when macOS marks the registration as awaiting approval). Audio carries a resampler quality preset (Mastering / High Quality) pushed through the engine via the ABI v8 setter, plus an informational footer reminding the user that the HAL buffer size is set in their interface software (UA Console / RME TotalMix / MOTU CueMix / Audio MIDI Setup). Advanced keeps the engine-diagnostics toggle and picks up an Open Logs Folder button. Menu-bar meters and Export / Import / Reset Configuration remain disabled placeholders.

XCUITest event-injection flows are **deferred under the SPM-only constraint** — see [docs/plan.md Phase 6 deviations](./docs/plan.md#phase-6--swiftui-ui) for the gap write-up and the recommended path when revisited. Real-hardware acceptance tests (soak, latency, sample-rate mismatch, multi-route sanity) from Phases 3–5 are deferred to the owner's rig — the simulated backend covers their logic.

Phase 7 landed in two slices. Persistence: routes and preferences round-trip through `~/Library/Application Support/Jbox/state.json`. `StateStore` writes atomically with a one-generation `.bak` backup and a 500 ms debounce; `AppState.load()` restores routes on launch with their durable UUIDs, pushes persisted preferences back into `@AppStorage`, and refuses to load state files with a forward schema version. Launch-at-login: the General Preferences tab's toggle now drives `SMAppService.mainApp.register()` / `.unregister()` through a `LaunchAtLoginController` that owns the in-memory state, surfaces a `requiresApproval` callout when macOS asks the user to confirm in System Settings, persists a `hasShownLaunchAtLoginNote` latch so the first-time explanation alert fires only once across the user's lifetime, and reconciles with the live system status on every launch (so an out-of-band toggle in System Settings is picked up). Scenes (and the sidebar shell that hosted them) have been **deferred to a future release** — full design preserved at [docs/spec.md § 4.10](./docs/spec.md#410-future-feature--scenes-with-sidebar); v1 carries no scene scaffolding, the feature returns as a `v1 → v2` schema migration when it lands.

Phase 7.5 (device-sharing opt-out) landed and was then **superseded and reverted by Phase 7.6** — the entire hog-mode + buffer-shrink machinery the Phase 7.5 toggle opted out of has been removed from the engine. Share is now the only mode and the toggle is gone.

Phase 7.6 (self-routing reliability) is **in progress**. The big simplification — drop hog mode and exclusive locking entirely — landed and removes ~400 lines of engine code plus the Phase 7.5 share-toggle ABI / UI / persistence surface (ABI v9 → v10 MAJOR break). A targeted walk-back then re-introduced a single no-hog `setBufferFrameSize` write per affected device, modeled on the way Superior Drummer requests low-latency buffers without disturbing co-resident apps; the user-visible Buffer Size picker on the Performance tier is back, and macOS resolves the effective buffer as the max across all active clients (ABI v10 → v11 MINOR additive). The pending follow-ups are device hot-plug listeners (sub-phase 7.6.4), sleep/wake handling (7.6.5), and return-code-aware teardown (7.6.3). See [docs/plan.md Phase 7.6](./docs/plan.md#phase-76--self-routing-reliability) and [docs/spec.md § 2.13](./docs/spec.md#213-multi-source-low-latency-monitoring-topology). The earlier in-house-driver prototype stays archived on branch `archive/phase7.6-own-driver`.

Release engineering is already operational: pushing a `v*` tag triggers `.github/workflows/release.yml`, which builds an ad-hoc-signed `Jbox.app` (with `JboxEngineCLI` bundled inside) and publishes a drag-to-install `JBox-<version>.dmg` as a draft pre-release. See [docs/releases.md](./docs/releases.md).

See [docs/plan.md](./docs/plan.md) for the full phased implementation roadmap.

Scope for **v1.0.0** (what the first release will do):

- Route selected channels of any input-capable device to selected channels of any output-capable device (arbitrary 1:N mapping; fan-out allowed, fan-in deferred).
- Multiple simultaneous routes, each independently started / stopped.
- Auto-resampling when source and destination sample rates differ.
- Drift correction between independent device clocks.
- Auto-waiting for missing devices; auto-recovery on device return.
- **Tiered latency modes** per route (Off / Low / Performance) — ring-sizing presets that govern drift-sampler residency; a direct-monitor fast path bypasses the ring + converter entirely for same-device (aggregate) Performance routes. On Performance, the user can also pick a per-route Buffer Size *preference* — JBox writes it once via `kAudioDevicePropertyBufferFrameSize` (no hog mode), and macOS resolves the actual buffer as the max across all active clients.
- **Per-route latency pill** plus an Advanced-only diagnostics panel with the full component breakdown.
- SwiftUI main window + menu bar extra + preferences.
- Ad-hoc signed `.app` for personal use; unsigned `.dmg` lane for small-audience sharing.

Explicitly **deferred** beyond v1 (see [docs/spec.md § Appendix A](./docs/spec.md#appendix-a--deferred--out-of-scope) for the full list):

- Fan-in / summing / any mixer features (fan-out shipped in Phase 6).
- Per-route gain, mute, or pan.
- Scenes (named presets that activate groups of routes together) — design preserved at [docs/spec.md § 4.10](./docs/spec.md#410-future-feature--scenes-with-sidebar).
- Global hotkeys.
- Developer ID signing + notarization.
- Mac App Store (architecturally ruled out).
- Sparkle auto-update.
- Localization.
- Network audio / MIDI routing.

---

## Working on these docs

These documents are the living source of truth. As design refinements happen during implementation, updates belong here — not in commit messages, not in issue threads. Commit doc changes together with the code changes that motivate them.

The conversational history behind the v1 design is summarized in [docs/spec.md](./docs/spec.md); when future design discussions lead to changes, update the relevant section and include a brief changelog entry at the top of the affected document (or, once the project has real history, in a dedicated `CHANGELOG.md`).

---

## License

Licensed under the [Apache License, Version 2.0](./LICENSE). Copyright © 2026 Shai Nagar.

You may use, modify, and redistribute this software under the terms of that license. The full text is in [`LICENSE`](./LICENSE) at the repository root.

---

## Contact

Repository owner: `sha1n` (git).
