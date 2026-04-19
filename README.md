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
5. **Personal use first.** v1 runs with only Xcode Command Line Tools (free) and ad-hoc code signing. No Apple Developer Program required. Distribution to others is possible via an unsigned `.zip` lane with clear Gatekeeper instructions.

---

## Documentation

- **[docs/spec.md](./docs/spec.md)** — the authoritative technical specification. Five sections: system architecture, audio engine internals, data model and persistence, UI design, and testing / build / distribution. Read this for design decisions and their rationale.
- **[docs/plan.md](./docs/plan.md)** — phased implementation plan with concrete tasks. Nine phases from empty repo to v1.0.0 release. Read this to follow or contribute to the implementation.
- **This file** — orientation. Scope, principles, documentation map, quick-start.

---

## Quick-start (for the builder)

### Prerequisites

- macOS 15 (Sequoia) or later.
- **Xcode Command Line Tools** installed:

  ```sh
  xcode-select --install
  ```

  Free, requires only an Apple ID, not the paid Developer Program.
- An editor of your choice. Xcode IDE is optional — any editor with Swift LSP support (VS Code with the Swift extension, Cursor, Nova, Vim with `sourcekit-lsp`, etc.) works.

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
swift test                          # unit + simulation integration tests
./scripts/rt_safety_scan.sh         # static RT-safety check on engine/rt/
```

Device-level integration tests (soak, latency measurement) require real hardware and are documented in [docs/plan.md § Phase 9](./docs/plan.md#phase-9--release-hardening-and-device-level-testing).

### First launch

On first launch, macOS prompts for audio-device access. Grant it — Jbox needs this to route audio between any device with a microphone-class designation (which includes every input-capable audio interface).

---

## Sharing Jbox with someone else

If you want to hand a build to a friend or colleague, run:

```sh
./scripts/package_unsigned_release.sh
```

This produces `Jbox-<version>.zip` containing the app and a `READ-THIS-FIRST.txt` that explains the one-time Gatekeeper approval they'll need to do (right-click → Open). No Apple Developer Program required on your side.

If the audience grows and that one-time Gatekeeper dance becomes painful, the next step is joining the Apple Developer Program ($99/year) and adding Developer ID signing + notarization to the release pipeline. This is a clean future flip — nothing in the code needs to change.

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
