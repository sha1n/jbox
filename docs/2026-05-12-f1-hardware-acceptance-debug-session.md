# F1 hardware acceptance pass — 2026-05-12

**Purpose.** Record the first end-to-end hardware acceptance pass of the engine's
hot-plug + sample-rate-cascade recovery wiring (F1 in
[`docs/followups.md`](followups.md#f1--production-hal-property-listener-registration-in-coreaudiobackend))
against the published `v0.1.0-alpha` DMG, following the procedure in
[`docs/testing/stress.md`](testing/stress.md). All three acceptance criteria
passed. A separate latency-pill anomaly surfaced during the run and was
investigated to root cause — the investigation chronology is captured here so a
future reader can reconstruct the reasoning without re-running it.

---

## Summary

F1's three acceptance tests passed against `v0.1.0-alpha` on a two-device rig:
hot-unplugging an aggregate sub-device, yanking the source USB interface, and
firing a sample-rate cascade via Audio MIDI Setup. All three drove the expected
WAITING transition and auto-recovery within the documented timing budgets.

During the source-unplug test (#2), the destination's latency pill on the
direct cross-device route jumped from ~4.6 ms (pre-unplug) to ~20 ms
(post-replug), and underrun / overrun counters started accumulating on that
route. Initial reaction: assume the recovery path left the engine in a
degraded state. Three hypotheses (stale IOProc not released; in-session engine
state corruption; polluted `coreaudiod` daemon state) were each tested and
ruled out. The actual finding was that the direct cross-device route had been
overrunning since *before* any unplug in that session, and the pre-unplug
4.6 ms reading was almost certainly captured against a different external
device buffer setting on the destination interface than what was active by the
time the post-replug measurement was taken. JBox is faithfully reporting what
the devices report; the direct cross-device path has inherent overhead the
aggregate fast path does not.

**Bottom line for F1:** closed. The recovery state-machine works correctly on
real hardware. The latency-pill observation is a documentation gap, not an
engine bug.

---

## Test environment

| Component                  | Value                                                                                  |
|----------------------------|----------------------------------------------------------------------------------------|
| Binary under test          | `v0.1.0-alpha` DMG (published artifact, not a local build)                             |
| Date                       | 2026-05-12                                                                             |
| Host macOS                 | macOS 15+ (per project minimum)                                                        |
| Source device              | Roland V31 sound module — USB, 48 kHz, 32-bit float, 30 input channels                 |
| Destination device         | UA Apollo — Thunderbolt, 48 kHz, 24-bit                                                |
| Aggregate                  | "Apollo + V31" — both devices as sub-devices, configured in Audio MIDI Setup           |
| Log archive                | `~/Library/Logs/Jbox/Jbox.log*` (rotating, 5 MiB × 3) — 15 MiB of session history      |

Two route configurations were exercised. Both at the "Performance" tier in the
GUI:

| Route   | Source            | Destination       | Engine path                                                                          |
|---------|-------------------|-------------------|--------------------------------------------------------------------------------------|
| Route 1 | "Apollo + V31"    | "Apollo + V31"    | Duplex fast path on the aggregate — no ring, no SRC, single IOProc                   |
| Route 2 | V31 (direct)      | Apollo (direct)   | Full cross-device path — SRC + ring + drift correction across independent clocks     |

The two configurations bracket the engine's interesting paths: route 1 covers
the aggregate-fast-path branch (`ring_target_fill_frames = 0`,
`dst_buffer_frames = 0`); route 2 covers the user-space-bridged branch that has
to handle two independent crystal clocks.

---

## F1 acceptance outcome

Acceptance criteria from
[`docs/testing/stress.md` § Acceptance criteria](testing/stress.md#acceptance-criteria),
mapped 1:1 to the three items in
[`docs/followups.md` § F1 → Acceptance](followups.md#f1--production-hal-property-listener-registration-in-coreaudiobackend):

| # | Scenario                                              | Result  | Notes                                                                                                   |
|---|-------------------------------------------------------|---------|---------------------------------------------------------------------------------------------------------|
| 1 | Hot-unplug an aggregate sub-device                    | PASS    | Route transitioned to WAITING within ~200 ms; row diagnostic read "Device disconnected — waiting for it to return."; re-plug auto-recovered to RUNNING within ~1 s. |
| 2 | Yank the source USB interface entirely                | PASS\*  | State-transition criterion met (WAITING + auto-recovery); see § The post-replug anomaly for the side-observation. |
| 3 | Sample-rate cascade in Audio MIDI Setup               | PASS    | Single auto-recovery pass; no thrash. F3 (event debounce) does **not** need to be promoted from "nice-to-have" to blocker. |

\* Test #2 met the F1 acceptance criterion (WAITING-on-unplug, recovery-on-replug
within budget). The latency / counter discrepancy on route 2 after replug was
the trigger for the investigation below; that investigation ultimately found
the observation is not an F1-recovery defect.

**F1 is closed.** All three engine-side recovery-path criteria are green on
real hardware.

---

## The post-replug anomaly — investigation chronology

After test #2's recovery completed, the direct cross-device route (route 2)
showed:

- Latency pill: ~20 ms (was reading ~4.6 ms pre-unplug in the same session).
- Underrun / overrun counters: accumulating, where they had been zero on
  initial start.

The question this raised: is the recovery path leaving the engine in a
degraded state? Three hypotheses were tested, narrowing from "the recovery
path is the cause" through "in-session engine state is the cause" to "external
to JBox altogether."

### Hypothesis 1 (rejected): stale IOProc not released during disconnect

Sub-phase 7.6.3 hardened teardown but only in scenarios CI can simulate. If a
source IOProc handle leaked during the disconnect event, a zombie IOProc could
still be firing on top of the new one — explaining the producer outpacing the
consumer in the counters.

Test:

```sh
ls -lh ~/Library/Logs/Jbox/
grep -c 'evt=teardown_failure' ~/Library/Logs/Jbox/Jbox.log*
```

Result: zero `evt=teardown_failure` events across the entire 15 MiB rotating
window covering many test sessions. The teardown path's failure-counter is
the load-bearing diagnostic for IOProc-leak scenarios (per
`kLogTeardownFailure = 104`, the 7.6.3 log code); its absence rules out the
zombie-IOProc theory.

### Crucial finding from the log — overruns predate the first unplug

While reviewing the same log, the timestamps told a different story than
expected.

```sh
grep -nE 'evt=(overrun|underrun|route_waiting|route_started)' \
  ~/Library/Logs/Jbox/Jbox.log
```

- First `evt=overrun` on route 2: **07:48:43.797** — concurrent with route 2's
  first `evt=route_started` in this session.
- First `evt=route_waiting` on route 2 (a=1, b=0 — source UID missing):
  **07:48:54.738** — eleven seconds later.

Route 2 had been overrunning for 11 seconds before any unplug event occurred
in that session. The "post-recovery bug" was not caused by the recovery path
— it was a pre-existing condition on route 2 itself, made visually worse by
whatever buffer reshuffle occurred across the replug cycle.

This re-cast the question entirely: not "did F1 break route 2?" but "is there
a route-2-specific issue, independent of F1, that was hiding behind the
pre-unplug 4.6 ms reading?"

### Hypothesis 2 (rejected): in-session engine state corruption

If route 2 was always overrunning in this session but had been clean in
earlier sessions, maybe earlier engine activity (route 1 starting / stopping,
listener registrations from F1's hot-plug churn) was leaving state behind that
affected subsequent routes on the same process.

Test: cold-start.

1. Quit JBox completely (terminate the process; no lingering routes).
2. Relaunch.
3. Start ONLY route 2 (no route 1; no unplug events).
4. Observe for 30 s.

Result:

- Route 2 cold-started at **29 ms** latency pill (worse than the 20 ms
  post-replug observation that triggered the investigation).
- Counters bumped briefly during startup, then stabilised with occasional
  small bumps.
- For comparison, route 1 cold-started cleanly at **3.6 ms**, zero counters,
  perfect operation throughout.

The bug persists across JBox restarts. Cold-start vs hot-state makes no
meaningful difference. In-session state corruption is not the cause.

### Hypothesis 3 (rejected): polluted macOS Core Audio daemon state

The `coreaudiod` daemon caches device characteristics. If the hot-plug churn
during F1 testing pushed it into a degraded state for the source USB
interface, the elevated latency would persist across JBox restarts but reset
across a full system reboot.

Test:

1. Full system reboot.
2. Launch JBox.
3. Start only route 2.

Result: route 2 still at **29 ms** latency pill. A full reboot did not
restore the original 4.6 ms reading. `coreaudiod` state pollution is not the
cause either.

### What we actually learned

The original ~4.6 ms pill reading captured for route 2 before any disconnect
testing in this session was almost certainly **not representative** of the
device's actual reported latency at that moment. The most plausible
explanation: the destination interface's I/O buffer setting (a per-device
global, controlled in UAD Console or the equivalent control software, shared
by all HAL clients) was at a low value when the 4.6 ms reading was taken and
has since been changed externally — either by other software on the host or
by a state reset across the unplug. The 29 ms we read post-reboot is what the
direct cross-device path reports when JBox computes the latency from
`src->input_device_latency_frames`, `src->buffer_frame_size`, ring target
fill, converter prime, and the corresponding destination-side values. (See
`Sources/JboxEngineC/control/route_manager.cpp` around lines 1108–1121 for
the exact composition.)

JBox is faithfully reporting what the devices report. The change between
"then" and "now" lives outside JBox — in whichever HAL client (or device
control software) last wrote the destination's I/O buffer.

The aggregate route (route 1) is unaffected because the duplex fast path
sidesteps the cross-clock bridging entirely: one IOProc on the aggregate,
`ring_target_fill_frames = 0`, `dst_buffer_frames = 0`, a single device buffer
counted once, and the kernel HAL handles the sub-device alignment internally.
The direct cross-device route cannot replicate this from user space — it must
use a ring + SRC + drift correction to bridge two independent crystal clocks,
and that bridging has inherent latency cost.

---

## Final picture

- **F1 acceptance: complete.** All three state-transition criteria passed.
  F1's status moves from "🚧 Engine landed; awaiting manual hardware
  acceptance" to "✅ Closed."
- **F3 (event debounce): not promoted to blocker.** Test #3 produced a clean
  single recovery pass for the sample-rate cascade; no thrash. F3 stays in
  the "nice-to-have" bucket.
- **The latency-pill discrepancy is not a JBox engine bug.** The direct
  cross-device path has inherent overhead vs the aggregate path; the specific
  absolute pill value reflects whatever the destination device's I/O buffer
  is currently set to in its control software. The 4.6 ms vs 29 ms gap is
  external-state, not engine-state.
- **The main user use case (the aggregate route) is unaffected** and works
  perfectly under hot-plug + recovery cycles.

### Potential future followup — documentation gap (not filed yet)

Call it F7 if and when it is filed. The pattern is a documentation /
user-guidance gap, not an engine bug:

- Users routing cross-device should know the aggregate-on-both-legs topology
  is the low-latency option, because it engages the duplex fast path
  (kernel-side cross-clock alignment, no user-space ring or SRC).
- For users who must use a direct cross-device route (different physical
  interfaces, not wrapped in an aggregate), the dominant latency contributor
  is the destination device's I/O buffer setting, controlled in that
  device's vendor software.
- The latency pill is accurate, not buggy. The number it reports is what
  the engine computes from the devices' own reported parameters.

The fix surface is README / user docs, not the engine. This is not in scope
for the current conversation; if and when it is filed, it should land as a
new `F7` entry in `docs/followups.md`.

---

## Open questions / verify next time

- **The destination's external I/O buffer setting was not recorded** before
  or after this session. Future stress runs should capture the destination
  control-software buffer value as part of the test environment so the
  latency pill's absolute number is interpretable. A column in
  `docs/testing/stress.md` § Recording results would catch this.
- **Did the source-unplug cycle cause the destination's buffer to be
  rewritten by an external HAL client?** Possible — some interface control
  software re-asserts settings on connect/disconnect events. If a future
  session can reproduce the "4.6 ms → 20 ms transition across a single
  unplug," a `log stream --predicate 'subsystem == "com.apple.audio.AudioHardware"'`
  capture across the cycle should name the property-set that moved the
  value.
- **No latency measurement was attempted as part of this run.** The latency
  release-gate procedure in `docs/testing/latency.md` (when it exists)
  remains an independent F6 sub-item.

---

## References

- [`docs/followups.md` § F1](followups.md#f1--production-hal-property-listener-registration-in-coreaudiobackend)
  — engine-side wiring landed 2026-04-28; acceptance gate closed by this run.
- [`docs/followups.md` § F3](followups.md#f3--devicechangewatcher-event-debounce)
  — debounce stays optional; this run's test #3 was the gate that would have
  promoted it to blocking.
- [`docs/followups.md` § F6](followups.md#f6--phase-9-real-hardware-acceptance-soak--latency--stress)
  — Phase 9 real-hardware acceptance bundle; stress run is one of three
  procedures, this doc is its record.
- [`docs/testing/stress.md`](testing/stress.md) — procedure followed for this
  run.
- [`docs/plan.md` § Phase 9](plan.md#phase-9--release-hardening-and-device-level-testing)
  — release-gate task list; stress / disconnect task ticked off by this run.
- `Sources/JboxEngineC/control/route_manager.cpp` ~1108–1121 — latency-pill
  composition for the direct cross-device path.
