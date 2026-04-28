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

**Status:** ⏳ Pending. Hardware-gated; lands as a focused commit
after manual testing on a real device topology.

### Problem

7.6.4 shipped the engine-side hot-plug auto-recovery (`Device
ChangeWatcher` + `RouteManager::handleDeviceChanges` + the 10 Hz
consumer thread on `Engine`) and the simulator path that drives it
deterministically in CI. The production `CoreAudioBackend::set
DeviceChangeListener` is currently a stub: it stores the callback
pointer but never fires it. Until this is wired, the entire 7.6.4
recovery path is dead in production — devices that come and go
trigger no events; routes stuck in WAITING never auto-recover.

### What to do

In `CoreAudioBackend::setDeviceChangeListener`, install three
HAL property listeners and translate their callbacks into
`DeviceChangeEvent` invocations on the stored callback:

- `kAudioHardwarePropertyDevices` on `kAudioObjectSystemObject` →
  emit `kDeviceListChanged` with empty `uid` (the change isn't
  scoped to one device — it's an enumeration shift).
- `kAudioDevicePropertyDeviceIsAlive` per enumerated device →
  emit `kDeviceIsNotAlive` with the affected device's UID when
  the new value reads as 0.
- `kAudioAggregateDevicePropertyActiveSubDeviceList` per
  aggregate device → emit `kAggregateMembersChanged` with the
  aggregate's UID.

Re-installation on `kAudioHardwarePropertyDevices` events: when
new devices appear we need to install per-device + per-aggregate
listeners on them too. The natural shape is to re-walk
`enumerate()` after each `kAudioHardwarePropertyDevices` event
and reconcile the listener set.

Teardown: `AudioObjectRemovePropertyListener` for every installed
listener in `~CoreAudioBackend()` and on `setDeviceChangeListener
(nullptr, nullptr)`.

### Research needed

1. **Callback thread.** Does `AudioObjectAddPropertyListener`
   invoke its callback on a dedicated HAL thread, on the caller's
   thread, or on a system dispatch queue? The answer determines
   whether `DeviceChangeWatcher`'s mutex is contended, whether we
   need to repost to a known queue, and whether the simulator's
   synchronous-listener-on-test-thread model is a faithful enough
   approximation.

2. **Listener queue control.** Apple supports
   `AudioObjectAddPropertyListenerBlock` which takes a
   `dispatch_queue_t`. If the function-pointer variant gives no
   thread control, the block variant probably should be used to
   serialise on a chosen queue (perhaps a dedicated serial queue
   owned by `CoreAudioBackend`).

3. **`kAudioDevicePropertyDeviceIsAlive` semantics.** Does it fire
   for both transitions (alive → not-alive *and* the reverse)?
   The 7.6.4 reaction layer cares about both — `kDeviceIsNotAlive`
   on transition-to-zero, `kDeviceListChanged` on transition-back.
   Verify and document.

4. **Aggregate sub-device list mutations during sample-rate
   cascades.** Apple's docs are vague on whether
   `kAudioAggregateDevicePropertyActiveSubDeviceList` fires for
   transient list changes during a sample-rate cascade. If it
   does, we need the F3 debounce (below) before this is shippable.

### Pitfalls

- **HAL callback re-entrance.** If a property listener callback
  triggers further property reads (e.g., the listener calls
  `enumerate()` which calls `AudioObjectGetPropertyData`), Apple
  may serialise differently than expected. Avoid re-entrance —
  the watcher's `onEvent` should just store the event and return.

- **Race against `~Engine`.** Engine destruction's order is:
  stop sampler thread → stop hot-plug thread → clear
  `power_watcher_`'s sleep handler → drainer stop → member
  destruction (which destroys `watcher_` after `rm_`). HAL
  callbacks could fire between the hot-plug thread stopping and
  the watcher being destroyed. The watcher's destructor unregisters
  its callback, but if HAL fires a callback that's already
  in flight on another thread, the watcher's mutex must protect
  against use-after-free. Verify the mutex acquire order; consider
  a "shutting down" flag the listener checks before touching the
  queue.

- **Listener registration cost.** Per-device listeners scale with
  device count. On a system with 30+ enumerated devices (Audio
  MIDI Setup users with dozens of aggregates / virtual devices)
  this is 30+ kAudioDevicePropertyDeviceIsAlive listeners. Should
  be fine; flag if traces show otherwise.

- **The `uid` empty / non-empty contract** in `DeviceChangeEvent`
  — `kDeviceListChanged` may have empty uid (system-wide event)
  or non-empty (specific device went away). The simulator's
  `simulateDeviceRemoval` fires kDeviceListChanged with the
  removed UID; production may fire it with empty uid. The
  `RouteManager::handleDeviceChanges` reaction handles both —
  `kDeviceListChanged` triggers a `dm_.refresh()` + retry-WAITING
  pass regardless of `uid`. Confirm production semantics match.

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

### References

- Existing simulator path: `[device_change_watcher]` (3 cases) +
  `[sim_backend][device_change]` (3 cases) +
  `[route_manager][device_loss]` (6 cases). Production behaviour
  should match these.
- Apple docs: `AudioObjectAddPropertyListener`,
  `kAudioHardwarePropertyDevices`,
  `kAudioDevicePropertyDeviceIsAlive`,
  `kAudioAggregateDevicePropertyActiveSubDeviceList`.
- 7.6.4 deviation entry in `plan.md` for the design rationale and
  the deferred-bridge note.
- `Sources/JboxEngineC/control/core_audio_backend.cpp` — the stub
  that needs filling in.

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
