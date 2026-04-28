# Route Gain + Mixer-Strip UI — Design

| | |
|-|-|
| **Date** | 2026-04-28 |
| **Status** | Draft, awaiting user review |
| **Scope** | Per-route master fader, per-channel trims, mute, mixer-strip UI, DAW-standard meter scale, driver-published channel names |
| **Out of scope** | Needle-style VU meter (separate spec), VCA groups, true downmix / sum, pan |
| **ABI impact** | New v14 (MINOR, additive) |

---

## 1. Motivation

Today a Jbox route passes audio through with no level control — input level into the source device is the only knob the user has. Use cases for a per-route fader:

- Trim a route that's hot enough to clip the destination's preamp / converter.
- Bring a quiet route up by a few dB to match peers.
- Mute a route without tearing it down (state preserved, no re-prime hit on un-mute).
- Match a small subset of channels (per-channel trim) when the source has unequal levels (e.g., one mic louder than the others on a multi-input bus).

The route is the natural unit: one signal flow, one set of controls. The UI should look and feel like a console channel strip so engineers don't have to relearn anything.

## 2. Goals

- **Per-route master fader** — single VCA-style fader controlling the whole route's level. Range −∞ … +12 dB, 0 dB at unity, double-click reset.
- **Per-channel trims** — one fader per mapped channel, same range and taper, for unequal balancing.
- **Mute** — separate toggle on the master, click-free (smoothed gate).
- **Mixer-strip layout** — SOURCE meter group → channel strips (trim + dest meter) → MASTER strip (master fader) on the far right.
- **DAW-standard dBFS meter scale** — 0, −3, −6, −12, −18, −24, −36, −48, −60 dBFS, linear in dB across the bar height.
- **Image preservation** — master is a per-channel scalar applied uniformly; relative channel amplitudes unchanged when master moves.
- **Persistence** — fader state, trim values, mute state survive app relaunch.
- **Click-free** — slider drags and mute toggles smooth on the RT thread; no zipper, no pops.
- **Backward-compatible ABI** — additive only; legacy zero-init callers stay at unity gain.
- **Driver-published channel names** in strip headers (using existing `ChannelLabel` / `EngineStore.channelNames` infrastructure), with numeric fallbacks and tooltips for the full source / dest pair.

## 3. Non-goals

- **Needle-style VU meter.** Parked in a separate spec. This spec ships the master strip with just the master fader; the VU spec will own its own strip-layout adjustment when it lands.
- **VCA groups** (one master controlling several routes' faders simultaneously).
- **True downmix / sum** (folding N channels into M < N with summing).
- **Pan / balance.**
- **Automation, MIDI mapping, OSC.**
- **Per-channel mute** (only the master mutes; trim-to-`−∞` covers per-channel kill).

---

## 4. UI design

### 4.1 Mixer-strip layout

When a route is expanded in `RouteListView`, the meter panel becomes a **mixer strip**, structured left-to-right:

```
┌────────┬──────────────────────────────┬───────────┐
│ SOURCE │ CH 1   CH 2   CH 3   CH N    │  MASTER   │
│        │ ┌──┐  ┌──┐  ┌──┐   ┌──┐      │  ┌────┐   │
│ ║║║║   │ ║trim║trim║trim║  ║trim║     │  ║mast║   │
│        │ │meter│meter│meter│ │meter│  │  ║ er ║   │
│        │ └──┘  └──┘  └──┘   └──┘      │  └────┘   │
│        │                              │  MUTE     │
└────────┴──────────────────────────────┴───────────┘
```

- **SOURCE group** — preserved from today's `MeterPanel`. Compact column of pre-fader bar meters showing input level. Width-collapses gently as channel count grows.
- **Channel strip** — one per `mapping[i]`. Top-to-bottom: channel label (the source channel's name; see § 4.7), `+12` cap label, **bar zone** containing fader track + dest bar meter side-by-side, `−60` cap label, dB readout.
- **MASTER strip** — pinned to the far right with a slightly heavier border (`#27272a` background, `#555` border) to read as a separate group. Top-to-bottom: `MASTER` label, `+12` cap, **bar zone** containing the master fader, `−∞` cap, dB readout, `MUTE` button.

### 4.2 Bar-zone alignment rule

A single CSS-style `.bar-zone` height (200 px expanded / 170 px compact for ≥6 channels) is shared by every meter and every fader track in the strip. Labels and readouts sit in fixed-height bands above and below it. This is what keeps the v3 layout looking aligned — no component within a strip extends past the shared `bar-zone` rectangle.

### 4.3 DAW-standard meter scale

Marks: `0, −3, −6, −12, −18, −24, −36, −48, −60` dBFS. Linear in dB across the bar height (existing behavior: `MeterLevel.fractionFor(dB:)` is already linear, `floorDb = −60`). The current scale (`0, −3, −6, −20, −40, −60`) is replaced.

Color zones unchanged from today (`MeterLevel.Zone`):
- **Gray** — peak < −60 dBFS (silent)
- **Green** — −60 ≤ peak < −6
- **Yellow** — −6 ≤ peak < −3
- **Red** — peak ≥ −3

Peak-hold tick (`PeakHoldTracker`) unchanged.

### 4.4 Fader behavior

- **Range** — −∞ … +12 dB. Below position 4% (1/25 of the throw), the fader snaps to −∞ dB (fully muted via the gain path, separate from the explicit `MUTE` button).
- **Taper** — log curve, 0 dB at 75% throw, +12 dB at 100%. Pure module `FaderTaper` exposes `dbForPosition(_ pos: Float) -> Float` and `positionForFader(_ db: Float) -> Float`. Implementation: piecewise — linear in dB from +12 dB (top) down through 0 dB (75%) and on to −60 dB (5%), then a sharp kink to −∞ in the bottom 5%. (The kink prevents the bottom of the throw from feeling dead while staying above the digital floor.)
- **Reset to unity** — double-click on the fader cap or the readout text snaps to 0 dB.
- **dB readout** — single decimal place, `+0.0 dB`, `−1.5 dB`, etc. `−∞` rendered when the gain is fully muted.
- **Keyboard** — when the fader has focus, ↑/↓ adjust by ±0.5 dB; ⇧↑/⇧↓ by ±0.1 dB; ⌥-double-click resets.
- **Drag accuracy** — vertical drag of 1 px ≈ `0.075 dB` near unity (200 px throw covering 72 dB of useful range). Holding ⇧ while dragging slows by 5×.

### 4.5 Mute

- One `MUTE` button per master strip (not per channel — see Non-goals 3.6).
- Toggle. When muted, the master fader stays at its current position (state preserved); a smoothed gate ramps the multiplier to 0 over ~10 ms. Un-mute ramps back to the fader's current level.
- Visual: button background switches to red; the master dB readout shows `MUTED` instead of the fader's dB.

### 4.6 Persistence

Faders, trims, and mute state persist via `StateStore` / `StoredRoute` (additive — see § 8).

### 4.7 Channel labels

Today's `MeterPanel` uses purely numeric labels (`1`, `2`, …). The new mixer strip wires up the existing — but currently unused — `EngineStore.channelNames(uid:direction:)` + `ChannelLabel.format(index:names:)` infrastructure to show the driver-published name when one exists.

**Per-strip header.** Each strip's header shows the **source channel** name (the `mapping[i].src` side), formatted via `ChannelLabel.format` so it falls back to `"Ch N"` when the device publishes nothing for that channel. Source side wins because in a typical Jbox topology the destination is the user's predictable interface (main outs, monitor pair) while the source varies (different mics, different software outputs) — the more informative side.

**Width handling.** Long names (`"ADAT In 7 (Talkback)"`) truncate with `...` at the strip width. Full text and the full `src → dst` pair are exposed via the SwiftUI `.help(...)` tooltip on the strip header — accessible to both pointer hover and VoiceOver.

**Compact tier (≥ 6 channels).** Strips are too narrow for meaningful names; fall back to numeric index in the header. Tooltip still carries the full pair.

**Caching.** `EngineStore` already caches device + channel-name snapshots; no new fetch happens during strip render. Names refresh when devices re-enumerate (existing `refreshDevices` path).

---

## 5. Engine design

### 5.1 RT path

Gain is applied inside the existing per-channel output loop in `outputIOProcCallback` and `duplexIOProcCallback` (`Sources/JboxEngineC/control/route_manager.cpp`). For each output sample `s` written to channel `ch`:

```cpp
const float g = current_master * current_trim[ch];   // ← new
const float out = s * g;
samples[f * channel_count + dst_ch] = out;
// dest meter updateMax sees post-fader value (correct mixer behavior)
```

`current_master` and `current_trim[ch]` are RT-thread-local floats updated each block by the smoother (§ 5.2). The source meter in `inputIOProcCallback` is unchanged — it stays pre-fader.

**Cost.** One extra multiply per output sample per channel (`s * g`) and one extra multiply per channel per block to recompute `current_master * current_trim[ch]` outside the per-frame loop. Negligible.

### 5.2 Smoothing

A new RT-safe header `Sources/JboxEngineC/rt/gain_smoother.hpp` exposes:

```cpp
namespace jbox::rt {
struct GainSmoother {
  float current = 1.0f;
  float alpha   = 0.0f;   // computed from sample rate at attemptStart

  void  setAlphaForTimeConstant(double sample_rate, double seconds);
  float step(float target);              // one-pole IIR per block
};
}
```

Time constant: **10 ms**, giving a 95% settling time of ~30 ms — fast enough to feel instantaneous, slow enough to kill zipper noise on fast slider drags. Computed once per route at `attemptStart` from the destination device's nominal rate.

The smoother runs **once per block, not once per sample** (per-sample smoothing has no audible benefit at 10 ms with sub-ms blocks, and saves N MACs per channel). The smoothed value is held constant inside the block.

Mute is a separate atomic boolean target; when muted, the smoother's target becomes `0.0f`. When un-muted, the target returns to `master_target * trim_target[ch]`. Same smoothing dynamics ensure click-free.

### 5.3 RouteRecord state

`RouteRecord` (control side) gains:

```cpp
// Targets — written by control thread, read by RT thread
std::atomic<float>  target_master_gain{1.0f};
std::array<std::atomic<float>, kAtomicMeterMaxChannels> target_trim_gain;   // init in addRoute()
std::atomic<bool>   target_muted{false};

// RT-thread-local
jbox::rt::GainSmoother master_smoother;
std::array<jbox::rt::GainSmoother, kAtomicMeterMaxChannels> trim_smoothers;
```

`target_trim_gain[i]` is explicitly stored to `1.0f` in `RouteManager::addRoute` for `i < channels_count` (the rest stay at the default `std::atomic<float>` value and are never read because the RT path bounds reads by `channels_count`). Atomics use `relaxed` ordering (same as the existing target-rate pattern in `route_manager.cpp:232`).

The control thread converts dB → linear amplitude (`std::pow(10.0f, db / 20.0f)`) before storing into the atomic, so the RT path stays free of `pow` calls. `−∞ dB` maps to literal `0.0f`.

---

## 6. ABI v14 (MINOR, additive)

### 6.1 New `RouteConfig` fields

```c
typedef struct {
    /* ... existing v11 fields ... */
    float          master_gain_db;       /* 0.0 = unity (default) */
    const float*   channel_trims_db;     /* nullable; if null, all 0.0 */
    size_t         channel_trims_count;  /* must equal mapping_count if non-null */
    int            muted;                /* 0 = unmuted (default), 1 = muted */
} jbox_route_config_t;
```

Zero-init keeps backward behavior (unity gain, no trim, unmuted). `channel_trims_db` is caller-owned; the engine copies on `addRoute`.

### 6.2 New setters

```c
jbox_error_code_t jbox_engine_set_route_master_gain_db(
    jbox_engine_t* engine,
    jbox_route_id_t route_id,
    float db);

jbox_error_code_t jbox_engine_set_route_channel_trim_db(
    jbox_engine_t* engine,
    jbox_route_id_t route_id,
    uint32_t channel_index,    /* 0-based, < mapping_count */
    float db);

jbox_error_code_t jbox_engine_set_route_mute(
    jbox_engine_t* engine,
    jbox_route_id_t route_id,
    int muted);                /* 0 = unmuted, 1 = muted */
```

All three are thread-safe (atomic store), bounded in time, return `JBOX_OK` or an error code (`JBOX_ERR_NOT_FOUND`, `JBOX_ERR_INVALID_ARGUMENT` for an out-of-range channel).

dB values clamped to `[−∞, +12]` on entry. A value of `−∞` (`-std::numeric_limits<float>::infinity()`) maps to linear 0; very negative finite values (e.g. < −120 dB) also map to 0 to avoid denormals.

### 6.3 No new poll surface

The current `jbox_route_status_t` doesn't need new fields; the Swift side will track its own commanded values for UI, since the engine side is just storing what was last written and there's no engine-derived information to read back. (If we ever want to expose the *smoothed* current value for diagnostics, that's an additive v13.)

### 6.4 Header bump

`JBOX_ENGINE_ABI_VERSION` → 14; banner comment in `jbox_engine.h` adds the v12 / v13 / v14 history entries (the v12 / v13 error-code additions were committed without backfilling the comment block — fold them into the same diff so the history block stays current).

---

## 7. Swift wrapper + UI

### 7.1 `Route` model

`Sources/JboxEngineSwift/JboxEngine.swift` (or wherever `Route` lives) gains:

```swift
public struct Route {
    // ... existing fields ...
    public var masterGainDb: Float    // default 0.0
    public var trimDbs: [Float]       // .count == mapping.count, default [0.0, ...]
    public var muted: Bool            // default false
}
```

These are `var`, mirrored from `StoredRoute` on load and from user-driven setter calls during runtime. UI binds directly to them through `EngineStore` actions.

### 7.2 `EngineStore` setters

```swift
@MainActor
public extension EngineStore {
    func setMasterGainDb(routeId: UInt32, db: Float)
    func setChannelTrimDb(routeId: UInt32, channelIndex: Int, db: Float)
    func setRouteMuted(routeId: UInt32, muted: Bool)
}
```

Each updates the `Route` struct in `routes`, calls the matching C ABI function, and writes through `StateStore` to persist (debounced — see § 8).

### 7.3 `FaderTaper` module

```swift
public enum FaderTaper {
    /// pos in 0...1 → dB.
    public static func dbForPosition(_ pos: Float) -> Float
    /// dB → pos in 0...1.
    public static func positionFor(db: Float) -> Float
    /// dB → linear amplitude (10^(dB/20), with -∞ → 0).
    public static func amplitudeFor(db: Float) -> Float

    public static let maxDb: Float = 12.0
    public static let unityDb: Float = 0.0
    public static let minFiniteDb: Float = -60.0
    public static let unityPosition: Float = 0.75
    public static let muteThresholdPosition: Float = 0.04
}
```

Lives in `Sources/JboxEngineSwift/` so it's unit-testable without SwiftUI.

### 7.4 Widgets

**`FaderSlider`** — vertical slider in `Sources/JboxApp/`. Renders the track + cap inside a fixed-height frame, exposes `value: Binding<Float>` (in dB). Internally converts to/from position via `FaderTaper`. Provides `.onDoubleTap { value = 0 }` and keyboard arrow handling.

**`MasterFaderStrip`** — composes the master `FaderSlider`, the dB readout, and the `MUTE` button.

**`ChannelStripColumn`** — composes a `FaderSlider` (for the trim) and a `ChannelBar` (existing dest meter) inside the shared `.bar-zone` rectangle, with channel label / dB readout in the fixed bands above and below.

### 7.5 `MeterPanel` rewrite

`Sources/JboxApp/MeterBar.swift` `MeterPanel` is rewritten around the strip layout:

- **SOURCE group** (left) — compact pre-fader bars, kept from today.
- **Meter dB scale column** — single shared `DbScale` to the left of the channel-strip cluster, marked at the DAW scale of § 4.3 (`0, −3, −6, −12, −18, −24, −36, −48, −60` dBFS). One column for the whole row instead of one per side as today.
- **Channel strips** (middle) — `HStack` of `ChannelStripColumn` per `mapping[i]`. Inside each: trim fader (with `+12` / `−∞` fader-scale labels above/below the bar zone) and dest meter (reads off the shared dB scale column).
- **Master strip** (right) — `MasterFaderStrip`.

The current standalone DEST `BarGroup` is **removed** — the dest bars now live inside each channel strip beside their trim fader. The fader scale (`+12` / `0` / `−∞` dB) and the meter scale (`0` / `−6` / `−60` dBFS) are kept visually separate: the fader scale labels live inside each strip's top/bottom bands, the meter scale is the shared `DbScale` column. They share the bar-zone height but mean different things. `DiagnosticsBlock` stays unchanged.

### 7.6 `MeterLevel` update

`Sources/JboxEngineSwift/MeterLevel.swift` doesn't change shape; only the marks the UI draws change (in `DbScale`). New constant `MeterLevel.dawScaleMarks: [(dB: Float, label: String)]` exported for `DbScale` and any future scale renderer.

---

## 8. Persistence

`StoredRoute` (in `Sources/JboxEngineSwift/Persistence/StoredAppState.swift`) gains three optional fields:

```swift
public struct StoredRoute: Codable {
    // ... existing fields ...
    public var masterGainDb: Float?    // absent or nil → 0.0
    public var trimDbs: [Float]?       // absent or nil → [0.0, ...]
    public var muted: Bool?            // absent or nil → false
}
```

The optional-with-default pattern means existing `state.json` files without these keys load unchanged.

Writes are debounced through the existing `StateStore` mechanism (no per-keystroke disk write). On app quit, a final flush ensures the latest fader positions land.

Schema version bump: existing `StateStore` uses an additive-friendly approach today; no version-bump migration needed unless `StateStore` enforces strict schema (verify during implementation, file follow-up if so).

---

## 9. Testing

### 9.1 C++ (Catch2)

- `gain_smoother_test.cpp`
  - Step input: 95% settling within 30 ms ± 5 ms at 48 kHz, 96 kHz.
  - No overshoot.
  - Mute target = 0 reaches < `1e-6` linear within 50 ms.
  - Multiple consecutive `step()` calls after a target change produce a monotonic ramp.
- `route_manager_gain_test.cpp` (extends the existing route_manager test surface or new fixture)
  - Output samples scaled correctly under unity, +6 dB, −6 dB, mute.
  - Atomic target writes from the "control thread" are picked up on the next "RT block".

### 9.2 Swift (Swift Testing)

- `FaderTaperTests.swift`
  - `dbForPosition(0.75) == 0.0` (within ε).
  - `dbForPosition(1.0) == 12.0`.
  - `dbForPosition(0.04) == −∞`.
  - Round-trip `positionFor(dbForPosition(p)) ≈ p` for representative positions.
  - `amplitudeFor(db: 0) == 1.0`, `amplitudeFor(db: −6) ≈ 0.5012`, `amplitudeFor(db: −∞) == 0`.
- `RouteGainPersistenceTests.swift`
  - Round-trip a `StoredRoute` with the new fields.
  - Round-trip a `StoredRoute` *without* the new fields (legacy on-disk shape) and verify defaults.
- `EngineStoreGainTests.swift`
  - `setMasterGainDb` updates the model and forwards to the C ABI.
  - `setRouteMuted(true)` toggles `muted` and persists.

### 9.3 RT-safety scan

`gain_smoother.hpp` lives in `Sources/JboxEngineC/rt/` and is covered by the existing `scripts/rt_safety_scan.sh`.

### 9.4 UI

`#Preview` block for `MeterPanel` in 2/4/8/16 channel configurations to eyeball alignment. No XCUITest (consistent with existing `Sources/JboxApp/` tests-via-previews policy).

### 9.5 Integration / soak

Manual: a long-running route with master moved through a slow sweep ±12 dB; verify zero `underrun_count` increase and zero clicks audibly. (Existing soak-test harness from `docs/spec.md § 5` adopts the new fader sweep as one of its checks.)

---

## 10. Effort estimate

| Slice | Estimate |
|-------|----------|
| `gain_smoother.hpp` + tests | 0.5 d |
| RT path wiring in `route_manager.cpp` + tests | 0.5 d |
| ABI v14 (header, setters, control plumbing) | 0.5 d |
| `FaderTaper` + tests | 0.25 d |
| `Route` / `EngineStore` setters + persistence + tests | 0.5 d |
| `FaderSlider`, `MasterFaderStrip`, `ChannelStripColumn` | 0.75 d |
| `MeterPanel` rewrite + new `DbScale` marks + channel-name labels | 0.75 d |
| Manual soak + bug iteration | 0.5 d |
| **Total** | **~4.25 days** |

---

## 11. Open questions

None blocking. Items deferred but worth tracking:

- VCA groups — separate spec when there's a use case.
- Per-channel mute — covered by per-channel trim → −∞; revisit if users find it cumbersome.
- Whether `StateStore` needs a schema-version bump — verify during implementation, file as `R<n>` in `docs/refactoring-backlog.md` if so.

## 12. References

- Audio path: `Sources/JboxEngineC/control/route_manager.cpp` (`inputIOProcCallback`, `outputIOProcCallback`, `duplexIOProcCallback`).
- Existing meter primitives: `Sources/JboxEngineC/rt/atomic_meter.hpp`, `Sources/JboxEngineSwift/MeterLevel.swift`, `Sources/JboxEngineSwift/PeakHoldTracker.swift`.
- ABI: `Sources/JboxEngineC/include/jbox_engine.h`.
- Spec sections: `docs/spec.md § 2.8` (metering), `§ 2.10` (RT-safety), `§ 2.11` (ABI), `§ 3.1` (route config), `§ 4.5` (meters).
