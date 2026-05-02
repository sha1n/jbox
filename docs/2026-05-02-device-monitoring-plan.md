# Device-Disconnect Monitoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the F1 reaction-layer gap so a route on an aggregate device transitions to WAITING when one of its sub-devices is unplugged, and add an independent stall watchdog that detects "no audio flowing" for any other failure mode.

**Architecture:** Three coordinated changes — (1) aggregate-aware UID matching in `RouteManager::handleDeviceChanges`, fed by `BackendDeviceInfo::is_aggregate` + `aggregate_member_uids` populated during `enumerate()`; (2) per-route stall watchdog ticked from `Engine::hotPlugThreadLoop` at 10 Hz; (3) a testable Swift helper that maps `(state, lastError)` to a row error string, consumed by `RouteListView`.

**Tech Stack:** C++17 (engine), Swift 6 + Swift Testing (UI), Catch2 v3 (engine tests), SwiftPM only (no Xcode project).

**Spec:** `docs/2026-05-02-device-monitoring-design.md` (commit `73c5338`).

---

## File Structure

**Engine (C++):**
- Modify: `Sources/JboxEngineC/control/device_backend.hpp` — add `is_aggregate` + `aggregate_member_uids` to `BackendDeviceInfo`.
- Modify: `Sources/JboxEngineC/control/simulated_backend.cpp` — surface those fields through `enumerate()`.
- Modify: `Sources/JboxEngineC/control/device_manager.hpp` / `.cpp` — `appendAggregateMembers(out, uid)` helper.
- Modify: `Sources/JboxEngineC/control/route_manager.hpp` / `.cpp` — `RouteRecord::watched_uids` + stall-watchdog fields; `attemptStart` populates `watched_uids` and resets stall state; `handleDeviceChanges` walks `watched_uids` and re-expands on `kAggregateMembersChanged`; new `tickStallWatchdog(now)` method.
- Modify: `Sources/JboxEngineC/control/engine.hpp` / `.cpp` — `hotPlugThreadLoop` calls `rm_.tickStallWatchdog(now)` each tick.
- Modify: `Sources/JboxEngineC/control/core_audio_backend.cpp` — `enumerate()` populates the aggregate fields using the existing `getActiveSubDevices` helper.
- Modify: `Sources/JboxEngineC/include/jbox_engine.h` — bump `JBOX_ENGINE_ABI_VERSION` from 14 to 15; append `JBOX_ERR_DEVICE_STALLED = 10` to `jbox_error_code_t`.
- Modify: `Sources/JboxEngineC/control/bridge_api.cpp` — extend `jbox_error_code_name` switch.

**Engine tests (Catch2):**
- Modify: `Tests/JboxEngineCxxTests/route_manager_test.cpp` — 7 new cases (4 `[aggregate_loss]`, 3 `[stall]`).
- Modify: `Tests/JboxEngineCxxTests/device_manager_test.cpp` — 2 new helper cases.

**Swift UI:**
- Create: `Sources/JboxEngineSwift/RouteRowErrorText.swift` — pure mapping function `routeRowErrorText(state:lastError:)`.
- Modify: `Sources/JboxApp/RouteListView.swift` — `errorText` accessor delegates to the helper.

**Swift tests (Swift Testing):**
- Create: `Tests/JboxEngineTests/RouteRowErrorTextTests.swift` — 4 cases.

**Docs:**
- Modify: `docs/plan.md` — new sub-phase `7.6.6 — Aggregate-loss detection + stall watchdog` under Phase 7.6 with deviation entry citing this plan + commits.
- Modify: `docs/followups.md` — note that F1's manual-acceptance test #1 (aggregate sub-device yank) now has a regression test on the simulator path; production HAL pass remains the user's gate.

---

## Task 1: Add `is_aggregate` + `aggregate_member_uids` to `BackendDeviceInfo`

**Files:**
- Modify: `Sources/JboxEngineC/control/device_backend.hpp:48-67`

This is a struct field addition with no new behavior. No test required — downstream tasks consume the fields and bring their own tests.

- [ ] **Step 1: Add the fields**

Edit `Sources/JboxEngineC/control/device_backend.hpp`. Replace the closing `};` of `BackendDeviceInfo` (around line 67) with the two new fields above it:

```cpp
    std::uint32_t output_safety_offset_frames = 0;

    // Phase 7.6.6 (aggregate-loss detection): set to true on aggregate
    // devices; `aggregate_member_uids` then carries the active sub-
    // device list reported by macOS (kAudioAggregateDevicePropertyActive
    // SubDeviceList). Empty / false on non-aggregate devices. Populated
    // during enumerate(); used by RouteManager to expand the per-route
    // watched-UID set so a sub-device IsAlive=0 event matches a route
    // that was started against the aggregate.
    bool                       is_aggregate = false;
    std::vector<std::string>   aggregate_member_uids;
};
```

The header already includes `<string>` and `<vector>` (search to confirm). If not, add them at the top alongside the other `#include`s.

- [ ] **Step 2: Verify the header includes string + vector**

Run:
```sh
grep -nE "#include <(string|vector)>" Sources/JboxEngineC/control/device_backend.hpp
```
Expected: both lines present. If either is missing, add it under the existing `#include` block.

- [ ] **Step 3: Build to confirm no compile breaks**

Run: `swift build` from the repo root.
Expected: clean build (struct field additions are backward-compatible with all existing call sites because they zero-/default-init).

- [ ] **Step 4: Commit**

```bash
git add Sources/JboxEngineC/control/device_backend.hpp
git commit -m "$(cat <<'EOF'
feat(engine): add aggregate-member fields to BackendDeviceInfo

Phase 7.6.6 prep: is_aggregate + aggregate_member_uids surface the
HAL's active-sub-device list to the engine, so RouteManager can detect
when a sub-device of an aggregate that backs a running route is lost.

Both fields default-init to non-aggregate values; existing producers and
consumers compile unchanged. Population in enumerate() lands in
follow-up commits (CoreAudioBackend + SimulatedBackend).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: SimulatedBackend surfaces `is_aggregate` + `aggregate_member_uids` through `enumerate()`

**Files:**
- Modify: `Sources/JboxEngineC/control/simulated_backend.cpp:161-168`
- Test: `Tests/JboxEngineCxxTests/device_manager_test.cpp` (extend with the new helper cases)

The simulator already stores `sub_device_uids` on its `DeviceSlot` (set by `addAggregateDevice`). Today `enumerate()` only copies `slot.info` — not the sub-device list. This task makes `enumerate()` set `is_aggregate = !sub_device_uids.empty()` and copy the vector.

- [ ] **Step 1: Write the failing test**

Add a new case to `Tests/JboxEngineCxxTests/device_manager_test.cpp`. First peek at the existing structure:

```sh
head -30 Tests/JboxEngineCxxTests/device_manager_test.cpp
```

Append at end of file:

```cpp
TEST_CASE("DeviceManager: aggregate device exposes its active sub-device UIDs",
          "[device_manager][aggregate_members]") {
    using jbox::control::BackendDeviceInfo;
    using jbox::control::DeviceManager;
    using jbox::control::SimulatedBackend;
    using jbox::control::kBackendDirectionInput;

    auto b = std::make_unique<SimulatedBackend>();

    BackendDeviceInfo member_a;
    member_a.uid = "member-a";
    member_a.name = "member-a";
    member_a.direction = kBackendDirectionInput;
    member_a.input_channel_count = 2;
    member_a.nominal_sample_rate = 48000.0;
    member_a.buffer_frame_size = 64;
    b->addDevice(member_a);

    BackendDeviceInfo member_b = member_a;
    member_b.uid = "member-b";
    member_b.name = "member-b";
    b->addDevice(member_b);

    BackendDeviceInfo agg;
    agg.uid = "agg";
    agg.name = "agg";
    agg.direction = kBackendDirectionInput;
    agg.input_channel_count = 4;
    agg.nominal_sample_rate = 48000.0;
    agg.buffer_frame_size = 64;
    b->addAggregateDevice(agg, {"member-a", "member-b"});

    DeviceManager dm(std::move(b));
    dm.refresh();

    const auto* info = dm.findByUid("agg");
    REQUIRE(info != nullptr);
    REQUIRE(info->is_aggregate);
    REQUIRE(info->aggregate_member_uids ==
            std::vector<std::string>{"member-a", "member-b"});

    const auto* member_info = dm.findByUid("member-a");
    REQUIRE(member_info != nullptr);
    REQUIRE_FALSE(member_info->is_aggregate);
    REQUIRE(member_info->aggregate_member_uids.empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make cxx-test` (or `swift run JboxEngineCxxTests '[aggregate_members]'`)
Expected: FAIL with `is_aggregate` being `false` on the aggregate device — because `SimulatedBackend::enumerate()` doesn't set it yet.

- [ ] **Step 3: Implement — make `enumerate()` populate the new fields**

Edit `Sources/JboxEngineC/control/simulated_backend.cpp:161-168`. Replace the existing body of `SimulatedBackend::enumerate`:

```cpp
std::vector<BackendDeviceInfo> SimulatedBackend::enumerate() {
    std::vector<BackendDeviceInfo> out;
    out.reserve(devices_.size());
    for (const auto& [uid, slot] : devices_) {
        BackendDeviceInfo info = slot.info;
        // Phase 7.6.6: surface the simulator's stored sub-device list
        // through the public BackendDeviceInfo fields so DeviceManager
        // / RouteManager / tests don't have to reach into SlotState
        // to discover aggregate composition. Mirrors what
        // CoreAudioBackend::enumerate publishes from
        // kAudioAggregateDevicePropertyActiveSubDeviceList.
        info.is_aggregate = !slot.sub_device_uids.empty();
        info.aggregate_member_uids = slot.sub_device_uids;
        out.push_back(std::move(info));
    }
    return out;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `swift run JboxEngineCxxTests '[aggregate_members]'`
Expected: PASS.

- [ ] **Step 5: Run the full C++ suite to verify no regressions**

Run: `make cxx-test`
Expected: green.

- [ ] **Step 6: Commit**

```bash
git add Sources/JboxEngineC/control/simulated_backend.cpp Tests/JboxEngineCxxTests/device_manager_test.cpp
git commit -m "$(cat <<'EOF'
feat(engine): SimulatedBackend exposes aggregate sub-device UIDs

enumerate() now sets BackendDeviceInfo::is_aggregate and
aggregate_member_uids from the slot's stored sub_device_uids list.
Mirrors what CoreAudioBackend will publish from
kAudioAggregateDevicePropertyActiveSubDeviceList in a follow-up.

Adds [device_manager][aggregate_members] regression on
DeviceManager::findByUid round-tripping the new fields.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `DeviceManager::appendAggregateMembers` helper

**Files:**
- Modify: `Sources/JboxEngineC/control/device_manager.hpp:33-70`
- Modify: `Sources/JboxEngineC/control/device_manager.cpp:7-29`
- Test: `Tests/JboxEngineCxxTests/device_manager_test.cpp`

A pure helper that pushes the active sub-device UIDs of `uid` (if it's an aggregate) onto `out`. No-op for non-aggregate UIDs and unknown UIDs.

- [ ] **Step 1: Write the failing test**

Append to `Tests/JboxEngineCxxTests/device_manager_test.cpp`:

```cpp
TEST_CASE("DeviceManager: appendAggregateMembers is a no-op for non-aggregate UIDs",
          "[device_manager][aggregate_members]") {
    using jbox::control::BackendDeviceInfo;
    using jbox::control::DeviceManager;
    using jbox::control::SimulatedBackend;
    using jbox::control::kBackendDirectionInput;

    auto b = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo info;
    info.uid = "plain";
    info.name = "plain";
    info.direction = kBackendDirectionInput;
    info.input_channel_count = 2;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    b->addDevice(info);
    DeviceManager dm(std::move(b));
    dm.refresh();

    std::vector<std::string> out{"pre-existing"};
    dm.appendAggregateMembers(out, "plain");
    REQUIRE(out == std::vector<std::string>{"pre-existing"});

    dm.appendAggregateMembers(out, "unknown-uid");
    REQUIRE(out == std::vector<std::string>{"pre-existing"});
}

TEST_CASE("DeviceManager: appendAggregateMembers appends each active sub-device UID",
          "[device_manager][aggregate_members]") {
    using jbox::control::BackendDeviceInfo;
    using jbox::control::DeviceManager;
    using jbox::control::SimulatedBackend;
    using jbox::control::kBackendDirectionInput;

    auto b = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo m;
    m.direction = kBackendDirectionInput;
    m.input_channel_count = 2;
    m.nominal_sample_rate = 48000.0;
    m.buffer_frame_size = 64;
    m.uid = "a"; m.name = "a"; b->addDevice(m);
    m.uid = "b"; m.name = "b"; b->addDevice(m);
    BackendDeviceInfo agg = m;
    agg.uid = "agg"; agg.name = "agg"; agg.input_channel_count = 4;
    b->addAggregateDevice(agg, {"a", "b"});
    DeviceManager dm(std::move(b));
    dm.refresh();

    std::vector<std::string> out;
    dm.appendAggregateMembers(out, "agg");
    REQUIRE(out == std::vector<std::string>{"a", "b"});

    // Idempotent on repeat — appends the same set again.
    dm.appendAggregateMembers(out, "agg");
    REQUIRE(out.size() == 4);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `swift run JboxEngineCxxTests '[device_manager][aggregate_members]'`
Expected: FAIL — compile error, `appendAggregateMembers` is not a member of `DeviceManager`.

- [ ] **Step 3: Add the declaration**

Edit `Sources/JboxEngineC/control/device_manager.hpp`. Add a public method between `isPresent` and `backend()` (around line 60):

```cpp
    // Phase 7.6.6 helper: when `uid` belongs to an aggregate device in
    // the cached snapshot, push each of its active sub-device UIDs
    // onto `out`. Pure read; no-op for non-aggregate / unknown UIDs.
    // Used by RouteManager::attemptStart to expand a route's watched-
    // UID set to include sub-devices whose loss must fail the route.
    void appendAggregateMembers(std::vector<std::string>& out,
                                const std::string&        uid) const;
```

- [ ] **Step 4: Implement**

Edit `Sources/JboxEngineC/control/device_manager.cpp`. Append after `findByUid`:

```cpp
void DeviceManager::appendAggregateMembers(std::vector<std::string>& out,
                                            const std::string&        uid) const {
    const BackendDeviceInfo* info = findByUid(uid);
    if (info == nullptr || !info->is_aggregate) return;
    for (const auto& member_uid : info->aggregate_member_uids) {
        out.push_back(member_uid);
    }
}
```

- [ ] **Step 5: Run to verify pass**

Run: `swift run JboxEngineCxxTests '[device_manager][aggregate_members]'`
Expected: PASS for all three cases (the round-trip test from Task 2 plus the two new ones).

- [ ] **Step 6: Run the full C++ suite**

Run: `make cxx-test`
Expected: green.

- [ ] **Step 7: Commit**

```bash
git add Sources/JboxEngineC/control/device_manager.hpp Sources/JboxEngineC/control/device_manager.cpp Tests/JboxEngineCxxTests/device_manager_test.cpp
git commit -m "$(cat <<'EOF'
feat(engine): DeviceManager::appendAggregateMembers helper

Pure read against the cached enumeration snapshot; pushes each active
sub-device UID of an aggregate onto an output vector. No-op for
non-aggregate and unknown UIDs. Used by upcoming RouteManager work to
expand watched UIDs so a sub-device hot-unplug fails the route that was
started against the aggregate.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `RouteRecord::watched_uids` field + populate from `attemptStart`

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.hpp:54-60` (RouteRecord)
- Modify: `Sources/JboxEngineC/control/route_manager.cpp` (`attemptStart`, `teardown`)

This task adds the field and populates it. Behavior change in `handleDeviceChanges` lands in the next task — this one just gives us the data.

- [ ] **Step 1: Add the field to `RouteRecord`**

Edit `Sources/JboxEngineC/control/route_manager.hpp`. Insert after `dest_uid` (around line 59), before the `mapping` field:

```cpp
    std::string              source_uid;
    std::string              dest_uid;
    // Phase 7.6.6: every UID whose loss should fail this route. Always
    // contains source_uid + dest_uid; for routes built on an aggregate,
    // also contains each of the aggregate's active sub-device UIDs at
    // start time. Re-computed at attemptStart and on each
    // kAggregateMembersChanged event for any aggregate this route
    // depends on. Cleared on teardown.
    std::vector<std::string> watched_uids;
    std::vector<ChannelEdge> mapping;
```

- [ ] **Step 2: Find the two attemptStart success-path entry points**

```sh
grep -n "JBOX_ROUTE_STATE_RUNNING" Sources/JboxEngineC/control/route_manager.cpp
```
Expected: `r.state ... = JBOX_ROUTE_STATE_RUNNING;` at two lines (~893 for the duplex path, ~1081 for the full path).

- [ ] **Step 3: Add a private helper to RouteManager**

Edit `Sources/JboxEngineC/control/route_manager.hpp:374-385`. Insert between `releaseRouteResources` and `getOrCreateMux`:

```cpp
    // Phase 7.6.6: rebuild r.watched_uids = {source_uid, dest_uid} ∪
    // (aggregate members of source_uid) ∪ (aggregate members of
    // dest_uid). Called at attemptStart's RUNNING transition and from
    // handleDeviceChanges on kAggregateMembersChanged for an aggregate
    // referenced by this route.
    void rebuildWatchedUids(RouteRecord& r);
```

- [ ] **Step 4: Implement the helper**

Edit `Sources/JboxEngineC/control/route_manager.cpp`. Find the line with `void RouteManager::teardown(`:

```sh
grep -n "void RouteManager::teardown" Sources/JboxEngineC/control/route_manager.cpp
```

Insert this implementation just above `teardown`:

```cpp
void RouteManager::rebuildWatchedUids(RouteRecord& r) {
    r.watched_uids.clear();
    r.watched_uids.reserve(4);
    r.watched_uids.push_back(r.source_uid);
    if (r.dest_uid != r.source_uid) {
        r.watched_uids.push_back(r.dest_uid);
    }
    dm_.appendAggregateMembers(r.watched_uids, r.source_uid);
    if (r.dest_uid != r.source_uid) {
        dm_.appendAggregateMembers(r.watched_uids, r.dest_uid);
    }
}
```

- [ ] **Step 5: Wire `rebuildWatchedUids` into both attemptStart success paths**

In each of the two RUNNING-transition spots in `attemptStart`, immediately AFTER the line `r.state = JBOX_ROUTE_STATE_RUNNING;` and BEFORE `r.last_error = JBOX_OK;` (or the kLogRouteStarted log), add:

```cpp
        rebuildWatchedUids(r);
```

There are two sites — both should be edited identically. To distinguish them when using the Edit tool, search for unique surrounding context. The two existing snippets are:

Duplex fast path (~line 893):
```cpp
        r.duplex_ioproc_id = id;
        r.duplex_mode      = true;
        r.state            = JBOX_ROUTE_STATE_RUNNING;
        r.last_error       = JBOX_OK;
```
Becomes:
```cpp
        r.duplex_ioproc_id = id;
        r.duplex_mode      = true;
        r.state            = JBOX_ROUTE_STATE_RUNNING;
        rebuildWatchedUids(r);
        r.last_error       = JBOX_OK;
```

Full path (~line 1081):
```cpp
    r.state      = JBOX_ROUTE_STATE_RUNNING;
    r.last_error = JBOX_OK;
```
Becomes:
```cpp
    r.state      = JBOX_ROUTE_STATE_RUNNING;
    rebuildWatchedUids(r);
    r.last_error = JBOX_OK;
```

- [ ] **Step 6: Clear `watched_uids` in teardown**

Find the `RouteManager::teardown` body and add a `r.watched_uids.clear();` line. The pattern in this file is to clear all per-run state in teardown; add it alongside the existing `releaseRouteResources(r);` call. Specifically, find the sequence:

```cpp
    releaseRouteResources(r);
    r.state      = JBOX_ROUTE_STATE_STOPPED;
    r.last_error = JBOX_OK;
}
```

and change it to:

```cpp
    releaseRouteResources(r);
    r.watched_uids.clear();
    r.state      = JBOX_ROUTE_STATE_STOPPED;
    r.last_error = JBOX_OK;
}
```

- [ ] **Step 7: Build and run existing C++ suite**

Run: `make cxx-test`
Expected: all existing tests still green. (No new test in this task — the field's effects are exercised by the next task's test.)

- [ ] **Step 8: Commit**

```bash
git add Sources/JboxEngineC/control/route_manager.hpp Sources/JboxEngineC/control/route_manager.cpp
git commit -m "$(cat <<'EOF'
feat(engine): RouteRecord::watched_uids tracks aggregate members

Every route now carries a flat set of every UID whose loss should fail
it: source_uid + dest_uid + (active sub-device UIDs of either, when
aggregate). Populated at attemptStart's RUNNING transition via
rebuildWatchedUids; cleared on teardown.

Reaction-layer matcher rewrite that consumes this set lands in the
following commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `handleDeviceChanges` matcher walks `watched_uids` (closes the aggregate-loss gap)

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.cpp:687-714` (the `kDeviceIsNotAlive` branch)
- Test: `Tests/JboxEngineCxxTests/route_manager_test.cpp`

This task wires aggregate-aware loss detection. The `kAggregateMembersChanged` re-expansion path is Task 6.

- [ ] **Step 1: Write the failing test**

Append to `Tests/JboxEngineCxxTests/route_manager_test.cpp` (after the existing `[device_loss]` block):

```cpp
TEST_CASE("RouteManager: aggregate sub-device IsNotAlive transitions running route to WAITING + DEVICE_GONE",
          "[route_manager][aggregate_loss]") {
    using std::vector;
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    // Two physical devices ...
    backend->addDevice(makeInputDevice("phys-a", 2));
    backend->addDevice(makeInputDevice("phys-b", 2));
    // ... wrapped in an aggregate that the route binds to.
    BackendDeviceInfo agg = makeInputDevice("agg", 4);
    backend->addAggregateDevice(agg, {"phys-a", "phys-b"});
    backend->addDevice(makeOutputDevice("dst", 4));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"agg", "dst", m, "agg-route"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // Sub-device of the aggregate vanishes. macOS fires
    // kDeviceIsNotAlive on the SUB-device's UID, not the aggregate's.
    // Today (pre-fix) this misses the matcher and the route stays
    // RUNNING even though audio has stopped.
    backend->removeDevice("phys-a");
    rm.handleDeviceChanges({
        {DeviceChangeEvent::kDeviceIsNotAlive,  "phys-a"},
    });

    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state      == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_ERR_DEVICE_GONE);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `swift run JboxEngineCxxTests '[aggregate_loss]'`
Expected: FAIL — `status.state` is still `RUNNING` after the IsNotAlive event because the matcher only checks `r.source_uid` / `r.dest_uid`.

- [ ] **Step 3: Rewrite the matcher**

Edit `Sources/JboxEngineC/control/route_manager.cpp`. Find the `kDeviceIsNotAlive` case in `handleDeviceChanges` (around line 687-707):

```cpp
            case DeviceChangeEvent::kDeviceIsNotAlive: {
                if (ev.uid.empty()) continue;
                for (auto& [id, rec] : routes_) {
                    auto& r = *rec;
                    if (r.state != JBOX_ROUTE_STATE_RUNNING) continue;
                    if (r.source_uid != ev.uid && r.dest_uid != ev.uid) continue;
                    // Tear down — releases ring / converter / scratch /
                    // mux attachments — then transition to WAITING with
                    // the device-loss origin code. teardown() always
                    // sets state = STOPPED + last_error = JBOX_OK; we
                    // override both immediately afterward.
                    teardown(r);
                    r.state      = JBOX_ROUTE_STATE_WAITING;
                    r.last_error = JBOX_ERR_DEVICE_GONE;
                    tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting,
                               id,
                               r.source_uid == ev.uid ? 1u : 0u,
                               r.dest_uid   == ev.uid ? 1u : 0u);
                }
                break;
            }
```

Replace with:

```cpp
            case DeviceChangeEvent::kDeviceIsNotAlive: {
                if (ev.uid.empty()) continue;
                for (auto& [id, rec] : routes_) {
                    auto& r = *rec;
                    if (r.state != JBOX_ROUTE_STATE_RUNNING) continue;
                    // Phase 7.6.6: walk r.watched_uids — set populated
                    // by attemptStart to include source_uid + dest_uid
                    // plus any aggregate sub-device UIDs. Pre-7.6.6
                    // this only checked source_uid / dest_uid; an
                    // unplugged sub-device of an aggregate that backed
                    // the route was missed entirely.
                    const bool matched = std::find(r.watched_uids.begin(),
                                                   r.watched_uids.end(),
                                                   ev.uid) != r.watched_uids.end();
                    if (!matched) continue;
                    teardown(r);
                    r.state      = JBOX_ROUTE_STATE_WAITING;
                    r.last_error = JBOX_ERR_DEVICE_GONE;
                    tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting,
                               id,
                               r.source_uid == ev.uid ? 1u : 0u,
                               r.dest_uid   == ev.uid ? 1u : 0u);
                }
                break;
            }
```

- [ ] **Step 4: Run to verify pass**

Run: `swift run JboxEngineCxxTests '[aggregate_loss]'`
Expected: PASS.

- [ ] **Step 5: Run the full `[route_manager]` group**

Run: `swift run JboxEngineCxxTests '[route_manager]'`
Expected: green. The existing `[device_loss]` cases (5 cases) still pass — they bind to non-aggregate `src` / `dst`, and `watched_uids` for those routes still contains those UIDs (because `rebuildWatchedUids` always pushes source_uid + dest_uid first).

- [ ] **Step 6: Commit**

```bash
git add Sources/JboxEngineC/control/route_manager.cpp Tests/JboxEngineCxxTests/route_manager_test.cpp
git commit -m "$(cat <<'EOF'
fix(engine): aggregate sub-device loss now transitions route to WAITING

handleDeviceChanges' kDeviceIsNotAlive matcher walks RouteRecord::
watched_uids instead of just source_uid / dest_uid. Closes the
real-world gap where a USB interface that's a member of an aggregate
device was unplugged but the route stayed RUNNING — observed on
multi-day app sessions; the simulator path that 7.6.4 originally
shipped didn't exercise aggregate composition.

Adds [route_manager][aggregate_loss] regression: route on aggregate
{phys-a, phys-b} → IsNotAlive(phys-a) → WAITING + DEVICE_GONE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `kAggregateMembersChanged` re-expands `watched_uids` and tears down on member loss

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.cpp` (the `kAggregateMembersChanged` branch + post-loop refresh pass)
- Test: `Tests/JboxEngineCxxTests/route_manager_test.cpp`

When an aggregate's active sub-device list shrinks past what the route was binding to, we tear the route down even if no IsNotAlive event matched (some macOS edges only fire `kAggregateMembersChanged`). When the list grows or shifts without losing a member, the route stays RUNNING.

- [ ] **Step 1: Write two failing tests**

Append to `Tests/JboxEngineCxxTests/route_manager_test.cpp`:

```cpp
TEST_CASE("RouteManager: aggregate members changed — load-bearing member missing tears down running route",
          "[route_manager][aggregate_loss]") {
    using std::vector;
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeInputDevice("phys-a", 2));
    backend->addDevice(makeInputDevice("phys-b", 2));
    BackendDeviceInfo agg = makeInputDevice("agg", 4);
    backend->addAggregateDevice(agg, {"phys-a", "phys-b"});
    backend->addDevice(makeOutputDevice("dst", 4));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"agg", "dst", m, "agg-shrink"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // The aggregate now reports only one of its two original members.
    // No kDeviceIsNotAlive event fires for phys-a (some macOS paths
    // only emit the aggregate-level signal). Re-expanding watched_uids
    // and re-checking against the freshly refreshed dm_ must surface
    // the loss anyway.
    backend->removeDevice("phys-a");
    BackendDeviceInfo agg_smaller = makeInputDevice("agg", 2);
    backend->addAggregateDevice(agg_smaller, {"phys-b"});
    rm.handleDeviceChanges({
        {DeviceChangeEvent::kAggregateMembersChanged, "agg"},
    });

    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state      == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_ERR_DEVICE_GONE);
}

TEST_CASE("RouteManager: aggregate members changed without loss leaves running route alone",
          "[route_manager][aggregate_loss]") {
    using std::vector;
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeInputDevice("phys-a", 2));
    backend->addDevice(makeInputDevice("phys-b", 2));
    BackendDeviceInfo agg = makeInputDevice("agg", 4);
    backend->addAggregateDevice(agg, {"phys-a", "phys-b"});
    backend->addDevice(makeOutputDevice("dst", 4));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"agg", "dst", m, "agg-grow"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Aggregate gains a member. No member of the original set is lost,
    // so the route stays RUNNING.
    backend->addDevice(makeInputDevice("phys-c", 2));
    BackendDeviceInfo agg_bigger = makeInputDevice("agg", 6);
    backend->addAggregateDevice(agg_bigger, {"phys-a", "phys-b", "phys-c"});
    rm.handleDeviceChanges({
        {DeviceChangeEvent::kAggregateMembersChanged, "agg"},
    });

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `swift run JboxEngineCxxTests '[aggregate_loss]'`
Expected: FAIL on the "members changed — load-bearing member missing" case (route stays RUNNING because the existing handler treats `kAggregateMembersChanged` purely as a "list changed → retry WAITING" signal).

- [ ] **Step 3: Implement re-expansion + member-loss check**

Edit `Sources/JboxEngineC/control/route_manager.cpp`. Find the `handleDeviceChanges` body. Two changes:

**3a.** At the top of the function, add a local `aggregates_to_reexpand` set right next to the existing `any_list_change` flag:

```cpp
    if (events.empty()) return;

    bool any_list_change = false;
    std::unordered_set<std::string> aggregates_to_reexpand;
    for (const auto& ev : events) {
        switch (ev.kind) {
```

You'll need to add `#include <unordered_set>` near the top of the file if it isn't already there:

```sh
grep -n "#include <unordered_" Sources/JboxEngineC/control/route_manager.cpp
```

**3b.** Update the `kAggregateMembersChanged` branch (currently combined with `kDeviceListChanged`):

Replace:
```cpp
            case DeviceChangeEvent::kDeviceListChanged:
            case DeviceChangeEvent::kAggregateMembersChanged: {
                any_list_change = true;
                break;
            }
```

With:
```cpp
            case DeviceChangeEvent::kDeviceListChanged: {
                any_list_change = true;
                break;
            }
            case DeviceChangeEvent::kAggregateMembersChanged: {
                any_list_change = true;
                if (!ev.uid.empty()) {
                    aggregates_to_reexpand.insert(ev.uid);
                }
                break;
            }
```

**3c.** Update the post-loop "(2)" refresh pass. Currently:

```cpp
    if (any_list_change) {
        dm_.refresh();
        for (auto& [id, rec] : routes_) {
            auto& r = *rec;
            if (r.state != JBOX_ROUTE_STATE_WAITING) continue;
            (void)attemptStart(r);
        }
    }
}
```

Replace with:

```cpp
    if (any_list_change) {
        dm_.refresh();

        // Phase 7.6.6: for each running route whose watched_uids
        // includes any of the aggregates whose membership just changed,
        // rebuild watched_uids against the refreshed dm_ and check
        // whether a previously-load-bearing UID is now absent. If so,
        // tear down with DEVICE_GONE — covers the macOS edge where the
        // HAL fires kAggregateMembersChanged without a per-member
        // kDeviceIsNotAlive.
        if (!aggregates_to_reexpand.empty()) {
            for (auto& [id, rec] : routes_) {
                auto& r = *rec;
                if (r.state != JBOX_ROUTE_STATE_RUNNING) continue;
                bool route_uses_changed_aggregate = false;
                for (const auto& agg_uid : aggregates_to_reexpand) {
                    if (std::find(r.watched_uids.begin(),
                                  r.watched_uids.end(),
                                  agg_uid) != r.watched_uids.end()) {
                        route_uses_changed_aggregate = true;
                        break;
                    }
                }
                if (!route_uses_changed_aggregate) continue;

                // Snapshot the previous set, rebuild against current
                // dm_, and see if anything we used to watch has gone
                // missing.
                const std::vector<std::string> previous = r.watched_uids;
                rebuildWatchedUids(r);
                bool member_missing = false;
                for (const auto& uid : previous) {
                    if (uid == r.source_uid || uid == r.dest_uid) continue;
                    if (dm_.findByUid(uid) == nullptr) {
                        member_missing = true;
                        break;
                    }
                }
                if (member_missing) {
                    teardown(r);
                    r.state      = JBOX_ROUTE_STATE_WAITING;
                    r.last_error = JBOX_ERR_DEVICE_GONE;
                    tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting,
                               id, 0u, 0u);
                }
            }
        }

        // Existing path: retry every WAITING route in case the topology
        // change brought devices back. attemptStart re-evaluates
        // watched_uids on the RUNNING transition, so a re-expanded set
        // covers the new aggregate composition.
        for (auto& [id, rec] : routes_) {
            auto& r = *rec;
            if (r.state != JBOX_ROUTE_STATE_WAITING) continue;
            (void)attemptStart(r);
        }
    }
}
```

- [ ] **Step 4: Run to verify pass**

Run: `swift run JboxEngineCxxTests '[aggregate_loss]'`
Expected: PASS for all four `[aggregate_loss]` cases (Task 5's case + this task's two + the round-trip from Task 2).

- [ ] **Step 5: Run the full `[route_manager]` group**

Run: `swift run JboxEngineCxxTests '[route_manager]'`
Expected: green. Particularly verify the `[device_loss]` cases haven't regressed.

- [ ] **Step 6: Commit**

```bash
git add Sources/JboxEngineC/control/route_manager.cpp Tests/JboxEngineCxxTests/route_manager_test.cpp
git commit -m "$(cat <<'EOF'
fix(engine): handle kAggregateMembersChanged with member loss

When the aggregate that backs a running route loses one of its active
sub-devices and macOS only fires kAggregateMembersChanged (no per-member
kDeviceIsNotAlive), handleDeviceChanges now re-expands watched_uids
against the refreshed device snapshot and tears the route down when a
previously-watched member is gone. Aggregates that grow or reshape
without losing a member don't disturb running routes.

Adds two [aggregate_loss] regressions: shrink-with-removal and
grow-without-removal.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Two more `[aggregate_loss]` corner cases

**Files:**
- Test: `Tests/JboxEngineCxxTests/route_manager_test.cpp`

Lock down two edges that fall out of the current implementation but deserve explicit coverage.

- [ ] **Step 1: Add the tests**

Append to `Tests/JboxEngineCxxTests/route_manager_test.cpp`:

```cpp
TEST_CASE("RouteManager: aggregate sub-device loss is idempotent on repeats",
          "[route_manager][aggregate_loss]") {
    using std::vector;
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeInputDevice("phys-a", 2));
    backend->addDevice(makeInputDevice("phys-b", 2));
    BackendDeviceInfo agg = makeInputDevice("agg", 4);
    backend->addAggregateDevice(agg, {"phys-a", "phys-b"});
    backend->addDevice(makeOutputDevice("dst", 4));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"agg", "dst", m, "idempotent-agg"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    backend->removeDevice("phys-a");
    rm.handleDeviceChanges({
        {DeviceChangeEvent::kDeviceIsNotAlive, "phys-a"},
    });
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_ERR_DEVICE_GONE);

    // Replay the same event — the route is already WAITING; no state churn.
    rm.handleDeviceChanges({
        {DeviceChangeEvent::kDeviceIsNotAlive, "phys-a"},
        {DeviceChangeEvent::kDeviceIsNotAlive, "phys-a"},
    });
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state      == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_ERR_DEVICE_GONE);
}

TEST_CASE("RouteManager: route on non-aggregate device is unaffected by unrelated kAggregateMembersChanged",
          "[route_manager][aggregate_loss]") {
    using std::vector;
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeInputDevice("src", 2));
    backend->addDevice(makeOutputDevice("dst", 2));
    // An aggregate exists in the system but no route uses it.
    backend->addDevice(makeInputDevice("phys-a", 2));
    BackendDeviceInfo agg = makeInputDevice("unused-agg", 2);
    backend->addAggregateDevice(agg, {"phys-a"});
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "non-agg-route"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    rm.handleDeviceChanges({
        {DeviceChangeEvent::kAggregateMembersChanged, "unused-agg"},
    });

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
}
```

- [ ] **Step 2: Run**

Run: `swift run JboxEngineCxxTests '[aggregate_loss]'`
Expected: PASS — both cases follow from the implementation already in place.

- [ ] **Step 3: Commit**

```bash
git add Tests/JboxEngineCxxTests/route_manager_test.cpp
git commit -m "$(cat <<'EOF'
test(engine): aggregate-loss idempotency and unrelated-aggregate cases

Locks in two corner cases for the 7.6.6 reaction layer: replaying a
sub-device IsNotAlive event leaves an already-WAITING route alone, and
a kAggregateMembersChanged event for an aggregate no route depends on
doesn't disturb running routes on unrelated non-aggregate devices.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Add `JBOX_ERR_DEVICE_STALLED` to the ABI (v14 → v15)

**Files:**
- Modify: `Sources/JboxEngineC/include/jbox_engine.h:88-122` (header constant + enum)
- Modify: `Sources/JboxEngineC/control/bridge_api.cpp:154-168` (`jbox_error_code_name`)

- [ ] **Step 1: Bump the ABI version constant**

Edit `Sources/JboxEngineC/include/jbox_engine.h`. Replace:

```c
#define JBOX_ENGINE_ABI_VERSION 14u
```

with:

```c
#define JBOX_ENGINE_ABI_VERSION 15u
```

- [ ] **Step 2: Append the v15 history entry above the constant**

Find the v14 entry in the comment block (around line 80-87) and append immediately after its closing comment (before `*/`):

```c
 *  15  MINOR — added JBOX_ERR_DEVICE_STALLED so callers can show a
 *              dedicated "no audio flowing" status when the engine's
 *              stall watchdog catches a RUNNING route whose IOProc
 *              has stopped advancing for ≥ 1.5 s. Distinct from
 *              DEVICE_GONE (HAL signalled the loss explicitly) and
 *              SYSTEM_SUSPENDED (sleep/wake recovery in flight).
 */
#define JBOX_ENGINE_ABI_VERSION 15u
```

(The `*/` and `#define` lines are the existing termination of the block; only the new bullet is added immediately above `*/`.)

- [ ] **Step 3: Append the new error variant to the enum**

In the same header, find `jbox_error_code_t` and add the new variant after `JBOX_ERR_SYSTEM_SUSPENDED`:

```c
    JBOX_ERR_SYSTEM_SUSPENDED    = 9,
    /* ABI v15 (Phase 7.6.6): a route in WAITING because the engine's
     * stall watchdog observed both frames_produced and frames_consumed
     * frozen for ≥ 1.5 s while the route was RUNNING. Set by
     * RouteManager::tickStallWatchdog(now) on any "silent death"
     * failure mode the HAL listeners didn't surface (another app
     * hogged the device, an aggregate's IOProc froze without an
     * IsAlive=0, etc.). Recovery rides on the existing
     * kDeviceListChanged retry path or a manual stop+start. */
    JBOX_ERR_DEVICE_STALLED      = 10
} jbox_error_code_t;
```

(Note: the trailing comma on the `JBOX_ERR_SYSTEM_SUSPENDED = 9` line is required; the existing enum may not currently have one — add it if so.)

- [ ] **Step 4: Extend `jbox_error_code_name`**

Edit `Sources/JboxEngineC/control/bridge_api.cpp`. Find `jbox_error_code_name` (around line 154) and add a case before the closing brace:

```cpp
        case JBOX_ERR_DEVICE_GONE:        return "device gone";
        case JBOX_ERR_SYSTEM_SUSPENDED:   return "system suspended";
        case JBOX_ERR_DEVICE_STALLED:     return "device stalled";
    }
    return "unknown";
}
```

- [ ] **Step 5: Build to confirm clean compile**

Run: `swift build`
Expected: clean. The Swift bridge picks up `JBOX_ERR_DEVICE_STALLED` automatically — Swift's `jbox_error_code_t` import already covers all enum values.

- [ ] **Step 6: Run the C++ suite**

Run: `make cxx-test`
Expected: green. (No new test for the ABI bump itself — the v14 → v15 contract is exercised once the watchdog producer ships in Task 10.)

- [ ] **Step 7: Commit**

```bash
git add Sources/JboxEngineC/include/jbox_engine.h Sources/JboxEngineC/control/bridge_api.cpp
git commit -m "$(cat <<'EOF'
feat(abi): bump v14→v15 with JBOX_ERR_DEVICE_STALLED

New non-error variant of jbox_error_code_t carries the origin code for
routes that the upcoming stall watchdog transitions to WAITING. Same
shape as DEVICE_GONE / SYSTEM_SUSPENDED — last_error is informational
on a WAITING route. Header history note + jbox_error_code_name branch
added; the watchdog producer lands in a follow-up.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: `RouteRecord` stall-watchdog fields + `RouteManager::tickStallWatchdog`

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.hpp` (RouteRecord + RouteManager declarations)
- Modify: `Sources/JboxEngineC/control/route_manager.cpp`
- Test: `Tests/JboxEngineCxxTests/route_manager_test.cpp`

- [ ] **Step 1: Write the failing test**

Append to `Tests/JboxEngineCxxTests/route_manager_test.cpp`:

```cpp
TEST_CASE("RouteManager: tickStallWatchdog transitions running route to WAITING + DEVICE_STALLED after 15 frozen ticks",
          "[route_manager][stall]") {
    using std::chrono::steady_clock;
    Fixture f(2, 2);
    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "stall"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // No deliverBuffer between ticks — frames_produced / frames_consumed
    // never advance. After kStallTickThreshold (15) ticks the watchdog
    // must fire.
    auto now = steady_clock::now();
    for (int i = 0; i < 14; ++i) {
        f.rm->tickStallWatchdog(now);
        REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
        REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
    }
    f.rm->tickStallWatchdog(now);  // 15th tick — fires.
    REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state      == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_ERR_DEVICE_STALLED);
}

TEST_CASE("RouteManager: tickStallWatchdog resets when frames advance on either side",
          "[route_manager][stall]") {
    using std::chrono::steady_clock;
    Fixture f(2, 2);
    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "stall-reset"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    auto now = steady_clock::now();
    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2, 0.5f);
    std::vector<float> output(kFrames * 2, 0.0f);

    // 14 frozen ticks bring stall_ticks to 14.
    for (int i = 0; i < 14; ++i) {
        f.rm->tickStallWatchdog(now);
    }
    // One delivery on the source advances frames_produced AND
    // frames_consumed (the existing simulator pumping advances both
    // when the consumer is also delivered — but for this test, even a
    // source-only delivery advances frames_produced via the IOProc
    // bookkeeping in route_manager.cpp). Either way, the next tick
    // should reset.
    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);
    f.backend->deliverBuffer("dst", kFrames, nullptr, output.data());
    f.rm->tickStallWatchdog(now);

    jbox_route_status_t status{};
    REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // From here, another full 15 frozen ticks are required to fire.
    for (int i = 0; i < 14; ++i) {
        f.rm->tickStallWatchdog(now);
        REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
        REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
    }
    f.rm->tickStallWatchdog(now);
    REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state      == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_ERR_DEVICE_STALLED);
}

TEST_CASE("RouteManager: tickStallWatchdog ignores non-RUNNING routes",
          "[route_manager][stall]") {
    using std::chrono::steady_clock;
    // Build manager with only a destination — startRoute lands in WAITING.
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"missing-src", "dst", m, "stall-ignores-waiting"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);

    auto now = steady_clock::now();
    for (int i = 0; i < 50; ++i) rm.tickStallWatchdog(now);

    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);
    // Crucially: last_error stays as it was (JBOX_OK for initial
    // WAITING) — watchdog did not stomp it.
    REQUIRE(status.last_error == JBOX_OK);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `swift run JboxEngineCxxTests '[stall]'`
Expected: FAIL — compile error, `tickStallWatchdog` is not a member of `RouteManager`.

- [ ] **Step 3: Add the watchdog fields to `RouteRecord`**

Edit `Sources/JboxEngineC/control/route_manager.hpp`. After the existing counter declarations (`frames_consumed`, `underrun_count`, `overrun_count`, around line 152) add:

```cpp
    std::atomic<std::uint64_t> overrun_count{0};

    // Phase 7.6.6 stall watchdog. tickStallWatchdog samples the two
    // frame counters every 100 ms; when neither has advanced since the
    // previous tick AND the route is RUNNING, stall_ticks increments.
    // At kStallTickThreshold (15) the watchdog tears the route down
    // and transitions to WAITING + JBOX_ERR_DEVICE_STALLED. State is
    // re-primed on every entry into RUNNING so a fresh start clears
    // the counter.
    std::uint64_t last_seen_frames_produced = 0;
    std::uint64_t last_seen_frames_consumed = 0;
    std::uint8_t  stall_ticks               = 0;
```

- [ ] **Step 4: Declare `tickStallWatchdog` on `RouteManager`**

Same header. Add a public method right after `tickWakeRetries` (around line 328):

```cpp
    void tickWakeRetries(std::chrono::steady_clock::time_point now);

    // Phase 7.6.6: stall watchdog. Driven by the engine's hot-plug
    // tick at 10 Hz alongside tickHotPlug / tickPower. When a RUNNING
    // route's frames_produced and frames_consumed have both stayed
    // unchanged for kStallTickThreshold (15) consecutive calls, the
    // route is torn down and transitions to WAITING + DEVICE_STALLED.
    // `now` is unused today but parameterized to match the cadence
    // of tickWakeRetries(now); future tightening (per-route deadline
    // arithmetic) won't break the engine wiring.
    void tickStallWatchdog(std::chrono::steady_clock::time_point now);
```

- [ ] **Step 5: Implement `tickStallWatchdog` + reset on RUNNING transition**

Edit `Sources/JboxEngineC/control/route_manager.cpp`. Add a small helper near the existing `rebuildWatchedUids`:

```cpp
namespace {
constexpr std::uint8_t kStallTickThreshold = 15;  // 1.5 s at 10 Hz
}  // namespace

void RouteManager::resetStallWatchdog(RouteRecord& r) {
    r.last_seen_frames_produced = r.frames_produced.load(std::memory_order_relaxed);
    r.last_seen_frames_consumed = r.frames_consumed.load(std::memory_order_relaxed);
    r.stall_ticks = 0;
}
```

Declare `resetStallWatchdog` in the header alongside `rebuildWatchedUids`:

```cpp
    void rebuildWatchedUids(RouteRecord& r);
    // Re-prime the stall-watchdog snapshot on transitions into RUNNING.
    void resetStallWatchdog(RouteRecord& r);
```

Then call `resetStallWatchdog(r)` immediately after each `rebuildWatchedUids(r)` call in the two RUNNING transitions:

Duplex path:
```cpp
        r.state            = JBOX_ROUTE_STATE_RUNNING;
        rebuildWatchedUids(r);
        resetStallWatchdog(r);
        r.last_error       = JBOX_OK;
```

Full path:
```cpp
    r.state      = JBOX_ROUTE_STATE_RUNNING;
    rebuildWatchedUids(r);
    resetStallWatchdog(r);
    r.last_error = JBOX_OK;
```

Now add the `tickStallWatchdog` body. Append at the end of the file's namespace, after `tickWakeRetries`:

```cpp
void RouteManager::tickStallWatchdog(
    std::chrono::steady_clock::time_point now) {
    (void)now;  // reserved for future per-route deadline arithmetic
    for (auto& [id, rec] : routes_) {
        auto& r = *rec;
        if (r.state != JBOX_ROUTE_STATE_RUNNING) continue;
        const auto fp = r.frames_produced.load(std::memory_order_relaxed);
        const auto fc = r.frames_consumed.load(std::memory_order_relaxed);
        if (fp != r.last_seen_frames_produced ||
            fc != r.last_seen_frames_consumed) {
            r.last_seen_frames_produced = fp;
            r.last_seen_frames_consumed = fc;
            r.stall_ticks = 0;
            continue;
        }
        if (++r.stall_ticks < kStallTickThreshold) continue;

        // Stall confirmed. Teardown sets state=STOPPED + last_error=
        // JBOX_OK; we override to WAITING + DEVICE_STALLED so the UI
        // can render the dedicated diagnostic. Recovery rides on the
        // existing kDeviceListChanged retry path or a manual stop+
        // start; the watchdog itself doesn't auto-retry.
        teardown(r);
        r.state      = JBOX_ROUTE_STATE_WAITING;
        r.last_error = JBOX_ERR_DEVICE_STALLED;
        tryPushLog(log_queue_, jbox::rt::kLogRouteWaiting, id, 0u, 0u);
    }
}
```

- [ ] **Step 6: Run the new tests**

Run: `swift run JboxEngineCxxTests '[stall]'`
Expected: PASS for all three cases.

- [ ] **Step 7: Run full route_manager group**

Run: `swift run JboxEngineCxxTests '[route_manager]'`
Expected: green. Existing cases that drive the route to RUNNING and then deliver buffers (e.g. `end-to-end sample flow`) advance frames every cycle and never hit the stall threshold.

- [ ] **Step 8: Commit**

```bash
git add Sources/JboxEngineC/control/route_manager.hpp Sources/JboxEngineC/control/route_manager.cpp Tests/JboxEngineCxxTests/route_manager_test.cpp
git commit -m "$(cat <<'EOF'
feat(engine): add per-route stall watchdog (DEVICE_STALLED)

RouteManager::tickStallWatchdog(now) samples each RUNNING route's
frames_produced + frames_consumed every call. When neither advances
across kStallTickThreshold (15 = 1.5 s at the engine's 10 Hz hot-plug
tick) the route is torn down and transitions to WAITING +
JBOX_ERR_DEVICE_STALLED.

Watchdog state is re-primed on every entry into RUNNING via the new
resetStallWatchdog helper; non-RUNNING routes are skipped so an
already-WAITING route doesn't get its last_error stomped.

Three [stall] cases cover: (a) 15-tick freeze fires, (b) frame advance
on either counter resets the counter, (c) WAITING routes are
unaffected.

Engine wiring (hotPlugThreadLoop calls tickStallWatchdog) lands in a
follow-up so this commit ships purely as RouteManager API.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Wire `tickStallWatchdog` into `Engine::hotPlugThreadLoop`

**Files:**
- Modify: `Sources/JboxEngineC/control/engine.cpp:98-110`
- Test: covered by an Engine integration test if one exists, otherwise verified manually

This is one new line in the existing 10 Hz loop.

- [ ] **Step 1: Add the call**

Edit `Sources/JboxEngineC/control/engine.cpp`. Replace `hotPlugThreadLoop`:

```cpp
void Engine::hotPlugThreadLoop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    constexpr auto period = milliseconds(100);  // 10 Hz
    while (hotplug_running_.load(std::memory_order_relaxed)) {
        next += period;
        const auto now = steady_clock::now();
        if (next < now) next = now;
        std::this_thread::sleep_until(next);
        tickHotPlug();
        tickPower();
        // Phase 7.6.6: stall watchdog rides on the same 10 Hz cadence;
        // tickStallWatchdog is cheap (one atomic load per running
        // route) so a separate thread is unnecessary.
        rm_.tickStallWatchdog(steady_clock::now());
    }
}
```

(Note: the `now` for `tickStallWatchdog` is read fresh because `tickHotPlug` / `tickPower` may have done non-trivial work on the prior `now`.)

- [ ] **Step 2: Build**

Run: `swift build`
Expected: clean.

- [ ] **Step 3: Run the full engine suite**

Run: `make cxx-test`
Expected: green.

- [ ] **Step 4: Run the full Swift suite**

Run: `make swift-test`
Expected: green.

- [ ] **Step 5: Commit**

```bash
git add Sources/JboxEngineC/control/engine.cpp
git commit -m "$(cat <<'EOF'
feat(engine): wire stall watchdog into 10 Hz hot-plug loop

Engine::hotPlugThreadLoop calls rm_.tickStallWatchdog(now) on every
period alongside tickHotPlug / tickPower. Watchdog state is owned by
RouteManager + RouteRecord; the loop is just a fixed-cadence driver.

Non-spawn-sampler tests are unaffected — they never start the loop.
Engine integration paths (jbox_engine_create) pick the watchdog up
automatically with the existing 7.6.4 / 7.6.5 wiring.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: `CoreAudioBackend::enumerate` populates `is_aggregate` + `aggregate_member_uids`

**Files:**
- Modify: `Sources/JboxEngineC/control/core_audio_backend.cpp:337-409`

The simulator path is now correct (Task 2). Production-side population uses the existing `getActiveSubDevices` helper at `core_audio_backend.cpp:886`.

This task has no new automated test — CI cannot exercise real CoreAudio aggregates. The simulator-path tests (Task 2) plus the `[core_audio][hal_translation]` cases give us regression coverage; real-hardware acceptance is the user's gate (see `docs/followups.md` § F1).

- [ ] **Step 1: Add the population pass to `enumerate()`**

Edit `Sources/JboxEngineC/control/core_audio_backend.cpp`. Find the `enumerate` body. Just before the existing `// F1: publish ...` block (around line 393-394), insert:

```cpp
    // Phase 7.6.6: surface aggregate composition. For each device whose
    // active-sub-device list is non-empty (= aggregate per HAL
    // semantics), set is_aggregate = true and translate each sub-
    // AudioObjectID to its UID via the device_ids_ reverse map we just
    // built. Cheap: getActiveSubDevices is one HAL call per device,
    // and returns an empty list immediately for non-aggregates.
    std::unordered_map<AudioDeviceID, std::string> id_to_uid;
    id_to_uid.reserve(device_ids_.size());
    for (const auto& [uid, id] : device_ids_) {
        id_to_uid[id] = uid;
    }
    for (auto& info : result) {
        auto it = device_ids_.find(info.uid);
        if (it == device_ids_.end()) continue;
        const AudioDeviceID id = it->second;
        const auto subs = getActiveSubDevices(id);
        if (subs.empty()) continue;
        info.is_aggregate = true;
        info.aggregate_member_uids.reserve(subs.size());
        for (AudioObjectID sub : subs) {
            auto jt = id_to_uid.find(sub);
            if (jt != id_to_uid.end()) {
                info.aggregate_member_uids.push_back(jt->second);
            }
        }
    }
```

The `getActiveSubDevices` helper is in the anonymous namespace at line 882 of the same file — it's already in scope.

- [ ] **Step 2: Build (without running tests)**

Run: `swift build`
Expected: clean.

- [ ] **Step 3: Run the full C++ suite + RT scan**

Run: `make verify`
Expected: green. The RT-safety scanner doesn't touch `enumerate()` (control-path only).

- [ ] **Step 4: Commit**

```bash
git add Sources/JboxEngineC/control/core_audio_backend.cpp
git commit -m "$(cat <<'EOF'
feat(engine): CoreAudioBackend enumerate fills aggregate composition

Production enumerate() now sets BackendDeviceInfo::is_aggregate +
aggregate_member_uids for every device whose
kAudioAggregateDevicePropertyActiveSubDeviceList read returns a
non-empty list. Reuses the existing getActiveSubDevices helper (also
called by setBufferFrameSize fan-out). Cheap — one HAL call per
device, empty for non-aggregates.

Closes the production half of the 7.6.6 aggregate-loss reaction:
RouteManager::handleDeviceChanges now matches sub-device UIDs the HAL
listener fires for.

CI exercises the simulator path; real-hardware acceptance is the
user's gate per docs/followups.md § F1 acceptance test #1.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Extract `routeRowErrorText` Swift helper into `JboxEngineSwift`

**Files:**
- Create: `Sources/JboxEngineSwift/RouteRowErrorText.swift`
- Test: `Tests/JboxEngineTests/RouteRowErrorTextTests.swift`

Following the existing pattern (`Sources/JboxEngineSwift/MixerLayout.swift` with tests in `Tests/JboxEngineTests/MixerLayoutTests.swift`), the testable string-mapping logic lives in the engine wrapper module.

- [ ] **Step 1: Create the test file**

Create `Tests/JboxEngineTests/RouteRowErrorTextTests.swift`:

```swift
import Testing
import JboxEngineC
@testable import JboxEngineSwift

@Suite("RouteRowErrorText")
struct RouteRowErrorTextTests {
    @Test("returns nil for WAITING with JBOX_OK (initial waiting)")
    func waitingWithoutErrorReturnsNil() {
        #expect(routeRowErrorText(state: .waiting, lastError: JBOX_OK) == nil)
    }

    @Test("returns the device-disconnected line for WAITING with DEVICE_GONE")
    func waitingWithDeviceGone() {
        let text = routeRowErrorText(state: .waiting, lastError: JBOX_ERR_DEVICE_GONE)
        #expect(text == "Device disconnected — waiting for it to return.")
    }

    @Test("returns the stalled line for WAITING with DEVICE_STALLED")
    func waitingWithDeviceStalled() {
        let text = routeRowErrorText(state: .waiting, lastError: JBOX_ERR_DEVICE_STALLED)
        #expect(text == "No audio — device stopped responding.")
    }

    @Test("returns the sleep-recovery line for WAITING with SYSTEM_SUSPENDED")
    func waitingWithSystemSuspended() {
        let text = routeRowErrorText(state: .waiting, lastError: JBOX_ERR_SYSTEM_SUSPENDED)
        #expect(text == "Recovering from sleep…")
    }

    @Test("returns jbox_error_code_name for ERROR state")
    func errorStateReturnsCodeName() {
        let text = routeRowErrorText(state: .error, lastError: JBOX_ERR_MAPPING_INVALID)
        #expect(text == "mapping invalid")
    }

    @Test("returns nil for RUNNING regardless of last_error")
    func runningReturnsNil() {
        #expect(routeRowErrorText(state: .running, lastError: JBOX_OK) == nil)
        // Defensive: even if last_error somehow lingered, RUNNING shouldn't surface text.
        #expect(routeRowErrorText(state: .running, lastError: JBOX_ERR_DEVICE_GONE) == nil)
    }

    @Test("returns nil for STOPPED regardless of last_error")
    func stoppedReturnsNil() {
        #expect(routeRowErrorText(state: .stopped, lastError: JBOX_OK) == nil)
    }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `swift test --filter RouteRowErrorText`
Expected: FAIL — `routeRowErrorText` is not defined.

- [ ] **Step 3: Create the helper**

Create `Sources/JboxEngineSwift/RouteRowErrorText.swift`:

```swift
import JboxEngineC

/// Map a route's `(state, lastError)` pair to the diagnostic string the
/// row should render below the route name. `nil` means "no diagnostic
/// text" — the row keeps its primary status glyph + label only.
///
/// Phase 7.6.6 introduces the WAITING-with-error variants. Prior to
/// 7.6.6 the row only rendered text on the hard ERROR state; an
/// initial-WAITING (no devices yet, last_error == JBOX_OK) and a
/// device-loss WAITING (last_error == JBOX_ERR_DEVICE_GONE) looked
/// identical to the user.
public func routeRowErrorText(state: RouteState,
                              lastError: jbox_error_code_t) -> String? {
    switch state {
    case .error:
        return String(cString: jbox_error_code_name(lastError))
    case .waiting:
        guard lastError != JBOX_OK else { return nil }
        switch lastError {
        case JBOX_ERR_DEVICE_GONE:
            return "Device disconnected — waiting for it to return."
        case JBOX_ERR_DEVICE_STALLED:
            return "No audio — device stopped responding."
        case JBOX_ERR_SYSTEM_SUSPENDED:
            return "Recovering from sleep…"
        default:
            return String(cString: jbox_error_code_name(lastError))
        }
    case .running, .starting, .stopped:
        return nil
    }
}
```

- [ ] **Step 4: Run to verify pass**

Run: `swift test --filter RouteRowErrorText`
Expected: PASS for all 7 cases.

- [ ] **Step 5: Run the full Swift suite**

Run: `make swift-test`
Expected: green.

- [ ] **Step 6: Commit**

```bash
git add Sources/JboxEngineSwift/RouteRowErrorText.swift Tests/JboxEngineTests/RouteRowErrorTextTests.swift
git commit -m "$(cat <<'EOF'
feat(swift): routeRowErrorText helper for WAITING-with-error rows

Pure (RouteState, jbox_error_code_t) → String? mapping lives in
JboxEngineSwift so JboxEngineTests can exercise the human strings
directly. Covers the three 7.6.6-relevant variants (DEVICE_GONE,
DEVICE_STALLED, SYSTEM_SUSPENDED) plus the existing ERROR-state
fallback through jbox_error_code_name.

RouteListView consumption lands in the next commit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: `RouteListView` consumes `routeRowErrorText`

**Files:**
- Modify: `Sources/JboxApp/RouteListView.swift:244-247`

- [ ] **Step 1: Replace the existing `errorText` accessor**

Edit `Sources/JboxApp/RouteListView.swift`. Find:

```swift
    private var errorText: String? {
        guard route.status.state == .error else { return nil }
        return String(cString: jbox_error_code_name(route.status.lastError))
    }
```

Replace with:

```swift
    private var errorText: String? {
        routeRowErrorText(state: route.status.state,
                          lastError: route.status.lastError)
    }
```

- [ ] **Step 2: Build**

Run: `swift build`
Expected: clean. `routeRowErrorText` is in `JboxEngineSwift`, which `JboxApp` already imports.

- [ ] **Step 3: Smoke-test the app**

Run: `make app && open build/Jbox.app` (or `make run`).
Expected: app launches; no behavior regression on running routes (text below route name still shows for ERROR-state routes; new red text appears below WAITING-with-error rows once a device is unplugged).

If you have a real aggregate setup available, exercise the manual hardware acceptance test from `docs/2026-05-02-device-monitoring-design.md § Manual hardware acceptance` test #1 (yank a USB interface that's a member of an aggregate the route uses) and confirm the row flips to "Device disconnected — waiting for it to return." within ~1 s. If hardware isn't available, the engine-side test (Task 5) plus the Swift Testing case (Task 12) are sufficient regression coverage.

- [ ] **Step 4: Run all tests one more time**

Run: `make verify`
Expected: green (full pipeline: RT-scan + Release build + Swift tests + C++ tests + TSan).

- [ ] **Step 5: Commit**

```bash
git add Sources/JboxApp/RouteListView.swift
git commit -m "$(cat <<'EOF'
ui(routes): show diagnostic text on WAITING rows with last_error

RouteListView's row error-text accessor delegates to the new
JboxEngineSwift.routeRowErrorText helper. WAITING routes whose
last_error is non-OK now show a red caption ("Device disconnected — …",
"No audio — …", "Recovering from sleep…") so the user can tell at a
glance whether a route is waiting on a first plug-in vs recovering
from a hot-unplug, stall, or sleep.

The orange-clock glyph is unchanged; only the secondary diagnostic
line is added.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: Update `docs/plan.md` and `docs/followups.md`

**Files:**
- Modify: `docs/plan.md` (Phase 7.6 section — add sub-phase 7.6.6)
- Modify: `docs/followups.md` (note that F1's acceptance test #1 has simulator-path coverage now)

- [ ] **Step 1: Locate the Phase 7.6 deviations block in `plan.md`**

```sh
grep -n "Phase 7.6" docs/plan.md | head -10
```

You should see the existing sub-phases 7.6.3 / 7.6.4 / 7.6.5. Add a new sub-phase header `#### Sub-phase 7.6.6 — Aggregate-loss detection + stall watchdog ✅ (engine; UI)` after 7.6.5's body. Body should be a checklist matching the convention used by the other sub-phases:

```markdown
#### Sub-phase 7.6.6 — Aggregate-loss detection + stall watchdog ✅ (engine; UI)

- [x] **Aggregate-aware UID matching.** `BackendDeviceInfo` gains `is_aggregate` + `aggregate_member_uids`, populated by both backends during `enumerate()`. `RouteRecord::watched_uids` carries `source_uid` + `dest_uid` + each aggregate's active sub-device UIDs; `attemptStart` populates it on RUNNING transition; `RouteManager::handleDeviceChanges` walks the set on `kDeviceIsNotAlive`. Closes the real-world bug where a USB interface that was a member of an aggregate was unplugged and the route stayed RUNNING. Multi-day app sessions surfaced this; the simulator path 7.6.4 originally shipped didn't exercise aggregate composition. See `docs/2026-05-02-device-monitoring-{design,plan}.md`.
- [x] **`kAggregateMembersChanged` re-expansion.** When the aggregate that backs a running route loses one of its active sub-devices and macOS only fires the aggregate-level signal (no per-member `kDeviceIsNotAlive`), `handleDeviceChanges` re-expands `watched_uids` against the refreshed `dm_` and tears the route down when a previously-watched member is gone. Aggregates that grow or reshape without losing a member don't disturb running routes.
- [x] **Stall watchdog (`JBOX_ERR_DEVICE_STALLED`, ABI v14 → v15 additive).** Per-route counters `last_seen_frames_produced` / `last_seen_frames_consumed` / `stall_ticks`; `RouteManager::tickStallWatchdog(now)` increments `stall_ticks` while both counters stay frozen and the route is `RUNNING`. At 15 ticks (1.5 s on the 10 Hz hot-plug cadence) the route is torn down and transitions to `WAITING + JBOX_ERR_DEVICE_STALLED`. Independent safety net for any "silent death" failure mode the HAL listeners didn't surface (another app preempts the device, an aggregate IOProc freezes without an IsAlive=0).
- [x] **UI: diagnostic text on WAITING rows.** `routeRowErrorText` (new) maps `(state, last_error)` to a row caption; `RouteListView` consumes it. WAITING rows whose `last_error` is `DEVICE_GONE` / `DEVICE_STALLED` / `SYSTEM_SUSPENDED` now show a red caption distinguishing them from "first plug-in pending" (last_error == JBOX_OK).
- [x] **Tests.** 7 new Catch2 cases (4 `[aggregate_loss]`, 3 `[stall]`) on the simulator path + 2 new `[device_manager][aggregate_members]` helper cases + 7 Swift Testing cases for `routeRowErrorText`. Total `make verify` impact: +16 cases, all green.

**Manual hardware acceptance (user's gate, mirrors F1).** Build an aggregate in AMS containing two physical interfaces, start a route on the aggregate, yank one interface. Within ~1 s the route should flip to orange-clock + "Device disconnected — waiting for it to return." Re-plug → orange clock disappears, route returns to RUNNING. See `docs/2026-05-02-device-monitoring-design.md § Manual hardware acceptance` for the three-test set.
```

(Adjust headings / numbering to match the existing in-file conventions if they differ.)

- [ ] **Step 2: Add a deviation entry under Phase 7.6's deviations block**

Same file. The existing deviations list is in chronological order (most recent at the top of its block). Insert at the top:

```markdown
- **7.6.6 design — aggregate-aware UID matching + stall watchdog (2026-05-02).** *Goal:* close the real-world bug where a route on an aggregate stayed RUNNING when one of the aggregate's sub-devices was unplugged. *Choices made:*
  - **`watched_uids` on `RouteRecord` over a reverse lookup on `DeviceManager`.** A flat set per route, populated at `attemptStart`, lives where the matcher already iterates. Avoids growing `DeviceManager`'s API surface (previous brainstorming option 1B). Trade-off: the cached set goes stale if AMS reconfigures the aggregate while the route is running, so `handleDeviceChanges` re-expands on every `kAggregateMembersChanged`.
  - **Stall watchdog as an independent safety net rather than relying solely on HAL signals.** Frames-not-advancing is the most direct signal possible — the watchdog catches "silent death" cases that no HAL property listener fires for (another app temporarily preempts the IOProc, an aggregate's internal scheduling goes wrong, an undocumented Apple edge). 1.5 s threshold chosen to dominate any legitimate IOProc gap by two orders of magnitude.
  - **No `kAudioDevicePropertyDeviceIsRunningSomewhere` tiebreaker.** Brainstormed and dropped — the watchdog as-is is conservative enough (both counters must freeze together) and the property is global, not per-IOProc. Capture as a follow-up if real-hardware testing reveals false positives.
  - *Diff:* +14 commits / +~600 LOC engine + tests + docs. `make verify` green.
```

- [ ] **Step 3: Update `docs/followups.md` § F1**

```sh
grep -n "F1 — " docs/followups.md
```

Find F1's acceptance section. Add a note at the bottom of its acceptance subsection:

```markdown
**2026-05-02 update.** F1's acceptance test #1 (yank a sub-device of a running aggregate) now has a simulator-path regression at `[route_manager][aggregate_loss]` (4 cases) — landed under sub-phase 7.6.6 (`docs/2026-05-02-device-monitoring-{design,plan}.md`). Production HAL pass remains the user's gate; F1's status stays "🚧 Engine landed; awaiting manual hardware acceptance."
```

- [ ] **Step 4: Verify the doc renders sensibly**

```sh
grep -n "7.6.6\|aggregate-loss\|DEVICE_STALLED" docs/plan.md docs/followups.md
```

Skim the surrounding paragraphs visually — the new entries should fit the cadence of the existing prose. Fix up any heading-level mismatches.

- [ ] **Step 5: Commit**

```bash
git add docs/plan.md docs/followups.md
git commit -m "$(cat <<'EOF'
docs(7.6.6): record aggregate-loss + stall watchdog sub-phase

plan.md gains sub-phase 7.6.6 with the four checklist items
(aggregate-aware UID matching, kAggregateMembersChanged re-expansion,
stall watchdog at ABI v15, UI diagnostic text) and a deviation entry
covering the design choices.

followups.md § F1 notes that acceptance test #1 (aggregate sub-device
yank) now has simulator-path regression coverage; production HAL pass
remains the user's gate.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

Spec coverage check (each section / requirement → task):

- **Aggregate-aware UID matching** — Tasks 1, 2, 3, 4, 5, 6, 11.
- **`watched_uids` on RouteRecord** — Task 4.
- **`kAggregateMembersChanged` re-expansion** — Task 6.
- **Stall watchdog** — Tasks 8, 9, 10.
- **`JBOX_ERR_DEVICE_STALLED`** ABI bump — Task 8.
- **UI diagnostic text** — Tasks 12, 13.
- **Tests (Catch2 + Swift Testing)** — Tasks 2, 3, 5, 6, 7, 9, 12.
- **Manual hardware acceptance** — referenced in Tasks 13, 14 (no engineering task; user-driven).
- **Docs** — Task 14.
- **R1 naming smell unchanged** — confirmed in design doc, no plan task needed.
- **F3 debounce interaction** — design doc explains, no plan task needed.

Type / signature consistency check:

- `appendAggregateMembers(out, uid)` — defined in Task 3 (`device_manager.{hpp,cpp}`), called in Task 4 (`rebuildWatchedUids`).
- `rebuildWatchedUids(r)` — declared and defined in Task 4, called from `attemptStart` (Task 4) and `handleDeviceChanges` (Task 6).
- `resetStallWatchdog(r)` — declared and defined in Task 9, called from `attemptStart` (Task 9, two sites).
- `tickStallWatchdog(now)` — declared in Task 9, called from `Engine::hotPlugThreadLoop` (Task 10).
- `routeRowErrorText(state:lastError:)` — defined in Task 12, consumed in Task 13.
- `JBOX_ERR_DEVICE_STALLED` — declared in Task 8, consumed in Tasks 9, 12.
- `BackendDeviceInfo::is_aggregate` / `aggregate_member_uids` — declared in Task 1, populated in Tasks 2 and 11, consumed in Task 3.
- `RouteRecord::watched_uids` — declared in Task 4, consumed in Tasks 5, 6, written by Task 4.
- `RouteRecord::last_seen_frames_*` / `stall_ticks` — declared in Task 9, consumed in Task 9.

All identifiers consistent.

Placeholder scan: no "TBD" / "TODO" / "implement later" / "fill in details" / "similar to Task N" / "add appropriate error handling" left in the plan.

---

## Execution

Plan complete and saved to `docs/2026-05-02-device-monitoring-plan.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?
