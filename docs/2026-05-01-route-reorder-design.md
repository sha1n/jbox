# Route reorder via drag (design)

**Date:** 2026-05-01
**Status:** Brainstormed; awaiting plan.
**Scope:** UI affordance + one new `EngineStore` mutation. No engine, ABI, or schema changes.

## Goal

Let the user drag route strips in the main window's route list to reorder them, and have that order survive a quit / relaunch.

## Non-goals

- Reordering inside the menu-bar list (the menu-bar `VStack` mirrors the main-window order read-only).
- A visible grab handle on each strip (additive change for later if discoverability turns out to be a problem; nothing in this design forecloses adding one).
- Engine-side ordering. `RouteManager`'s IOProc scheduling is per-device; route-list order is purely a UI / persistence concern.
- Drag-between-windows / multi-window. Jbox ships a single-instance `Window` scene by design.
- Schema migration. `StoredAppState.routes: [StoredRoute]` is already an ordered array; `currentSchemaVersion` stays at `1`.

## Source of truth

`EngineStore.routes: [Route]` — array index = display order = persisted order. No new sort key, no new schema field, no new ABI surface.

## What changes

### 1. `EngineStore.moveRoute(from:to:)` — new `@MainActor` mutation

Mirrors the shape of `addRoute` / `removeRoute`. Lives in `Sources/JboxEngineSwift/EngineStore.swift` next to the other route mutations.

```swift
public func moveRoute(from offsets: IndexSet, to destination: Int) {
    let before = routes.map(\.id)
    // Implement List.onMove semantics manually (Array.move is SwiftUI-only).
    // Extract items, remove from back to front to keep indices stable,
    // then insert before destination (adjusted for removals).
    let items = offsets.map { routes[$0] }
    var adjusted = destination
    for idx in offsets.reversed() {
        routes.remove(at: idx)
        if idx < destination { adjusted -= 1 }
    }
    routes.insert(contentsOf: items, at: adjusted)
    let after = routes.map(\.id)
    guard before != after else { return }

    let offsetStr = offsets.map(String.init).joined(separator: ",")
    JboxLog.engine.notice(
        "moveRoute from=\(offsetStr, privacy: .public) to=\(destination, privacy: .public) count=\(self.routes.count, privacy: .public)")
    onRoutesChanged?()
}
```

> **Implementation deviation from the spec snippet (recorded during Task 1):** `MutableCollection.move(fromOffsets:toOffset:)` is provided by SwiftUI, not Foundation, and is not reachable from `EngineStoreSwift` which imports only `Foundation` + `Observation`. The shipped body reimplements the same semantics manually (back-to-front removal + destination decremented for each removal preceding it). The class-level `@MainActor` on `EngineStore` already isolates `moveRoute`, so no per-method `@MainActor` is needed. See `docs/plan.md` Phase 6 deviation entry for the full record.

Semantics:

- **No-op moves do not fire `onRoutesChanged`.** Empty `IndexSet`, same-position drops, and any drop-immediately-past-self (`{N} → N+1`) — all of which the manual algorithm yields as identity arrays — are silent. This avoids spurious `state.json` snapshots when the user drags a strip back to where it was.
- **Multi-row drags work natively.** `IndexSet`'s ascending-index iteration plus the back-to-front removal preserves the relative order of the moved set, matching SwiftUI's documented `List.onMove` contract.
- **Engine is not touched.** No `engine.*` call. No new ABI symbol. No new `RouteRecord` field. No new C++ test.
- **Logging matches the existing notice-on-mutation style** used by `addRoute`, `removeRoute`, and the gain setters. `privacy: .public` on every interpolation so the line is readable under Hardened Runtime in shipped builds.

### 2. `RouteListView` — hook `List.onMove`

Single addition to `Sources/JboxApp/RouteListView.swift`'s `detailContent` builder:

```swift
List {
    ForEach(store.routes) { route in
        RouteRow(...)
            .listRowSeparator(.hidden)
            .listRowBackground(Color.clear)
            .padding(.vertical, 4)
    }
    .onMove { offsets, destination in
        store.moveRoute(from: offsets, to: destination)
    }
}
.listStyle(.inset)
```

What this gives us for free on macOS:

- The macOS `List` reorder cursor (open-hand) on hover over a row's non-interactive area.
- A horizontal drop-line indicator between rows during a drag.
- Identity stability: `Route` is `Identifiable` keyed on the engine's `UInt32` id, so reordering doesn't recreate `RouteRow` views — `@State` (`isRenaming`, `renameDraft`, `renameFieldFocused`) rides along with each row.

VoiceOver behaviour on macOS for `List.onMove` is documented for iOS (rotor "Move Up" / "Move Down" actions) but is less explicit on the macOS side. The implementation plan's manual smoke step should verify VoiceOver reorder works; if it doesn't, a small follow-up adds explicit `.accessibilityAction(named: "Move Up"/"Move Down")` modifiers per row. Not a blocker for the drag affordance itself.

Existing button affordances inside the strip (chevron, Start/Stop, edit, trash) keep their click semantics — `Button` consumes the gesture before `List`'s drag recognizer sees it. The mappingSummary text and the row's background area are draggable.

### 3. Menu-bar list — no edit

`MenuBarContent.swift`'s `VStack { ForEach(store.routes) { ... } }` re-renders on `routes` mutation, so the new order shows up there automatically with zero changes to that file.

### 4. Persistence — no edit

The persistence path is already wired:

- `EngineStore.moveRoute` fires `onRoutesChanged?()`.
- `JboxApp.swift` (`s.onRoutesChanged = { ... }`, line ≈523) calls `StateStore.snapshot(...)` which debounces 500 ms then writes `state.json`.
- `StoredAppState.routes: [StoredRoute]` is a JSON array; `JSONEncoder` preserves order.
- `JboxApp.restoreRoutes(into:)` (line 591) iterates `persisted.routes` in array order, calling `store.addRoute(cfg, persistId:, createdAt:)` sequentially. `EngineStore.addRoute` appends, so restored order = stored order.

No `currentSchemaVersion` bump. No `StoredAppState` field. No `StoredRoute` field. No migration step.

## Test plan

### Swift Testing — `Tests/JboxEngineTests/EngineStoreTests.swift` (new cases in the existing suite)

1. `moveRoute moves a single row down and fires onRoutesChanged once`
2. `moveRoute moves a single row up and fires onRoutesChanged once`
3. `moveRoute with multi-row IndexSet preserves the relative order of the moved set` (contiguous selection — the only shape `List.onMove` produces in practice)
4. `moveRoute with a non-contiguous IndexSet straddling the destination preserves the relative order of the moved set` (defensive against future refactors of the manual index-adjustment math — `IndexSet([0, 3]) → 2` over a four-element list, where one removed index sits below `destination` and the other above, so the two arms of `if idx < destination { adjusted -= 1 }` are exercised in opposite directions)
5. `moveRoute is a no-op when destination equals source position` — `onRoutesChanged` counter stays at 0 (`{N} → N` identity)
6. `moveRoute is a no-op when destination is one past the source (List.onMove identity)` — `onRoutesChanged` counter stays at 0 (`{N} → N+1` identity; pins the index-adjustment branch independently)
7. `moveRoute is a no-op for an empty IndexSet` — `onRoutesChanged` counter stays at 0
8. `moveRoute on a single-row list is identity for any valid destination` (single-row List.onMove only ever produces destinations 0 or 1 — both identity)
9. `moveRoute fires onRoutesChanged exactly once per non-trivial move` (regression against accidental double-fires)

Construction: build an `EngineStore` via a shared `makeStoreWithThreeRoutes()` helper that runs against the live Core Audio backend (matching the existing `EngineStoreTests` idiom); seed three routes via `addRoute`; install a counter closure on `onRoutesChanged`; exercise `moveRoute` and assert both `routes.map(\.id)` and the counter. Cases 4 and 8 build their own routes inline because they need a different cardinality (four routes for the non-contiguous case, one for the single-row list).

> **Test count deviation from the original spec (recorded across Task 2 and the post-review pass):** The original spec listed six cases. Two were added during the Task 2 code-quality review — `moveRouteAdjacentPositionNoOp` (the `{N} → N+1` identity, distinct path from `{N} → N` in the manual move-equivalent body) and `moveRouteSingleRowList` (single-row identity). One was added during the post-review code-review pass on commit `09d062d` — `moveRouteNonContiguousIndexSet`, which exercises the `IndexSet([0, 3]) → 2` straddling-destination case to pin both arms of the `if idx < destination { adjusted -= 1 }` decrement against future refactors. Final count: 9 `moveRoute*` cases, 3 polling-discipline `IsQuietOnNoChange` cases.

### Persistence round-trip — already covered

`Tests/JboxEngineTests/PersistedStateTests.swift:365` (`multipleRoutesPreserveOrder`) already pins that `StoredAppState` round-trip preserves the `routes` array order across encode/decode. No new persistence test needed.

### Manual smoke

UI gestures are not covered by automated tests in this codebase (per `CLAUDE.md`'s "UI-only changes that can't be unit-tested" clause). The commit message will say so. Procedure:

1. `make run`.
2. Add three routes through the UI.
3. Drag the third strip above the first; confirm visual reorder.
4. Quit Jbox; relaunch.
5. Confirm route order matches what step 3 produced.
6. Confirm running / stopped routes both reorder cleanly (state is per-route, not per-position).

## Doc updates the change carries

- `docs/plan.md` — append a one-line entry under Phase 6 (UI) — drag-to-reorder landed; reference this design doc and the implementation commit hash.
- `docs/spec.md` — add a sentence to the route-list section (§ 4.1) stating that route order is user-controlled (drag in the main window) and persisted in `state.json`.
- No `README.md` changes (install / build / commands story unchanged).

## Open risks

- **Drag conflicts with `RouteRow`'s expand chevron / Start-Stop / edit / trash buttons.** Mitigation: `Button` consumes the gesture before `List`'s drag recognizer fires. Verified by the way SwiftUI's gesture-priority system already works for `List.onDelete` + buttons in the same row across the rest of the codebase. Manual smoke covers it.
- **Reordering during an inline rename.** The headline switches to a `TextField` while `isRenaming` is true; `TextField` consumes pointer events, so the user cannot start a drag from the field. Dragging from the rest of the strip while another row is being renamed is fine — the rename `@State` is per-row and rides along with the row's identity.
- **A drag that ends with no actual position change.** Covered by the no-op short-circuit in `moveRoute`; `onRoutesChanged` does not fire, so `StateStore` doesn't write.
- **Drag cancelled by a genuine state transition on a visible row (residual, not addressed).** The diff-before-write fix (next bullet) suppresses *spurious* `@Observable` fires on unchanged ticks; it does NOT suppress fires from real state changes. If a route the user can see transitions while a drag is in flight (e.g., another route flips `running → waiting` because its source device disconnected, or `setMasterGainDb` is called via menu-bar control), the resulting `routes[i].status = …` change legitimately fires `willSet` and `NSTableView` cancels the drop. This is inherent to `@Observable` × `NSTableView` and not a bug in our code; the user's recourse is to retry the drag. Not worth a custom `DropDelegate` to mitigate at v1.
- **`@Observable` notification storm cancels in-flight drags (surfaced during smoke, not anticipated in the original design).** `EngineStore` is `@Observable`. Apple's Observation framework is asymmetric on equal-value writes: *direct* property-setter writes (`self.meters = next`) get a willSet short-circuit when the new value equals the old, but *subscript-through-collection* writes (`routes[i].status = …`, `latencyComponents[id] = …`) go through the `_modify` accessor and fire willSet unconditionally. The 4 Hz `pollStatuses` tick over three idle routes was therefore producing 12 unconditional fires/sec down the subscript path, and `NSTableView` under SwiftUI's `List` treats every "data source did change" signal as a reason to invalidate the proposed drop — the drag would cancel mid-gesture. *Mitigation:* explicit `Equatable` diff-before-write guards at the two subscript-write sites (`pollStatuses` plus the `refreshStatus` one-liner). The `pollMeters` direct-setter path needs no guard. *Regression coverage:* `pollStatusesIsQuietOnNoChange` pins the `pollStatuses` subscript-write guard (verified to fail if either of its two arms is removed); `refreshStatusIsQuietOnNoChange` pins the `refreshStatus` arm independently by driving an idempotent `stopRoute(id)` on an already-stopped route (the engine returns `JBOX_OK`, the store's success arm calls `refreshStatus`, the guard must skip the equal-value write); `pollMetersIsQuietOnNoChange` pins the framework contract for direct-setter shorts (so a future Apple change to Observation that drops the short-circuit fails the test loudly). Recorded in `docs/plan.md` Phase 6 deviations as a separate bullet alongside the drag-to-reorder feature itself; the asymmetry generalises across `EngineStore` and is filed as `R3` in `docs/refactoring-backlog.md` so future periodic-tick paths know to mirror the guard.

  > **Mitigation superseded (2026-05-01 follow-up amend).** The `Equatable` diff-before-write guard described above held only on *idle* routes. `RouteStatus` carries four monotonic counter fields (`framesProduced` / `framesConsumed` / `underrunCount` / `overrunCount`) that tick every poll for any route in `.running`, so the full-equality compare detected a difference on every poll once audio flowed and the `routes[i].status` subscript-write fired anyway — the gesture still cancelled. Resolution: split `RouteStatus` at the `EngineStore` boundary. Stable fields (`state`, `lastError`, `estimatedLatencyUs`) drive the array-subscript write via a new exposed-`static` `EngineStore.statusFieldsAreObservablyEqual(_:_:)` predicate; volatile counters publish into a new `routeCounters: [UInt32: RouteCounters]` direct-setter dict that the framework shorts on equality and that — even when it fires — invalidates only observers of `routeCounters`, never the `routes` ForEach. `MeterPanel.DiagnosticsBlock` reads counters from the dict; values inside `routes[i].status` are stale by design (the `RouteStatus.framesProduced` doc-comment calls this out for any reader landing there directly). New regression tests: four pure-logic predicate cases (`statusFieldsAreObservablyEqual…`), two dict-lifecycle cases (`pollStatusesPublishesRouteCounters`, `removeRoutePrunesRouteCounters`), one `refreshStatus` dict-population case (`refreshStatusPublishesRouteCounters`), and one live-Core-Audio integration (`pollStatusesIsQuietOnRoutesWhenRunningCountersTick`) that brings a real route to `.running` and asserts observer quiet across counter advance. The full canonical record lives in [`docs/plan.md` Phase 6 deviations](./plan.md#phase-6--swiftui-ui) under "@Observable × subscript-write asymmetry, second order — counter ticks slip past full-equality diff"; the addendum on `R3` in `docs/refactoring-backlog.md` extends the watch-item to "high-frequency-ticking sub-fields defeat any full-`Equatable` guard."

## Effort estimate

- Production: ≈30 LOC (`EngineStore.moveRoute` + the `.onMove` modifier).
- Tests: ≈70 LOC (6 EngineStore cases; persistence round-trip already covered).
- Docs: ≈15 LOC across `plan.md` and `spec.md`.

Single commit. No ABI bump. No schema bump. No engine touch.

> **Estimate deviation (recorded post-implementation):** the actual diff was +171 LOC tests + 34 LOC engine-store + 3 LOC view, plus the polling-discipline side-fix discovered during smoke (initially 2 `withObservationTracking` tests; a third — `refreshStatusIsQuietOnNoChange` — was added during the post-review amend pass to pin the `refreshStatus` arm independently after a code review showed it had no isolated coverage), plus a defensive `moveRouteNonContiguousIndexSet` test added in the same pass to pin the manual `Array.move`-equivalent body's index-adjustment math against future refactors, plus `R2` (suite-wide live-Core-Audio test-skip pattern) and `R3` (`@Observable` × subscript-write asymmetry watch-item) entries in `docs/refactoring-backlog.md`. Implemented as a sequence of TDD slices that were squashed into a single commit on `feature/route-reorder` during the post-review amend pass — the earlier estimate of "single commit" did hold for the *visible* history. ABI / schema / engine claims (no bumps, no touch) all held.
