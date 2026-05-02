# Tighten device-disconnect monitoring (design)

**Date:** 2026-05-02
**Status:** Brainstormed; awaiting plan.
**Scope:** Engine reaction layer (C++) + one ABI-additive error code + Swift UI text. No new threads, no new public Swift APIs.

## Goal

Routes should auto-detect when a device they depend on stops working — including the **aggregate sub-device** case that F1's reaction layer doesn't currently catch — and the UI should clearly indicate the failure. Today, when a sub-device of an aggregate is unplugged, the route stays `RUNNING` (green) even though audio has stopped or degraded.

## Non-goals

- A new public Swift / C ABI surface beyond one additive error code variant. No new state-machine states, no new tick callback, no new entry point.
- Active recovery beyond what 7.6.4 already does. The watchdog signals failure; recovery still rides on the existing `kDeviceListChanged` / `kAggregateMembersChanged` retry path or a manual stop+start.
- Hog-mode opt-in or "exclusive" semantics on Jbox's side. Per `CLAUDE.md` the Phase 7.6 deviation, Jbox is share-only by construction.
- Rewriting `R1` (`jbox_error_code_t` carrying non-error variants). Adding `JBOX_ERR_DEVICE_STALLED` extends the same shape; the focused rename remains in `docs/refactoring-backlog.md`.
- Power / sleep / wake (F2) — orthogonal axis already designed.

## Background

Two diagnostic findings during brainstorming:

1. **F1 reaction-layer gap (real bug).** `RouteManager::handleDeviceChanges` matches `kDeviceIsNotAlive` events against `r.source_uid` / `r.dest_uid` (`route_manager.cpp:692`). When the route is built on an aggregate, those fields hold the *aggregate's* UID, not the constituent sub-device's. macOS fires `kAudioDevicePropertyDeviceIsAlive` on the sub-device that vanished, which our backend translates to `DeviceChangeEvent{kDeviceIsNotAlive, uid="<sub-device>"}`. The matcher misses; the route stays `RUNNING`. The aggregate's `kAggregateMembersChanged` event does fire and triggers `dm_.refresh()` + retry-WAITING, but it never tears down a *running* route.

2. **No "frames not advancing" safety net.** Even when the topology event is missed (Apple listener didn't fire, an undocumented edge, or another app temporarily preempts the IOProc), there's no independent signal that a `RUNNING` route's audio has actually stopped flowing.

The user reported the symptom against an aggregate-on-route configuration during multi-day use, so the gap is hit in practice — not just theoretical.

## Solution at a glance

Three coordinated changes, smallest surface that closes the user-visible gap:

1. **(Engine, C++)** Expand each route's tracked-UID set at `attemptStart` time to include any aggregate's active sub-device UIDs. The matcher in `handleDeviceChanges` checks the set instead of just `source_uid` / `dest_uid`. Refresh the set on every `kAggregateMembersChanged` event for that route's aggregate UIDs.

2. **(Engine, C++)** Add a per-route stall watchdog on the existing 10 Hz hot-plug consumer thread. If a `RUNNING` route's `framesProduced` AND `framesConsumed` both fail to advance for ≥ `kStallTicks = 15` (1.5 s at 10 Hz), tear down the route, transition to `WAITING`, set `last_error = JBOX_ERR_DEVICE_STALLED`. ABI v13 → v14 additive (new error variant only).

3. **(Swift UI)** Show a per-route error message on `WAITING` rows whose `lastError != JBOX_OK` — currently the row only reads `lastError` when `state == .error`. Match the orange-clock visual for transient WAITING but render a red secondary line with the human-readable error name.

## Architecture

### 1. Aggregate-aware UID matching

```
RouteRecord {
    std::string source_uid;            // unchanged — primary identity
    std::string dest_uid;               // unchanged — primary identity
    std::vector<std::string>            // NEW — flat set of UIDs whose
        watched_uids;                  //       loss should fail this route
    ...
};
```

`watched_uids` is populated in `attemptStart` after device resolution succeeds:

```cpp
r.watched_uids.clear();
r.watched_uids.push_back(r.source_uid);
r.watched_uids.push_back(r.dest_uid);
appendAggregateMembers(r.watched_uids, r.source_uid);
appendAggregateMembers(r.watched_uids, r.dest_uid);
```

`appendAggregateMembers(out, uid)` is a new pure helper on `DeviceManager` that walks the cached enumeration: if the UID belongs to an aggregate, push every active sub-device UID onto `out`. For non-aggregate devices the helper is a no-op.

This requires extending `BackendDeviceInfo` (`device_backend.hpp:48`) with two fields, populated during `enumerate()`:

```cpp
struct BackendDeviceInfo {
    ...existing fields...
    bool                       is_aggregate = false;          // NEW
    std::vector<std::string>   aggregate_member_uids;         // NEW (active list)
};
```

`CoreAudioBackend::enumerate()` reads `kAudioAggregateDevicePropertyActiveSubDeviceList` for each aggregate (it already does so for the F1 listener-install path; the same read can be reused). The simulator backend gains a setter on its existing `Device` config struct so tests can stage aggregate fixtures. No new HAL property is read; no per-query HAL trip in the reaction layer.

`teardown(r)` clears `watched_uids` so a torn-down route can't be matched by a late event during its WAITING phase.

`handleDeviceChanges` matcher becomes:

```cpp
auto matches = [&](const RouteRecord& r) {
    return std::find(r.watched_uids.begin(),
                     r.watched_uids.end(),
                     ev.uid) != r.watched_uids.end();
};
```

`kAggregateMembersChanged` for a UID present in any route's `watched_uids` triggers a re-expansion of that route's set on the next refresh pass:

```cpp
case DeviceChangeEvent::kAggregateMembersChanged:
    any_list_change = true;
    aggregates_to_reexpand.insert(ev.uid);
    break;
```

In the post-loop refresh phase, after `dm_.refresh()`:

```cpp
for (auto& [id, rec] : routes_) {
    auto& r = *rec;
    if (r.state == JBOX_ROUTE_STATE_RUNNING) {
        for (const auto& agg_uid : aggregates_to_reexpand) {
            if (std::find(r.watched_uids.begin(), r.watched_uids.end(),
                          agg_uid) != r.watched_uids.end()) {
                // re-derive members; if a member that the route
                // depends on has just vanished, fall through to
                // teardown-and-WAITING with last_error = DEVICE_GONE.
                rebuildWatchedUids(r);
                if (anyWatchedUidMissing(r)) {
                    teardown(r);
                    r.state      = JBOX_ROUTE_STATE_WAITING;
                    r.last_error = JBOX_ERR_DEVICE_GONE;
                }
            }
        }
    }
}
```

`anyWatchedUidMissing` walks `r.watched_uids` against the freshly refreshed `dm_`. If any UID has disappeared from the enumeration, it's load-bearing. (The aggregate itself stays present — it's the constituent that's gone.)

### 2. Stall watchdog

State on `RouteRecord`:

```cpp
std::uint64_t last_seen_frames_produced = 0;  // tick-cadence sample
std::uint64_t last_seen_frames_consumed = 0;
std::uint8_t  stall_ticks               = 0;  // consecutive 100ms ticks
                                              // with both counters frozen
```

Reset on every state transition into `RUNNING` (in `attemptStart`'s success path) and on every advance: when the freshly read counter differs from the stored sample, update the sample and reset `stall_ticks = 0`.

A new tick method `RouteManager::tickStallWatchdog(now)` is called from `Engine::hotPlugThreadLoop` on the same 10 Hz cadence as `tickHotPlug` / `tickPower`. Body:

```cpp
constexpr std::uint8_t kStallTickThreshold = 15;  // 1.5 s at 10 Hz

for (auto& [id, rec] : routes_) {
    auto& r = *rec;
    if (r.state != JBOX_ROUTE_STATE_RUNNING) {
        r.stall_ticks = 0;
        continue;
    }
    const auto fp = r.frames_produced.load(std::memory_order_relaxed);
    const auto fc = r.frames_consumed.load(std::memory_order_relaxed);
    const bool fp_advanced = (fp != r.last_seen_frames_produced);
    const bool fc_advanced = (fc != r.last_seen_frames_consumed);
    if (fp_advanced || fc_advanced) {
        r.last_seen_frames_produced = fp;
        r.last_seen_frames_consumed = fc;
        r.stall_ticks = 0;
        continue;
    }
    if (++r.stall_ticks < kStallTickThreshold) continue;
    teardown(r);
    r.state      = JBOX_ROUTE_STATE_WAITING;
    r.last_error = JBOX_ERR_DEVICE_STALLED;
    tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting, id, /*src*/ 0, /*dst*/ 0);
}
```

The `kLogRouteWaiting` log uses payload `value_a=0, value_b=0` to differentiate from F1's "lost source" (`value_a=1`) / "lost dest" (`value_b=1`) at the log layer. A new RT log code (`kLogRouteStalled`) would be cleaner — adding it is acceptable since `rt_log_codes.hpp` is internal — but `kLogRouteWaiting` already covers the "transition to WAITING" semantics, so the value_a/value_b payload encoding is sufficient.

**Why 1.5 s.** A typical buffer round-trip at 64 frames @ 48 kHz is ~1.3 ms. Even on the slowest-supported configuration (256 frames @ 44.1 kHz, ~5.8 ms), 1.5 s is two orders of magnitude headroom. False positives would require Core Audio to genuinely stop calling our IOProc for 1.5 s — at which point the route IS broken from the user's perspective regardless of root cause. Tunable via the named constant if real-hardware testing surfaces edge cases.

**Why both counters.** A source-only stall (input gone, output still pulling silence into the dest) advances `framesConsumed` but not `framesProduced`. A dest-only stall (output gone, source still draining the ring) advances `framesProduced` but not `framesConsumed`. We want to flag *either*. The condition `(fp_advanced || fc_advanced)` resets stall_ticks when *either* side is alive — meaning the watchdog only fires when *both* have been frozen for the threshold window. This is conservative (fewer false positives) and matches the user-observable symptom: "audio fully stopped." A future tightening could fire on either-side stall — flag for follow-up if real-hardware testing shows partial stalls happen.

### 3. UI: WAITING-with-error secondary text

In `Sources/JboxApp/RouteListView.swift`:

```swift
private var errorText: String? {
    let errCode = route.status.lastError
    let hasError = errCode.rawValue != JBOX_OK.rawValue
    switch route.status.state {
    case .error:                 return String(cString: jbox_error_code_name(errCode))
    case .waiting where hasError: return waitingErrorText(errCode)
    default:                     return nil
    }
}

private func waitingErrorText(_ code: jbox_error_code_t) -> String {
    switch code {
    case JBOX_ERR_DEVICE_GONE:      return "Device disconnected — waiting for it to return."
    case JBOX_ERR_DEVICE_STALLED:   return "No audio — device stopped responding."
    case JBOX_ERR_SYSTEM_SUSPENDED: return "Recovering from sleep…"
    default:                        return String(cString: jbox_error_code_name(code))
    }
}
```

The icon stays orange-clock for `.waiting` regardless of `lastError` — the secondary text carries the diagnostic. Visually distinct from `.error` (red triangle) but still calling out that something is wrong, not just "first plug-in pending."

### Data flow

```
USB sub-device unplug
    │
    ▼
Apple HAL fires:
  • kAudioHardwarePropertyDevices on system object
  • kAudioDevicePropertyDeviceIsAlive on sub-device id
  • kAudioAggregateDevicePropertyActiveSubDeviceList on aggregate id
    │
    ▼
CoreAudioBackend::onHalPropertyEvent translates to DeviceChangeEvents
    │
    ▼
DeviceChangeWatcher accumulates → hotplug_thread (10 Hz) drains
    │
    ▼
RouteManager::handleDeviceChanges:
  (1) for each kDeviceIsNotAlive: match against r.watched_uids
      → if hit, teardown + WAITING + DEVICE_GONE
  (2) for each kAggregateMembersChanged: queue aggregate UID for re-expand
  (3) refresh dm_; for each running route whose watched_uids contains a
      re-expansion target, rebuild + check; teardown if any member missing
  (4) for each WAITING route: attemptStart (auto-recovery)
    │
    ▼
Engine::hotPlugThreadLoop also calls tickStallWatchdog every 100 ms:
  • bumps stall_ticks for RUNNING routes whose counters froze
  • teardown + WAITING + DEVICE_STALLED at threshold
    │
    ▼
Swift EngineStore polls jbox_engine_get_route_status → status.lastError
    │
    ▼
RouteListView renders red secondary text when waiting AND lastError != JBOX_OK
```

## Error-state semantics

| State path | `last_error` after transition | UI signals |
|---|---|---|
| `addRoute` initial WAITING (device not yet present) | `JBOX_OK` | Orange clock + "Waiting for device" (no red text) |
| F1 sub-device-of-aggregate unplugged (NEW) | `JBOX_ERR_DEVICE_GONE` | Orange clock + red "Device disconnected — waiting for it to return." |
| F1 direct-device unplugged (already shipped) | `JBOX_ERR_DEVICE_GONE` | Same as above |
| Stall watchdog (NEW) | `JBOX_ERR_DEVICE_STALLED` | Orange clock + red "No audio — device stopped responding." |
| 7.6.5 sleep/wake (already shipped) | `JBOX_ERR_SYSTEM_SUSPENDED` | Orange clock + red "Recovering from sleep…" |
| Hard ERROR state (config invalid, channel mismatch, …) | varies | Red triangle + error name (current behavior) |

Recovery in all WAITING-with-error cases rides on the existing `kDeviceListChanged` / `kAggregateMembersChanged` retry-WAITING path. A user-driven stop+start always works as a manual recovery.

## ABI

`JBOX_ENGINE_ABI_VERSION`: 13 → **14**, MINOR additive.

```c
typedef enum {
    JBOX_OK                  = 0,
    ...
    JBOX_ERR_DEVICE_GONE     = 8,
    JBOX_ERR_SYSTEM_SUSPENDED = 9,
    JBOX_ERR_DEVICE_STALLED  = 10,    /* NEW v14 */
} jbox_error_code_t;
```

`jbox_error_code_name` gets a new branch returning `"DEVICE_STALLED"`. Header comment is extended to call the variant out under v14.

`docs/refactoring-backlog.md` § R1 (the `jbox_error_code_t` naming smell) stays as-is — adding another non-error variant doesn't deepen the smell.

## Tests

### C++ (Catch2)

New cases in `Tests/JboxEngineCxxTests/route_manager_test.cpp` (or a new fixture if the file's already large):

- `[route_manager][aggregate_loss] when a route's aggregate loses an active sub-device, route transitions to WAITING + DEVICE_GONE` — uses simulated backend with an aggregate device; emit `kDeviceIsNotAlive(sub-device-uid)`; expect route teardown.
- `[route_manager][aggregate_loss] aggregate members changed without a member loss does not affect a running route` — emit `kAggregateMembersChanged` where the new member set is a superset; expect route stays RUNNING.
- `[route_manager][aggregate_loss] aggregate members changed with a load-bearing member missing tears down running route` — emit `kAggregateMembersChanged` where the active list shrinks past what the route needs.
- `[route_manager][stall] running route transitions to WAITING + DEVICE_STALLED after kStallTicks ticks with no advance` — drive `tickStallWatchdog(now)` 15× without bumping counters; expect transition.
- `[route_manager][stall] running route stays RUNNING when only one counter advances per tick` — bump `framesProduced` each tick, leave `framesConsumed` frozen; expect no stall (per "either-side advance resets" rule).
- `[route_manager][stall] WAITING route does not get stall-checked` — start route, transition to WAITING externally, drive ticks; `stall_ticks` stays 0.
- `[route_manager][stall] state transition into RUNNING resets the stall counter` — induce stall but don't fire; restart; counter back to 0.

New cases in `Tests/JboxEngineCxxTests/device_manager_test.cpp`:

- `[device_manager][aggregate_members] non-aggregate UID returns no members` — pure helper test.
- `[device_manager][aggregate_members] aggregate UID returns its active sub-device UIDs` — pure helper test.

The existing F1 lifecycle tests (`[core_audio][hal_listener_lifecycle]`) stay green unchanged.

### Swift (Swift Testing)

`Tests/JboxAppTests/RouteRowErrorTextTests.swift` (new):

- `errorText is nil when WAITING + JBOX_OK` — initial-WAITING case stays unchanged.
- `errorText is the red string when WAITING + DEVICE_GONE`.
- `errorText is the red string when WAITING + DEVICE_STALLED`.
- `errorText is jbox_error_code_name when state == .error` — regression on the existing path.

The view itself does not need a snapshot test — the conditional secondary text is pure logic.

### Manual hardware acceptance

Mirrors `docs/followups.md` § F1 acceptance, plus three new cases:

1. Build an aggregate in AMS containing the laptop's built-in audio + a USB interface. Start a route on that aggregate. Yank the USB interface. Within ~1 s: route → WAITING + "Device disconnected — waiting for it to return." Re-plug. Within ~1 s: route → RUNNING.
2. Same setup. Start the route. From a Terminal, `kill -STOP <pid-of-coreaudiod>` for ~2 s, then `kill -CONT`. Watchdog should fire within ~1.5 s (DEVICE_STALLED), then the WAITING-route retry should bring it back. *(This is a synthetic stall — easier to reproduce than waiting for a real Apple bug.)*
3. Open Audio MIDI Setup; with a route running on a non-aggregate device, change that device's sample rate. Existing 7.6.4 path should already handle this; verify still RUNNING (or briefly WAITING then RUNNING) and no false stall fires.

## Pitfalls and how the design handles them

- **Stall-watchdog races with attemptStart.** Solved by resetting `last_seen_frames_*` and `stall_ticks` on every successful transition into RUNNING. A route that just started has `stall_ticks = 0`; the next tick reads the freshly advancing counters and resets again. False fires require 1.5 s of complete IOProc inactivity, which by definition is the symptom we want to flag.

- **Watchdog firing during route teardown.** `teardown()` sets `state = STOPPED`. The watchdog skips non-`RUNNING` states. No race.

- **Aggregate members repeated across routes.** Two routes built on the same aggregate share the sub-device UIDs in their respective `watched_uids`. That's fine — the matcher walks each route's set independently; a single `kDeviceIsNotAlive` event tears down both.

- **`watched_uids` staleness vs. AMS reconfiguration.** `kAggregateMembersChanged` triggers `rebuildWatchedUids` for the affected route. `dm_.refresh()` runs first, so the new active list is already cached.

- **Aggregate that *adds* a member.** Members appearing is a no-op for the loss-detection path; existing 7.6.4 retry-WAITING logic picks up the new topology if the route was already WAITING. `rebuildWatchedUids` extends `watched_uids` so future losses on the new member are seen.

- **R1 naming smell deepens?** No new dimension. `JBOX_ERR_DEVICE_STALLED` is the same shape as `JBOX_ERR_DEVICE_GONE` / `JBOX_ERR_SYSTEM_SUSPENDED` — non-error variant in `jbox_error_code_t`. R1's eventual rename absorbs all four together.

- **F3 (debounce) interaction.** F3 lives at `DeviceChangeWatcher::onEvent`. The watchdog lives downstream at `RouteManager::tickStallWatchdog`. Independent paths, no interaction. Adding the watchdog does not change the trade-off for F3.

## Out of scope for this design (followups)

- **`kAudioDevicePropertyDeviceIsRunningSomewhere` as a tiebreaker.** Only worth adding if real-hardware testing reveals false-positive stalls. Capture as a follow-up only if testing demands it.
- **A new `kLogRouteStalled` RT log code.** Optional cleanup — current encoding via `kLogRouteWaiting + value_a=0,value_b=0` is sufficient. Capture in `docs/refactoring-backlog.md` if log-side disambiguation becomes useful for the file-sink consumer.
- **Hot-recover the route automatically on stall (not just on topology change).** Today the WAITING route retries on `kDeviceListChanged` / `kAggregateMembersChanged`. A pure stall (no topology event) recovers via the next list-change tick OR a manual restart. A periodic auto-retry-WAITING tick (e.g. once per second) is a reasonable follow-up if hardware testing shows post-stall recovery is too slow in practice.
- **F1 manual hardware acceptance pass.** Out of scope here; remains the user's gate per `docs/followups.md` § F1.

## References

- `docs/spec.md § 2.7` — per-route HAL buffer-frame-size preference (the only HAL property write Jbox issues).
- `docs/plan.md` Phase 7.6 — full reaction-layer design rationale (idempotent reactions, no debounce, deferred backend wiring).
- `docs/followups.md § F1` — production HAL listener registration; engine half landed 2026-04-28.
- `docs/refactoring-backlog.md § R1` — `jbox_error_code_t` / `last_error` naming smell.
- `Sources/JboxEngineC/control/route_manager.cpp:657` — `handleDeviceChanges` reaction layer.
- `Sources/JboxEngineC/control/engine.cpp:98` — `hotPlugThreadLoop`, the 10 Hz consumer; gains `tickStallWatchdog` call.
- `Sources/JboxApp/RouteListView.swift:244` — current `errorText` accessor; gains the WAITING-with-error branch.
