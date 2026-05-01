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
@MainActor
public func moveRoute(from offsets: IndexSet, to destination: Int) {
    let before = routes.map(\.id)
    routes.move(fromOffsets: offsets, toOffset: destination)
    let after = routes.map(\.id)
    guard before != after else { return }

    JboxLog.engine.notice(
        "moveRoute from=\(offsets.map(String.init).joined(separator: ","), privacy: .public) "
        + "to=\(destination) count=\(routes.count)")
    onRoutesChanged?()
}
```

Semantics:

- **No-op moves do not fire `onRoutesChanged`.** Empty `IndexSet`, same-position drops, and any move where the resulting `id`-sequence equals the input are silent. This avoids spurious `state.json` snapshots when the user drags a strip back to where it was.
- **Multi-row drags work natively.** Swift's `Array.move(fromOffsets:toOffset:)` handles a multi-index `IndexSet` and preserves the relative order of the moved set.
- **Engine is not touched.** No `engine.*` call. No new ABI symbol. No new `RouteRecord` field. No new C++ test.
- **Logging matches the existing notice-on-mutation style** used by `addRoute`, `removeRoute`, and the gain setters. `privacy: .public` on the `from=` payload because indices carry no PII.

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
3. `moveRoute with multi-row IndexSet preserves the relative order of the moved set`
4. `moveRoute is a no-op when destination equals source position` — `onRoutesChanged` counter stays at 0
5. `moveRoute is a no-op for an empty IndexSet` — `onRoutesChanged` counter stays at 0
6. `moveRoute fires onRoutesChanged exactly once per non-trivial move` (regression against accidental double-fires)

Construction: build an `EngineStore` over a `SimulatedBackend` per existing test idiom; seed three routes via `addRoute`; install a counter closure on `onRoutesChanged`; exercise `moveRoute` and assert both `routes.map(\.id)` and the counter.

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

## Effort estimate

- Production: ≈30 LOC (`EngineStore.moveRoute` + the `.onMove` modifier).
- Tests: ≈70 LOC (6 EngineStore cases; persistence round-trip already covered).
- Docs: ≈15 LOC across `plan.md` and `spec.md`.

Single commit. No ABI bump. No schema bump. No engine touch.
