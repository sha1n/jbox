# Stress / disconnect test runbook

**Purpose.** Prove Jbox's hot-plug and aggregate-loss auto-recovery work end-to-end on real hardware. This is one of the [release gates in `docs/spec.md` § 5.6](../spec.md#56-release-gates); it must pass before any `v1.x.y` tag is published.

The engine-side hot-plug machinery (`DeviceChangeWatcher` + `RouteManager::handleDeviceChanges` + the `CoreAudioBackend` HAL listeners that drive them) has full simulator-path regression coverage in CI under `[device_change_watcher]`, `[sim_backend][device_change]`, `[route_manager][device_loss]`, `[route_manager][aggregate_loss]`, `[core_audio][hal_translation]`, and `[core_audio][hal_listener_lifecycle]`. CI cannot fire real HAL property-change events, so the production wiring needs a manual hardware pass per release — that is what this runbook documents.

**What this test is not.** It is not a soak / drift test (see `soak.md`) and not a latency measurement (see `latency.md`). It is also not yet a rapid start/stop matrix — that pass is in scope for the same Phase 9 task but is deferred until the disconnect-recovery acceptance below is green; the matrix design will land in this file when it does.

**First acceptance pass: 2026-05-12.** All three scenarios below have been run against the published `v0.1.0-alpha` DMG on a two-device rig; all three passed. The run record + post-replug latency-pill investigation chronology lives in [`docs/2026-05-12-f1-hardware-acceptance-debug-session.md`](../2026-05-12-f1-hardware-acceptance-debug-session.md) — read it before re-running this procedure on a similar rig so you know which side-observations are already understood.

---

## Acceptance criteria

The three tests below correspond 1:1 with the acceptance items in [`docs/followups.md` § F1 → Acceptance](../followups.md#f1--production-hal-property-listener-registration-in-coreaudiobackend). All three must pass for F1 to close.

| # | Scenario                                          | Pass condition                                                                                                                       |
|---|---------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------|
| 1 | Hot-unplug a sub-device of a running aggregate    | Route transitions to WAITING with `last_error == JBOX_ERR_DEVICE_GONE` within ~200 ms. On replug, auto-recovers to RUNNING within ~1 s. |
| 2 | Yank the source USB interface entirely            | Same outcome as #1.                                                                                                                  |
| 3 | Change sample rate of an aggregate's active member in Audio MIDI Setup | **One** auto-recovery pass — not a thrash of N start/stop cycles as the cascade fires N property-list-changed events.                |

If #3 thrashes (more than ~2 `evt=route_started` events for a single sample-rate change), F3 (event debounce) is promoted from "nice-to-have" to a blocking follow-up for `v1.0.0`.

---

## Prerequisites

- **A built Jbox.** Two paths:
  - **Released artifact (preferred).** Download the `Jbox-*.dmg` from the current [GitHub release](https://github.com/sha1n/jbox/releases) — testing the actual binary users will run is the point of an acceptance pass. The `v0.1.0-alpha` engine matches HEAD; only UI / CI commits have landed since.
  - **Local build.** `make build` produces `build/Jbox-*.dmg`; ad-hoc-signed with the audio-input entitlement.
- **An aggregate device with at least two sub-devices**, configured in Audio MIDI Setup before the run. Both sub-devices should be currently connected. At least one must be a hot-pluggable interface (USB, Thunderbolt is acceptable but cable-unplug etiquette is less forgiving).
- **A separate hot-pluggable source interface** for test #2 — if the only hot-pluggable interface is already inside the aggregate for tests #1 and #3, test #2 can reuse it (just re-target the route to that interface as a stand-alone source between tests).
- **A signal source** feeding the route's input channels (instrument, signal generator, DAW playback) and headphones / monitors on the destination — the audible-signal cross-check is what catches "route says RUNNING but no audio."
- **System Settings → Sound** open to a non-Jbox device, so macOS does not redirect default-output to a vanishing aggregate during the test.

---

## Procedure

The GUI variant is canonical for this test — the route row's diagnostic text is the load-bearing signal that the route hit `JBOX_ERR_DEVICE_GONE` specifically (vs initial-WAITING with `last_error == JBOX_OK`), and the CLI status line does not surface `last_error` today. The CLI is acceptable for the auto-recovery-timing portion if you also `grep` the log file for the post-recovery `evt=route_started`.

### 1. Set up the environment

1. Quit any DAW or other Core Audio client that might also be holding the aggregate's sub-devices.
2. In Audio MIDI Setup, confirm the aggregate's sub-device list and current sample rate. Note both — you will read them again at the end of test #3.
3. Install `Jbox.app` to `/Applications` (or run from `make run`). Launch it.
4. In a terminal:
   ```sh
   LOG_DIR="$HOME/jbox-stress-$(date -u +%Y%m%dT%H%M%SZ)"
   mkdir -p "$LOG_DIR"

   # Live mirror — leave this running for the whole session.
   log stream --predicate 'subsystem == "com.jbox.app" AND category == "engine"' \
     | tee "$LOG_DIR/os-log.txt"
   ```
   The same events also land in `~/Library/Logs/Jbox/Jbox.log` (rotating, 5 MiB × 3). Both sinks should agree.

5. In Jbox, add a route whose **source** uses the source interface and whose **destination** uses the aggregate (or vice versa — anything that puts the aggregate on at least one leg). Start it. Confirm the route row turns green / RUNNING and the audible signal is clean at the destination.

### 2. Test #1 — hot-unplug an aggregate sub-device

1. Note the wall-clock time. Yank the cable of an active sub-device of the aggregate (USB unplug; Thunderbolt unplug is acceptable but you must use Eject first if the cable is also a system-disk Thunderbolt chain).
2. **Within ~200 ms:**
   - Route row turns yellow / WAITING.
   - Diagnostic text reads: **"Device disconnected — waiting for it to return."** This string maps to `last_error == JBOX_ERR_DEVICE_GONE`; the initial-WAITING state does not render any diagnostic text. *(Source: `Sources/JboxEngineSwift/RouteRowErrorText.swift`.)*
   - Audible signal cuts cleanly at the destination — no click, no loop of the last buffer.
   - `os-log.txt` records `jbox evt=route_waiting route=<N> a=… b=…`. The `a` and `b` fields are `1` when the named UID was the route's source / destination directly; they are `0,0` when the yanked device was an aggregate sub-device but not the route's source or destination UID itself. Either pattern is valid for this test — the route_waiting log is the recovery trigger; the route-row text is the user-visible error code.
3. Replug the same cable.
4. **Within ~1 s:**
   - Route row turns green / RUNNING again.
   - Audible signal resumes at the destination.
   - `os-log.txt` records `jbox evt=route_started route=<N> a=<src_ch> b=<dst_ch>`.
5. Verify the recovery pass was a single transition — `grep 'evt=route_started' "$LOG_DIR/os-log.txt"` should show exactly one started event after the unplug (plus the original one before the unplug).

### 3. Test #2 — yank the source USB interface

1. (If the source interface was *also* the unplugged sub-device in test #1, this test is redundant — record it as "covered by test #1" and skip.)
2. Note the wall-clock time. Yank the source USB interface's cable.
3. Expected outcome: same as test #1 — WAITING within ~200 ms, route-row text "Device disconnected — waiting for it to return.", clean audio cut.
4. Replug. Auto-recovery within ~1 s; `evt=route_started` in the log.

### 4. Test #3 — sample-rate cascade

1. With the route running, open Audio MIDI Setup. Select an **active sub-device** of the aggregate (i.e. a sub-device that is currently driving the route, not just listed).
2. In the Format dropdown, pick a different sample rate. (e.g. 44100 → 48000 if both are supported by the device.)
3. Watch the route row and the `os-log.txt` stream. Acceptable outcomes:
   - **Pass:** route briefly flips to WAITING (one event), then back to RUNNING (one event). Diagnostic text may flash "Device disconnected — waiting for it to return." or may skip directly. Aggregate composition refreshes silently. The audible signal may glitch once but resumes cleanly within ~1 s.
   - **Fail (F3 becomes blocking):** the route flaps repeatedly — three or more `evt=route_started` lines for a single sample-rate change. This is the thrash case the F1 followup calls out.
4. Verify with `grep -c 'evt=route_started' "$LOG_DIR/os-log.txt"` — count after step 3 should be the count before step 3 + at most 2.
5. Restore the original sample rate in Audio MIDI Setup (so the system is back to its baseline state for any follow-up runs).

### 5. Tear down

1. Stop the route in the GUI.
2. Quit `Jbox.app`.
3. Ctrl-C the `log stream` tail.
4. Copy `~/Library/Logs/Jbox/Jbox.log` (the rotating file) into `$LOG_DIR` so the archive holds both views of the run.

---

## Recording results

For every release-gate stress pass, capture:

- Date, builder, git SHA of the binary under test (the alpha DMG was built at `git describe --tags`).
- Aggregate name + UID + member device names / UIDs.
- Source / destination devices used per test.
- Per test (1, 2, 3): pass / fail, observed timing (~200 ms to WAITING, ~1 s to RUNNING — note any deviation), number of `evt=route_started` events for test #3.
- The `os-log.txt` capture and the `Jbox.log` rotating file (zip together if archiving).
- Anything weird that wasn't on the pass / fail axis but caught your eye — these are usually where the next bug lives.

Drop a one-paragraph summary into the release notes / GitHub Release draft for the tag. The intent is reproducibility — the next person should be able to read the summary and re-run the same three scenarios.

---

## Failure triage

If any scenario fails, the test is a release blocker until it is understood. Common patterns and where to look:

- **Route stays RUNNING after unplug (no WAITING transition).** Production HAL listeners aren't firing for that property. Check `CoreAudioBackend::setDeviceChangeListener` and the `enumerate()` reconcile path actually registered listeners on the yanked device; capture an `lldb` symbol-walked breakpoint on `core_audio_hal_translation.cpp::translateHalPropertyEvent` and re-yank. File against F1 — the engine half is supposed to handle this.
- **Route transitions to WAITING but row text reads no diagnostic** (or reads the wrong error message). `last_error` is not being set to `JBOX_ERR_DEVICE_GONE`. Trace through `RouteManager::handleDeviceChanges` to see which branch the event hit. File a bug; this is a regression in the 7.6.6 watched-UID logic.
- **Route does not auto-recover on replug.** The `kDeviceListChanged` retry path is broken. Verify the `log stream` shows the system-object listener firing on the replug; if not, that listener installation failed in `CoreAudioBackend`. If yes, the bug is in `RouteManager::attemptStart` or `dm_.refresh()`.
- **Test #3 thrashes (≥ 3 `evt=route_started` for one rate change).** Open `docs/followups.md` § F3 (event debounce) and promote it from optional to a `v1.0.0` blocker. The simulator-path coverage already exists; F3 just needs the production debounce wiring.
- **Audio resumes but the signal is wrong** (wrong channels, wrong gain, distorted). Not an F1 issue — the recovery is succeeding but `attemptStart` is rebuilding the route with stale or wrong configuration. File separately; capture the route's pre-disconnect and post-recovery `pollStatus` output.
- **App crashes during unplug or replug.** Re-open `docs/followups.md` § F4 — the intermittent stress-crash there may be the same underlying issue. Attach the crash report (`~/Library/Logs/DiagnosticReports/Jbox-*.ips`) to the bug.
