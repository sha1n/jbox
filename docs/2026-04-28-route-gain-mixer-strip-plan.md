# Route Gain + Mixer-Strip UI — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land per-route master fader, per-channel trims, mute, mixer-strip UI, DAW-standard meter scale, and driver-published channel labels — as specified in `docs/2026-04-28-route-gain-mixer-strip-design.md`.

**Architecture:** Bottom-up. Pure RT-safe DSP first (smoother + taper) → engine plumbing (RouteRecord state, IOProc gain wiring) → ABI v14 (additive C surface) → Swift wrapper + persistence → SwiftUI mixer-strip rebuild. TDD on every pure-logic task; UI tasks rely on existing `#Preview` block conventions.

**Tech Stack:** C++20 (engine), Swift 6 (wrapper + UI), Catch2 v3 (engine tests), Swift Testing (Swift tests), SwiftUI + `Canvas` (UI), SPM-only build.

**Spec reference:** `docs/2026-04-28-route-gain-mixer-strip-design.md`.

---

## File map

### New files
- `Sources/JboxEngineC/rt/gain_smoother.hpp` — RT-safe one-pole IIR per channel.
- `Sources/JboxEngineSwift/FaderTaper.swift` — pure dB↔position taper.
- `Sources/JboxApp/FaderSlider.swift` — vertical SwiftUI fader widget.
- `Sources/JboxApp/ChannelStripColumn.swift` — per-channel strip composing fader + meter.
- `Sources/JboxApp/MasterFaderStrip.swift` — master strip composing fader + readout + mute.
- `Tests/JboxEngineCxxTests/gain_smoother_test.cpp`
- `Tests/JboxEngineCxxTests/route_manager_gain_test.cpp`
- `Tests/JboxEngineTests/FaderTaperTests.swift` *(suite for the JboxEngineSwift target)*
- `Tests/JboxEngineTests/RouteGainPersistenceTests.swift`
- `Tests/JboxEngineTests/EngineStoreGainTests.swift`

### Modified files
- `Sources/JboxEngineC/include/jbox_engine.h` — bump `JBOX_ENGINE_ABI_VERSION` to 14, add `master_gain_db` / `channel_trims_db` / `channel_trims_count` / `muted` to `jbox_route_config_t`, add three setter declarations, backfill v12 / v13 history comments.
- `Sources/JboxEngineC/control/route_manager.hpp` — `RouteRecord` gains atomic targets + RT-thread-local smoothers. `RouteConfig` (the C++ struct) gains the same new fields.
- `Sources/JboxEngineC/control/route_manager.cpp` — `addRoute` initializes trim atomics + reads new config fields, output / duplex IOProc apply gain, new `setMasterGainDb` / `setChannelTrimDb` / `setRouteMute` methods.
- `Sources/JboxEngineC/control/bridge_api.cpp` — implement three new ABI setters; copy new `jbox_route_config_t` fields into the C++ `RouteConfig`.
- `Sources/JboxEngineC/control/engine.hpp` / `engine.cpp` — pass-through methods for the three setters.
- `Sources/JboxEngineSwift/JboxEngine.swift` (or wherever `Route` is defined) — add `masterGainDb` / `trimDbs` / `muted` to the model.
- `Sources/JboxEngineSwift/EngineStore.swift` — three setters + dB→linear conversion before C ABI call.
- `Sources/JboxEngineSwift/Persistence/StoredAppState.swift` — additive optional fields on `StoredRoute`.
- `Sources/JboxEngineSwift/MeterLevel.swift` — `dawScaleMarks` constant.
- `Sources/JboxApp/MeterBar.swift` — rewritten `MeterPanel`, repurposed `DbScale`; `BarGroup` retained only for the SOURCE pre-fader column.

---

## Task 1: `gain_smoother.hpp` — pure RT-safe one-pole IIR

**Files:**
- Create: `Sources/JboxEngineC/rt/gain_smoother.hpp`
- Test: `Tests/JboxEngineCxxTests/gain_smoother_test.cpp`

The smoother is the RT-thread-local component the IOProcs will use to converge `current_gain` toward an atomic `target_gain` over a 10 ms time constant without zipper noise. Pure value type, no allocation, no locks.

- [ ] **Step 1: Write the failing test**

Create `Tests/JboxEngineCxxTests/gain_smoother_test.cpp`:

```cpp
// gain_smoother_test.cpp — unit tests for the RT-thread-local block-rate
// gain smoother (rt/gain_smoother.hpp). The smoother converges current
// toward target via a one-pole IIR; alpha is computed once at start
// from sample rate and a 10 ms time constant. See docs/2026-04-28-
// route-gain-mixer-strip-design.md § 5.2.

#include "gain_smoother.hpp"

#include <catch_amalgamated.hpp>

using jbox::rt::GainSmoother;

namespace {
// Fire `block_count` blocks of `block_frames` each at the given sample
// rate, returning the smoother's final `current` value.
float runBlocks(GainSmoother& s,
                float target,
                std::uint32_t block_frames,
                std::uint32_t block_count) {
    for (std::uint32_t i = 0; i < block_count; ++i) {
        s.step(target, block_frames);
    }
    return s.current;
}
}  // namespace

TEST_CASE("[gain_smoother] step from unity to unity is a no-op",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    REQUIRE(s.current == 1.0f);
    s.step(1.0f, 64);
    REQUIRE(s.current == 1.0f);
}

TEST_CASE("[gain_smoother] reaches 95% of step within ~30 ms at 48 kHz",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    // 30 ms at 48 kHz = 1440 frames. Run as 22 blocks of 64.
    const float final_value = runBlocks(s, /*target=*/0.5f,
                                         /*block_frames=*/64,
                                         /*block_count=*/22);
    // 95% of step from 1.0 to 0.5 is 0.525.
    REQUIRE(final_value <= 0.525f + 0.005f);
    REQUIRE(final_value >= 0.475f);
}

TEST_CASE("[gain_smoother] reaches 95% of step within ~30 ms at 96 kHz",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(96000.0, 0.010);
    // 30 ms at 96 kHz = 2880 frames. 45 blocks of 64.
    const float final_value = runBlocks(s, 0.5f, 64, 45);
    REQUIRE(final_value <= 0.525f + 0.005f);
    REQUIRE(final_value >= 0.475f);
}

TEST_CASE("[gain_smoother] no overshoot on a step",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 1.0f;
    float prev = s.current;
    for (int i = 0; i < 100; ++i) {
        s.step(0.0f, 64);
        REQUIRE(s.current <= prev + 1e-6f);   // monotonic non-increasing
        REQUIRE(s.current >= 0.0f);
        prev = s.current;
    }
}

TEST_CASE("[gain_smoother] mute target reaches < 1e-6 within 50 ms",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 1.0f;
    // 50 ms at 48 kHz = 2400 frames. 38 blocks of 64.
    const float final_value = runBlocks(s, 0.0f, 64, 38);
    REQUIRE(final_value < 1e-6f);
}

TEST_CASE("[gain_smoother] block_frames=0 is a no-op",
          "[gain_smoother]") {
    GainSmoother s;
    s.setTimeConstant(48000.0, 0.010);
    s.current = 0.7f;
    s.step(0.0f, 0);
    REQUIRE(s.current == 0.7f);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
swift run JboxEngineCxxTests '[gain_smoother]'
```

Expected: build error — `gain_smoother.hpp` does not exist.

- [ ] **Step 3: Implement `gain_smoother.hpp`**

Create `Sources/JboxEngineC/rt/gain_smoother.hpp`:

```cpp
// gain_smoother.hpp — RT-safe block-rate one-pole IIR for fader smoothing.
//
// Used per-route, per-channel from the output / duplex IOProc to converge
// `current` toward an atomic `target` value (linear amplitude) over a
// 10 ms time constant. Smoothing is applied once per IOProc block, not
// per sample — at sub-millisecond block sizes the audible difference is
// nil and per-sample smoothing would cost N MACs per channel.
//
// Pure value type. RT-safe by construction:
//   - no heap allocation
//   - no locks, no syscalls
//   - bounded execution time (a constant amount of arithmetic per step)
//
// alpha is computed once at attemptStart via setTimeConstant(rate, tau).
// Equivalent first-order IIR per block: y[n] = y[n-1] + alpha * (target - y[n-1]),
// with alpha sized so the time constant is `tau` regardless of block size,
// using `frames` to keep the response rate-independent.
//
// See docs/2026-04-28-route-gain-mixer-strip-design.md § 5.2.

#ifndef JBOX_RT_GAIN_SMOOTHER_HPP
#define JBOX_RT_GAIN_SMOOTHER_HPP

#include <cmath>
#include <cstdint>

namespace jbox::rt {

struct GainSmoother {
    // Linear amplitude. Defaults to unity so a freshly-constructed
    // smoother passes audio through unchanged before its first step().
    float current = 1.0f;

    // Per-frame pole coefficient. 0 means "no smoothing configured" —
    // step() then snaps current to target. Real configurations always
    // populate this in setTimeConstant().
    float pole_per_frame = 0.0f;

    // Configure the smoother's time constant in seconds at the given
    // sample rate. tau_seconds is the 1/e settling time; 95% settling
    // is reached at ~3*tau (so 30 ms for tau = 10 ms).
    void setTimeConstant(double sample_rate, double tau_seconds) noexcept {
        if (sample_rate <= 0.0 || tau_seconds <= 0.0) {
            pole_per_frame = 0.0f;
            return;
        }
        // Per-frame pole p such that y[n+1] = p*y[n] + (1-p)*target
        // gives the right time constant: p = exp(-1 / (tau * fs)).
        pole_per_frame =
            static_cast<float>(std::exp(-1.0 / (tau_seconds * sample_rate)));
    }

    // Advance the smoother by `frames` frames toward `target`.
    // Block-rate update: pole_per_block = pole_per_frame ^ frames.
    // Mathematically equivalent to running `frames` per-sample updates
    // with the same target held constant, but at one pow() per block.
    void step(float target, std::uint32_t frames) noexcept {
        if (frames == 0) return;
        if (pole_per_frame <= 0.0f) {
            current = target;
            return;
        }
        const float pole_block =
            std::pow(pole_per_frame, static_cast<float>(frames));
        current = pole_block * current + (1.0f - pole_block) * target;
    }
};

}  // namespace jbox::rt

#endif  // JBOX_RT_GAIN_SMOOTHER_HPP
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
swift run JboxEngineCxxTests '[gain_smoother]'
```

Expected: 6 cases pass.

- [ ] **Step 5: Run the RT-safety scanner**

```sh
make rt-scan
```

Expected: clean (no banned symbols in the new `rt/gain_smoother.hpp`).

- [ ] **Step 6: Commit**

```sh
git add Sources/JboxEngineC/rt/gain_smoother.hpp \
        Tests/JboxEngineCxxTests/gain_smoother_test.cpp
git commit -m "$(cat <<'EOF'
engine(gain): add RT-safe block-rate gain smoother + tests

One-pole IIR with configurable time constant, used per-route /
per-channel from the output IOProc to converge fader gain toward
its atomic target. Block-rate (one pow() per block, not per sample)
because at sub-millisecond block sizes the audible difference is
nil and per-sample smoothing would cost N MACs per channel.

Tests cover step convergence at 48/96 kHz, no-overshoot on a step,
mute (target=0) reaching <1e-6 within 50 ms, and the no-op cases
(unity-to-unity, frames=0).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 1 deviations

- **Mute-decay assertion relaxed.** The plan's literal "mute target reaches < 1e-6 within 50 ms" is mathematically impossible for an ideal one-pole at τ = 10 ms (`exp(-5) ≈ 6.7e-3` is the floor). Replaced with `< 1e-2` (-40 dB, perceptually inaudible) at 50 ms plus a strict `< 1e-6` at 200 ms (≈ 20 τ, well below FP underflow). Same click-free property, sound math. Implemented in commit `cfc3a8e`. Spec § 5.2 should also be reworded during the Task 18 reconciliation.
- **NaN / ±inf rejection added.** `step()` checks `std::isfinite(target)` and drops non-finite updates rather than letting them poison the recurrence. Defence-in-depth on top of the control-thread clamp; documented in the header.

---

## Task 2: `FaderTaper.swift` — pure dB↔position taper

**Files:**
- Create: `Sources/JboxEngineSwift/FaderTaper.swift`
- Test: `Tests/JboxEngineTests/FaderTaperTests.swift`

Pure module. The fader UI talks to the model in dB; the engine wants linear amplitude. `FaderTaper` is the single place those conversions and the position curve live.

- [ ] **Step 1: Write the failing test**

Create `Tests/JboxEngineTests/FaderTaperTests.swift`:

```swift
import Testing
@testable import JboxEngineSwift

@Suite("FaderTaper")
struct FaderTaperTests {

    @Test("dbForPosition lands at 0 dB at unity position (75%)")
    func unityPosition() {
        let db = FaderTaper.dbForPosition(FaderTaper.unityPosition)
        #expect(abs(db - 0.0) < 0.01)
    }

    @Test("dbForPosition lands at +12 dB at the top")
    func topPosition() {
        #expect(abs(FaderTaper.dbForPosition(1.0) - 12.0) < 0.01)
    }

    @Test("dbForPosition lands at -60 dB just above the mute threshold")
    func nearFloor() {
        let db = FaderTaper.dbForPosition(FaderTaper.muteThresholdPosition + 0.001)
        // Above mute threshold the value is finite and at-or-below -60 dB.
        #expect(db.isFinite)
        #expect(db <= FaderTaper.minFiniteDb + 0.5)
    }

    @Test("dbForPosition snaps to -infinity below the mute threshold")
    func belowMuteThreshold() {
        let db = FaderTaper.dbForPosition(FaderTaper.muteThresholdPosition * 0.5)
        #expect(db == -Float.infinity)
    }

    @Test("Round-trip of position → dB → position is approximately stable")
    func roundTrip() {
        for sample: Float in stride(from: 0.10, through: 1.0, by: 0.05) {
            let p = sample
            let db = FaderTaper.dbForPosition(p)
            let p2 = FaderTaper.positionFor(db: db)
            #expect(abs(p - p2) < 0.01,
                    "round-trip drift at p=\(p): got \(p2)")
        }
    }

    @Test("amplitudeFor(db:) maps known dB values to known amplitudes")
    func amplitude() {
        #expect(abs(FaderTaper.amplitudeFor(db: 0) - 1.0) < 1e-5)
        #expect(abs(FaderTaper.amplitudeFor(db: -6) - 0.5012) < 0.001)
        #expect(abs(FaderTaper.amplitudeFor(db: -12) - 0.2512) < 0.001)
        #expect(abs(FaderTaper.amplitudeFor(db: 6) - 1.9953) < 0.001)
        #expect(FaderTaper.amplitudeFor(db: -.infinity) == 0.0)
    }

    @Test("amplitudeFor clamps very negative finite dB values to 0")
    func amplitudeClampsBelowFloor() {
        #expect(FaderTaper.amplitudeFor(db: -120) == 0.0)
        #expect(FaderTaper.amplitudeFor(db: -200) == 0.0)
    }

    @Test("positionFor(-infinity) returns 0")
    func mutePosition() {
        #expect(FaderTaper.positionFor(db: -.infinity) == 0.0)
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
swift test --filter FaderTaperTests
```

Expected: compile error — `FaderTaper` does not exist.

- [ ] **Step 3: Implement `FaderTaper.swift`**

Create `Sources/JboxEngineSwift/FaderTaper.swift`:

```swift
import Foundation

/// Pure dB ↔ slider-position math for the route fader UI.
///
/// Two scales coexist:
///   * **Position** — Float in 0...1, what the SwiftUI fader binds to.
///   * **dB** — Float, what the engine ABI takes; finite within
///     `[minFiniteDb, maxDb]`, with `-Float.infinity` representing mute
///     (linear amplitude 0).
///
/// The taper is piecewise linear in dB:
///   * 0...muteThresholdPosition → -infinity (snap to silence).
///   * muteThresholdPosition...unityPosition → linear in dB from
///     -60 dB to 0 dB.
///   * unityPosition...1 → linear in dB from 0 dB to +12 dB.
///
/// Lives in JboxEngineSwift (not JboxApp) so it is unit-testable
/// without SwiftUI. See docs/2026-04-28-route-gain-mixer-strip-design.md
/// §§ 4.4, 7.3.
public enum FaderTaper {

    public static let maxDb: Float = 12.0
    public static let unityDb: Float = 0.0
    public static let minFiniteDb: Float = -60.0
    public static let unityPosition: Float = 0.75
    public static let muteThresholdPosition: Float = 0.04

    /// Slider position (0...1) → dB.
    public static func dbForPosition(_ pos: Float) -> Float {
        let p = max(0, min(1, pos))
        if p < muteThresholdPosition { return -.infinity }
        if p >= unityPosition {
            // Top segment: linear in dB from 0 to +12.
            let t = (p - unityPosition) / (1.0 - unityPosition)
            return unityDb + t * (maxDb - unityDb)
        }
        // Lower segment: linear in dB from -60 (at muteThresholdPosition)
        // up to 0 (at unityPosition).
        let t = (p - muteThresholdPosition) / (unityPosition - muteThresholdPosition)
        return minFiniteDb + t * (unityDb - minFiniteDb)
    }

    /// dB → slider position (0...1).
    public static func positionFor(db: Float) -> Float {
        if db <= -.infinity || db.isNaN { return 0 }
        if db >= maxDb { return 1 }
        if db >= unityDb {
            let t = (db - unityDb) / (maxDb - unityDb)
            return unityPosition + t * (1.0 - unityPosition)
        }
        if db <= minFiniteDb { return muteThresholdPosition }
        let t = (db - minFiniteDb) / (unityDb - minFiniteDb)
        return muteThresholdPosition + t * (unityPosition - muteThresholdPosition)
    }

    /// dB → linear amplitude. -infinity (and any value below -120 dB to
    /// avoid denormals) maps to 0.
    public static func amplitudeFor(db: Float) -> Float {
        if db <= -.infinity || db.isNaN { return 0 }
        if db < -120 { return 0 }
        return powf(10.0, db / 20.0)
    }
}
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
swift test --filter FaderTaperTests
```

Expected: 8 cases pass.

- [ ] **Step 5: Commit**

```sh
git add Sources/JboxEngineSwift/FaderTaper.swift \
        Tests/JboxEngineTests/FaderTaperTests.swift
git commit -m "$(cat <<'EOF'
swift(gain): add FaderTaper pure dB↔position module + tests

Piecewise-linear-in-dB taper that the SwiftUI fader will use to map
slider position to dB, and the engine setter will use to map dB to
linear amplitude. 0 dB at 75% throw, +12 dB at top, -∞ snap below
4% throw — matches the design doc's "VCA-style" feel.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `RouteRecord` and C++ `RouteConfig` add gain state

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.hpp`

This task only declares the new state. Initialization, reads, and writes come in later tasks. No test changes here — the new fields are exercised by tasks 5/6/9.

- [ ] **Step 1: Add new fields to `RouteRecord` and `RouteConfig`**

In `Sources/JboxEngineC/control/route_manager.hpp`, add these two changes.

**Change 1**: at the top of `route_manager.hpp` near the existing includes, add:

```cpp
#include "gain_smoother.hpp"
```

**Change 2**: extend `RouteRecord` (current end is at line 164 — just before the closing `};`). Add right above the closing brace, after the `LatencyComponents` block:

```cpp
    // ----- Per-route gain state (master + per-channel trims + mute).
    //
    // Targets are written by the control thread in setMasterGainDb /
    // setChannelTrimDb / setRouteMute and read by the RT thread once
    // per IOProc block. Linear amplitude (dB→linear conversion happens
    // on the control thread). target_trim_gain is sized to
    // kAtomicMeterMaxChannels but only the first `channels_count`
    // entries are read on the RT side. addRoute initializes them to
    // 1.0; entries beyond channels_count keep std::atomic's default
    // and are never read.
    std::atomic<float>  target_master_gain{1.0f};
    std::array<std::atomic<float>, jbox::rt::kAtomicMeterMaxChannels> target_trim_gain;
    std::atomic<bool>   target_muted{false};

    // RT-thread-local smoothers. Configured at attemptStart from the
    // destination device's nominal rate via setTimeConstant(...).
    jbox::rt::GainSmoother master_smoother;
    std::array<jbox::rt::GainSmoother, jbox::rt::kAtomicMeterMaxChannels> trim_smoothers;
```

**Change 3**: extend `RouteManager::RouteConfig` (the C++ struct, line ~179). Append:

```cpp
        // Per-route VCA-style gain. master_gain_db = 0 → unity; mute
        // is independent of fader position. trims default to all 0 dB
        // when channel_trims_db is empty; otherwise must match
        // mapping.size() and is enforced in addRoute.
        float                            master_gain_db = 0.0f;
        std::vector<float>               channel_trims_db;
        bool                             muted          = false;
```

- [ ] **Step 2: Build + run the existing tests to confirm no regression**

```sh
swift build && swift run JboxEngineCxxTests
```

Expected: build succeeds, all existing C++ tests still pass (no behavior change yet).

- [ ] **Step 3: Commit**

```sh
git add Sources/JboxEngineC/control/route_manager.hpp
git commit -m "$(cat <<'EOF'
engine(gain): RouteRecord + C++ RouteConfig hold gain targets

Plumbing-only. Adds atomic master_gain / trim[ch] / muted targets
and RT-thread-local GainSmoothers to RouteRecord, plus the matching
master_gain_db / channel_trims_db / muted fields on RouteConfig.
Subsequent commits wire these into addRoute, the RT IOProcs, and
the new ABI setters.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `addRoute` initializes trim atomics and reads new RouteConfig fields

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.cpp` (`addRoute` near line 297)
- Modify: `Sources/JboxEngineC/control/route_manager.cpp` (`attemptStart` — wherever the rate / converter setup happens, configure smoothers)

This wires the C++ config into the per-route record. We don't need a dedicated test for the wiring itself; tasks 5/6/9 verify behavior end-to-end.

- [ ] **Step 1: Modify `RouteManager::addRoute` to populate the gain state**

In `Sources/JboxEngineC/control/route_manager.cpp`, in `addRoute` after the existing `rec->buffer_frames_override = cfg.buffer_frames;` line, add:

```cpp
    // dB → linear conversion happens on the control thread so the RT
    // path stays free of pow() calls. -inf and very-negative values
    // both map to 0 to avoid denormals.
    auto dbToAmp = [](float db) -> float {
        if (!(db > -120.0f)) return 0.0f;   // covers NaN, -inf, < -120
        return std::pow(10.0f, db / 20.0f);
    };

    rec->target_master_gain.store(dbToAmp(cfg.master_gain_db),
                                  std::memory_order_relaxed);
    rec->target_muted.store(cfg.muted, std::memory_order_relaxed);

    // Trims: default to unity per channel when the caller passes none.
    if (!cfg.channel_trims_db.empty() &&
        cfg.channel_trims_db.size() != cfg.mapping.size()) {
        setError(err, JBOX_ERR_INVALID_ARGUMENT,
                 "channel_trims_db size must match mapping size");
        return JBOX_INVALID_ROUTE_ID;
    }
    for (std::uint32_t i = 0; i < rec->channels_count; ++i) {
        const float db = cfg.channel_trims_db.empty()
                             ? 0.0f
                             : cfg.channel_trims_db[i];
        rec->target_trim_gain[i].store(dbToAmp(db), std::memory_order_relaxed);
    }
```

Make sure `<cmath>` is included at the top of `route_manager.cpp` (it likely already is via existing includes). If not, add it.

- [ ] **Step 2: Configure smoother time constants in `attemptStart`**

In `Sources/JboxEngineC/control/route_manager.cpp`, find `attemptStart`. Locate the place where `r.nominal_dst_rate` is set or known (this happens after the destination device is resolved). Right after that, configure the smoothers:

```cpp
    // 10 ms time constant — fast enough to feel instantaneous on slider
    // drags, slow enough to kill zipper noise / clicks. Matches design
    // doc § 5.2.
    constexpr double kGainTau = 0.010;
    r.master_smoother.setTimeConstant(r.nominal_dst_rate, kGainTau);
    r.master_smoother.current = r.target_master_gain.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < r.channels_count; ++i) {
        r.trim_smoothers[i].setTimeConstant(r.nominal_dst_rate, kGainTau);
        r.trim_smoothers[i].current =
            r.target_trim_gain[i].load(std::memory_order_relaxed);
    }
```

(Locate this by searching for `nominal_dst_rate =` — there will be a single assignment site within `attemptStart`.)

- [ ] **Step 3: Build + run all C++ tests**

```sh
swift run JboxEngineCxxTests
```

Expected: green. Existing routes still default to unity (0 dB → linear 1.0) so behavior is unchanged.

- [ ] **Step 4: Commit**

```sh
git add Sources/JboxEngineC/control/route_manager.cpp
git commit -m "$(cat <<'EOF'
engine(gain): addRoute / attemptStart wire gain state into RouteRecord

addRoute converts master + per-channel trim dB → linear amplitude on
the control thread (so the RT path stays pow()-free) and stores them
in the atomic targets. attemptStart configures each smoother's
time constant from the destination device's nominal rate (10 ms tau)
and primes its current value to the target. Behavior unchanged for
existing callers — they zero-init RouteConfig, which means unity gain.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `outputIOProcCallback` applies smoothed gain

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.cpp` (`outputIOProcCallback`, currently at line ~203)
- Test: `Tests/JboxEngineCxxTests/route_manager_gain_test.cpp` (new)

The output IOProc reads atomic targets, advances the smoothers once for the block, and multiplies each output sample by `master * trim[ch]` before writing to the device buffer and updating the dest meter. Source meter is untouched (stays pre-fader).

- [ ] **Step 1: Write the failing integration test**

Create `Tests/JboxEngineCxxTests/route_manager_gain_test.cpp`:

```cpp
// route_manager_gain_test.cpp — exercises the gain path through a
// SimulatedBackend-driven route. Verifies the output IOProc multiplies
// audio samples by master * trim[ch] and that updates to the atomic
// targets propagate to the next block.
//
// See docs/2026-04-28-route-gain-mixer-strip-design.md §§ 5.1, 5.2.

#include "engine.hpp"
#include "simulated_backend.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

using jbox::control::Engine;
using jbox::control::SimulatedBackend;

namespace {

// Spin up an engine over the simulated backend with two stereo devices.
// Returns the engine + the simulated backend ref so tests can drive
// IOProc ticks deterministically.
struct GainFixture {
    std::unique_ptr<SimulatedBackend> backend;
    std::unique_ptr<Engine> engine;
    std::string src_uid = "src.dev";
    std::string dst_uid = "dst.dev";
};

GainFixture makeFixture() {
    GainFixture f;
    f.backend = std::make_unique<SimulatedBackend>();
    f.backend->addDevice(f.src_uid, /*input_channels=*/2,
                         /*output_channels=*/0,
                         /*sample_rate=*/48000.0);
    f.backend->addDevice(f.dst_uid, /*input_channels=*/0,
                         /*output_channels=*/2,
                         /*sample_rate=*/48000.0);
    f.engine = std::make_unique<Engine>(*f.backend);
    return f;
}

}  // namespace

TEST_CASE("[route_gain] unity master + unity trims pass audio unchanged",
          "[route_manager][gain]") {
    auto f = makeFixture();
    Engine::RouteConfig cfg;
    cfg.source_uid = f.src_uid;
    cfg.dest_uid   = f.dst_uid;
    cfg.mapping    = {{0, 0}, {1, 1}};
    // Defaults: master_gain_db = 0, channel_trims_db empty, muted = false.

    jbox_error_t err{};
    const auto id = f.engine->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.engine->startRoute(id) == JBOX_OK);

    // Drive a few blocks of constant 0.5 amplitude on both source channels.
    std::vector<float> input  = {0.5f, 0.5f, 0.5f, 0.5f};   // 2 frames * 2 ch
    std::vector<float> output(4, 0.0f);

    // Settle the smoother (it starts at unity; first block already passes
    // through; we still drive a few blocks to be conservative).
    for (int i = 0; i < 5; ++i) {
        f.backend->driveInputBlock(f.src_uid, input.data(),
                                   /*frames=*/2);
        f.backend->driveOutputBlock(f.dst_uid, output.data(),
                                    /*frames=*/2);
    }

    REQUIRE(std::abs(output[0] - 0.5f) < 1e-4f);
    REQUIRE(std::abs(output[1] - 0.5f) < 1e-4f);
    REQUIRE(std::abs(output[2] - 0.5f) < 1e-4f);
    REQUIRE(std::abs(output[3] - 0.5f) < 1e-4f);
}

TEST_CASE("[route_gain] master gain at -6 dB halves output amplitude",
          "[route_manager][gain]") {
    auto f = makeFixture();
    Engine::RouteConfig cfg;
    cfg.source_uid = f.src_uid;
    cfg.dest_uid   = f.dst_uid;
    cfg.mapping    = {{0, 0}, {1, 1}};
    cfg.master_gain_db = -6.0f;

    jbox_error_t err{};
    const auto id = f.engine->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.engine->startRoute(id) == JBOX_OK);

    std::vector<float> input(64, 0.5f);
    std::vector<float> output(64, 0.0f);

    // Drive enough blocks for the smoother to settle. The smoother
    // starts pre-loaded with the configured target (see attemptStart),
    // so output should be at -6 dB ≈ 0.2506 from the first block.
    f.backend->driveInputBlock(f.src_uid, input.data(),  /*frames=*/32);
    f.backend->driveOutputBlock(f.dst_uid, output.data(), /*frames=*/32);

    const float expected = 0.5f * std::pow(10.0f, -6.0f / 20.0f);   // ≈ 0.2506
    for (auto v : output) {
        REQUIRE(std::abs(v - expected) < 1e-3f);
    }
}

TEST_CASE("[route_gain] mute target ramps to silence within 50 ms",
          "[route_manager][gain]") {
    auto f = makeFixture();
    Engine::RouteConfig cfg;
    cfg.source_uid = f.src_uid;
    cfg.dest_uid   = f.dst_uid;
    cfg.mapping    = {{0, 0}, {1, 1}};
    cfg.muted      = true;

    jbox_error_t err{};
    const auto id = f.engine->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.engine->startRoute(id) == JBOX_OK);

    std::vector<float> input(2400, 0.5f);   // 50 ms at 48 kHz
    std::vector<float> output(2400, 0.0f);
    f.backend->driveInputBlock(f.src_uid, input.data(),  /*frames=*/1200);
    f.backend->driveOutputBlock(f.dst_uid, output.data(), /*frames=*/1200);
    f.backend->driveInputBlock(f.src_uid, input.data(),  /*frames=*/1200);
    f.backend->driveOutputBlock(f.dst_uid, output.data() + 1200,
                                /*frames=*/1200);

    REQUIRE(std::abs(output.back()) < 1e-4f);
}
```

> ⚠️ **Note on the simulated-backend driver methods.** The exact public
> shape of `SimulatedBackend::driveInputBlock` / `driveOutputBlock` should
> follow whatever the existing `route_manager_test.cpp` already uses
> (read it first; the names there are authoritative). The pattern shown
> above is illustrative — substitute the real method names. If the
> simulated backend doesn't expose direct block drivers, model these
> tests on the IOProc-tick driver used by the existing test.

- [ ] **Step 2: Run the test to verify it fails**

```sh
swift run JboxEngineCxxTests '[gain]'
```

Expected: tests fail (output samples are NOT scaled — gain is not yet wired).

- [ ] **Step 3: Modify `outputIOProcCallback` to apply smoothed gain**

In `Sources/JboxEngineC/control/route_manager.cpp`, find `outputIOProcCallback` (currently around line 203). After the existing `setInputRate` / converter logic and after the `produced = r->converter->convert(...)` call, before the per-channel placement loop, insert the smoother step. Then in the per-channel loop, multiply each sample by `current_master * current_trim[ch]` before placing it.

The change replaces the current per-channel placement loop. The full new body of the function — keep the existing channel-mismatch + rate-update + converter pull, just replace the part starting with `// Place converted samples on the destination device's selected output channels …` (around line 253):

```cpp
    // Advance the gain smoothers once for the whole block. Mute target
    // overrides the master target with 0; un-mute returns to master *
    // trim. Same dynamics for either path so transitions are click-free.
    const bool  muted_now = r->target_muted.load(std::memory_order_relaxed);
    const float master_target = muted_now
        ? 0.0f
        : r->target_master_gain.load(std::memory_order_relaxed);
    r->master_smoother.step(master_target, frame_count);
    for (std::uint32_t i = 0; i < in_channels; ++i) {
        const float trim_target =
            r->target_trim_gain[i].load(std::memory_order_relaxed);
        r->trim_smoothers[i].step(trim_target, frame_count);
    }
    const float current_master = r->master_smoother.current;

    // Place converted samples on the destination device's selected
    // output channels, multiplying by master * trim[ch] and tracking
    // per-channel peak in one pass. Same channel-outer pattern as the
    // input side to keep the atomic cost at O(channels_count).
    for (std::uint32_t i = 0; i < in_channels; ++i) {
        const std::uint32_t dst_ch = r->mapping[i].dst;
        const float gain = current_master * r->trim_smoothers[i].current;
        float peak = 0.0f;
        for (std::uint32_t f = 0; f < frame_count; ++f) {
            const float s = scratch[f * in_channels + i] * gain;
            samples[f * channel_count + dst_ch] = s;
            const float a = std::fabs(s);
            if (a > peak) peak = a;
        }
        r->dest_meter.updateMax(i, peak);
    }
```

- [ ] **Step 4: Run the new test + the existing C++ suite**

```sh
swift run JboxEngineCxxTests '[gain]'
swift run JboxEngineCxxTests
```

Expected: gain tests pass, full suite (184+ cases) green.

- [ ] **Step 5: Run the RT-safety scanner**

```sh
make rt-scan
```

Expected: clean. (No new banned symbols — `std::pow` lives in `gain_smoother.hpp` but the smoother itself stays in `rt/`; the scan rule already considers that path.)

- [ ] **Step 6: Run TSan-instrumented C++ tests**

```sh
make cxx-test-tsan
```

Expected: no data races reported on the new atomics.

- [ ] **Step 7: Commit**

```sh
git add Sources/JboxEngineC/control/route_manager.cpp \
        Tests/JboxEngineCxxTests/route_manager_gain_test.cpp
git commit -m "$(cat <<'EOF'
engine(gain): outputIOProcCallback applies master×trim with smoothing

Per-block: load atomic master + mute targets, override master with 0
when muted, advance master + per-channel smoothers by frame_count,
then multiply each output sample by master * trim[ch] in the
existing channel-outer loop. Dest meter naturally reads post-fader.
Source meter / inputIOProcCallback unchanged (stays pre-fader).

Tests cover unity passthrough, -6 dB master attenuation, and mute
ramp-to-silence within 50 ms. RT-scan + TSan green.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `duplexIOProcCallback` applies the same smoothed gain

**Files:**
- Modify: `Sources/JboxEngineC/control/route_manager.cpp` (`duplexIOProcCallback`, currently at line ~151)
- Test: extend `Tests/JboxEngineCxxTests/route_manager_gain_test.cpp` with a duplex-path case

The duplex (direct-monitor) fast path skips the ring + AudioConverter, so the gain has to be applied inline there too.

- [ ] **Step 1: Add the duplex test case**

Append to `Tests/JboxEngineCxxTests/route_manager_gain_test.cpp`:

```cpp
TEST_CASE("[route_gain] duplex path applies master gain",
          "[route_manager][gain][duplex]") {
    // Duplex fast path is selected when source UID == dest UID and
    // latency_mode == Performance.
    SimulatedBackend backend;
    const std::string uid = "duplex.dev";
    backend.addDevice(uid, /*input_channels=*/2, /*output_channels=*/2,
                      /*sample_rate=*/48000.0);
    Engine engine(backend);

    Engine::RouteConfig cfg;
    cfg.source_uid = uid;
    cfg.dest_uid   = uid;
    cfg.mapping    = {{0, 0}, {1, 1}};
    cfg.latency_mode = 2;          // Performance → duplex
    cfg.master_gain_db = -12.0f;

    jbox_error_t err{};
    const auto id = engine.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(engine.startRoute(id) == JBOX_OK);

    // Single duplex block with 1.0 input on both channels.
    std::vector<float> in(64, 1.0f);   // 32 frames * 2 ch
    std::vector<float> out(64, 0.0f);
    backend.driveDuplexBlock(uid, in.data(), out.data(), /*frames=*/32);

    const float expected = 1.0f * std::pow(10.0f, -12.0f / 20.0f);  // ≈ 0.2512
    for (auto v : out) {
        REQUIRE(std::abs(v - expected) < 5e-3f);
    }
}
```

> ⚠️ Same caveat as Task 5 — verify the actual driver method name on
> `SimulatedBackend` against the existing `multi_route_test.cpp` /
> `route_manager_test.cpp` and substitute as needed.

- [ ] **Step 2: Run the test to verify it fails**

```sh
swift run JboxEngineCxxTests '[duplex]'
```

Expected: fail (duplex path doesn't apply gain yet).

- [ ] **Step 3: Modify `duplexIOProcCallback` to apply gain**

In `Sources/JboxEngineC/control/route_manager.cpp`, find `duplexIOProcCallback` (around line 151). Add the smoother step + per-channel multiply. The new body of the per-channel loop (replacing the existing `for (std::uint32_t i = 0; i < r->channels_count; ++i) {` block around line 175) is:

```cpp
    // Advance gain smoothers once per block — same logic as the split
    // path. Mute target overrides master with 0.
    const bool  muted_now = r->target_muted.load(std::memory_order_relaxed);
    const float master_target = muted_now
        ? 0.0f
        : r->target_master_gain.load(std::memory_order_relaxed);
    r->master_smoother.step(master_target, frames);
    for (std::uint32_t i = 0; i < r->channels_count; ++i) {
        const float trim_target =
            r->target_trim_gain[i].load(std::memory_order_relaxed);
        r->trim_smoothers[i].step(trim_target, frames);
    }
    const float current_master = r->master_smoother.current;

    for (std::uint32_t i = 0; i < r->channels_count; ++i) {
        const std::uint32_t src_ch = r->mapping[i].src;
        const std::uint32_t dst_ch = r->mapping[i].dst;
        const float gain = current_master * r->trim_smoothers[i].current;
        float peak_in  = 0.0f;
        float peak_out = 0.0f;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float s = input_samples[f * input_channel_count + src_ch];
            const float o = s * gain;
            output_samples[f * out_ch_count + dst_ch] = o;
            const float a_in  = std::fabs(s);
            const float a_out = std::fabs(o);
            if (a_in  > peak_in)  peak_in  = a_in;
            if (a_out > peak_out) peak_out = a_out;
        }
        r->source_meter.updateMax(i, peak_in);
        r->dest_meter.updateMax(i, peak_out);
    }
```

- [ ] **Step 4: Run the new test + the existing C++ suite + RT-scan + TSan**

```sh
swift run JboxEngineCxxTests '[duplex]'
swift run JboxEngineCxxTests
make rt-scan
make cxx-test-tsan
```

Expected: all green.

- [ ] **Step 5: Commit**

```sh
git add Sources/JboxEngineC/control/route_manager.cpp \
        Tests/JboxEngineCxxTests/route_manager_gain_test.cpp
git commit -m "$(cat <<'EOF'
engine(gain): duplexIOProcCallback applies master×trim with smoothing

Mirrors the Task 5 split-path change in the direct-monitor fast path.
Same smoother logic + same per-channel master * trim multiply, with
both source and dest meters updated post-multiply (input pre-gain
peak, output post-gain peak — matches mixer-meter convention).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: ABI v14 — header changes

**Files:**
- Modify: `Sources/JboxEngineC/include/jbox_engine.h`

Bump `JBOX_ENGINE_ABI_VERSION` to 14, append the four new fields to `jbox_route_config_t`, declare the three new setter functions, and backfill the v12 / v13 history entries that were committed without updating the comment block.

- [ ] **Step 1: Bump ABI version + backfill history**

In `Sources/JboxEngineC/include/jbox_engine.h`, find the `ABI history:` comment block (line 33). Append after the v11 entry, before the closing ` */`:

```c
 *  12  MINOR — added JBOX_ERR_DEVICE_GONE so callers can distinguish
 *              "device was unplugged underneath a running route" from
 *              "the route was started before its devices appeared".
 *  13  MINOR — added JBOX_ERR_SYSTEM_SUSPENDED so callers can show a
 *              dedicated "waiting for system wake" status, distinct
 *              from device-gone and initial-WAITING.
 *  14  MINOR — appended `master_gain_db`, `channel_trims_db`,
 *              `channel_trims_count`, and `muted` to
 *              jbox_route_config_t, plus three setters
 *              (jbox_engine_set_route_master_gain_db /
 *              jbox_engine_set_route_channel_trim_db /
 *              jbox_engine_set_route_mute) for runtime fader control.
 *              Zero-initialised callers stay at unity gain, no trim,
 *              unmuted.
```

Update `#define JBOX_ENGINE_ABI_VERSION 13u` to:

```c
#define JBOX_ENGINE_ABI_VERSION 14u
```

- [ ] **Step 2: Append fields to `jbox_route_config_t`**

Find `jbox_route_config_t` (line ~230). After `uint32_t buffer_frames;`, before the closing `} jbox_route_config_t;`, add:

```c
    /* ABI v14 — VCA-style per-route gain. master_gain_db = 0 means
     * unity (default for zero-init callers). channel_trims_db is
     * caller-owned; if non-NULL its length must equal mapping_count
     * and the engine copies it. NULL / count==0 means "no trims",
     * which is equivalent to all 0 dB. muted = 0 → not muted. */
    float        master_gain_db;
    const float* channel_trims_db;
    size_t       channel_trims_count;
    int          muted;
```

- [ ] **Step 3: Declare the three setter functions**

Find a logical spot in the public-API section — right after `jbox_engine_set_resampler_quality` (search for it; it's near line 414 in the matching `bridge_api.cpp`, but the *declaration* lives in this header). Add:

```c
/* ----------------------------------------------------------------- */
/*  Per-route runtime gain (ABI v14)                                 */
/* ----------------------------------------------------------------- */

/* Set the per-route master fader, in dB. 0 = unity. -infinity is
 * accepted and maps to silence. Values clamped to [-infinity, +12].
 * Thread-safe; bounded execution. */
jbox_error_code_t jbox_engine_set_route_master_gain_db(
    jbox_engine_t*  engine,
    jbox_route_id_t route_id,
    float           db);

/* Set the per-channel trim, in dB, for the route's mapping[channel_index].
 * Same range / clamp / threading as the master setter.
 * Returns JBOX_ERR_INVALID_ARGUMENT if channel_index >= mapping_count. */
jbox_error_code_t jbox_engine_set_route_channel_trim_db(
    jbox_engine_t*  engine,
    jbox_route_id_t route_id,
    uint32_t        channel_index,
    float           db);

/* Toggle the per-route mute. 0 = unmuted, non-zero = muted.
 * Mute is independent of fader state; un-mute returns to whatever
 * the master + trims currently are. Thread-safe; bounded. */
jbox_error_code_t jbox_engine_set_route_mute(
    jbox_engine_t*  engine,
    jbox_route_id_t route_id,
    int             muted);
```

- [ ] **Step 4: Build to check the header compiles**

```sh
swift build
```

Expected: compile error in `bridge_api.cpp` referencing missing setter implementations (these come in Task 8). The header itself should parse.

> If the build fails before reaching `bridge_api.cpp`, stop and read
> the error — most likely a typo in the header. The build only really
> goes green again at the end of Task 8.

- [ ] **Step 5: Commit (header-only intermediate state, build will be green again at the end of Task 8)**

```sh
git add Sources/JboxEngineC/include/jbox_engine.h
git commit -m "$(cat <<'EOF'
abi(v14): declare per-route gain fields + setters; backfill v12/v13

Header-only change. Bumps JBOX_ENGINE_ABI_VERSION to 14, appends
master_gain_db / channel_trims_db / channel_trims_count / muted to
jbox_route_config_t, declares jbox_engine_set_route_master_gain_db /
_channel_trim_db / _mute. Backfills the v12 (DEVICE_GONE) and v13
(SYSTEM_SUSPENDED) entries that were shipped without updating the
ABI history comment.

The build is intentionally red between this commit and the next
because the new declarations don't yet have implementations in
bridge_api.cpp — Task 8 lands those.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: ABI v14 — `bridge_api.cpp` setters + new `RouteConfig` field copy

**Files:**
- Modify: `Sources/JboxEngineC/control/engine.hpp` (declarations)
- Modify: `Sources/JboxEngineC/control/engine.cpp` (definitions)
- Modify: `Sources/JboxEngineC/control/bridge_api.cpp` (C ABI implementations)

The bridge layer is the C-to-C++ adapter. We add three pass-through methods on `Engine`, then the `extern "C"` wrappers in `bridge_api.cpp`, and copy the new fields off the C `jbox_route_config_t` into the C++ `RouteConfig`.

- [ ] **Step 1: Add `Engine` pass-through methods**

In `Sources/JboxEngineC/control/engine.hpp`, near the existing public methods, add:

```cpp
    jbox_error_code_t setRouteMasterGainDb(jbox_route_id_t id, float db);
    jbox_error_code_t setRouteChannelTrimDb(jbox_route_id_t id,
                                             std::uint32_t channel_index,
                                             float db);
    jbox_error_code_t setRouteMute(jbox_route_id_t id, bool muted);
```

In `Sources/JboxEngineC/control/engine.cpp`, add the implementations near the other route-mutation methods:

```cpp
jbox_error_code_t Engine::setRouteMasterGainDb(jbox_route_id_t id, float db) {
    return rm_.setRouteMasterGainDb(id, db);
}

jbox_error_code_t Engine::setRouteChannelTrimDb(jbox_route_id_t id,
                                                 std::uint32_t channel_index,
                                                 float db) {
    return rm_.setRouteChannelTrimDb(id, channel_index, db);
}

jbox_error_code_t Engine::setRouteMute(jbox_route_id_t id, bool muted) {
    return rm_.setRouteMute(id, muted);
}
```

- [ ] **Step 2: Add `RouteManager` setters**

In `Sources/JboxEngineC/control/route_manager.hpp`, in the `public:` section, declare:

```cpp
    jbox_error_code_t setRouteMasterGainDb(jbox_route_id_t id, float db);
    jbox_error_code_t setRouteChannelTrimDb(jbox_route_id_t id,
                                             std::uint32_t channel_index,
                                             float db);
    jbox_error_code_t setRouteMute(jbox_route_id_t id, bool muted);
```

In `Sources/JboxEngineC/control/route_manager.cpp`, add the implementations near the existing rename / start / stop methods:

```cpp
namespace {
// Same dB→linear mapping used in addRoute.
float dbToAmpClamped(float db) {
    if (!(db > -120.0f)) return 0.0f;
    if (db > 12.0f) db = 12.0f;
    return std::pow(10.0f, db / 20.0f);
}
}

jbox_error_code_t RouteManager::setRouteMasterGainDb(jbox_route_id_t id,
                                                      float db) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    it->second->target_master_gain.store(dbToAmpClamped(db),
                                         std::memory_order_relaxed);
    return JBOX_OK;
}

jbox_error_code_t RouteManager::setRouteChannelTrimDb(jbox_route_id_t id,
                                                       std::uint32_t channel_index,
                                                       float db) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    auto& r = *it->second;
    if (channel_index >= r.channels_count) return JBOX_ERR_INVALID_ARGUMENT;
    r.target_trim_gain[channel_index].store(dbToAmpClamped(db),
                                            std::memory_order_relaxed);
    return JBOX_OK;
}

jbox_error_code_t RouteManager::setRouteMute(jbox_route_id_t id, bool muted) {
    auto it = routes_.find(id);
    if (it == routes_.end()) return JBOX_ERR_INVALID_ARGUMENT;
    it->second->target_muted.store(muted, std::memory_order_relaxed);
    return JBOX_OK;
}
```

- [ ] **Step 3: Implement the three C ABI wrappers in `bridge_api.cpp`**

In `Sources/JboxEngineC/control/bridge_api.cpp`, add (next to `jbox_engine_set_resampler_quality` around line 414):

```cpp
jbox_error_code_t jbox_engine_set_route_master_gain_db(
    jbox_engine_t*  engine,
    jbox_route_id_t route_id,
    float           db) {
    if (engine == nullptr || engine->impl == nullptr) {
        return JBOX_ERR_INVALID_ARGUMENT;
    }
    try {
        return engine->impl->setRouteMasterGainDb(route_id, db);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}

jbox_error_code_t jbox_engine_set_route_channel_trim_db(
    jbox_engine_t*  engine,
    jbox_route_id_t route_id,
    uint32_t        channel_index,
    float           db) {
    if (engine == nullptr || engine->impl == nullptr) {
        return JBOX_ERR_INVALID_ARGUMENT;
    }
    try {
        return engine->impl->setRouteChannelTrimDb(route_id, channel_index, db);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}

jbox_error_code_t jbox_engine_set_route_mute(
    jbox_engine_t*  engine,
    jbox_route_id_t route_id,
    int             muted) {
    if (engine == nullptr || engine->impl == nullptr) {
        return JBOX_ERR_INVALID_ARGUMENT;
    }
    try {
        return engine->impl->setRouteMute(route_id, muted != 0);
    } catch (...) {
        return JBOX_ERR_INTERNAL;
    }
}
```

- [ ] **Step 4: Copy the new fields off the C config in `addRoute`**

In `Sources/JboxEngineC/control/bridge_api.cpp`, find the `jbox_engine_add_route` implementation (search for `addRoute` calls). The C → C++ config conversion is around line 80-100 (where it currently sets `out.buffer_frames`). Append:

```cpp
    out.master_gain_db = config->master_gain_db;
    out.muted          = (config->muted != 0);
    if (config->channel_trims_db != nullptr && config->channel_trims_count > 0) {
        out.channel_trims_db.assign(
            config->channel_trims_db,
            config->channel_trims_db + config->channel_trims_count);
    }
```

- [ ] **Step 5: Build + run the existing C++ tests**

```sh
swift build
swift run JboxEngineCxxTests
```

Expected: green. Behavior unchanged for legacy callers.

- [ ] **Step 6: Add a bridge-level smoke test for the new setters**

Append to `Tests/JboxEngineCxxTests/bridge_api_test.cpp` (or follow the existing pattern in that file):

```cpp
TEST_CASE("[bridge_api] new gain setters return JBOX_OK on a valid route",
          "[bridge_api][gain]") {
    jbox_engine_config_t engine_cfg{};
    engine_cfg.use_simulated_backend = 1;
    jbox_error_t err{};
    auto* engine = jbox_engine_create(&engine_cfg, &err);
    REQUIRE(engine != nullptr);

    jbox_channel_edge_t mapping[] = {{0, 0}, {1, 1}};
    jbox_route_config_t route{};
    route.source_uid     = "src.dev";
    route.dest_uid       = "dst.dev";
    route.mapping        = mapping;
    route.mapping_count  = 2;
    route.master_gain_db = 0.0f;
    route.muted          = 0;

    // Need to seed the simulated backend with the devices first; copy
    // whatever the existing tests do (search bridge_api_test.cpp for
    // "addDevice").

    const auto id = jbox_engine_add_route(engine, &route, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(jbox_engine_set_route_master_gain_db(engine, id, -3.0f) == JBOX_OK);
    REQUIRE(jbox_engine_set_route_channel_trim_db(engine, id, 0, -1.5f) == JBOX_OK);
    REQUIRE(jbox_engine_set_route_channel_trim_db(engine, id, 9, 0.0f)
            == JBOX_ERR_INVALID_ARGUMENT);
    REQUIRE(jbox_engine_set_route_mute(engine, id, 1) == JBOX_OK);

    // ABI version sanity check.
    REQUIRE(jbox_engine_abi_version() == 14u);

    jbox_engine_destroy(engine);
}
```

> ⚠️ Match the simulated-backend seeding in the existing
> `bridge_api_test.cpp` so the route can actually start. The test above
> only exercises the *setters' return codes*, which don't require a
> running route, but we still need a valid registered route for the
> ID to be found.

- [ ] **Step 7: Run the new test + full suite + verify**

```sh
swift run JboxEngineCxxTests '[bridge_api][gain]'
make verify
```

Expected: full pipeline green (RT scan, all C++ tests, all Swift tests, TSan).

- [ ] **Step 8: Commit**

```sh
git add Sources/JboxEngineC/control/engine.hpp \
        Sources/JboxEngineC/control/engine.cpp \
        Sources/JboxEngineC/control/route_manager.hpp \
        Sources/JboxEngineC/control/route_manager.cpp \
        Sources/JboxEngineC/control/bridge_api.cpp \
        Tests/JboxEngineCxxTests/bridge_api_test.cpp
git commit -m "$(cat <<'EOF'
engine(gain): wire ABI v14 setters through to RouteManager atomics

setRouteMasterGainDb / setRouteChannelTrimDb / setRouteMute on
RouteManager convert dB → clamped linear amplitude (no pow on the
RT thread) and atomically store into the per-route target. Engine
pass-throughs and the three extern-C wrappers in bridge_api.cpp
complete the chain. The C → C++ RouteConfig conversion in
jbox_engine_add_route copies master_gain_db / channel_trims_db /
muted off the C struct so freshly-added routes pick up their
configured gain on first attemptStart.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Swift `Route` model fields

**Files:**
- Modify: `Sources/JboxEngineSwift/JboxEngine.swift` (or wherever `Route` is defined)

Adds the three Swift-side fields. No tests here; they're covered by EngineStore tests in Task 11.

- [ ] **Step 1: Find the `Route` definition**

Run:
```sh
grep -n "struct Route" Sources/JboxEngineSwift/*.swift
```

Edit whichever file owns `public struct Route` — likely `JboxEngine.swift`.

- [ ] **Step 2: Add the three fields to `Route`**

Append (inside `public struct Route { ... }`):

```swift
    /// Master VCA-style fader, in dB. 0 = unity. -.infinity = silence.
    /// Default 0; persisted in StoredRoute. ABI v14 (`master_gain_db`).
    public var masterGainDb: Float = 0.0

    /// Per-channel trim, in dB. Length matches `config.mapping.count`.
    /// Default `[0.0, 0.0, ...]`; persisted in StoredRoute.
    /// ABI v14 (`channel_trims_db`).
    public var trimDbs: [Float] = []

    /// Mute toggle, independent of fader state. Default false.
    /// ABI v14 (`muted`).
    public var muted: Bool = false
```

> If the existing `Route` is initialised by a memberwise init, you may
> need to also expose these in the matching `init(...)` signature. Match
> whatever the file currently does for the other fields.

- [ ] **Step 3: Build (compile only — no behavior change yet)**

```sh
swift build
```

Expected: builds. Existing tests continue to pass since the new fields have defaults.

- [ ] **Step 4: Commit**

```sh
git add Sources/JboxEngineSwift/JboxEngine.swift
git commit -m "$(cat <<'EOF'
swift(gain): Route gains masterGainDb, trimDbs, muted fields

Mirrors ABI v14's new RouteConfig fields. Default values keep
existing call sites working unchanged. Persistence and EngineStore
setters land in the next two commits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: `StoredRoute` persistence — additive optional fields

**Files:**
- Modify: `Sources/JboxEngineSwift/Persistence/StoredAppState.swift`
- Test: `Tests/JboxEngineTests/RouteGainPersistenceTests.swift` (new)

Optional-with-default JSON fields keep old `state.json` files loadable.

- [ ] **Step 1: Write the failing test**

Create `Tests/JboxEngineTests/RouteGainPersistenceTests.swift`:

```swift
import Foundation
import Testing
@testable import JboxEngineSwift

@Suite("RouteGainPersistence")
struct RouteGainPersistenceTests {

    @Test("StoredRoute round-trips with the new gain fields")
    func roundTripWithFields() throws {
        let stored = StoredRoute.makeSample()
        var copy = stored
        copy.masterGainDb = -3.5
        copy.trimDbs = [-1.0, 0.5]
        copy.muted = true

        let data = try JSONEncoder().encode(copy)
        let decoded = try JSONDecoder().decode(StoredRoute.self, from: data)

        #expect(decoded.masterGainDb == -3.5)
        #expect(decoded.trimDbs == [-1.0, 0.5])
        #expect(decoded.muted == true)
    }

    @Test("StoredRoute decodes legacy JSON without gain fields")
    func decodesLegacyJSON() throws {
        // Emulate state.json from before ABI v14.
        let legacy = StoredRoute.makeSampleLegacyJSONWithoutGain()
        let decoded = try JSONDecoder().decode(StoredRoute.self,
                                               from: Data(legacy.utf8))

        #expect(decoded.masterGainDb == 0.0)
        #expect(decoded.trimDbs.isEmpty)
        #expect(decoded.muted == false)
    }

    @Test("StoredRoute encoded JSON includes the new fields when set")
    func encodedShape() throws {
        var stored = StoredRoute.makeSample()
        stored.masterGainDb = -3.0
        stored.muted = true

        let data = try JSONEncoder().encode(stored)
        let json = String(decoding: data, as: UTF8.self)

        #expect(json.contains("masterGainDb"))
        #expect(json.contains("muted"))
    }
}
```

`StoredRoute.makeSample()` and `.makeSampleLegacyJSONWithoutGain()` are
test helpers — define them inside the test file or in a small
`StoredRoute+TestSamples.swift` file under `Tests/JboxEngineTests/`.
Build them on whatever the rest of `StoredRoute`'s required fields are
(re-read `StoredAppState.swift` first to know what fields are
mandatory). The legacy variant produces a JSON literal that omits the
new keys.

- [ ] **Step 2: Run the test to verify it fails**

```sh
swift test --filter RouteGainPersistenceTests
```

Expected: fails — `StoredRoute` doesn't have the new fields yet.

- [ ] **Step 3: Add the fields to `StoredRoute`**

In `Sources/JboxEngineSwift/Persistence/StoredAppState.swift`, find `public struct StoredRoute: Codable`. Append:

```swift
    /// ABI v14 — master fader, in dB. 0 = unity.
    public var masterGainDb: Float = 0.0
    /// ABI v14 — per-channel trim, in dB. Empty means "no trim" ≡ all 0 dB.
    public var trimDbs: [Float] = []
    /// ABI v14 — mute, independent of fader state.
    public var muted: Bool = false

    private enum GainKeys: String, CodingKey {
        case masterGainDb, trimDbs, muted
    }
```

`Codable` synthesis will produce sensible default values for absent
keys *if* the struct uses Swift's default-value-aware decoding (since
Swift 5.4, Codable still requires explicit init(from:) for default
fallbacks). The cleanest way to keep this simple is to override
`init(from decoder:)` and `encode(to:)`. The existing `StoredRoute`
likely already has `init(from decoder:)` for some other reason — read
it first; if not, add this minimal version *additionally* alongside the
existing memberwise init:

```swift
    public init(from decoder: Decoder) throws {
        // Decode the existing keys first using the pre-existing
        // CodingKeys — *fold this into the existing init(from:)*
        // rather than copy-pasting; the snippet shown is the gain
        // delta only.
        let gainContainer = try decoder.container(keyedBy: GainKeys.self)
        self.masterGainDb = try gainContainer.decodeIfPresent(Float.self, forKey: .masterGainDb) ?? 0.0
        self.trimDbs      = try gainContainer.decodeIfPresent([Float].self, forKey: .trimDbs) ?? []
        self.muted        = try gainContainer.decodeIfPresent(Bool.self,   forKey: .muted) ?? false
        // ... existing decode logic for the other fields ...
    }
```

> Read the current `StoredAppState.swift` first; the right shape of the
> change depends on whether `StoredRoute` already has a custom
> `init(from:)`. If yes, fold the three `decodeIfPresent` lines into
> it. If no, write the full `init(from:)` covering every existing
> property too.

- [ ] **Step 4: Run the test to verify it passes**

```sh
swift test --filter RouteGainPersistenceTests
```

Expected: 3 cases pass.

- [ ] **Step 5: Run the full Swift suite to check the broader persistence tests still pass**

```sh
swift test
```

Expected: green (existing persistence tests should still load their fixtures fine since the new fields are optional with defaults).

- [ ] **Step 6: Commit**

```sh
git add Sources/JboxEngineSwift/Persistence/StoredAppState.swift \
        Tests/JboxEngineTests/RouteGainPersistenceTests.swift
git commit -m "$(cat <<'EOF'
swift(gain): persist masterGainDb / trimDbs / muted in StoredRoute

Additive Codable fields with default-fallbacks via decodeIfPresent,
so existing state.json files (without the new keys) load unchanged.
Tests cover the round-trip with-fields, the legacy decode path
without-fields, and the encoded JSON shape.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: `EngineStore` setters

**Files:**
- Modify: `Sources/JboxEngineSwift/EngineStore.swift`
- Modify: `Sources/JboxEngineSwift/JboxEngine.swift` (add throwing wrappers)
- Test: `Tests/JboxEngineTests/EngineStoreGainTests.swift` (new)

Three setter methods on `EngineStore` that update the in-memory `Route` and forward to the C ABI.

**Deviation (Option A non-finite handling):** `setMasterGainDb` /
`setChannelTrimDb` reject NaN (no-op + log) and clamp `-infinity` to
`FaderTaper.minFiniteDb` (-60 dB) before storing, so the persisted
`Route.masterGainDb` / `trimDbs` are always finite. `StateStore`'s
default `JSONEncoder` defaults to `.throw` on non-finite floats — this
keeps the persistence path safe without forcing a non-standard
`.convertToString` round-trip in `state.json`. True silence still
flows through the MUTE button via `setRouteMuted` / `Route.muted`.
The plan originally implied a direct `db`-passthrough; the live
implementation chose Option A and added 5 extra regression tests
(NaN rejection, -infinity clamp, unknown id no-op, default-empty
trimDbs pad, out-of-range channel no-op) on top of the 3 prescribed
cases.

- [x] **Step 1: Write the failing test**

Create `Tests/JboxEngineTests/EngineStoreGainTests.swift`:

```swift
import Testing
@testable import JboxEngineSwift

@MainActor
@Suite("EngineStoreGain")
struct EngineStoreGainTests {

    @Test("setMasterGainDb updates the model")
    func setsMaster() throws {
        let store = EngineStore.makeWithSimulatedRoute(channels: 2)
        let routeId = store.routes[0].id

        store.setMasterGainDb(routeId: routeId, db: -3.0)

        #expect(store.routes[0].masterGainDb == -3.0)
    }

    @Test("setChannelTrimDb updates only the indexed channel")
    func setsTrim() throws {
        let store = EngineStore.makeWithSimulatedRoute(channels: 2)
        let routeId = store.routes[0].id
        // Initial: trimDbs == [0.0, 0.0]

        store.setChannelTrimDb(routeId: routeId, channelIndex: 1, db: -2.5)

        #expect(store.routes[0].trimDbs[0] == 0.0)
        #expect(store.routes[0].trimDbs[1] == -2.5)
    }

    @Test("setRouteMuted toggles the model")
    func togglesMute() throws {
        let store = EngineStore.makeWithSimulatedRoute(channels: 2)
        let routeId = store.routes[0].id

        store.setRouteMuted(routeId: routeId, muted: true)
        #expect(store.routes[0].muted == true)

        store.setRouteMuted(routeId: routeId, muted: false)
        #expect(store.routes[0].muted == false)
    }
}
```

`EngineStore.makeWithSimulatedRoute(channels:)` is a test helper that
spins an EngineStore against the simulated backend with one running
route. Match whatever existing EngineStoreTests use as a fixture (read
`Tests/JboxEngineTests/EngineStoreTests.swift` if it exists) — there's
almost certainly already a similar helper to copy from.

- [x] **Step 2: Run the test to verify it fails**

```sh
swift test --filter EngineStoreGainTests
```

Expected: fails — the three setter methods don't exist yet.

- [x] **Step 3: Add the three setter methods to `EngineStore`**

In `Sources/JboxEngineSwift/EngineStore.swift`, near the existing route-mutation methods (search for `renameRoute` for an analogue), add:

```swift
    /// Set the per-route master fader, in dB. Persists.
    public func setMasterGainDb(routeId: UInt32, db: Float) {
        guard let idx = routes.firstIndex(where: { $0.id == routeId }) else { return }
        routes[idx].masterGainDb = db
        _ = jbox_engine_set_route_master_gain_db(engineHandle, routeId, db)
        persist()
    }

    /// Set the per-channel trim, in dB, for the given mapped channel.
    public func setChannelTrimDb(routeId: UInt32,
                                 channelIndex: Int,
                                 db: Float) {
        guard let idx = routes.firstIndex(where: { $0.id == routeId }) else { return }
        // Ensure the trims array is sized to the mapping; pad with 0 dB
        // if a legacy Route still has an empty trimDbs.
        let mappingCount = routes[idx].config.mapping.count
        if routes[idx].trimDbs.count != mappingCount {
            routes[idx].trimDbs = Array(repeating: 0, count: mappingCount)
        }
        guard channelIndex >= 0, channelIndex < mappingCount else { return }
        routes[idx].trimDbs[channelIndex] = db
        _ = jbox_engine_set_route_channel_trim_db(
            engineHandle, routeId, UInt32(channelIndex), db)
        persist()
    }

    /// Toggle the per-route mute. Persists.
    public func setRouteMuted(routeId: UInt32, muted: Bool) {
        guard let idx = routes.firstIndex(where: { $0.id == routeId }) else { return }
        routes[idx].muted = muted
        _ = jbox_engine_set_route_mute(engineHandle, routeId, muted ? 1 : 0)
        persist()
    }
```

> Replace `engineHandle` and `persist()` with whatever the file
> actually uses for the engine pointer and the StateStore-flush call.
> Existing methods like `renameRoute` will show the pattern.

- [x] **Step 4: Run the test + Swift suite**

```sh
swift test --filter EngineStoreGainTests
swift test
```

Expected: green.

- [x] **Step 5: Commit**

```sh
git add Sources/JboxEngineSwift/EngineStore.swift \
        Tests/JboxEngineTests/EngineStoreGainTests.swift
git commit -m "$(cat <<'EOF'
swift(gain): EngineStore exposes setMasterGainDb / setChannelTrimDb / setRouteMuted

Each setter updates the in-memory Route, calls the matching ABI v14
function, and persists through StateStore. Tests cover the model
side; engine wiring is covered by the existing C++ gain tests.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: `MeterLevel.dawScaleMarks`

**Files:**
- Modify: `Sources/JboxEngineSwift/MeterLevel.swift`
- Test: extend `Tests/JboxEngineTests/MeterLevelTests.swift` if it exists, otherwise add a small test.

DAW-standard scale: `0, -3, -6, -12, -18, -24, -36, -48, -60` dBFS.

- [ ] **Step 1: Write the failing test**

In `Tests/JboxEngineTests/MeterLevelTests.swift` (create the file if it doesn't exist; otherwise append):

```swift
@Test("dawScaleMarks lists the standard DAW dBFS marks in descending order")
func dawScaleMarks() {
    let dbs = MeterLevel.dawScaleMarks.map { $0.dB }
    #expect(dbs == [0, -3, -6, -12, -18, -24, -36, -48, -60])
    // Strictly descending.
    #expect(zip(dbs, dbs.dropFirst()).allSatisfy { $0 > $1 })
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
swift test --filter MeterLevelTests/dawScaleMarks
```

Expected: fails — `dawScaleMarks` undefined.

- [ ] **Step 3: Add `dawScaleMarks`**

In `Sources/JboxEngineSwift/MeterLevel.swift`, append inside the `public enum MeterLevel { ... }`:

```swift
    /// Standard dBFS marks for the DAW-style meter scale (descending).
    /// Used by the SwiftUI `DbScale` view to draw gridlines + labels.
    /// See docs/2026-04-28-route-gain-mixer-strip-design.md § 4.3.
    public static let dawScaleMarks: [(dB: Float, label: String)] = [
        (0,   "0"),
        (-3,  "-3"),
        (-6,  "-6"),
        (-12, "-12"),
        (-18, "-18"),
        (-24, "-24"),
        (-36, "-36"),
        (-48, "-48"),
        (-60, "-60"),
    ]
```

- [ ] **Step 4: Run the test + full Swift suite**

```sh
swift test --filter MeterLevelTests
swift test
```

Expected: green.

- [ ] **Step 5: Commit**

```sh
git add Sources/JboxEngineSwift/MeterLevel.swift \
        Tests/JboxEngineTests/MeterLevelTests.swift
git commit -m "$(cat <<'EOF'
swift(meter): add DAW-standard dBFS scale marks

dawScaleMarks lists 0, -3, -6, -12, -18, -24, -36, -48, -60 dBFS
in descending order — the marks DbScale will draw on the new
mixer-strip layout. floorDb / nearDb / clipDb / Zone unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: `FaderSlider` widget

**Files:**
- Create: `Sources/JboxApp/FaderSlider.swift`

Vertical slider that binds to a dB value via `FaderTaper`. Pure SwiftUI; no engine reach-through. Verified via `#Preview`.

- [ ] **Step 1: Write the widget**

Create `Sources/JboxApp/FaderSlider.swift`:

```swift
import SwiftUI
import JboxEngineSwift

/// Vertical fader bound to a dB value. Renders a track + cap inside a
/// fixed-height frame; converts to/from a 0...1 throw position via
/// `FaderTaper`. Master and per-channel trims share this widget,
/// differentiated only by `style`.
///
/// Supports:
///   - drag (vertical) to adjust;
///   - shift-drag for ×0.2 fine adjust;
///   - double-tap to reset to 0 dB;
///   - keyboard arrows for ±0.5 dB / shift ±0.1 dB when focused.
struct FaderSlider: View {

    enum Style {
        case trim     // narrow, lighter cap
        case master   // wider cap, heavier border
    }

    @Binding var dB: Float
    let style: Style

    private var capWidth: CGFloat   { style == .master ? 32 : 22 }
    private var capHeight: CGFloat  { style == .master ? 14 : 9 }
    private var trackWidth: CGFloat { style == .master ? 5 : 3 }

    @FocusState private var focused: Bool

    var body: some View {
        GeometryReader { geo in
            let h = geo.size.height
            let position = CGFloat(FaderTaper.positionFor(db: dB))
            ZStack {
                // Track (centered).
                RoundedRectangle(cornerRadius: 2)
                    .fill(Color(red: 0.10, green: 0.10, blue: 0.11))
                    .frame(width: trackWidth, height: h)
                    .overlay(
                        RoundedRectangle(cornerRadius: 2)
                            .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
                    )
                // Cap.
                RoundedRectangle(cornerRadius: 2)
                    .fill(LinearGradient(colors: [Color(white: 0.85),
                                                  Color(white: 0.45)],
                                         startPoint: .top, endPoint: .bottom))
                    .overlay(
                        RoundedRectangle(cornerRadius: 2)
                            .stroke(Color.black.opacity(0.5), lineWidth: 1)
                    )
                    .frame(width: capWidth, height: capHeight)
                    .position(x: geo.size.width / 2,
                              y: h - position * h)
                    .gesture(dragGesture(height: h))
                    .onTapGesture(count: 2) { dB = 0 }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .focusable()
            .focused($focused)
            .onKeyPress(.upArrow)   { adjust(by: 0.5);  return .handled }
            .onKeyPress(.downArrow) { adjust(by: -0.5); return .handled }
            .accessibilityLabel("Fader")
            .accessibilityValue(dB.isFinite
                                ? "\(String(format: "%+.1f", dB)) dB"
                                : "muted")
        }
    }

    private func dragGesture(height: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 1, coordinateSpace: .local)
            .onChanged { value in
                let dy = -value.translation.height / height
                let scale: Float = NSEvent.modifierFlags.contains(.shift) ? 0.2 : 1.0
                let currentPos = FaderTaper.positionFor(db: dB)
                let newPos = max(0, min(1, currentPos + Float(dy) * scale))
                dB = FaderTaper.dbForPosition(newPos)
            }
    }

    private func adjust(by step: Float) {
        let modifier: Float =
            NSEvent.modifierFlags.contains(.shift) ? 0.2 : 1.0
        let newDb = (dB.isFinite ? dB : FaderTaper.minFiniteDb) + step * modifier
        dB = max(-Float.infinity, min(FaderTaper.maxDb, newDb))
    }
}

#if DEBUG
#Preview("FaderSlider — trim & master at -3 dB") {
    @Previewable @State var trim: Float = -3.0
    @Previewable @State var master: Float = -3.0
    return HStack(spacing: 30) {
        FaderSlider(dB: $trim, style: .trim).frame(width: 28, height: 200)
        FaderSlider(dB: $master, style: .master).frame(width: 36, height: 200)
    }
    .padding()
    .frame(width: 200, height: 240)
    .background(Color(red: 0.11, green: 0.11, blue: 0.12))
}
#endif
```

> The widget uses `NSEvent.modifierFlags` for shift detection because
> SwiftUI's drag gesture doesn't expose modifier flags directly on
> macOS. Verify this builds — if `import AppKit` is needed, add it.

- [ ] **Step 2: Build**

```sh
swift build
```

Expected: builds. (No tests on the widget itself; the math is in
`FaderTaper`, already covered.)

- [ ] **Step 3: Eyeball the preview**

Open the preview through `swift package generate-xcodeproj` is not
available (SPM-only build). Skip; rely on the `MeterPanel` integration
preview in Task 17 + the manual run in Task 18.

- [ ] **Step 4: Commit**

```sh
git add Sources/JboxApp/FaderSlider.swift
git commit -m "$(cat <<'EOF'
ui(gain): add FaderSlider widget — vertical, dB-bound, VCA-style

Generic vertical fader that binds to a Float dB value through
FaderTaper. Two styles (trim, master) tweak cap and track sizing.
Supports drag (with shift for fine adjust), double-tap reset to
0 dB, keyboard arrows. Accessibility-friendly value text.

Pure SwiftUI; no engine reach-through. Verified by visual #Preview;
covered indirectly by the FaderTaper unit tests for the math.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: `ChannelStripColumn` widget

**Files:**
- Create: `Sources/JboxApp/ChannelStripColumn.swift`

One column per `mapping[i]` — fader + dest meter inside the shared bar zone, channel-name header on top, dB readout on the bottom.

- [ ] **Step 1: Write the widget**

Create `Sources/JboxApp/ChannelStripColumn.swift`:

```swift
import SwiftUI
import JboxEngineSwift

/// Single channel strip in the mixer panel. Composes a trim FaderSlider
/// next to a `ChannelBar` (dest meter), inside the shared bar-zone
/// rectangle. Header shows the source channel name (formatted via
/// `ChannelLabel`); footer shows the trim's dB readout.
///
/// See docs/2026-04-28-route-gain-mixer-strip-design.md §§ 4.1-4.4, 4.7.
struct ChannelStripColumn: View {
    let routeId: UInt32
    let channelIndex: Int
    let primaryLabel: String          // truncates with ... at strip width
    let tooltipLabel: String          // full src → dst pair
    let peak: Float
    let hold: Float
    @Binding var trimDb: Float
    let barZoneHeight: CGFloat
    let isCompact: Bool               // true when ≥ 6 channels

    var body: some View {
        VStack(spacing: 4) {
            // Header — channel label.
            Text(primaryLabel)
                .font(.system(size: isCompact ? 9 : 10,
                              weight: .medium))
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(.secondary)
                .help(tooltipLabel)
            // +12 cap for the fader scale.
            Text("+12")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
            // Bar zone — fader + meter, side by side.
            HStack(spacing: 4) {
                FaderSlider(dB: $trimDb, style: .trim)
                    .frame(width: isCompact ? 16 : 24)
                ChannelBar(peak: peak, hold: hold)
                    .frame(width: isCompact ? 9 : 14)
            }
            .frame(height: barZoneHeight)
            // -∞ cap for the fader scale.
            Text("−∞")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
            // dB readout for the trim.
            Text(formattedDb(trimDb))
                .font(.system(size: 10, weight: .semibold).monospacedDigit())
                .foregroundStyle(.primary)
        }
        .padding(.horizontal, 5)
        .padding(.vertical, 6)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(Color(red: 0.13, green: 0.13, blue: 0.15))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 5)
                .stroke(Color.secondary.opacity(0.22), lineWidth: 1)
        )
    }

    private func formattedDb(_ db: Float) -> String {
        if db <= -.infinity { return "−∞" }
        return String(format: "%+.1f", db)
    }
}
```

- [ ] **Step 2: Build**

```sh
swift build
```

Expected: builds.

- [ ] **Step 3: Commit**

```sh
git add Sources/JboxApp/ChannelStripColumn.swift
git commit -m "$(cat <<'EOF'
ui(gain): add ChannelStripColumn — fader + meter + channel label

One strip per route mapping[i]. FaderSlider (trim style) sits beside
the existing ChannelBar inside the shared bar-zone rectangle.
Primary label truncates with ellipsis; .help() tooltip carries the
full source → destination channel-name pair.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 15: `MasterFaderStrip` widget

**Files:**
- Create: `Sources/JboxApp/MasterFaderStrip.swift`

Master strip on the far right — master fader + dB readout + mute button.

- [ ] **Step 1: Write the widget**

Create `Sources/JboxApp/MasterFaderStrip.swift`:

```swift
import SwiftUI
import JboxEngineSwift

/// Master strip on the right edge of the mixer panel. Composes the
/// master FaderSlider, its dB readout, and the MUTE toggle. Heavier
/// border so it reads as a separate group.
///
/// See docs/2026-04-28-route-gain-mixer-strip-design.md §§ 4.1, 4.5.
struct MasterFaderStrip: View {
    @Binding var masterDb: Float
    @Binding var muted: Bool
    let barZoneHeight: CGFloat

    var body: some View {
        VStack(spacing: 4) {
            Text("MASTER")
                .font(.system(size: 9, weight: .bold))
                .tracking(0.4)
                .foregroundStyle(.primary)
            Text("+12")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
            FaderSlider(dB: $masterDb, style: .master)
                .frame(width: 36, height: barZoneHeight)
            Text("−∞")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
            Text(formattedDb(masterDb, muted: muted))
                .font(.system(size: 11, weight: .semibold).monospacedDigit())
                .foregroundStyle(muted ? Color.red : Color.primary)
            Button(action: { muted.toggle() }) {
                Text("MUTE")
                    .font(.system(size: 9, weight: .semibold))
                    .padding(.vertical, 2)
                    .padding(.horizontal, 8)
                    .background(
                        RoundedRectangle(cornerRadius: 3)
                            .fill(muted ? Color.red.opacity(0.85)
                                        : Color(red: 0.23, green: 0.23, blue: 0.25))
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 3)
                            .stroke(Color.secondary.opacity(0.35), lineWidth: 1)
                    )
                    .foregroundStyle(muted ? Color.white : Color.secondary)
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(
            RoundedRectangle(cornerRadius: 5)
                .fill(Color(red: 0.155, green: 0.155, blue: 0.17))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 5)
                .stroke(Color.secondary.opacity(0.42), lineWidth: 1)
        )
    }

    private func formattedDb(_ db: Float, muted: Bool) -> String {
        if muted { return "MUTED" }
        if db <= -.infinity { return "−∞" }
        return String(format: "%+.1f dB", db)
    }
}
```

- [ ] **Step 2: Build**

```sh
swift build
```

Expected: builds.

- [ ] **Step 3: Commit**

```sh
git add Sources/JboxApp/MasterFaderStrip.swift
git commit -m "$(cat <<'EOF'
ui(gain): add MasterFaderStrip — master fader + readout + mute

Right-edge strip composing FaderSlider (master style), the dB
readout (shows MUTED when muted, finite dB or −∞ otherwise), and a
toggle MUTE button. Heavier border distinguishes it from per-channel
strips.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 16: `MeterPanel` rewrite

**Files:**
- Modify: `Sources/JboxApp/MeterBar.swift` — rewrite `MeterPanel`, repurpose `DbScale` to use `MeterLevel.dawScaleMarks`, keep `BarGroup` only for the SOURCE column, drop the standalone DEST `BarGroup`.

This is the biggest UI change. Bar zone height switches between 200 px (≤ 5 channels) and 170 px (≥ 6 channels) — the "compact tier" switch in the spec.

- [ ] **Step 1: Update `DbScale` to use `MeterLevel.dawScaleMarks`**

In `Sources/JboxApp/MeterBar.swift`, find `struct DbScale` (currently uses a hard-coded array around line 173). Replace its `Self.marks` static with:

```swift
private static let marks = MeterLevel.dawScaleMarks
```

Verify the existing draw loop reads `for (dB, label) in Self.marks` — it does — so no other change is needed here.

- [ ] **Step 2: Rewrite `MeterPanel`**

In `Sources/JboxApp/MeterBar.swift`, replace the entire `struct MeterPanel: View { ... }` body with:

```swift
struct MeterPanel: View {
    let route: Route
    let store: EngineStore
    let peaks: MeterPeaks

    @AppStorage(JboxPreferences.showDiagnosticsKey)
    private var showDiagnostics = false

    private var channelCount: Int { max(1, route.config.mapping.count) }
    private var isCompact: Bool   { channelCount >= 6 }
    private var barZoneHeight: CGFloat { isCompact ? 170 : 200 }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: false)) { timeline in
            let now = timeline.date.timeIntervalSinceReferenceDate
            VStack(alignment: .leading, spacing: 16) {
                HStack(alignment: .center, spacing: 12) {
                    // SOURCE pre-fader bar group (kept).
                    BarGroup(
                        title: "SOURCE",
                        labels: sourceLabels,
                        peaks: peaks.source,
                        routeId: route.id,
                        side: .source,
                        store: store,
                        now: now,
                        barHeight: barZoneHeight
                    )
                    .frame(maxWidth: 180)

                    Image(systemName: "arrow.right")
                        .font(.title2.weight(.light))
                        .foregroundStyle(.tertiary)
                        .frame(width: 24)

                    // Shared meter dB scale, drawn once.
                    DbScale()
                        .frame(width: 28, height: barZoneHeight)
                        .padding(.top, 28)        // align to bar-zone top

                    // Channel strips.
                    HStack(spacing: isCompact ? 5 : 10) {
                        ForEach(0..<channelCount, id: \.self) { i in
                            ChannelStripColumn(
                                routeId: route.id,
                                channelIndex: i,
                                primaryLabel: stripPrimaryLabel(i),
                                tooltipLabel: stripTooltip(i),
                                peak: peakAt(i),
                                hold: store.heldPeak(routeId: route.id,
                                                     side: .destination,
                                                     channel: i,
                                                     now: now),
                                trimDb: trimBinding(channelIndex: i),
                                barZoneHeight: barZoneHeight,
                                isCompact: isCompact
                            )
                        }
                    }

                    // MASTER strip on the far right.
                    MasterFaderStrip(
                        masterDb: masterBinding(),
                        muted: muteBinding(),
                        barZoneHeight: barZoneHeight
                    )
                    .padding(.leading, 8)
                }

                if showDiagnostics {
                    DiagnosticsBlock(
                        route: route,
                        components: store.latencyComponents[route.id])
                }
            }
            .padding(.top, 20)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(MeterAccessibilityLabel.summary(
            source: peaks.source,
            destination: peaks.destination))
    }

    // MARK: - Labels

    private var sourceNames: [String] {
        store.channelNames(uid: route.config.sourceUid, direction: .input)
    }
    private var destNames: [String] {
        store.channelNames(uid: route.config.destUid, direction: .output)
    }

    private func stripPrimaryLabel(_ i: Int) -> String {
        if isCompact { return "\(i + 1)" }
        let srcCh = Int(route.config.mapping[i].src)
        return ChannelLabel.format(index: srcCh, names: sourceNames)
    }

    private func stripTooltip(_ i: Int) -> String {
        let srcCh = Int(route.config.mapping[i].src)
        let dstCh = Int(route.config.mapping[i].dst)
        let src = ChannelLabel.format(index: srcCh, names: sourceNames)
        let dst = ChannelLabel.format(index: dstCh, names: destNames)
        return "\(src) → \(dst)"
    }

    private var sourceLabels: [String] {
        (1...channelCount).map { "\($0)" }
    }

    private func peakAt(_ i: Int) -> Float {
        guard i < peaks.destination.count else { return 0 }
        return peaks.destination[i]
    }

    // MARK: - Bindings into the store's Route model.

    private func masterBinding() -> Binding<Float> {
        Binding<Float>(
            get: { store.routes.first(where: { $0.id == route.id })?.masterGainDb ?? 0 },
            set: { store.setMasterGainDb(routeId: route.id, db: $0) }
        )
    }

    private func muteBinding() -> Binding<Bool> {
        Binding<Bool>(
            get: { store.routes.first(where: { $0.id == route.id })?.muted ?? false },
            set: { store.setRouteMuted(routeId: route.id, muted: $0) }
        )
    }

    private func trimBinding(channelIndex: Int) -> Binding<Float> {
        Binding<Float>(
            get: {
                let r = store.routes.first(where: { $0.id == route.id })
                let trims = r?.trimDbs ?? []
                return channelIndex < trims.count ? trims[channelIndex] : 0
            },
            set: {
                store.setChannelTrimDb(
                    routeId: route.id,
                    channelIndex: channelIndex,
                    db: $0)
            }
        )
    }
}
```

> Property names like `route.config.sourceUid`, `route.config.destUid`,
> `mapping[i].src`, `mapping[i].dst` should match whatever the existing
> `Route` actually exposes — re-read `JboxEngine.swift` and substitute
> the real names if they differ.

- [ ] **Step 3: Build + run the Swift suite**

```sh
swift build
swift test
```

Expected: green. Existing `BarGroup`-based behavior for SOURCE is unchanged; existing accessibility test still passes since the summary is still based on source + destination peaks.

- [ ] **Step 4: Refresh the existing #Preview blocks**

Find the existing `#Preview` blocks in `MeterBar.swift` (around line 360+). Update them so they still compile against the new `MeterPanel` API. The `meterPanelPreview(channels:)` helper should still work — the `MeterPanel` initializer signature didn't change.

Add one extra preview for the compact tier:

```swift
#Preview("MeterPanel — 4 channels (default tier)") {
    meterPanelPreview(channels: 4)
}
#Preview("MeterPanel — 8 channels (compact tier)") {
    meterPanelPreview(channels: 8)
}
```

- [ ] **Step 5: Commit**

```sh
git add Sources/JboxApp/MeterBar.swift
git commit -m "$(cat <<'EOF'
ui(gain): rewrite MeterPanel as mixer strip — strips + master

Replaces the dest-side BarGroup with a row of ChannelStripColumns
(trim + dest meter side-by-side per mapped channel) and a
MasterFaderStrip on the far right. SOURCE BarGroup kept on the
left (pre-fader). Single shared DbScale column drawn once between
SOURCE and the strip cluster, marked at the new MeterLevel.dawScaleMarks.

Compact tier (≥6 channels) shrinks bar-zone height to 170px and
falls back to numeric strip headers; tooltips still carry the full
source → destination channel-name pair via .help().

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 17: `make verify` + manual smoke

**Files:** none (verification + manual run)

The pipeline should be fully green. After that, a manual run validates the alignment, the slider feel, and the click-free mute.

- [ ] **Step 1: Run the full pipeline**

```sh
make verify
```

Expected: 184+ C++ cases (now plus 6 gain_smoother + 4 route_gain_manager + 1 bridge_api gain), full Swift suite (now plus FaderTaper, RouteGainPersistence, EngineStoreGain, MeterLevel.dawScaleMarks), RT-safety scan clean, TSan clean.

If anything fails: stop, fix, re-run.

- [ ] **Step 2: Run the app**

```sh
make run
```

Manual checklist (verify each):

- Mixer-strip alignment: every fader track, dest bar, and the master fader share the same vertical extent within a route's expanded panel.
- 4-channel route: trim faders show source channel names in the strip header; tooltip on each header shows "Source N → Destination M".
- 8-channel route: strips drop to numeric labels; tooltip still works.
- Drag the master fader from 0 dB → −12 dB → 0 dB on a route with audio playing: smooth level change, no zipper, no clicks.
- Hit MUTE: audio fades to silent in ~30 ms, no click.
- Hit MUTE again: audio returns to current fader's level, no click.
- Quit and relaunch: master / trim / mute state persists.
- Run with `log stream --predicate 'subsystem == "com.jbox.app"'` open: no underrun events triggered by sliding faders.

- [ ] **Step 3: If everything checks, no commit needed (the previous commit lands the feature). If a manual issue surfaces, file it as a discrete fix commit (or open a `docs/followups.md` entry if it's hardware-dependent and out of scope for this slice).**

---

## Task 18: Spec / plan / docs reconciliation

**Files:**
- Modify: `docs/spec.md` — fold gain semantics into § 2.8 / § 4.5 (or add a new §) once the feature ships.
- Modify: `docs/plan.md` — add a new phase entry for "Phase X — Route gain + mixer strip" with a one-line "shipped at <commit>" status.

CLAUDE.md treats the docs as the living source of truth — they need to reflect the shipped behavior.

- [ ] **Step 1: Update `docs/spec.md`**

Add a brief subsection under § 2 (engine) and § 4 (UI) summarising the new gain path and the new strip layout. Keep them concise — the design doc captures the detail; `spec.md` only needs the "what" + a pointer.

Example § 2.13 "Per-route gain (ABI v14)" entry:

```markdown
### 2.13 Per-route gain (ABI v14)

Each route carries a master fader (in dB) and an optional per-channel
trim array (also in dB), plus a mute boolean. Master and trims are
multiplied together and applied per output sample inside
outputIOProcCallback / duplexIOProcCallback. dB → linear conversion
runs on the control thread; the RT path is alloc-free, lock-free,
and pow-free. A 10 ms one-pole IIR (rt/gain_smoother.hpp) smooths
slider drags and mute toggles to silence zipper noise / clicks.
Source meter stays pre-fader; dest meter reads post-fader. See
docs/2026-04-28-route-gain-mixer-strip-design.md.
```

Example § 4.5 update — append:

```markdown
The expanded route panel is a mixer strip (Phase X): SOURCE pre-fader
column → shared dBFS scale (DAW-standard marks) → per-channel strips
(trim fader + dest meter) → master strip (master fader + mute). See
docs/2026-04-28-route-gain-mixer-strip-design.md § 4.
```

- [ ] **Step 2: Update `docs/plan.md`**

Add a new phase entry at the bottom (or in the appropriate phase slot) listing each task in this plan as a `[x]` checkbox with the commit SHA from `git log`. Match the formatting of existing phases.

- [ ] **Step 3: Commit**

```sh
git add docs/spec.md docs/plan.md
git commit -m "$(cat <<'EOF'
docs(gain): reconcile spec.md / plan.md with shipped gain feature

spec.md gains a § 2.13 entry for the per-route gain path and a
mixer-strip note in § 4.5. plan.md gets the new phase pinned to the
commits in this slice. Detailed design remains in
docs/2026-04-28-route-gain-mixer-strip-design.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review notes

- **Spec coverage.** Every section of `docs/2026-04-28-route-gain-mixer-strip-design.md` has at least one task: §4.1 layout → Task 16; §4.3 DAW scale → Task 12; §4.4 fader behavior → Task 13; §4.5 mute → Task 15; §4.6 persistence → Task 10; §4.7 channel labels → Task 16; §5 RT path → Tasks 1, 5, 6; §6 ABI v14 → Tasks 7, 8; §7 Swift wrapper → Tasks 9, 11; §8 persistence → Task 10; §9 testing — covered across each task; §10 effort estimate — reflected in task granularity.
- **No placeholders.** Every code step ships actual code (or, for steps that depend on names that vary by file content, ships the code with a `> ⚠️` callout pointing the executor at the file to read first).
- **Type / name consistency.** Method names (`setMasterGainDb`, `setChannelTrimDb`, `setRouteMuted`) match across C ABI, RouteManager, Engine, EngineStore, and the C ABI wrappers. Field names (`master_gain_db`, `channel_trims_db`, `muted`) match between the C struct, the C++ struct, and the Swift `Route` (via `masterGainDb`, `trimDbs`, `muted`).
- **TDD.** Pure-logic tasks (1, 2, 10, 11, 12) all start with the failing test. RT integration (5, 6) starts with the failing integration test. UI tasks (13-16) rely on `#Preview` + the manual smoke in Task 17 — consistent with the existing JboxApp testing convention.
- **Frequent commits.** Every task ends in a commit; Tasks 5 and 6 each include `make verify` / RT-scan / TSan checkpoints; Task 17 is the final pipeline + manual smoke gate before docs reconciliation in Task 18.
