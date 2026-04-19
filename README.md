# Jbox

**A native macOS audio routing utility.** Jbox routes selected channels from one Core Audio device to selected channels on another, in real time, with low latency and robust drift correction between independent clocks.

---

## What Jbox does

Jbox is a **generic Core Audio routing tool**. It bridges any input-capable device to any output-capable device, with:

- **Arbitrary 1:1 channel mapping** — pick any N source channels, any N destination channels, map them in any order.
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

- **Not a mixer.** No summing, no splitting, no gain, no mute. Pure 1:1 routing.
- **Not a DAW.** No timeline, no plugins, no recording, no MIDI.
- **Not a virtual audio driver.** Jbox does not create devices that other apps see. It routes between existing devices.
- **Not a network audio tool.** No Dante, no AVB, no NDI, no IP streaming.

---

## Design principles

1. **Pro-audio routing semantics.** Channel numbering is 1-indexed in the UI (0-indexed internally). Devices are identified by Core Audio UID (stable across reboots). Each route is independent; lifecycle and errors are per-route.
2. **Top-performance real-time engine.** The audio-processing path is written in C++ with strict real-time discipline: no allocations, no locks, no syscalls on the audio thread. Engineered for near-zero added latency.
3. **UI is replaceable.** The engine is an independent C++ library exposed via a stable public C API. Today's SwiftUI UI is one implementation of that API; it can be rewritten or replaced without touching the engine.
4. **Do not step on other apps.** Jbox does not create aggregate devices, does not change sample rates, and does not change buffer sizes without explicit user opt-in. Other apps sharing the same hardware are unaffected.
5. **Personal use first.** v1 runs with free Apple tooling and ad-hoc code signing. No paid Apple Developer Program required. Development works entirely from the command line — the Xcode IDE is never required (though Xcode.app must be installed for its frameworks; see Prerequisites). Distribution to others is possible via an unsigned `.zip` lane with clear Gatekeeper instructions.

---

## Documentation

- **[docs/spec.md](./docs/spec.md)** — the authoritative technical specification. Five sections: system architecture, audio engine internals, data model and persistence, UI design, and testing / build / distribution. Read this for design decisions and their rationale.
- **[docs/plan.md](./docs/plan.md)** — phased implementation plan with concrete tasks. Nine phases from empty repo to v1.0.0 release. Read this to follow or contribute to the implementation.
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

### Test

```sh
./scripts/verify.sh                 # runs the full pipeline (same as CI)
```

`verify.sh` is the canonical "is my tree green?" command and mirrors exactly what CI runs on each push. It covers:

1. RT-safety static scan on `Sources/JboxEngineC/rt/`
2. Release build of all SPM targets
3. Swift-side tests (Swift Testing)
4. C++ engine tests via Catch2, with per-test timings
5. C++ engine tests under ThreadSanitizer

For faster iteration, the individual steps can be run directly:

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

**Phase 0 — specification.** The design is locked; implementation has not yet begun. See [docs/plan.md](./docs/plan.md) for the phased implementation roadmap.

Scope for **v1.0.0** (what the first release will do):

- Route selected channels of any input-capable device to selected channels of any output-capable device (arbitrary 1:1 mapping).
- Multiple simultaneous routes, each independently started / stopped.
- Named scenes to activate groups of routes together.
- Auto-resampling when source and destination sample rates differ.
- Drift correction between independent device clocks.
- Auto-waiting for missing devices; auto-recovery on device return.
- SwiftUI main window + menu bar extra + preferences.
- Ad-hoc signed `.app` for personal use; unsigned `.zip` lane for small-audience sharing.

Explicitly **deferred** beyond v1 (see [docs/spec.md § Appendix A](./docs/spec.md#appendix-a--deferred--out-of-scope) for the full list):

- Fan-out / fan-in / summing / any mixer features.
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
