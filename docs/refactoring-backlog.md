# Refactoring backlog

Things in the codebase we want to fix but have deferred to a focused
refactoring pass rather than folding into a feature slice. The
companion to `docs/plan.md` (which tracks forward feature work) and
`docs/spec.md` (which tracks the design contract).

Each entry should describe the **symptom**, the **root cause**, the
**surface area** to update if/when we tackle it, and any **open
questions** that need a decision before refactoring. Entries do
*not* propose a final answer — they're prompts for a future
focused-sprint conversation.

---

## R1 — `jbox_error_code_t` carries non-error variants

**Status:** open. Surfaced 2026-04-28 during 7.6.4 work; deferred so
the feature work could land without an ABI rename absorbing the slice.

### Symptom

Across the C ABI, the Swift wrapper, and the engine internals, an
"error code" type carries `JBOX_OK` as one of its variants:

```c
typedef enum {
    JBOX_OK                      = 0,
    JBOX_ERR_INVALID_ARGUMENT    = 1,
    JBOX_ERR_DEVICE_NOT_FOUND    = 2,
    JBOX_ERR_MAPPING_INVALID     = 3,
    JBOX_ERR_RESOURCE_EXHAUSTED  = 4,
    JBOX_ERR_DEVICE_BUSY         = 5,
    JBOX_ERR_NOT_IMPLEMENTED     = 6,
    JBOX_ERR_INTERNAL            = 7,
    JBOX_ERR_DEVICE_GONE         = 8     /* added 2026-04-28 in v12 */
} jbox_error_code_t;
```

This produces locally-incoherent reads at the use sites:

```c
jbox_route_status_t status;
status.last_error = JBOX_OK;          /* "last error: ok" -- contradiction */
```

```swift
let status = try engine.pollStatus(id: id)
if status.lastError == .ok { ... }    /* "if last error is ok" -- contradiction */
```

The asymmetric variant naming is part of the same smell:
`JBOX_OK` carries no `_ERR_` infix while every other variant does, so
the type's name maps to most of the variants but not all of them.

### Root cause

The type was conceived for the failure cases first; `OK` was added as
a sentinel to mean "no failure" rather than as a peer outcome with a
parallel design. The single-enum-with-OK-sentinel pattern is common
in C APIs (e.g. `OSStatus` on macOS, `errno`, `int main(void)` return
codes), but it carries a documentation tax: every reader has to keep
"this thing can also be 0/OK" in their head.

In a project that values precise naming as documentation (see
`CLAUDE.md` Pre-commit checklist item 3, and the broader pattern of
the codebase), the tax is too high.

### Why it matters beyond cosmetics

Type names are signposts that drive decisions:

* **Refactoring direction.** A reader naming a related field is
  guided by the type's name. `last_error` was chosen because the
  type is `jbox_error_code_t`. Adding `JBOX_ERR_DEVICE_GONE` to mean
  "route is in WAITING because the device disappeared" then reads
  awkwardly: a "device-gone error" stored on a route that is in a
  recoverable state, not an error state.

* **Behavioural contracts.** It's tempting to write `if (status.
  last_error != JBOX_OK) {...}` as "this route has a problem", but
  that's wrong today: `last_error == JBOX_ERR_DEVICE_GONE` is set
  while the route is happily WAITING for auto-recovery — the
  *current* state is recoverable, not erroneous. The naming actively
  obscures that nuance.

* **Bug surface.** Anyone writing a UI affordance that "highlights
  routes with errors" by checking `status.lastError != .ok` will
  flag DEVICE_GONE routes as errors, when the right UX is "show
  this is auto-recovering, no user action needed". The type name
  is the misleading signal.

### Surface area

If/when refactored, the rename touches:

* **C ABI** — `Sources/JboxEngineC/include/jbox_engine.h`. Type rename
  + every variant rename (or a partial: keep `JBOX_OK` /
  `JBOX_ERR_*` prefixes, only rename the type).
* **C ABI consumers in C++** — `Sources/JboxEngineC/control/*.{hpp,cpp}`.
  Most touch the type indirectly through `RouteRecord::last_error`
  and the `jbox_route_status_t::last_error` plumbing. ABI MAJOR
  bump (current: v12).
* **Swift wrapper** — `Sources/JboxEngineSwift/` (the `JboxEngine`
  bridging types, `RouteStatus.lastError`).
* **SwiftUI app** — `Sources/JboxApp/` consumers of the status
  field. The placeholder UI today does little with this field; a
  rename here is mostly mechanical.
* **Tests** — Catch2 cases asserting `status.last_error == ...`
  and Swift Testing equivalents.
* **`docs/spec.md`** — § 3.x ABI sections that document the type.
* **`docs/plan.md`** — historical references would stay; new uses
  pick up the new name.

### Open questions (decide before doing the work)

1. **Is the right refactor a *type rename*, a *type split*, or
   *field renames only*?**

   * **Type rename only**: `jbox_error_code_t` → `jbox_result_t` /
     `jbox_status_code_t` / `jbox_outcome_t`. Keep `JBOX_OK` +
     `JBOX_ERR_*` variant names. Smallest sweep. Resolves "type
     name promises only errors but holds OK"; doesn't resolve the
     asymmetric variant prefix (`JBOX_OK` still feels like an
     outlier among the `JBOX_ERR_*` variants).

   * **Type rename + variant rename**: rename everything to one
     consistent prefix family (`JBOX_RESULT_OK`, `JBOX_RESULT_ERR_*`
     or similar). Larger sweep; better consistency.

   * **Type split**: introduce `jbox_result_t` (success/failure as a
     bool-like outcome) separate from `jbox_error_code_t` (which
     carries only failure variants and is only meaningful when
     `result == failure`). Mirrors Rust's `Result<T, E>` shape; more
     ergonomic at the use site (no "is this an error or OK?"
     branch on a single value); but requires every status field
     that can be either to carry both, and a discipline for what
     `error_code` means when `result == ok` (probably `nullopt`
     or "undefined / don't read"). Heaviest change.

2. **Field-level renames** — `RouteRecord::last_error`,
   `jbox_route_status_t::last_error`, `RouteStatus.lastError`. Likely
   names: `last_result`, `state_reason`, `status_code`. Pick depends
   on (1).

3. **Should `JBOX_ERR_DEVICE_GONE` be split out into a non-error
   "state reason" enum?** The DEVICE_GONE case isn't an error in the
   "operation failed" sense — it's a structural fact about the route's
   current state. Splitting it might be cleaner than overloading the
   error-code type. But splitting introduces a second small enum that
   the UI has to consult, which is its own complexity.

4. **What to do with `jbox_error_t` (the struct holding code +
   message)?** Currently used as an out-parameter for `addRoute`. Same
   smell — the struct is named for failure, but is also used to
   communicate success ("populate this struct; if `code == JBOX_OK`,
   the call succeeded"). Renaming the field type pulls this in.

5. **Pre-1.0 status — when to do it?** The project hasn't shipped v1
   yet (Phase 9 still pending), and both sides of the C ABI are owned
   in this repo. Breaking ABI is comparatively cheap *now*, much more
   expensive once external integrations exist. So the calendar
   pressure is "before v1.0.0". Reasonable slot: between Phase 7.6
   completing and Phase 8 starting. Out-of-cycle is fine too; this
   is a focused-sprint candidate, not a phase milestone.

6. **Scope creep risk.** While the type is open for renaming, it's
   tempting to also fix neighbouring fields and rationalise other
   parts of the C ABI. The discipline question: do this as a single
   focused diff, or use the rename as a wedge for broader cleanup?
   The first is safer (smaller blast radius, easier to review); the
   second is more efficient if multiple smells share root causes.

### Not blocking

7.6.4 lands with the existing `last_error` name on
`jbox_route_status_t`. `JBOX_ERR_DEVICE_GONE` is set on
`r.last_error` whenever the listener path forces RUNNING → WAITING.
The naming smell is recorded here so it isn't lost; the fix is a
deliberate later sprint, not a side-effect of feature work.

---

## R2 — Live-Core-Audio test "skip" pattern actually fails on no-device CI

**Status:** resolved 2026-05-08. Surfaced 2026-05-01 during the
drag-to-reorder review and confirmed in CI on 2026-05-08 when the
suite finally ran end-to-end on a GitHub `macos-15` runner — both
`pollMetersIsQuietOnNoChange` and
`pollStatusesIsQuietOnRoutesWhenRunningCountersTick` failed because
the `Issue.record + return / throw CancellationError()` shape never
actually skipped. Fix landed alongside the coverage CI work; see the
**Resolution** section at the end of this entry.

### Symptom

`Tests/JboxEngineTests/EngineStoreTests.swift` repeatedly uses this
shape to "skip" tests on CI runners without an input- and
output-capable Core Audio device:

```swift
guard let src = store.devices.first(where: { $0.directionInput }),
      let dst = store.devices.first(where: { $0.directionOutput })
else {
    Issue.record("CI runner expected to expose at least one ...")
    return  // or `throw CancellationError()` from a `throws` test
}
```

`Issue.record` records a **failed expectation**. Swift Testing
surfaces the recorded `Issue` as a test failure, not a skip.
Throwing `CancellationError` from a `throws` test does not
override that — the test is already marked failed by the time the
throw propagates. On a runner with no audio input devices (rare
but possible: ephemeral CI containers, headless macOS sandboxes),
every test in the suite reports as a regression even though the
failure is purely environmental.

### Root cause

The pattern was written before Swift Testing's `.disabled(if:)` /
`Test.Trait` machinery for conditional execution stabilised, and
no one came back to retrofit it. Swift Testing now has
`@Test(.disabled(if: shouldSkip(), "reason"))` and (newer)
`SkipError` mechanics that mark tests as skipped rather than
failed.

### Surface area

`Tests/JboxEngineTests/EngineStoreTests.swift` only — every
`@Test` whose first lines refresh devices and `guard … else
{ Issue.record(...); return }`. ~10 sites at last count. Other
test suites (`PersistedStateTests`, `MeterLevelTests`,
`MixerLayoutTests`, `MixerPanelLayoutTests`,
`PeakHoldTrackerTests`) don't hit Core Audio and are not
affected.

### Open questions (decide before doing the work)

1. **One-trait-per-suite or per-test?** A suite-level
   `.enabled(if: hasCoreAudioDevices())` keeps boilerplate out of
   each `@Test`; a per-test trait lets specific tests opt out
   independently. Recommend suite-level — every test in the file
   is live-Core-Audio by design.

2. **Where does the `hasCoreAudioDevices()` predicate live?**
   `EngineStoreTests` `@testable import`s `JboxEngineSwift`, so
   it can stand up an `EngineStore` and call `refreshDevices()`.
   Cleanest: a `private static let` at suite scope, evaluated
   once.

3. **Helper `makeStoreWithThreeRoutes` (added 2026-05-01) inherits
   the same shape.** It would collapse to a non-optional return
   under a suite-level enable trait. Coupling: the trait removes
   the `throw CancellationError()` workaround the helper has at
   `EngineStoreTests.swift:801-808`.

4. **Behaviour on developer machines?** Every macOS Mac has at
   least the built-in microphone + speakers; the predicate would
   always evaluate true locally. The test surface won't shrink in
   practice — only the failure mode on a no-device CI runner
   improves from "noisy red" to "clean skipped".

### Not blocking

Drag-to-reorder lands with `Issue.record(...) + throw
CancellationError()` in `makeStoreWithThreeRoutes` (matching the
existing pattern). The "real" fix is suite-wide, not a one-helper
patch, so it belongs in a focused refactoring pass rather than the
feature commit.

### Resolution (2026-05-08)

Suite-level `.enabled(if: hasIOCapableDevices())` trait. The
predicate is a `nonisolated` free function in
`Tests/JboxEngineTests/EngineStoreTests.swift` that walks Core Audio
directly via `kAudioHardwarePropertyDevices` +
`kAudioDevicePropertyStreamConfiguration` (no `EngineStore`
involvement, so the autoclosure stays `@Sendable`). Returns true on
every Mac that has at least one input-stream and one output-stream
capable device.

With the trait in place, ~22 environmental `guard … else
{ Issue.record(…); return }` blocks (and 3 `throw
CancellationError()` variants in `makeStoreWithThreeRoutes` and two
test bodies) collapsed to `try #require(...)` lookups:

```swift
let src = try #require(store.devices.first(where: { $0.inputChannelCount  >= 1 }))
let dst = try #require(store.devices.first(where: { $0.outputChannelCount >= 1 }))
```

`try #require` rather than force-unwrap (`!`) because the suite trait
walks Core Audio directly while test bodies enumerate via
`JboxEngineC`. The two paths agree in practice, but if the engine's
filtering ever diverges from raw enumeration, `try #require` surfaces
a Swift Testing failure with the source location instead of a runtime
crash. The 6 pre-existing force-unwraps on `directionInput` /
`directionOutput` were converted to the same idiom for consistency.

**Runtime IOProc condition (one test only).**
`pollStatusesIsQuietOnRoutesWhenRunningCountersTick` additionally
needs the host to actually drive the IOProc — true on dev machines,
not on sandboxed CI runners that enumerate devices but never
schedule the audio engine. The suite-level trait can't predict that,
so the test keeps its two runtime `guard … else { return }` checks
but drops `Issue.record` from each body. On hosts where IOProc
doesn't tick the test passes vacuously; the dev-machine run still
exercises the regression. Pure-logic
`statusFieldsAreObservablyEqual…` tests pin the diff predicate
itself deterministically and run everywhere.

**What stays.** `Issue.record(...)` calls that mark a *real*
expectation failure (e.g., `addRoute` was supposed to throw
`MAPPING_INVALID` and didn't) — those are correct uses and keep the
test failed when the engine breaks its contract.

---

## R3 — `@Observable` × subscript-write asymmetry: latent risk in `EngineStore`

**Status:** open / watch. Surfaced 2026-05-01 during the
drag-to-reorder smoke pass. Initial fix shipped two diff-before-write
guards at the two periodic-tick sites (`pollStatuses`,
`refreshStatus`); a follow-up the same day amended commit `77f8e0b`
after user smoke surfaced that the full-`Equatable` predicate the
guards used was load-bearing only for *idle* routes. `RouteStatus`'s
four monotonic counter fields (`framesProduced` / `framesConsumed` /
`underrunCount` / `overrunCount`) tick on every `pollStatuses` pass
for any route in `.running`, so the full-equality compare detected
a difference every poll on running routes and the `routes[i].status`
subscript write fired anyway, re-cancelling the drag whenever audio
flowed. Resolution: split `RouteStatus` into two channels at the
`EngineStore` boundary — stable fields (`state`, `lastError`,
`estimatedLatencyUs`) drive the array-subscript write via the new
`EngineStore.statusFieldsAreObservablyEqual(_:_:)` predicate;
counters publish into a separate `routeCounters: [UInt32:
RouteCounters]` direct-setter dict that the framework short-circuits
on equality and that — even on unequal writes — invalidates only
observers of `routeCounters`, not the `routes` ForEach. No other
call site in `EngineStore` needs a guard *today* — the remaining
subscript-write sites are all user-action-driven, not periodic — but
the asymmetry is non-obvious enough that a future contributor adding
a new periodic mutation path could regress the drag UX silently.

### The asymmetry (and why we now guard direct setters too)

Apple's `@Observable` macro (Observation framework) used to look
asymmetric on equal-value writes:

- **Direct property setter** (`self.meters = next`): equal-value
  writes appeared to be short-circuited at willSet on Swift 6.3+
  toolchains.
- **Subscript-through-collection** (`routes[i].status = …`,
  `latencyComponents[id] = …`, `routes[i].config.name = …`):
  goes through `Array` / `Dictionary`'s `_modify` accessor and
  fires the observation registrar's willSet **unconditionally**,
  even when the new value equals the old. Verified empirically
  by `EngineStoreTests.pollStatusesIsQuietOnNoChange` /
  `refreshStatusIsQuietOnNoChange` (both fail without explicit
  diff guards).

The "direct-setter short-circuit" turned out to be **toolchain-
dependent**: surfaced 2026-05-08 when the GitHub `macos-15` runner
(Swift 6.1.2) failed `pollMetersIsQuietOnNoChange` while the dev mac
(Swift 6.3.1) passed it. Older Observation runtimes fire willSet
unconditionally on direct setters too. The published behaviour of
Jbox shouldn't depend on which Swift the host runs, so `pollMeters`
now uses the same explicit `if meters != next { meters = next }`
guard the subscript-write paths use. The test was repurposed to pin
the guard rather than the framework optimisation, so it's
deterministic on every supported toolchain.

Spurious fires are not just a perf concern — they propagate to
SwiftUI's `List` which sits on top of `NSTableView`, and
`NSTableView` interprets every "data source did change" signal
as a reason to invalidate any in-flight `List.onMove` drag drop.
Three idle routes at a 4 Hz tick produces 12 unconditional fires/
sec, enough to make drag-to-reorder unusable.

### Surface area in current code

Subscript-write sites in `Sources/JboxEngineSwift/EngineStore.swift`
that do **not** currently have a diff-before-write guard. None of
these need one today (all are user-action-driven, fire at most
once per gesture, and the user expects the observation fire), but
each is a latent regression vector if its caller becomes periodic:

| Site (line) | Path | Why no guard today |
|---|---|---|
| `renameRoute` (~527) | `routes[idx].config.name = …` | One write per Enter on a rename field. |
| `setMasterGainDb` (~561) | `routes[idx].masterGainDb = …` | One write per fader drag tick. The fader does fire repeatedly during a drag but is itself the source of UI state, so cancelling its own drag is moot. |
| `setChannelTrimDb` (~603–610) | `routes[idx].trimDbs[…] = …`, `routes[idx].trimDbs = …` (pad) | One write per per-channel fader drag tick. Same reasoning as master gain. |
| `setRouteMuted` (~633) | `routes[idx].muted = …` | One write per mute toggle. |
| `setChannelMuted` (~655–661) | `routes[idx].channelMuted[…] = …` | One write per per-channel mute toggle. |
| `replaceRoute` (~753) | `routes[idx] = Route(…)` | Whole-element replacement, not a sub-field write. |

The two periodic sites (`pollStatuses`, `refreshStatus`) DO have
guards today, with regression coverage, and the load-bearing
inline comments at both sites reference this rule. `pollMeters`
intentionally has no guard — its `meters = next` write is a
direct property setter, framework already shorts.

### What to do

This is a *watch* item, not a refactoring slot. Concrete actions:

1. **When adding a new periodic tick that mutates `@Observable`
   state via subscript paths**, add an explicit
   `if newValue != routes[i].field` guard, mirroring the
   `pollStatuses` pattern (regression test:
   `pollStatusesIsQuietOnNoChange`). Inline-comment the *why*,
   not the *what* — the asymmetry is non-obvious enough that
   future readers will assume it's a redundant micro-opt.
   **Important addendum from the 2026-05-01 follow-up amend:** if
   the value being written is a *struct that itself contains
   high-frequency-ticking sub-fields* (the way `RouteStatus`
   contains `framesProduced` etc.), a full-`Equatable` diff guard
   is *not enough* — the ticking sub-fields will defeat it and the
   subscript write will fire every tick. Either narrow the diff
   predicate to the stable sub-fields and store the volatile
   sub-fields in a *direct-setter* sibling property (the
   `routeCounters` pattern), or split the struct so only stable
   data lives in the `routes[i]` slot.

2. **When promoting one of the user-action sites above to a
   periodic caller** (e.g., a "smooth gain ramp" timer that
   calls `setMasterGainDb` at 60 Hz), add a guard at that point.

3. **If Apple changes `@Observable` to short-circuit on subscript
   paths too**, `pollStatusesIsQuietOnNoChange` will pass even
   without the inline guards — at that point the guards become
   pure micro-opts and can be considered for removal. The
   `pollMetersIsQuietOnNoChange` watchdog is the canary that
   tells us the framework contract has shifted.

### Why this isn't a "fix it now" item

The fix is two one-line guards plus regression tests, both of
which already shipped in `09d062d` for the *currently periodic*
sites. The remaining sites would need either speculative new
guards (premature against today's call patterns) or a wholesale
rewrite of `EngineStore` to centralise mutations behind a single
`updateRoute(id, mutate:)` helper that diffs internally — that's
a much larger refactor with its own SOLID/DRY trade-offs (tests
would need to thread through the helper; the existing setter API
is already clear and well-covered).

### References

- Implementation: `Sources/JboxEngineSwift/EngineStore.swift` —
  `pollStatuses` writes `routes[i].status` via the
  `statusFieldsAreObservablyEqual` predicate and publishes counters
  into `routeCounters` via a direct setter; `refreshStatus` mirrors
  the same split for explicit user actions; `removeRoute` /
  `replaceRoute` prune the dict alongside `meters` /
  `latencyComponents`. Inline comments at all three sites reference
  the asymmetry and the running-route counter-tick failure mode.
- Regression tests: `Tests/JboxEngineTests/EngineStoreTests.swift` —
  pure-logic predicate cases
  (`statusFieldsAreObservablyEqual…`),
  dict-lifecycle cases (`pollStatusesPublishesRouteCounters`,
  `removeRoutePrunesRouteCounters`),
  no-change subscript-write guards (`pollStatusesIsQuietOnNoChange`,
  `refreshStatusIsQuietOnNoChange`),
  framework-contract direct-setter watchdog
  (`pollMetersIsQuietOnNoChange`),
  and the live-Core-Audio counter-tick regression
  (`pollStatusesIsQuietOnRoutesWhenRunningCountersTick`).
- Original surfacing + follow-up amend: `docs/plan.md` Phase 6
  deviations "@Observable × List.onMove drag cancellation
  (2026-05-01)" and "@Observable × subscript-write asymmetry,
  second order — counter ticks slip past full-equality diff
  (2026-05-01)".
- Risks discussion: `docs/2026-05-01-route-reorder-design.md` §
  "Risks not covered by automated tests".
