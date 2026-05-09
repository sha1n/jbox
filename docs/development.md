# JBox — Development guide

This document is for anyone with the repo cloned who wants to build, test, debug, or contribute to JBox. The user-facing landing page is [`README.md`](../README.md); the design spec is [`spec.md`](./spec.md); the implementation roadmap is [`plan.md`](./plan.md).

---

## Prerequisites

- macOS 15 (Sequoia) or later.
- **Xcode.app** installed (free — via the App Store, or via a direct `.xip` download from [developer.apple.com/download/all](https://developer.apple.com/download/all/) using a free Apple Developer account — **not** the paid $99/yr Developer Program). Xcode.app provides the `XCTest` and `Testing` frameworks that `swift test` requires; Command Line Tools alone are not sufficient.
- After first install, accept the Xcode license once: open `Xcode.app` and click "Agree," or run `sudo xcodebuild -license`.
- An editor of your choice. **Using the Xcode IDE is never required** — all development happens at the command line via `swift build` / `swift test`. Any editor with Swift LSP support works (VS Code with the Swift extension, Cursor, Nova, Vim with `sourcekit-lsp`, etc.).

---

## Build

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

---

## Run the engine CLI (headless)

Useful for testing the engine without the GUI:

```sh
swift run JboxEngineCLI --list-devices
swift run JboxEngineCLI --route '<src-uid>@<src-channels>-><dst-uid>@<dst-channels>'
# Example: --route 'AppleHDA:0@1,2->com.apple.audio.CoreAudio:7@3,4'
```

See [`Sources/JboxEngineCLI/main.swift`](../Sources/JboxEngineCLI/main.swift) for full options.

---

## Make targets (recommended)

Common commands are wrapped in a [`Makefile`](../Makefile) at the repo root:

```sh
make                # show all targets (default)
make clean          # wipe .build/ build/ test-results/ .swiftpm/
make build          # produce the distributable DMG (Jbox-<version>.dmg)
make test           # run the full test pipeline (same as CI)
make all            # clean + build + test

# Fine-grained:
make app            # build Jbox.app only (no DMG)
make dmg            # build the distribution DMG (same as `make build`)
make run            # build, bundle, and launch
make cli            # build just JboxEngineCLI
make verify         # run the full verification pipeline (same as `make test`)
make swift-test     # Swift Testing only (fast iteration)
make cxx-test       # C++ tests only (with per-test timings)
make cxx-test-tsan  # C++ tests under ThreadSanitizer
make rt-scan        # RT-safety static scanner only
make coverage       # generate Swift + C++ lcov reports under test-results/
```

The version used for `make build` defaults to `git describe`; override with `make VERSION=X.Y.Z build`. All output lands in git-ignored directories (`.build/`, `build/`, `test-results/`).

---

## Direct commands

Behind the Make targets, the canonical script is [`scripts/verify.sh`](../scripts/verify.sh) — the "is my tree green?" gate that mirrors CI exactly:

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

---

## Device-level integration tests

Soak, latency measurement, sample-rate-mismatch, and multi-route hardware tests require real Core Audio hardware and are documented in [`plan.md` § Phase 9](./plan.md#phase-9--release-hardening-and-device-level-testing). Procedures live alongside the plan under [`./testing/`](./testing/).

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
