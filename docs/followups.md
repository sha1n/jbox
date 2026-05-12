# Followups

Pending implementation work that's been deferred from the main path.
Companion to `docs/plan.md` (forward feature work) and
`docs/refactoring-backlog.md` (changes to existing working code for
quality).

Entries here are work items that need to land eventually but were
held back because they fall into one or more of:

- **Non-critical** — correctness already covered by another path;
  this is a refinement.
- **Off the main path** — required for production but not for the
  current sub-phase's exit criteria, often because the work is
  hardware-gated and CI cannot exercise it.
- **Not well-understood** — implementation requires research
  (Apple API quirks, threading model, hardware behaviour) before a
  design commitment makes sense.

Each entry should give the next implementer enough context to start
without re-reading every commit. If the work needs research, name
the questions and the pitfalls explicitly.

---

## F1 — Production HAL property listener registration in `CoreAudioBackend`

**Status:** ✅ Closed 2026-05-12 — hardware acceptance pass complete;
see [`2026-05-12-f1-hardware-acceptance-debug-session.md`](2026-05-12-f1-hardware-acceptance-debug-session.md)
for the run record. Engine wiring landed 2026-04-28; all three
acceptance criteria below verified on real hardware against the
`v0.1.0-alpha` DMG.

### Problem

7.6.4 shipped the engine-side hot-plug auto-recovery (`Device
ChangeWatcher` + `RouteManager::handleDeviceChanges` + the 10 Hz
consumer thread on `Engine`) and the simulator path that drives it
deterministically in CI. The production `CoreAudioBackend::set
DeviceChangeListener` was previously a stub: it stored the callback
pointer but never fired it. Until F1 lands the wiring, the entire
7.6.4 recovery path is dead in production — devices that come and
go trigger no events; routes stuck in WAITING never auto-recover.

The **engine half of F1 has now landed** (this commit): the three
HAL property listeners are installed against the system object +
each enumerated device + each aggregate; their callbacks translate
into `DeviceChangeEvent`s via a pure helper
(`core_audio_hal_translation.hpp`); enumerate() reconciles the
per-device listener set on every refresh. What remains is the
**manual hardware acceptance pass** below — CI cannot exercise real
hot-plug + sample-rate cascades.

### What was done (engine half)

Lands in `core_audio_backend.{hpp,cpp}` +
`core_audio_hal_translation.{hpp,cpp}` (new). Three HAL property
listeners install via `AudioObjectAddPropertyListener` (function-
pointer variant — see Research below for why-not-block) and
translate their callbacks into `DeviceChangeEvent` invocations:

- `kAudioHardwarePropertyDevices` on `kAudioObjectSystemObject` →
  emits `kDeviceListChanged` with empty `uid`.
- `kAudioDevicePropertyDeviceIsAlive` per enumerated device →
  reads back the current value and emits `kDeviceIsNotAlive` with
  the device's UID when it reads as 0.
- `kAudioAggregateDevicePropertyActiveSubDeviceList` per
  aggregate device → emits `kAggregateMembersChanged` with the
  aggregate's UID.

`enumerate()` reconciles the per-device listener set on every
call: it publishes a fresh `id_to_uid_` map under the lock,
releases the lock, then diff-installs listeners (remove for
AudioObjectIDs that disappeared, add for AudioObjectIDs that
arrived). The diff is naturally idempotent — repeated
`enumerate()` calls on a stable system are a no-op for listener
state. `~CoreAudioBackend()` clears the listener callback under
the lock and then removes every installed listener.

Translation lives in `core_audio_hal_translation.hpp` as a pure
helper, covered by 6 Catch2 cases under
`[core_audio][hal_translation]` (every selector branch — including
the IsAlive=1 no-event case, the empty-uid IsAlive=0 case, and
unrelated-selector hardening). 4 additional integration cases
under `[core_audio][hal_listener_lifecycle]` exercise the
register / re-register / enumerate-with-listener / destructor
sequence on the real HAL — these catch typos and argument-shape
bugs in the Apple API calls but cannot fire callbacks (CI has no
hot-plug events).

### Research outcomes (and the still-open questions)

1. **Callback thread.** `AudioObjectAddPropertyListener` (function-
   pointer variant) fires its callback on a HAL-internal thread.
   Apple has historically used multiple threads in this pool for
   different objects, so two callbacks for different listeners can
   run concurrently. The implementation acquires
   `callback_state_mutex_` in *shared* mode briefly to read
   `device_change_cb_` + `id_to_uid_`, then releases before doing
   any per-event work (including the `kAudioDevicePropertyDeviceIs
   Alive` readback, which can take arbitrary time). **Decision:**
   function-pointer variant chosen over the block variant — the
   shared-mutex pattern gives us control over the read-side without
   pulling in a `dispatch_queue_t` and the surrounding lifetime
   plumbing. The shipped code is correct under any HAL-thread
   topology; the trade-off is "sparse contention on a
   `std::shared_mutex`" over "one extra dispatch queue we own."

2. **Listener queue control.** Skipped — see (1). The block API +
   dispatch queue would have given a known-thread context; the
   shared-mutex pattern achieves the same correctness with less
   machinery. If the trace from F3's investigation shows actual
   contention, revisit.

3. **`kAudioDevicePropertyDeviceIsAlive` semantics.** Open. The
   shipped code reads `IsAlive` back inside the callback and only
   emits `kDeviceIsNotAlive` when the value is 0 — so spurious
   "alive→alive" callbacks are a no-op. The reverse edge is picked
   up by the `kAudioHardwarePropertyDevices` listener (a device
   coming back is a list-changed event). **Manual acceptance test
   #1 below pins this end-to-end.**

4. **Aggregate sub-device list mutations during sample-rate
   cascades.** Open. The reaction layer is idempotent (handle
   DeviceChanges collapses N kDeviceListChanged into one
   `dm_.refresh()` per drain) — so even a noisy cascade is bounded
   to one refresh per 10 Hz consumer-thread tick. **Manual
   acceptance test #3 below confirms ≤2 reactions per cascade; if
   it shows thrash, F3 becomes blocking, not optional.**

### Pitfalls (and how the shipped code handles them)

- **HAL callback re-entrance.** Resolved. The callback path
  (`onHalPropertyEvent`) only reads cross-thread state under the
  shared lock and emits to the watcher's stored callback — no
  call back into `enumerate()` and no further HAL property
  registration. Re-entrance from inside the listener block can't
  happen. The `IsAlive` readback is a single
  `AudioObjectGetPropertyData` call against the firing object; not
  re-entrant.

- **Race against `~CoreAudioBackend()` (and indirectly `~Engine`).**
  Resolved by the no-lock-during-Apple-calls pattern combined with
  Apple's documented "Remove blocks until in-flight callbacks
  complete" semantics. Destructor sequence: (1) clear
  `device_change_cb_` under exclusive lock so any callback that
  *just* acquired the shared lock observes a null cb and bails;
  (2) call `removeAllListeners()` outside the lock — Apple Remove
  blocks on any in-flight callback that started before step (1),
  but those callbacks are guaranteed to release the shared lock
  before completing (the shared section is short and contains no
  Apple calls), so Remove returns; (3) destroy the rest. Engine's
  destruction order (sampler stop → hot-plug stop → power watcher
  cleanup → drainer stop → member destruction) is unchanged; F1
  added no new Engine-level lifetimes.

- **Why no shutdown flag.** F1's research suggested a "shutting
  down" flag the listener checks. Skipped — clearing
  `device_change_cb_` under the shared-mutex contract achieves the
  same correctness and the destructor's `removeAllListeners()`
  call already blocks on any in-flight callback. The flag would
  duplicate one or both of those mechanisms.

- **Deadlock between Apple Remove and the callback path.**
  Resolved by the rule: the control thread NEVER holds
  `callback_state_mutex_` while calling Apple Add/Remove. Apple's
  Remove blocks on in-flight callbacks; in-flight callbacks
  acquire the shared lock briefly. If we held the exclusive lock
  during Remove, a callback firing on another HAL thread would
  block on the shared lock; Remove would block on the callback —
  hung. Search for "callback_state_mutex_" in
  `core_audio_backend.{hpp,cpp}` for the contract notes.

- **Listener registration cost.** Per-device listeners scale with
  device count. On a 30-device system, `setDeviceChangeListener`
  fires 30 IsAlive registrations (and N more on aggregates). Each
  is a single Apple call. The diff-style reconcile in
  `enumerate()` only adds/removes for the actual delta, so
  steady-state UI device-list refreshes are no-ops. Flag if traces
  show this is too slow.

- **The `uid` empty / non-empty contract** in `DeviceChangeEvent`
  — Production fires `kDeviceListChanged` with empty `uid` (the
  system-object listener has no device subject); the simulator
  fires it with the device's UID. `RouteManager::handle
  DeviceChanges` accepts both: `kDeviceListChanged` triggers a
  `dm_.refresh()` + retry-WAITING pass regardless of `uid`. The
  contract is "uid is informative, never load-bearing for
  list-changed."

### Acceptance / verification

Manual hardware tests, since CI can't exercise this path:

1. Engine running with a route that uses an aggregate device.
   Hot-unplug a sub-device of the aggregate. Within ~200 ms:
   route should transition to WAITING with `last_error =
   JBOX_ERR_DEVICE_GONE`. Re-plug the device. Within ~1 s:
   route should auto-recover to RUNNING.

2. Same setup, but yank the source USB interface entirely.
   Same outcome.

3. With Audio MIDI Setup, change the sample rate of an aggregate's
   active member. Verify a single auto-recovery pass — not a
   thrash of N starts/stops as the cascade fires N
   property-list-changed events. (If this thrashes, F3 debounce
   becomes blocking, not optional.)

**2026-05-02 update.** F1's acceptance test #1 (yank a sub-device of a running aggregate) now has simulator-path regression coverage under the `[route_manager][aggregate_loss]` Catch2 tag (6 cases total — direct sub-device IsNotAlive, kAggregateMembersChanged with member loss / without member loss, idempotency on repeats, unrelated-aggregate non-effect, plus the skip-set mechanism that keeps a torn-down route from being silently re-promoted by the same-drain WAITING-retry pass). Landed under sub-phase 7.6.6 (see `docs/plan.md` § Phase 7.6 → sub-phase 7.6.6). Production HAL pass remains the user's gate; F1's status stays "🚧 Engine landed; awaiting manual hardware acceptance."

**2026-05-12 update.** Hardware acceptance pass complete against the `v0.1.0-alpha` DMG. All three criteria above verified on a two-device rig (USB source + Thunderbolt destination, plus an aggregate wrapping both). Test #3 single-passed — F3 stays optional, not promoted to blocker. A side-anomaly on the post-replug latency pill was investigated and ruled out as an engine bug; full run record + investigation chronology at [`2026-05-12-f1-hardware-acceptance-debug-session.md`](2026-05-12-f1-hardware-acceptance-debug-session.md).

### References

- Existing simulator path: `[device_change_watcher]` (3 cases) +
  `[sim_backend][device_change]` (3 cases) +
  `[route_manager][device_loss]` (6 cases). Production behaviour
  should match these.
- Apple docs: `AudioObjectAddPropertyListener`,
  `kAudioHardwarePropertyDevices`,
  `kAudioDevicePropertyDeviceIsAlive`,
  `kAudioAggregateDevicePropertyActiveSubDeviceList`.
- 7.6.4 deviation entry in `plan.md` and the new F1 deviation in
  the same section for the design choices.
- `Sources/JboxEngineC/control/core_audio_backend.{hpp,cpp}` —
  shipped implementation; search "F1" or "callback_state_mutex_"
  for the contract notes.
- `Sources/JboxEngineC/control/core_audio_hal_translation.{hpp,cpp}`
  — pure translator + its `[core_audio][hal_translation]` test
  group.

---

## F2 — Production `MacosPowerEventSource` wrapping `IORegisterForSystemPower`

**Status:** ⏳ Pending. Hardware-gated; lands as a focused commit
after manual sleep/wake testing.

### Problem

7.6.5 shipped the engine-side sleep/wake auto-recovery
(`PowerStateWatcher` + `RouteManager::prepareForSleep` /
`recoverFromWake` / `tickWakeRetries` + the bounded-retry
schedule + Engine's optional source plumbing) and the
`SimulatedPowerEventSource` test fixture. No production
`MacosPowerEventSource` exists yet. Until it does, real-system
sleep/wake events do not reach the engine, and routes that were
running before sleep stay stuck in RUNNING-but-actually-silent
after wake until the user manually stops + restarts them.

### What to do

Create a new `Sources/JboxEngineC/control/macos_power_event_
source.{hpp,cpp}` implementing `IPowerEventSource`. Use Apple's
IOKit power management API:

- `IORegisterForSystemPower(refCon, &notify_port, callback,
  &notifier)` to register. Returns an `io_connect_t` handle.
- `IODeregisterForSystemPower(&notifier)` + release the
  `io_connect_t` in the destructor.
- `IOAllowPowerChange(io_connect, ack_token)` from inside the
  callback for `kIOMessageSystemWillSleep` events. The
  `ack_token` is the `messageArgument` parameter passed to the
  callback.

Translate IOPM messages into `PowerStateEvent`:

- `kIOMessageSystemWillSleep` → fire `kWillSleep` through the
  registered listener. After the listener returns, call
  `IOAllowPowerChange`.
- `kIOMessageSystemHasPoweredOn` → fire `kPoweredOn`.
- `kIOMessageCanSystemSleep` (idle sleep negotiation) → for v1,
  pass through with `IOAllowPowerChange` immediately. We don't
  veto sleep.
- `kIOMessageSystemWillNotSleep` (sleep cancelled) → no-op for
  the listener; nothing to undo because we haven't called
  prepareForSleep yet at that point.

The notification port (`IONotificationPortRef` from
`IONotificationPortGetRunLoopSource`) needs a CFRunLoop or a
dispatch queue to deliver events. See research below.

### Research needed

1. **Run loop / dispatch queue.** `IONotificationPortGet
   RunLoopSource` returns a `CFRunLoopSourceRef` for CFRunLoop
   integration; `IONotificationPortSetDispatchQueue` lets you
   attach a `dispatch_queue_t` instead. Engine's existing
   `hotplug_thread_` is a `std::thread` running a `sleep_until`
   loop — not a CFRunLoop and not a dispatch queue host. So:
   - Option A: spawn a separate thread for IOPM and run a
     CFRunLoop on it. Adds another thread to Engine.
   - Option B: use a dedicated `dispatch_queue_t` (likely a
     serial queue owned by `MacosPowerEventSource`). Apple's
     dispatch system manages the underlying thread; the source
     sees the callback from a known queue. Cleaner.
   - Option C: piggyback on the main thread's run loop. Couples
     the engine to the host's main thread; awkward for the CLI /
     headless flows.

   B is probably right. Verify by reading Apple's IOPMLib
   sample code.

2. **Ack contract on `kIOMessageCanSystemSleep`.** This is a
   *negotiation* message — apps can veto idle sleep. Confirm
   that calling `IOAllowPowerChange` immediately is a clean
   "vote yes, sleep is fine" and doesn't have side effects.

3. **Wake-from-dark-sleep vs full wake.** macOS distinguishes
   "dark wake" (background services run, display stays off) from
   user-initiated wake. Does `kIOMessageSystemHasPoweredOn` fire
   in both cases? If yes, we'll attempt to restart routes during
   dark wake, which is probably fine but worth confirming with
   a trace (running routes during dark wake produces audio that
   nobody will hear; not harmful, just wasteful).

4. **Acknowledgement timeout.** macOS allows ~30 s before it
   forces sleep regardless. The 7.6.5 design has the listener
   complete prepareForSleep synchronously (which calls teardown
   on every running route — fast, no I/O), so we should be well
   under the timeout. But if a route's teardown is unexpectedly
   slow (mux quiescence wait under contention?), the ack could
   slip. Add a hard cap — log + ack-anyway after some budget.

### Pitfalls

- **Late `kWillSleep` after Engine destruction.** Same race as
  7.6.4's HAL listeners: the IOPM dispatch thread could fire
  `kWillSleep` between the engine clearing the sleep handler
  (~Engine line that calls `power_watcher_->setSleepHandler({})`)
  and the watcher destruction (`~PowerStateWatcher`). The
  watcher's `onEvent` is mutex-protected internally; the empty
  `std::function` handler-call is a no-op; the
  `source.acknowledgeSleep()` call into the production source
  must remain valid until the source itself is destroyed. The
  destruction order in Engine declares power_watcher_ before rm_
  but power_source_ before power_watcher_ — confirm this is
  sound. Verify with TSan + an explicit destruction-during-sleep
  test if possible.

- **Run-loop thread shutdown.** If we go with option A
  (CFRunLoop), we need to stop the run loop cleanly during
  Engine destruction. `CFRunLoopStop()` from another thread
  works but the run loop must actually be running. Add a
  signalling primitive (semaphore?) so the destructor can
  block-wait for the IOPM thread to exit.

- **`IOAllowPowerChange` from wrong thread.** Apple's docs are
  ambiguous on whether the ack must come from the run loop /
  dispatch queue that received the callback. Treat it as
  thread-restricted until proven otherwise — call it from
  inside the callback, never elsewhere.

### Acceptance / verification

Manual hardware tests:

1. Start the app with a running route. Run `pmset sleepnow`.
   System sleeps. After wake, observe the route auto-recovers
   within ~600 ms (the 3-attempt bounded-retry budget). Logs
   show:
     - `kLogRouteWaiting` on prepareForSleep,
     - then `kLogRouteStarted` after recoverFromWake's first or
       second attempt succeeds.

2. Start with two running routes, one on the laptop's built-in
   audio (always present), one on a USB interface. Sleep.
   Unplug the USB interface during sleep. Wake. Built-in route
   should auto-recover; USB route should burn through 3
   attempts and stay in WAITING + SYSTEM_SUSPENDED.

3. Sleep with no running routes. Wake. Verify nothing crashes
   and `recoverFromWake` is a no-op (the existing test pins
   this; production should match).

### References

- Existing simulator path: 3 `[sim_power]` cases + 3
  `[power_state_watcher]` cases + 6 `[route_manager][sleep_wake]`
  cases. Production behaviour should match these.
- Apple docs: IOPMLib (`IORegisterForSystemPower`,
  `IODeregisterForSystemPower`, `IOAllowPowerChange`,
  `kIOMessageSystem*`).
- Sample code: Apple's `PowerManagement` examples (formerly in
  Developer Library; check current location).
- 7.6.5 deviation entry in `plan.md` for the design rationale.
- `Sources/JboxEngineC/control/simulated_power_event_source.{hpp,cpp}`
  for the shape `MacosPowerEventSource` should implement.
- `Sources/JboxEngineC/control/power_event_source.hpp` for the
  contract.

---

## F3 — `DeviceChangeWatcher` event debounce

**Status:** ⏳ Pending. Non-critical perf refinement. Add when
real-hardware traces show it's worth the complexity, or when F1
lands and a sample-rate cascade trace reveals thrash.

### Problem

7.6.4's `DeviceChangeWatcher::onEvent` pushes every backend event
onto its queue without dedup. `RouteManager::handleDeviceChanges`
already coalesces correctness — multiple `kDeviceListChanged`
events in a single drain trigger a single `dm_.refresh()` + retry
pass — but the watcher itself can accumulate, say, 20 events
during a sample-rate cascade and the drain still iterates them
all. Functionally fine; wasteful at the queue layer.

The plan checklist for 7.6.4 originally called for "~200ms
coalescing window" inside the watcher. The shipped design pushed
coalescing into the *reaction* layer (idempotent reactions) so
tests stay deterministic without an injectable clock. F3 would
add the watcher-layer dedup as a perf refinement.

### What to do

Add per-uid timestamp dedup at `DeviceChangeWatcher::onEvent`:

- Track the most recent timestamp per `(kind, uid)` pair.
- If a new event arrives within `kDebounceWindow` of the previous
  matching event, drop it.
- Window default: ~200 ms (the value the plan named originally).

Or, alternative: a single-deadline-per-uid model — the first
event on a uid starts a window; further events on the same uid
within the window are dropped; after the window expires, the
*latest* event in that window is forwarded. This is a "trailing
edge" debounce and matches the typical signal-processing meaning
better.

### Research needed

1. **What does a real sample-rate cascade actually look like?**
   Run F1, attach a trace logger to the `DeviceChangeWatcher`'s
   `onEvent` (timestamp + kind + uid), change a connected
   aggregate's sample rate via Audio MIDI Setup, count the
   events. If it's < 5 per cascade, debounce is unnecessary —
   the cost of the existing per-event refresh is small. If it's
   30+, debounce is worth the complexity.

2. **Per-uid vs global window.** Multiple sub-devices might
   cascade in parallel. A global 200 ms window could collapse
   meaningful cross-device events; per-uid is safer but more
   state. Decide based on the trace from (1).

3. **Trailing-edge vs leading-edge dedup.** Leading-edge: the
   first event in a window forwards immediately, subsequent
   events in the window drop. Trailing-edge: events in a window
   accumulate; the *last* one forwards on window expiry. Trailing
   loses ~200 ms of reactivity but coalesces cleaner. Leading
   wins for responsiveness on the first event of a burst. Probably
   leading is right for our use case (we want fast reaction; the
   coalesce is just to avoid waste, not to wait for stability).

### Pitfalls

- **Tests become timing-dependent.** `DeviceChangeWatcher`'s
  current tests use `std::chrono::steady_clock` only via the
  watcher's `drain()` (which doesn't read time). Adding debounce
  means the watcher reads `steady_clock::now()`. Tests need an
  injectable clock or a debug bypass.

- **Dropping a "real" event.** If a device flaps (remove → add →
  remove → add) within the window, naive dedup might drop the
  last "remove" and leave routes in an incorrect state. The
  reaction layer's idempotent nature mostly covers this, but
  verify with a flap-during-window test.

- **Memory footprint.** Per-uid timestamp tracking grows with the
  number of devices. A `std::unordered_map<std::string,
  std::chrono::steady_clock::time_point>` is fine for any
  realistic device count (≤100) but trim entries periodically if
  paranoid.

### Acceptance / verification

- Unit test: inject a clock; fire 5 `kDeviceListChanged` on the
  same uid within 50 ms simulated time; drain should return 1 or
  2 events (depending on leading/trailing choice), not 5.
- Manual: with F1 wired, change a sample rate via Audio MIDI
  Setup; trace shows ≤2 reactions per cascade (was N).
- No regression on the existing `[route_manager][device_loss]`
  cases — they don't depend on debounce, but verify.

### References

- 7.6.4 deviation entry in `plan.md` (the "idempotent reactions
  over a 200 ms timer-based debounce" choice).
- Existing watcher: `Sources/JboxEngineC/control/device_change_
  watcher.{hpp,cpp}`.
- Mental model for "leading vs trailing debounce":
  https://css-tricks.com/debouncing-throttling-explained-examples/
  (or any equivalent reference).

---

## F4 — Intermittent SIGSEGV in `[multi_route][stress]`

**Status:** ⏳ Pending. Pre-existing flake (observed on baseline
master before F1 landed). Captured here to prevent it from being
re-discovered every time `make verify` flakes red.

### Problem

`Tests/JboxEngineCxxTests/multi_route_stress_test.cpp` defines a
case at line 121 ("Multi-route stress: rapid route start/stop while
RT is dispatching") that segfaults intermittently — observed at
roughly 1-in-5 frequency in isolation on baseline master, often as
the first run after a fresh build. The suite as a whole passes on
re-run; the segfault is reproducible only by running the case
multiple times. The crash hits inside the case body — the
`stop.store(true)` / `rt_thread.join()` sequence around line ~150
or the destructor sequence at end-of-scope — and Catch2 reports
"SIGSEGV - Segmentation violation signal" with no stack frame.

The test uses `SimulatedBackend` only; F1 (CoreAudioBackend HAL
listener wiring) does not affect this code path. Stash-and-rerun
on baseline master with F1 absent reproduces the same crash at the
same case.

### What to do

Diagnose. Likely candidates:

1. The RT thread's `deliverBuffer("src")` / `deliverBuffer("dst")`
   loop races with the main thread's rapid `attachInput` /
   `detachInput` cycles. The mux's RCU-style active-route swap
   should make this safe (the comment at line 122-124 says so),
   but the segfault suggests a hole.

2. The anchor-route teardown order at scope exit may run while
   the RT thread is still mid-tick; the test does
   `rt_thread.join()` early but the route's internal teardown
   path destroys the IOProc record under the RouteManager
   destructor. Verify the order is safe under contention.

3. Could be a use-after-free in the simulated backend's
   `slot.input_callback` / `slot.output_callback` storage. The
   stress phase's rapid route mutations could leave a stale
   pointer in the callback slot when the RT thread still holds a
   `BackendDeviceInfo*` from a slot that was just reset.

Suggested approach: run the case in isolation under TSan (`swift
run --sanitize=thread JboxEngineCxxTests '[multi_route][stress]'`)
in a loop; collect the first sanitizer hit, work backwards from
the racing pair.

### Research needed

1. **Is the segfault TSan-detectable?** If yes, TSan output names
   the racing pair and the diagnosis is straightforward. If no,
   the bug is single-threaded (e.g., a destruction ordering
   issue) and TSan won't help.

2. **Does the segfault reproduce in `--sanitize=address`?** If
   yes, ASan tells us exactly which read/write violates which
   allocation. If no, the bug is concurrent (TSan should catch
   it).

3. **Was this introduced with sub-phase 7.6.3's bool-return
   contract on `closeCallback`?** That commit changed the
   teardown path to retain the IOProc record on failure;
   under stress, the test could be hitting an unintended
   "preserved-but-unsubscribed" state. Walk the commit history
   on `Tests/JboxEngineCxxTests/multi_route_stress_test.cpp` and
   `Sources/JboxEngineC/control/route_manager.cpp` from the d418562
   stress-test introduction forward.

### Pitfalls

- **Quarantining instead of fixing.** Tempting to mark the case
  `[!mayfail]` or skip it. Don't — the stress case exists
  because the engine has had real concurrency bugs there before;
  silencing it loses the regression net.

- **Adding sleeps to "stabilise" the test.** Anti-pattern. The
  fix should make the engine code robust under the contention
  the test creates, not slow the test down so the contention
  doesn't manifest.

### Acceptance / verification

- Cause identified and documented (TSan hit, ASan hit, or root-cause
  analysis from inspection).
- Engine code fix lands with a regression test that fails
  deterministically against the broken code.
- 100 consecutive runs of `swift run JboxEngineCxxTests
  '[multi_route][stress]'` pass.
- `make verify` no longer flakes on this case.

### References

- The test: `Tests/JboxEngineCxxTests/multi_route_stress_test.cpp`
  line 121 ("Multi-route stress: rapid route start/stop while RT
  is dispatching"), introduced by commit `d418562` (engine phase 5
  #4: seq-based RCU quiescence + concurrent stress tests).
- Observed on baseline `master` (commit `ae6cf17`) with no F1
  changes applied: stash-and-rerun reproduced the crash at run 4
  of 5.

---

## F5 — `clang-tidy` + `swiftlint` CI gates

**Status:** ⏳ Pending. Promised in the original Phase 1 / Phase 2
task lists and never landed; deferred from Phase 9 to the
`v0.2.0+` window when `v0.1.0-alpha` was cut without it.

### Problem

The lint surface JBox actually ships today is `scripts/rt_safety_
scan.sh` (RT-subtree banned-symbol grep), ThreadSanitizer in
`make verify`, the Catch2 + Swift Testing suites, and
compiler warnings at `-Werror`-equivalent strictness. That covers
the load-bearing real-time invariants and most logic-level bugs,
but the project ships C++ control-thread code (`Sources/JboxEngine
C/control/`) and Swift app code (`Sources/JboxEngineSwift/`,
`Sources/JboxApp/`) without a conventional lint pass against
either. A real-release gate is conventional; the `v1.0.0` exit
criteria in `docs/plan.md § Phase 9` list it explicitly.

### What to do

Per `docs/plan.md § Phase 9` (lines 1039–1045):

1. `bear -- swift build` (or equivalent) wired into a Makefile
   target so `compile_commands.json` is reproducible on a fresh
   clone and in CI.
2. `.clang-tidy` config focused on `Sources/JboxEngineC/control/`
   only. The `rt/` subtree stays under `rt_safety_scan.sh`'s
   narrower contract — clang-tidy's general-purpose checks would
   raise too much noise about `noexcept` / containers / etc.
   that the RT scanner already rules out at a coarser level.
   Tune the check set to zero false positives on the current
   tree before flipping CI to error-level.
3. `.swiftlint.yml` config covering `Sources/JboxEngineSwift/`
   and `Sources/JboxApp/`. Tune rule set to zero false positives.
4. `make lint` Makefile target invoking both linters.
5. GitHub Actions step in `ci.yml` that runs `make lint` and
   fails on warnings.
6. Any pre-existing findings either fixed or explicitly
   suppressed at the call site with justification.

### Pitfalls

- **Flipping CI to error-level before tuning.** A noisy
  configuration burns one or two PRs on triage churn; pin the
  ruleset against the current tree first, fix or suppress the
  baseline, then make CI red.
- **Letting clang-tidy's RT-equivalent checks duplicate
  `rt_safety_scan.sh`.** Keep the boundaries clear — RT scanner
  for the `rt/` subtree, clang-tidy for `control/`. Don't try to
  fold one into the other.
- **Adding lint to `make verify` while leaving the F4 stress
  flake unfixed.** A flaky pre-tag gate is worse than no gate;
  resolve F4 (or quarantine the case explicitly with rationale)
  before the gate goes red on transient noise.

### Acceptance / verification

- `make lint` succeeds on `master` with zero warnings.
- CI's `lint` step runs on every push to `master` and on PRs.
- A deliberate violation (e.g., introduce an unused variable in
  a Swift file or a `nullptr`-dereference path in a C++ file)
  fails CI.

### References

- `docs/plan.md § Phase 9` lines 1039–1045 — original task list.
- `scripts/rt_safety_scan.sh` — the in-tree static gate the lint
  pass complements rather than replaces.

---

## F6 — Phase 9 real-hardware acceptance (soak / latency / stress)

**Status:** ⏳ Pending. Procedures documented under `docs/
testing/`; runs on real hardware are the gate to `v1.0.0`. `v0.1.
0-alpha` ships without these runs by deliberate decision; the
limitation is surfaced in the Release body.

### Problem

`docs/testing/soak.md`, `docs/testing/latency.md`, and `docs/
testing/stress.md` document the three real-hardware acceptance
procedures that gate the `v1.0.0` tag (Phase 9 exit criteria).
None of them have been run end-to-end on real hardware and
recorded against the current `master`. The closest signal we
have today is the project owner's pre-tag manual smoke pass on
`v0.1.0-alpha`'s release commit — sufficient for an alpha but
not for `v1.0.0`.

### What to do

Three independent runs, each producing a recorded artifact:

1. **Soak.** Follow `docs/testing/soak.md`. Run a representative
   route for ≥ 30 minutes on real hardware (CLI canonical, GUI
   variant optional). Verify zero dropouts in logs, drift
   tracker in band, no clipping, plausible meter values. Record
   in the template at the end of `soak.md`.
2. **Latency.** Follow `docs/testing/latency.md`. Loopback
   test: patch a destination output back into a source input
   via cable; inject a test pulse; measure round-trip latency.
   Confirm within ±1 ms of theoretical expectation.
3. **Stress / disconnect.** Follow `docs/testing/stress.md`.
   Start and stop routes rapidly. Unplug and replug devices.
   Verify graceful recovery, no crashes, no stuck routes.

This work pairs naturally with closing **F1** (hot-plug
hardware acceptance) — the stress / disconnect run is the
acceptance pass for F1. Do not schedule them separately.

### Pitfalls

- **Treating the simulator regression coverage as a substitute.**
  Simulator passes prove the engine logic; real hardware can
  expose timing, IOProc-scheduler, and HAL-property-cascade
  behaviour the simulator cannot reproduce.
- **Running once and declaring done.** A 30-minute soak that
  passed once on a calm system may fail on the same system
  when other audio apps are running. The procedure templates
  ask for environmental notes for a reason.
- **Skipping log review.** "No audible problem" is not the
  acceptance criterion — `~/Library/Logs/Jbox/Jbox.log` (or
  `JboxEngineCLI.log`) must show zero `evt=underrun` /
  `evt=overrun` for the soak to pass.

### Acceptance / verification

- All three procedure docs have a recorded run against the
  same `master` SHA.
- Recorded artifacts live in `docs/testing/<procedure>-runs/`
  (or attached to the `v1.0.0` release notes).
- F1 hot-plug hardware acceptance passes in the same session.

### References

- `docs/plan.md § Phase 9` — exit criteria + task list.
- `docs/testing/soak.md`, `docs/testing/latency.md`,
  `docs/testing/stress.md` — the procedures themselves.
- `docs/followups.md § F1` — pairs with the stress / disconnect
  run.
