# Soak test runbook

**Purpose.** Prove a representative Jbox route runs for ≥ 30 minutes on real hardware without dropouts, drift-loop failures, or steady-state buffer drift. This is one of the [release gates in `docs/spec.md` § 5.6](../spec.md#56-release-gates) (#3); it must pass before any `v1.x.y` tag is published.

**What this test is not.** It is not a latency measurement (see `latency.md`) and not a hot-plug / start-stop stress run (see `stress.md`). Keep the scope tight — one route, one long observation window.

---

## Acceptance criteria

After ≥ 30 minutes of continuous operation, all of the following hold:

| Signal                                    | Pass condition                                      | Where to read it                                                                 |
|-------------------------------------------|-----------------------------------------------------|----------------------------------------------------------------------------------|
| Route state                               | `RUNNING` continuously                              | CLI status line / GUI route row                                                  |
| Underrun event count                      | **0** (both edge log AND total counter)             | `evt=underrun` in `os_log` / `JboxEngineCLI.log`; `underrun_count` in poll output |
| Overrun event count                       | **0** (both edge log AND total counter)             | `evt=overrun` in `os_log` / `JboxEngineCLI.log`; `overrun_count` in poll output  |
| Channel-mismatch / converter-short events | **0**                                               | `evt=channel_mismatch`, `evt=converter_short`                                    |
| Route-level error events                  | **0**                                               | `evt=route_error`, `evt=route_waiting`, `evt=teardown_failure`                   |
| Audible signal at the destination         | Continuous, no clicks / dropouts / level anomalies  | Headphones or monitors on the destination                                        |
| Meter peaks (GUI variant only)            | Plausible: bounded, no sustained clipping at full   | Mixer-strip meters in the route panel                                            |

A single `evt=underrun` or `evt=overrun` over the full window is a fail — the soak gates zero dropouts. RT log events are edge-triggered so one logged event per kind may represent more underlying occurrences; cross-check against the `underrun_count` / `overrun_count` totals from `poll_route_status` to see the true tally.

---

## Prerequisites

- A built Jbox: either `make app` (GUI) or `swift build -c release` (CLI). `make verify` should be green on the working tree.
- **Two distinct Core Audio devices.** The whole point of the test is exercising drift correction across independent clocks. Same-device routes use the direct-monitor fast path that bypasses the ring + SRC and therefore do not exercise drift correction; they are not a valid soak target.
- An audio source feeding at least one input channel of the source device — a hardware instrument, signal generator, mic on a stand, or a DAW playback chain. The signal should be continuous (silence is fine for the engine but makes the audible-check step useless).
- Headphones or monitors on the destination device for the audible-signal check.
- A way to leave the machine alone for ≥ 30 minutes — sleep should be inhibited (caffeine the screen / use `caffeinate` on the CLI variant) but the audio interfaces themselves should not be reset, reconnected, or re-clocked.
- Ideally: source and destination running at **different sample rates** (e.g. 44.1 kHz source → 48 kHz destination). This forces resampling on top of drift correction. If both devices are at the same rate, the test still proves drift correction but the SRC path is exercised at unity ratio.

---

## Procedure — CLI variant (canonical)

The CLI is the recommended path because it produces a clean stderr trace per second, writes to its own per-process log file, and is trivially scriptable for unattended runs.

### 1. Enumerate devices

```sh
swift run JboxEngineCLI --list-devices
```

Pick a `DIR=I*` row for the source and a `DIR=*O` row for the destination. Note the exact UID strings — they are the stable Core Audio identifiers and what the CLI consumes.

### 2. Start the soak run

```sh
SRC_UID="<source uid>"
DST_UID="<dest uid>"
LOG_DIR="$HOME/jbox-soak-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$LOG_DIR"

# Inhibit display / system sleep for the duration of the soak, then run.
caffeinate -dimsu \
  swift run JboxEngineCLI --route "${SRC_UID}@1,2->${DST_UID}@1,2" \
  2> "$LOG_DIR/cli-stderr.log"
```

The CLI prints `state=running produced=… consumed=… underruns=… overruns=…` once per second to stderr until SIGINT (Ctrl-C). Leave it running for **at least 30 minutes** of wall time. 60 minutes is preferred when convenient — drift loops can take 5–10 minutes to fully settle on real hardware.

### 3. Mirror the engine log in a second terminal

```sh
log stream --predicate 'subsystem == "com.jbox.app"' \
  | tee "$LOG_DIR/os-log.txt"
```

This captures `os_log` output in real time. The same events are also written to `~/Library/Logs/Jbox/JboxEngineCLI.log` (rotating, 5 MiB × 3) — both sinks should agree.

### 4. Stop the run

After ≥ 30 minutes, Ctrl-C the CLI. The final `state=…` stderr line carries the cumulative `underruns` / `overruns` counters; the `evt=route_stopped` line should be the last line in `os-log.txt`.

### 5. Verify acceptance

```sh
# These should all return zero matches:
grep -E 'evt=(underrun|overrun|channel_mismatch|converter_short|route_error|route_waiting|teardown_failure)' \
  "$LOG_DIR/os-log.txt" "$HOME/Library/Logs/Jbox/JboxEngineCLI.log"

# Cumulative counter from the final stderr line:
tail -n 5 "$LOG_DIR/cli-stderr.log"
```

Pass = no matches and `underruns=0 overruns=0` on the final line. Any non-zero counter or any matching event is a fail; record it (see § Recording).

---

## Procedure — GUI variant

Use this path for the visual / audible smoke that complements the CLI run, or when you want to verify the GUI plumbing alongside the engine.

1. Launch `Jbox.app` (`make run` for a fresh ad-hoc-signed bundle, or open an installed copy).
2. Add a route with the same source / destination / mapping you would use for the CLI run. Pick a non-`Performance` tier if both devices are aggregates and you want drift correction in the loop (Performance on a same-device aggregate engages the direct-monitor fast path).
3. Start the route. The route row should turn green (RUNNING).
4. Open the route's mixer panel and confirm meter activity matches the audible signal.
5. (Optional but recommended) Open Preferences → Advanced → enable engine diagnostics, then watch the latency pill and per-route diagnostics for any flapping over the run.
6. Tail logs in a terminal: `log stream --predicate 'subsystem == "com.jbox.app" AND category == "engine"' | tee soak-os-log.txt`.
7. Leave the app running for ≥ 30 minutes with the screen kept awake (`caffeinate -d`).
8. Stop the route, capture the rotating file at `~/Library/Logs/Jbox/Jbox.log`, and apply the same `grep -E` from § CLI variant step 5.

---

## Recording results

For every release-gate soak, capture:

- Date, builder, git SHA (`git rev-parse HEAD`).
- Source / destination device names + UIDs + nominal sample rates.
- Mapping used.
- Wall-clock duration (start → SIGINT).
- Final `underruns` / `overruns` counters.
- Whether the audible signal stayed clean for the full window.
- The `os_log` capture file and the relevant rotating log file (zip together if archiving).

Drop a one-paragraph summary into the release notes / GitHub Release draft for the tag. The intent is reproducibility — the next person should be able to read the summary and re-run the same configuration.

---

## Failure triage

If any acceptance signal fails, the soak is a release blocker until it is understood. Common patterns and where to look:

- **Underrun events appear within the first 30 seconds, then stop.** Often a startup transient as the drift loop converges. Re-run with the destination's HAL buffer raised one step (the user-facing buffer in the interface software, not anything Jbox writes); per [`docs/spec.md` § 2.7](../spec.md) Jbox honours whatever the device is running at.
- **Underruns or overruns accumulate at a steady rate throughout the run.** The drift loop is not converging. Capture a longer log with debug-level persistence enabled (`sudo log config --subsystem com.jbox.app --mode "level:debug,persist:debug"`), then look at the time series of `frames_produced` vs `frames_consumed` in the CLI stderr trace — a steadily growing or shrinking gap points at a sample-rate-mismatch or rate-deadband problem.
- **`evt=route_waiting` mid-run.** Source or destination dropped off the bus (sleep, USB hiccup, device reset). Out of scope for soak — see `stress.md` for the disconnect-recovery procedure.
- **`evt=route_error` mid-run.** The `value_a` field carries a `jbox_error_code_t`; cross-reference `Sources/JboxEngineC/include/jbox_engine.h` for the meaning. File a follow-up in `docs/followups.md` with the captured logs.
- **Audible clicks but no logged events.** Edge-triggered logging may be hiding subsequent events. Stop and restart the route to re-arm the edge trigger; if clicks return immediately, the drift loop is unstable in steady state. Capture `~/Library/Logs/Jbox/JboxEngineCLI.log` from the failing run and attach to the bug report.
