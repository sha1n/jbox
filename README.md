# Jbox

**A native macOS audio routing utility.** Jbox routes selected channels from one Core Audio device to selected channels on another, in real time, with low latency and robust drift correction between independent clocks.

---

## What Jbox does

Jbox is a **generic Core Audio routing tool**. It bridges any input-capable device to any output-capable device, with:

- **Arbitrary 1:N channel mapping** — pick any source channels, any destination channels, map them in any order. A single source channel may feed multiple destination channels (fan-out); multi-source summing (fan-in) is out of scope.
- **Multiple simultaneous routes** — more like a patchbay than a single-pair bridge.
- **Automatic drift correction** between devices with independent hardware clocks.
- **Automatic sample-rate conversion** when source and destination rates differ.
- **Graceful handling** of device disconnect / reconnect and missing-on-launch cases.
- **Named scenes** to activate groups of routes together.

Unlike macOS Aggregate Devices — the built-in mechanism for combining multiple audio interfaces — Jbox does not create a system-wide composite device. Routes are per-app, isolated, and dynamic. Other apps sharing the same hardware are unaffected.

---

## Who this is for

Anyone on macOS who wants to send audio from one device's outputs into another device's inputs (or virtual inputs), without building a macOS Aggregate Device.

A typical example: routing a hardware instrument or external sound module into specific virtual inputs on an audio interface, so the interface's DSP / console software can apply inserts, sends, or effects to that signal.

---

## What Jbox is not

- **Not a mixer.** No summing, no gain, no mute. 1:N routing only — a source channel can feed many destinations (fan-out), but two sources cannot combine into one destination (fan-in / summing stays out of scope).
- **Not a DAW.** No timeline, no plugins, no recording, no MIDI.
- **Not a virtual audio driver.** Jbox does not ship its own audio device. For the multi-source live-monitoring case (a hardware source + media apps reaching the same physical monitor outs), the recommended topology uses a macOS aggregate device plus the destination interface's hardware mixer — see [docs/spec.md § 2.13](./docs/spec.md#213-multi-source-low-latency-monitoring-topology). Users without a hardware-mixer-equipped interface can substitute a third-party loopback driver such as [BlackHole](https://github.com/ExistentialAudio/BlackHole) for the aggregate; Jbox treats it as an ordinary Core Audio device and needs no driver-specific code.
- **Not a network audio tool.** No Dante, no AVB, no NDI, no IP streaming.

---

## Design principles

1. **Pro-audio routing semantics.** Channel numbering is 1-indexed in the UI (0-indexed internally). Devices are identified by Core Audio UID (stable across reboots). Each route is independent; lifecycle and errors are per-route.
2. **Top-performance real-time engine.** The audio-processing path is written in C++ with strict real-time discipline: no allocations, no locks, no syscalls on the audio thread. Engineered for near-zero added latency.
3. **UI is replaceable.** The engine is an independent C++ library exposed via a stable public C API. Today's SwiftUI UI is one implementation of that API; it can be rewritten or replaced without touching the engine.
4. **Do not step on other apps by default; opt-in only when the user asks for it.** The default latency mode leaves the HAL's buffer size alone and coexists with other clients (Core Audio's max-across-clients policy). The opt-in Performance tier does change the buffer size and — when source and destination are the same device (typically an aggregate) — claims Core Audio hog mode for the route's lifetime, which disconnects other apps from that device until the route stops. The UI copy is explicit about this; users have to pick the tier deliberately.
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
make build      # produce the distributable DMG (Jbox-<version>.dmg)
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

On first launch, macOS prompts for audio-device access. Grant it — Jbox needs this to route audio between any device with a microphone-class designation (which includes every input-capable audio interface).

---

## Debugging and logs

Jbox emits structured log events through the unified logging system (`os_log`). There is no separate log file yet — rotating-file output to `~/Library/Logs/Jbox/` is scheduled for Phase 8 (see [docs/plan.md § Phase 8](./docs/plan.md#phase-8--packaging-and-installation)). In the meantime, everything is visible through the standard `log` command and Console.app.

### Subsystem and categories

All Jbox logging lives under a single subsystem so you can filter with one predicate:

```
subsystem: com.jbox.app
categories: app | engine | ui | bridge
```

- `app` — Swift app entry point (startup, engine init failures).
- `engine` — Swift side of the C bridge (device enumeration, route add / start / stop / remove + errors), and the real-time drainer output (underruns, overruns, channel mismatches, route lifecycle).
- `ui` — reserved for SwiftUI view-layer events (not heavily used yet).
- `bridge` — direct C-side calls around engine lifecycle and `add_route` accept / reject decisions.

### Live stream

Run Jbox in one terminal, then tail its output in another:

```sh
# Everything Jbox emits, live
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
# Last five minutes of Jbox activity
log show --predicate 'subsystem == "com.jbox.app"' --last 5m

# Same, but include info + debug (defaults hide these)
log show --predicate 'subsystem == "com.jbox.app"' --last 5m --info --debug
```

### Enabling info- and debug-level persistence

By default, `info` and `debug` messages are discarded almost immediately even if you request them in `log show`. To make them stick for live investigation:

```sh
# Turn persistence on for Jbox
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
- **Device shows up in one picker but not the other:** Jbox's source list is Core Audio devices with **input streams** (audio flowing hardware → Mac). A device that only appears in the Mac's output direction — because the Mac *sends* to it but cannot *read* from it — is correctly excluded from the source picker. If you expected a device to be a source but don't see it, verify in **Audio MIDI Setup** that it exposes input streams.

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
3. Runs `scripts/package_unsigned_release.sh` to wrap everything into `build/Jbox-<version>.dmg` (drag-to-install `.app` + uninstaller `.command` + README).
4. Creates or updates a GitHub Release for the tag **as a draft, pre-release**, attaching the DMG. Release notes are auto-generated from the commit history since the previous tag.

### Publishing the draft

The release is a draft by design — so you can sanity-check the DMG before it goes public.

1. Go to <https://github.com/sha1n/jbox/releases>.
2. Find the draft release for your tag and click it.
3. Download the attached `Jbox-<version>.dmg`. Optionally mount it and run it through your usual pre-release checks.
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

**Phase 6 — SwiftUI UI, first slice and refinement pass #1 shipped.** Phases 0–5 are code-complete; the C++ engine, Core Audio backend, drift correction + resampling, and the multi-route RCU dispatcher are all in place. The GUI supports add → start → stop → remove routes end-to-end against real Core Audio, with human-readable channel-label pickers in the add-route sheet, live 4 Hz route-status polling, 30 Hz meters, and an Advanced-only diagnostics panel. An `os_log`-based logging pipeline (with edge-triggered RT producers) is wired up under subsystem `com.jbox.app`.

Phase 6 refinements landed:
- **Fan-out mapping** (1:N) — one source channel can feed multiple destinations.
- **Computed per-route latency pill** — engine exposes the HAL + buffer + ring + SRC components through the C ABI; UI surfaces the total on the route row and the breakdown in the diagnostics panel.
- **Tiered latency modes** per route — Off (safe 8× ring, 4096 floor), Low (3× / 512 floor), Performance (2× / 256 floor + `ring/4` drift setpoint). The picker is per-route.
- **Direct-monitor fast path** for same-device Performance routes — bypasses the ring and SRC entirely, copies input → output in one duplex IOProc, aggregate-device aware.
- **HAL buffer-size control** — per-route override clamped into the device's supported range (intersected across aggregate sub-devices), with Core Audio hog-mode ownership for the route's lifetime so the request actually lands.
- **Edit existing routes** — non-disruptive inline rename on double-click (engine-side `jbox_engine_rename_route`, ABI v7), plus a pencil button that opens an edit sheet for full reconfigs (device / mapping / tier / buffer). Running routes are stopped, reconfigured, and restarted as a single action; the apply button reads "Apply and restart" when that will happen.
- **Menu bar extra** — a menu bar icon (a monochrome route glyph derived from the app icon, auto-tinted for light/dark mode) with a small colored status dot in the corner: absent when idle, green when running, red when any route needs attention. Clicking opens a window-style popover with a status summary, per-route Start/Stop toggles, bulk Start All / Stop All, and Open Jbox / Preferences / Quit actions. A scene picker placeholder sits in the layout ready for Phase 7 scene activation.
- **Preferences window (three tabs)** — General / Audio / Advanced. General carries an appearance picker (System / Light / Dark) wired to every scene via `.preferredColorScheme()`. Audio carries a buffer-size policy (seeds the per-route Performance-mode default) and a resampler quality preset (Mastering / High Quality) pushed through the engine via the new ABI v8 setter. Advanced keeps the engine-diagnostics toggle and picks up an Open Logs Folder button. Launch-at-login, menu-bar meters, and Export / Import / Reset Configuration are disabled placeholders that wait on Phase 7 persistence.

Still pending in Phase 6: VoiceOver label on the expanded meter panel, single-window enforcement, `XCUITest` flows, and SwiftUI preview providers. Real-hardware acceptance tests (soak, latency, sample-rate mismatch, multi-route sanity) from Phases 3–5 are deferred to the owner's rig — the simulated backend covers their logic.

Phase 7 persistence slice landed: routes and preferences round-trip through `~/Library/Application Support/Jbox/state.json`. `StateStore` writes atomically with a one-generation `.bak` backup and a 500 ms debounce; `AppState.load()` restores routes on launch with their durable UUIDs, pushes persisted preferences back into `@AppStorage`, and refuses to load state files with a forward schema version. Scene editor + activation logic and launch-at-login are still pending.

Phase 7.5 device-sharing slice landed: a per-route "Share device with other apps" checkbox (plus a matching Preferences default) opts the route out of Jbox's hog-mode policy so Music / Safari / Zoom can keep using a device while a Jbox route is active. Performance tier is unavailable when sharing (the fast path needs exclusivity) and is silently demoted to Low; the route row surfaces the demotion + a lock glyph when hog mode is active. Share-mode routes run at whatever HAL buffer size the device currently has — Jbox does not force a shrink on devices it doesn't exclusively own, because on aggregate devices a shared-client buffer change silently stalls Core Audio's IOProc scheduler. ABI v8 → v9 (`share_device` + `status_flags`). See plan.md Phase 7.5.

Phase 7.6 self-routing reliability is **in progress**. Manual hardware testing on a real aggregate device (live hardware source + media apps both reaching the destination interface's monitor outs through its hardware mixer) confirmed that the v1 multi-source-monitoring topology already works end-to-end *without* a virtual driver — but surfaced two reproducible bugs that must land before shipping. (1) An "Engine error" SwiftUI alert whose dismiss binding was a no-op re-presented on every render until the app was force-quit — fixed in this slice. (2) On aggregates with multiple sub-devices, partial hog-mode acquisition causes the duplex fast path to issue `requestBufferFrameSize` against sub-devices Jbox doesn't actually own, stalling Core Audio's IOProc scheduler so the first start produces silence (a stop/start cycle clears it). (3) HAL property listeners and sleep/wake handling are absent, so a hot-unplugged aggregate sub-device or a lid-close leaves Jbox holding stale device IDs + dangling hog mode that subsequent starts hit as "device busy". Phase 7.6 is re-scoped from the prior BlackHole-detection plan to fixing these reliability gaps; once fixed, BlackHole works as a drop-in substitute for the aggregate device with no Jbox-side driver-specific code. The earlier in-house-driver prototype stays archived on branch `archive/phase7.6-own-driver`. See [docs/plan.md Phase 7.6](./docs/plan.md#phase-76--self-routing-reliability) and [docs/spec.md § 2.13](./docs/spec.md#213-multi-source-low-latency-monitoring-topology).

Release engineering is already operational: pushing a `v*` tag triggers `.github/workflows/release.yml`, which builds an ad-hoc-signed `Jbox.app` (with `JboxEngineCLI` bundled inside) and publishes a drag-to-install `Jbox-<version>.dmg` as a draft pre-release. See [docs/releases.md](./docs/releases.md).

See [docs/plan.md](./docs/plan.md) for the full phased implementation roadmap.

Scope for **v1.0.0** (what the first release will do):

- Route selected channels of any input-capable device to selected channels of any output-capable device (arbitrary 1:N mapping; fan-out allowed, fan-in deferred).
- Multiple simultaneous routes, each independently started / stopped.
- Named scenes to activate groups of routes together.
- Auto-resampling when source and destination sample rates differ.
- Drift correction between independent device clocks.
- Auto-waiting for missing devices; auto-recovery on device return.
- **Tiered latency modes** per route (Off / Low / Performance) with an opt-in HAL buffer-size override and a direct-monitor fast path for same-device (aggregate) routes.
- **Per-route latency pill** plus an Advanced-only diagnostics panel with the full component breakdown.
- SwiftUI main window + menu bar extra + preferences.
- Ad-hoc signed `.app` for personal use; unsigned `.dmg` lane for small-audience sharing.

Explicitly **deferred** beyond v1 (see [docs/spec.md § Appendix A](./docs/spec.md#appendix-a--deferred--out-of-scope) for the full list):

- Fan-in / summing / any mixer features (fan-out shipped in Phase 6).
- Per-route gain, mute, or pan.
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

TBD — to be decided by the project owner before first public release. For private use there is no immediate need.

---

## Contact

Repository owner: `sha1n` (git).
