# Route Reorder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user drag route strips in the main window's route list to reorder them; persist that order across launches.

**Architecture:** Single new `@MainActor` mutation `EngineStore.moveRoute(from:to:)` mirrors the shape of `addRoute` / `removeRoute`. SwiftUI's native `List.onMove` modifier hooks the gesture in `RouteListView`. Persistence is automatic — `onRoutesChanged?()` already triggers `StateStore`'s debounced `state.json` snapshot, and `[StoredRoute]` is already an ordered JSON array. No engine touch, no ABI bump (stays at v14), no schema bump (stays at `currentSchemaVersion = 1`).

**Tech Stack:** Swift, SwiftUI, Swift Testing, `os.Logger`. macOS 15+. Existing live-Core-Audio test idiom in `Tests/JboxEngineTests/EngineStoreTests.swift`.

**Reference spec:** [`docs/2026-05-01-route-reorder-design.md`](./2026-05-01-route-reorder-design.md) — read this before starting.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Sources/JboxEngineSwift/EngineStore.swift` | Modify (add ~15 LOC after `removeRoute` at line ~416) | New `moveRoute(from:to:)` mutation. |
| `Tests/JboxEngineTests/EngineStoreTests.swift` | Modify (append ~150 LOC of new `@Test` cases inside the existing `EngineStoreTests` suite) | Behavioural coverage for `moveRoute`. |
| `Sources/JboxApp/RouteListView.swift` | Modify (1-line addition at line ~114, after the `ForEach` closing brace) | Hook `.onMove` to forward the gesture into `EngineStore.moveRoute`. |
| `docs/plan.md` | Modify | Add a Phase 6 "deviation" entry recording drag-to-reorder with the implementation commit hash. |
| `docs/spec.md` | Modify | Append one sentence to § 4.1 ("Main window") noting that route order is user-controlled and persisted. |

No new files. No directory changes. No schema, ABI, or engine changes.

---

## Task 1: Add `moveRoute` to `EngineStore` (TDD: happy path)

**Files:**
- Modify: `Sources/JboxEngineSwift/EngineStore.swift` (add method right after `removeRoute`, ~line 416)
- Test: `Tests/JboxEngineTests/EngineStoreTests.swift` (append new `@Test` cases inside the existing `@Suite("EngineStore (live Core Audio)")` struct that starts at line 11)

This task adds the simplest "happy path" test + the smallest implementation that passes it. Subsequent tasks layer on the no-op short-circuit and the broader test grid.

- [ ] **Step 1: Write the failing test for a single-row move down**

Append to `Tests/JboxEngineTests/EngineStoreTests.swift`, immediately before the closing `}` of the `EngineStoreTests` struct (just before the `// MARK: - Previews` block in `RouteListView.swift` is in a different file — this MARK lives at the bottom of the test struct if any; if no MARK exists, just append the case at the end of the struct):

```swift
    // MARK: - moveRoute

    /// Helper: build three routes with distinct names, all over the
    /// same single-input / single-output device pair (matching the
    /// existing `addRouteLatencyMode` idiom at line ~127). Returns the
    /// store + the three route IDs in insertion order.
    private func makeStoreWithThreeRoutes() throws
        -> (EngineStore, UInt32, UInt32, UInt32)
    {
        let store = try makeStore()
        store.refreshDevices()
        guard let src = store.devices.first(where: { $0.inputChannelCount  >= 1 }),
              let dst = store.devices.first(where: { $0.outputChannelCount >= 1 })
        else {
            Issue.record("CI runner expected to expose at least one input- and one output-capable device")
            throw CancellationError()
        }
        func cfg(_ name: String) -> RouteConfig {
            RouteConfig(
                source: DeviceReference(device: src),
                destination: DeviceReference(device: dst),
                mapping: [ChannelEdge(src: 0, dst: 0)],
                name: name)
        }
        let a = try store.addRoute(cfg("a"))
        let b = try store.addRoute(cfg("b"))
        let c = try store.addRoute(cfg("c"))
        return (store, a.id, b.id, c.id)
    }

    @Test("moveRoute moves a single row down and fires onRoutesChanged once")
    func moveRouteSingleRowDown() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Move row 0 ('a') to position 3 (after 'c').
        // List.onMove convention: destination is "before this index".
        store.moveRoute(from: IndexSet(integer: 0), to: 3)

        #expect(store.routes.map(\.id) == [b, c, a])
        #expect(fireCount == 1)
    }
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
swift test --filter "EngineStoreTests/moveRouteSingleRowDown" 2>&1 | tail -30
```

Expected: FAIL with a compile error referencing `moveRoute` not being a member of `EngineStore`.

- [ ] **Step 3: Implement the minimal `moveRoute` in `EngineStore`**

Open `Sources/JboxEngineSwift/EngineStore.swift`. Locate `removeRoute(_:)` ending at line ~416. Add the following method directly after it, before the `overallState` doc comment that begins at line ~418:

```swift
    /// Reorder the observable `routes` array. Pure UI/persistence
    /// concern — the engine does not know or care about route order;
    /// `RouteManager`'s IOProc scheduling is per-device.
    ///
    /// `IndexSet` and `destination` follow SwiftUI `List.onMove`
    /// convention: indices are positions in the current array;
    /// `destination` is "the index this row(s) should land *before*",
    /// so dragging row 0 to position 3 in a 3-row list moves it to
    /// the end.
    ///
    /// Fires `onRoutesChanged?()` only when the resulting `id`-sequence
    /// actually differs — empty `IndexSet` and same-position drops are
    /// silent so we don't trigger spurious `state.json` snapshots.
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

- [ ] **Step 4: Run the test to verify it passes**

```bash
swift test --filter "EngineStoreTests/moveRouteSingleRowDown" 2>&1 | tail -30
```

Expected: PASS, 1 test passing.

- [ ] **Step 5: Commit**

```bash
git add Sources/JboxEngineSwift/EngineStore.swift Tests/JboxEngineTests/EngineStoreTests.swift
git commit -m "$(cat <<'EOF'
feat(reorder): add EngineStore.moveRoute with no-op short-circuit

Adds a @MainActor mutation that reorders the observable routes
array and fires onRoutesChanged only on non-trivial moves. Engine
is not touched — order is purely a UI/persistence concern.

Coverage: single-row move-down test + helper. Subsequent commit
rounds out the no-op + multi-row + exactly-once-fire grid.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Round out `moveRoute` test coverage

**Files:**
- Test: `Tests/JboxEngineTests/EngineStoreTests.swift` (append five more `@Test` cases under the same `// MARK: - moveRoute` heading from Task 1)

These five cases pin every branch of the implementation. Because the implementation in Task 1 already has the no-op short-circuit, all five should pass on first run — that is the test suite verifying the impl is complete, not driving new code.

- [ ] **Step 1: Add the five remaining `@Test` cases**

Append after the `moveRouteSingleRowDown` case from Task 1:

```swift
    @Test("moveRoute moves a single row up and fires onRoutesChanged once")
    func moveRouteSingleRowUp() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Move row 2 ('c') to position 0 (before 'a').
        store.moveRoute(from: IndexSet(integer: 2), to: 0)

        #expect(store.routes.map(\.id) == [c, a, b])
        #expect(fireCount == 1)
    }

    @Test("moveRoute with multi-row IndexSet preserves the relative order of the moved set")
    func moveRouteMultiRow() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Drag rows {0, 1} ({a, b}) to position 3 (after c).
        // Expected: c first, then a then b in their original
        // relative order.
        store.moveRoute(from: IndexSet([0, 1]), to: 3)

        #expect(store.routes.map(\.id) == [c, a, b])
        #expect(fireCount == 1)
    }

    @Test("moveRoute is a no-op when destination equals source position")
    func moveRouteSamePositionNoOp() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        // Drag row 1 ('b') to position 1 — same place.
        // Swift's Array.move treats this as identity; we must NOT
        // fire onRoutesChanged for it (avoid spurious state.json
        // snapshots).
        store.moveRoute(from: IndexSet(integer: 1), to: 1)

        #expect(store.routes.map(\.id) == [a, b, c])
        #expect(fireCount == 0)
    }

    @Test("moveRoute is a no-op for an empty IndexSet")
    func moveRouteEmptyIndexSet() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        store.moveRoute(from: IndexSet(), to: 2)

        #expect(store.routes.map(\.id) == [a, b, c])
        #expect(fireCount == 0)
    }

    @Test("moveRoute fires onRoutesChanged exactly once per non-trivial move")
    func moveRouteFiresExactlyOnce() throws {
        let (store, a, b, c) = try makeStoreWithThreeRoutes()
        defer {
            store.removeRoute(a)
            store.removeRoute(b)
            store.removeRoute(c)
        }
        var fireCount = 0
        store.onRoutesChanged = { fireCount += 1 }

        store.moveRoute(from: IndexSet(integer: 0), to: 2)  // a → after b
        store.moveRoute(from: IndexSet(integer: 2), to: 0)  // last → first

        // Two non-trivial moves should fire exactly twice. Regression
        // guard against accidentally double-firing onRoutesChanged.
        #expect(fireCount == 2)
    }
```

- [ ] **Step 2: Run all `moveRoute` tests to verify they pass**

```bash
swift test --filter "EngineStoreTests/moveRoute" 2>&1 | tail -40
```

Expected: PASS — 6 tests passing (the original from Task 1 plus the five new ones).

- [ ] **Step 3: Commit**

```bash
git add Tests/JboxEngineTests/EngineStoreTests.swift
git commit -m "$(cat <<'EOF'
test(reorder): cover moveRoute no-op + multi-row + exactly-once-fire

Five additional cases pin: single-row up, multi-row IndexSet,
same-position drop is silent, empty IndexSet is silent, two
non-trivial moves fire exactly twice. All pass against the
implementation landed in the prior commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Hook `List.onMove` in `RouteListView`

**Files:**
- Modify: `Sources/JboxApp/RouteListView.swift` — `detailContent` property (currently lines 92–118)

This is the UI integration. SwiftUI's native `List.onMove` modifier handles every interaction concern (cursor, drop indicator, identity stability, button-vs-drag gesture priority) for free.

- [ ] **Step 1: Add the `.onMove` modifier**

Open `Sources/JboxApp/RouteListView.swift`. Locate the `detailContent` `@ViewBuilder` (line ~92). Inside the `else` branch (line ~101), the existing structure is:

```swift
        } else {
            List {
                ForEach(store.routes) { route in
                    RouteRow(
                        route: route,
                        store: store,
                        expanded: expandedRoutes.contains(route.id),
                        onToggleExpanded: { toggleExpansion(route.id) },
                        onEditRequested: { editingRoute = route }
                    )
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                    .padding(.vertical, 4)
                }
            }
            .listStyle(.inset)
        }
```

Add an `.onMove` modifier on the `ForEach` (it must attach to the `ForEach`, not the `List`, for SwiftUI to wire row reorder semantics correctly):

```swift
        } else {
            List {
                ForEach(store.routes) { route in
                    RouteRow(
                        route: route,
                        store: store,
                        expanded: expandedRoutes.contains(route.id),
                        onToggleExpanded: { toggleExpansion(route.id) },
                        onEditRequested: { editingRoute = route }
                    )
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                    .padding(.vertical, 4)
                }
                .onMove { offsets, destination in
                    store.moveRoute(from: offsets, to: destination)
                }
            }
            .listStyle(.inset)
        }
```

- [ ] **Step 2: Build and run the unit-test suite to confirm nothing regressed**

```bash
make swift-test 2>&1 | tail -20
```

Expected: all suites pass, including the six `moveRoute` cases from Tasks 1 and 2.

- [ ] **Step 3: Manual UI smoke (mandatory per CLAUDE.md "UI-only changes" clause)**

Build and launch the bundled app:

```bash
make run
```

Then exercise the gesture in the running app:

1. Add three routes through the UI (any source/destination combination — they don't need to start successfully; reorder works regardless of state).
2. Hover over the third route's strip on a non-button area (e.g. the source → destination summary text). Cursor should change to the macOS reorder hand.
3. Drag the third strip above the first. A horizontal drop-line indicator should follow the pointer; release. The strip should land at the top.
4. Quit Jbox (`⌘Q`).
5. Relaunch (`make run` again).
6. Confirm the route order persisted: the strip you moved is still at the top.
7. Try a same-position drag (drag a strip and drop it where it started). Expected: silent no-op, no visible flicker, no extra `state.json` write.
8. Verify the per-row buttons still work after the change: chevron expands meters, Start/Stop / edit / trash all respond to clicks. (`Button` consumes the gesture before `List`'s drag recognizer.)
9. Optional: with VoiceOver enabled, navigate to a route row, open the rotor (`VO+U`), and look for "Reorder" or "Move Up" / "Move Down" actions. If they're present, great. If not, the manual smoke is still successful — the design doc flags this as a follow-up; do not block on it.

If any step fails, stop and investigate before committing. Common failure modes:

- **Drag doesn't pick up the row.** Likely cause: `.onMove` got attached to the `List` instead of the `ForEach`. Re-check the indentation.
- **Drop ignores the gesture.** Likely cause: the closure isn't reaching `store.moveRoute` — add a `print` to confirm.
- **Reorder doesn't survive relaunch.** Likely cause: `onRoutesChanged` not firing — re-check the no-op short-circuit didn't accidentally short-circuit a real move (the `before != after` comparison should differ for a real move).

- [ ] **Step 4: Commit**

```bash
git add Sources/JboxApp/RouteListView.swift
git commit -m "$(cat <<'EOF'
feat(reorder): hook List.onMove into RouteListView

Drag any non-button area of a route strip to reorder. Native
macOS reorder cursor + drop-line indicator + identity-stable
RouteRow @State (rename, expand) ride along for free. Persistence
falls out of the existing onRoutesChanged → StateStore snapshot
path; state.json's routes array order survives quit/relaunch.

UI-only change: covered by manual smoke (no automated UI test
infrastructure in this repo per CLAUDE.md).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Doc updates (`docs/plan.md`, `docs/spec.md`)

**Files:**
- Modify: `docs/plan.md`
- Modify: `docs/spec.md`

Per the CLAUDE.md pre-commit checklist: doc changes belong in the same commit as the code that motivates them. We commit them separately here only because Tasks 1–3 are independent slices that can stand on their own; Task 4 closes the doc-update obligation as a single small commit. (Per the user-feedback rule, do not later squash these into Task 3 — that would create a "follow-up polish commit" smell. Instead, keep this as a deliberate doc commit; it is not a polish commit because it documents a new user-visible behaviour.)

- [ ] **Step 1: Add a Phase 6 deviation to `docs/plan.md`**

Open `docs/plan.md`. Locate the Phase 6 section (`## Phase 6 — SwiftUI UI`, line ~312). Phase 6 has its own milestone-table status entry already; we want to append a one-line note in the Phase 6 *deviations* sub-section. Find the Phase 6 deviations heading (search: `Phase 6 summary of deviations` or similar).

If there's no Phase 6 deviations section, look at the milestone table row for Phase 6 (line ~37) and append a parenthetical to the status cell:

```markdown
+ drag-to-reorder route strips (commit <abbrev hash>)
```

If a Phase 6 deviations sub-section exists, append a new bullet matching the existing style:

```markdown
- **Drag-to-reorder route strips (2026-05-01).** *Goal:* let the user
  arrange route strips in the main window's order of choice and have
  that order persist across launches. *Choices:* SwiftUI's native
  `List.onMove` modifier on the `ForEach` in `RouteListView` over a
  custom `DropDelegate` (no payoff for v1; native gives the macOS
  reorder cursor, drop-line indicator, and `RouteRow` identity
  stability for free). Single new `@MainActor` mutation
  `EngineStore.moveRoute(from:to:)` mirrors `addRoute` / `removeRoute`
  shape — no-op short-circuit on same-position drops avoids spurious
  `state.json` snapshots. Persistence is automatic: `[StoredRoute]`
  was already an ordered JSON array and `restoreRoutes` already
  rehydrates in array order. No engine, ABI, or schema changes (ABI
  stays at v14, `currentSchemaVersion` stays at 1). Six new
  `[engine_store][moveRoute]` Swift Testing cases. Manual smoke
  covers the gesture (no automated UI-test infrastructure per
  CLAUDE.md). *Diff:* +~150 LOC tests + ~15 LOC engine-store + 1 LOC
  view + doc updates. See [`docs/2026-05-01-route-reorder-design.md`](./2026-05-01-route-reorder-design.md).
```

After committing, replace `<abbrev hash>` with the abbreviated SHA from `git log --oneline -1` (the Task 3 commit). The plan should reference the implementation commit, not this Task-4 commit.

- [ ] **Step 2: Append a sentence to `docs/spec.md` § 4.1**

Open `docs/spec.md`. Locate § 4.1 ("Main window") at line ~600. Find the bullet about the route list (line ~622: `- **Route list:** rows show name, source → destination summary, ...`).

Append a new bullet directly after the route-list bullet:

```markdown
- **Route ordering:** the user controls the order of routes in the list by dragging strips (any non-button area is a drag handle). Order is persisted in `state.json` (`routes: [StoredRoute]` is already an ordered array — no schema change). Engine scheduling is per-device, so the visible order is purely a UI/persistence concern.
```

- [ ] **Step 3: Verify the doc edits build cleanly (sanity)**

`docs/plan.md` and `docs/spec.md` are Markdown — no build step. Just visually skim the diff:

```bash
git diff docs/plan.md docs/spec.md | head -80
```

Expected: only the two additions above; no whitespace damage to surrounding lines.

- [ ] **Step 4: Commit**

```bash
git add docs/plan.md docs/spec.md
git commit -m "$(cat <<'EOF'
docs(reorder): record drag-to-reorder under Phase 6 + spec § 4.1

Phase 6 deviation entry references the implementation commit and
the design doc. Spec § 4.1 gains a "Route ordering" bullet pinning
the user-visible contract: drag any non-button area, persists via
the existing routes array, no schema bump.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Final verification (`make verify`)

**Files:** none — verification only.

Per the CLAUDE.md pre-commit checklist, `make verify` must be green before claiming the work is done.

- [ ] **Step 1: Run the full pipeline**

```bash
make verify 2>&1 | tail -40
```

Expected: all of these pass — RT-safety scan clean, Release build clean, Swift Testing all passing (including the six new `moveRoute` cases), C++ Catch2 cases all passing (untouched by this change but the gate runs them anyway), TSan run clean.

If any step fails:

- **Swift test failure in `moveRoute` cases:** re-read Task 1 / Task 2 carefully; the most likely root cause is the `IndexSet` / `destination` semantic mismatch (SwiftUI's `List.onMove` uses "before this index" — `Array.move` matches it).
- **C++ failures:** unrelated to this change (this change touches no C++); investigate the underlying flake or pre-existing failure independently.
- **TSan failure:** unrelated to this change (`moveRoute` is `@MainActor`-isolated; no concurrency surface added).
- **RT-safety scan failure:** impossible — this change touches no `Sources/JboxEngineC/rt/` file.

- [ ] **Step 2: Confirm the working tree is clean**

```bash
git status
```

Expected: `nothing to commit, working tree clean` — all four prior commits (Task 1, Task 2, Task 3, Task 4) are pushed (or at least committed locally). No stray TODOs, no orphan files.

- [ ] **Step 3: Verify git log reads cleanly**

```bash
git log --oneline -5
```

Expected: roughly four new commits at the top, in this order (most recent first):

1. `docs(reorder): record drag-to-reorder under Phase 6 + spec § 4.1`
2. `feat(reorder): hook List.onMove into RouteListView`
3. `test(reorder): cover moveRoute no-op + multi-row + exactly-once-fire`
4. `feat(reorder): add EngineStore.moveRoute with no-op short-circuit`

All co-authored by `Claude Opus 4.7 (1M context)`. None contain `--no-verify` or `--no-gpg-sign`.

If the log shows a "polish" or "fix" commit on top of the four above, squash it into its predecessor before considering the work done (per the user-feedback memory: "no follow-up polish commits"). Use `git rebase -i HEAD~5` to squash; then re-run `make verify`.

- [ ] **Step 4: Hand back to the user**

Tell the user:
- Drag-to-reorder is implemented; quit/relaunch persistence verified manually.
- Branch state: 4 new commits on `master` (or whichever working branch was used).
- No remote push (`git push`) yet — that requires explicit user authorization per CLAUDE.md.
- Outstanding follow-up (optional): VoiceOver "Reorder" rotor — if the manual smoke step in Task 3.3 (#9) showed it absent, add a small `accessibilityAction(named:)` follow-up. Not blocking.

---

## Self-review notes

- **Spec coverage:** every section of `docs/2026-05-01-route-reorder-design.md` maps to a task — `EngineStore.moveRoute` → Task 1+2, `RouteListView` `.onMove` → Task 3, doc updates → Task 4, final verify → Task 5. The persistence-round-trip "no new test needed" point is explicitly carried into the test plan (no Task adds one).
- **Type consistency:** `moveRoute(from: IndexSet, to: Int)` is the only new symbol. The same signature appears in: the impl in Task 1 Step 3, all six tests in Tasks 1–2, and the call site in Task 3 Step 1.
- **No placeholders:** every code block is complete; every command shows its expected outcome; every commit message is fully written.
