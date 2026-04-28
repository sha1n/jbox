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
