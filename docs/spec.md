# Jbox — Technical Specification

**Status:** Draft v1 — design locked, ready for implementation.
**Target platform:** macOS 15 (Sequoia) and later.
**Scope:** this document is the authoritative technical specification for Jbox v1. It is a living document; as implementation decisions refine the design, updates are committed here.

---

## Table of Contents

1. [Section 1 — System Architecture](#section-1--system-architecture)
2. [Section 2 — Audio Engine Internals (C++)](#section-2--audio-engine-internals-c)
3. [Section 3 — Data Model and Persistence](#section-3--data-model-and-persistence)
4. [Section 4 — User Interface](#section-4--user-interface)
5. [Section 5 — Testing, Build, and Distribution](#section-5--testing-build-and-distribution)
6. [Appendix A — Deferred / Out-of-Scope](#appendix-a--deferred--out-of-scope)
7. [Appendix B — Glossary](#appendix-b--glossary)

---

## Preamble — Product Goals and Non-Goals

### What Jbox is

Jbox is a **native macOS audio routing utility** that takes selected channels from one Core Audio device and routes them — in real time, with minimal latency — to selected channels on another Core Audio device. The app supports an arbitrary number of simultaneous routes, handles devices with independent clocks through drift compensation, and recovers gracefully from device disconnect / reconnect events.

The motivating workflow is routing two output channels of a Roland V31 USB sound module into two virtual output channels of a Universal Audio Apollo so that UA Console can apply inserts and sends. But nothing in the design is specific to those devices — any Core Audio input-capable device can feed any Core Audio output-capable device.

### What Jbox is not

- **Not a mixer.** Jbox does 1:N channel mapping (fan-out allowed). No summing / fan-in, no per-channel gain, no mute.
- **Not a DAW.** No recording, no plugins, no timeline, no MIDI.
- **Not a virtual audio driver.** Jbox does not create devices that other apps see. It routes between existing Core Audio devices.
- **Not a broadcast router.** No network audio (Dante, AVB, NDI), no IP streaming.

### Core design principles

1. **Pro-audio routing semantics.** Channel numbering, device identity, and per-route lifecycle match how pro-audio practitioners think about routing. Channels are 1-indexed in the UI (0-indexed internally). Devices are identified by their Core Audio UID, which is stable across reboots and reconnects.
2. **Top-performance real-time engine.** The audio-processing path is written in C++ with strict real-time discipline: no allocations, no locks, no syscalls on the audio thread.
3. **UI is replaceable.** The engine is an independent library with a stable public C API. The entire v1 SwiftUI UI could be replaced by a CLI, a web UI, an AppKit UI, or anything else, without touching the engine.
4. **Do not step on other apps.** Jbox does not create aggregate devices, does not change device sample rates, and does not change device buffer sizes without explicit user opt-in. Other apps sharing the same hardware are unaffected by Jbox's presence.
5. **Personal use first.** v1 does not require a paid Apple Developer Program subscription, does not require anyone to use the Xcode IDE, and does not require Mac App Store distribution. Xcode.app must be installed for its frameworks (see § 5.2 for details), but development can happen entirely from the command line in any editor. The build produces an ad-hoc-signed `.app` that runs on the user's Mac, and optionally packages an unsigned `.dmg` for sharing with a small audience (with right-click → Open Gatekeeper instructions).

---

## Section 1 — System Architecture

### 1.1 Layers

The app is layered to isolate the real-time audio path from everything else. This isolation is a functional requirement: any lock, allocation, or syscall on the audio thread causes audible glitches.

1. **UI layer (Swift + SwiftUI).** Main window, menu bar extra, route list, scene picker, per-channel meters, preferences. Pure presentation and user input. Reads engine state through a Swift-exposed publisher; writes user intents as commands.
2. **Application layer (Swift).** Persistent state (routes, scenes, preferences stored as JSON on disk). Device enumeration cached with Core Audio notifications driving refresh. Route lifecycle orchestrator running the state machine `stopped → waiting → starting → running → error`. Owns the engine instance.
3. **Bridge layer (C API).** A small, stable, C-level interface between Swift and C++. Functions such as `jbox_engine_create`, `jbox_engine_start_route`, `jbox_engine_poll_status`, `jbox_engine_poll_meters`. No Swift or Objective-C types cross this boundary. This layer is the **public contract of the product**; the UI, the engine CLI, and any future alternative UI all target this API.
4. **Real-time audio engine (C++).** All performance-critical work. Registers HAL IOProcs per device. Owns per-route ring buffers, `AudioConverter` instances for resampling, drift trackers, and lock-free queues for control and metering. No STL containers that allocate in the callback, no `std::mutex`, no `new` / `delete` in the RT path.
5. **macOS platform.** Core Audio HAL, LaunchServices (for opt-in launch-at-login), file system for persistence, SwiftUI framework.

### 1.2 Thread topology

- **Main thread** — SwiftUI event loop. Never blocks. Reads atomics. Writes commands to an SPSC command queue.
- **Core Audio IOProc threads** — one per active device, managed by the OS (we don't create or own them). Every active route has an input IOProc on the source device and an output IOProc on the destination device. These are the real-time threads. They only: read input buffers, copy to ring buffers, read from ring buffers, resample, write to output buffers, update atomic meter values.
- **Engine control thread (background `DispatchQueue`)** — non-RT engine work: adding/removing routes (all allocations happen here, never on the RT path), device enumeration refresh, drift-correction setpoint updates that don't need sample-accuracy, persistence I/O.
- **No direct communication** between main thread and RT threads. All cross-thread communication goes through lock-free atomics or single-producer / single-consumer ring buffers.

### 1.3 Data flow during a running route

```
source device IOProc (RT thread)
  ├─ reads input buffer from Core Audio
  ├─ for each channel in the route's source-channel list:
  │    writes sample into the route's ring buffer
  └─ updates source-level atomic meter value

                  (ring buffer — lock-free SPSC,
                   sized for ~5–10 ms of audio;
                   fill level drives drift tracker)

destination device IOProc (RT thread)
  ├─ reads from the route's ring buffer
  ├─ feeds through AudioConverter (variable-ratio resampler)
  │    ratio = nominal * drift-tracker-adjustment
  ├─ writes output samples to the selected destination channels
  └─ updates destination-level atomic meter value
```

### 1.4 Trust boundaries — what code runs where

- **Inside the RT engine:** only pre-allocated memory, atomics, lock-free queues, Core Audio calls documented as RT-safe.
- **Outside the RT engine:** allocations, file I/O, persistence, JSON parsing / emission, SwiftUI updates, error dialog presentation, logging.
- **Testing implication:** the engine is a headless C++ library with its own unit tests and a deterministic simulation harness. The UI cannot ship-block the engine, and vice versa.

### 1.5 Platform and entitlement decisions

- **Minimum macOS version:** 15 (Sequoia). No backward-compatibility shims for older versions.
- **App Sandbox:** **off.** Sandbox restrictions conflict with HAL-level device control. This is the standard choice for pro-audio utilities (Loopback, Audio Hijack, SoundSource, etc. are all unsandboxed).
- **Hardened Runtime:** **on.** Compatible with all app behavior; required for notarization if that ever becomes relevant.
- **Entitlement `com.apple.security.device.audio-input`:** **on.** Required for Core Audio device access — macOS treats any input-capable device as "microphone-class."
- **`NSMicrophoneUsageDescription` key in Info.plist:** required. Populated with a clear user-facing explanation: "Jbox needs access to your audio devices to route audio between them."
- **No Mac App Store distribution ever.** Sandboxing would force architectural compromises incompatible with the near-zero-latency goal.

### 1.6 Versioning of the bridge API

- The C bridge is treated as the product's stable public API.
- Semantic versioning: `MAJOR.MINOR.PATCH`.
  - Additions (new functions, new enum values, new fields appended to structs) are `MINOR` bumps.
  - Breaking changes (removed functions, renamed symbols, reordered struct fields, behavior changes) are `MAJOR` bumps.
- The bridge header `jbox_engine.h` defines a `JBOX_ENGINE_ABI_VERSION` macro; clients may check it at compile time and at runtime via `jbox_engine_abi_version()`.

---

## Section 2 — Audio Engine Internals (C++)

This is the most performance-critical section of the system. Design choices here determine whether Jbox meets its latency and reliability goals.

### 2.1 Why per-device HAL IOProcs (not aggregate devices)

macOS offers two broad approaches to cross-device routing:

- **Aggregate Device approach:** create a composite device that contains both source and destination, rely on macOS's built-in drift correction. Used by pro DAWs (Logic, Cubase, Pro Tools, Ableton Live) when they need multi-device support — but DAWs only need to see "one device" once audio enters their internal graph.
- **Per-device HAL IOProc approach:** register an IOProc on each device, bridge them with lock-free ring buffers, implement drift correction ourselves. Used by pro routing tools (Loopback, Audio Hijack, Dante Via).

Jbox uses the **per-device HAL IOProc approach** because Jbox is a routing tool, not a DAW. Specifically:

- Aggregate devices are a **global system resource**; creating or modifying one disrupts other apps using those devices (e.g., Logic or UA Console would lose connection mid-session).
- Aggregate devices have a single primary clock; all drift correction is relative to it, limiting quality for many-to-many routing.
- Dynamic routing (adding/removing routes mid-session) is awkward with aggregates; natural with per-device IOProcs.
- Per-device IOProcs are the **industry standard for routing tools** specifically because of these reasons.

### 2.2 Device lifecycle

For each physical audio device referenced by any route (running or configured), the engine owns a `DeviceHandle`:

```
DeviceHandle {
  audioDeviceID: AudioDeviceID         // from Core Audio; not persistent
  uid: std::string                     // stable Core Audio UID (persistent identity)
  inputIOProcID: AudioDeviceIOProcID   // nullable; set when any route uses as source
  outputIOProcID: AudioDeviceIOProcID  // nullable; set when any route uses as destination
  nominalSampleRate: double            // observed; never set by us
  bufferFrameSize: UInt32              // observed; never set by us unless user overrode
  inputChannelCount: UInt32
  outputChannelCount: UInt32
  activeInputRoutes: std::atomic<RouteList*>   // RCU-style pointer
  activeOutputRoutes: std::atomic<RouteList*>  // RCU-style pointer
  state: DeviceState                   // disconnected | present | running
}
```

**Lazy IOProc registration.** When the first route using a device as a source transitions to `running`, the engine calls `AudioDeviceCreateIOProcIDWithBlock` to register an input IOProc on that device and `AudioDeviceStart` to start the device. When the last route using that device as a source stops, the engine calls `AudioDeviceStop` followed by `AudioDeviceDestroyIOProcID`. Same for output IOProcs. This minimizes system load when many routes are configured but stopped.

**Device enumeration.** On engine creation, and again whenever Core Audio posts a `kAudioHardwarePropertyDevices` property change, the engine enumerates all present devices, compares to the set of known devices, and updates the UID-keyed device registry. Devices that disappear are marked `disconnected` but their entries remain (so routes referencing them can transition to `waiting`). Devices that appear may unblock routes sitting in `waiting`.

### 2.3 Route lifecycle

A route's engine-side state:

```
Route {
  id: UUID
  sourceDevice: DeviceHandle*
  destDevice: DeviceHandle*
  mapping: std::vector<ChannelEdge>  // immutable after route start
  ringBuffer: RingBuffer*            // pre-allocated at start
  audioConverter: AudioConverterRef  // variable-ratio resampler
  driftTracker: DriftTracker         // PI controller state
  meters: MeterArray                 // atomic<float> per channel
  state: RouteState                  // stopped | waiting | starting | running | error
}
```

**Start sequence** (all on the control thread — never the RT thread):

1. Resolve source and destination `DeviceHandle`s by UID. If either missing, transition the route to `waiting`.
2. Allocate the ring buffer, using one of three sizing presets selected by `RouteConfig.latencyMode` (Phase 6 refinement #6; ABI v5+):
   - **Off (default, `mode = 0`).** `max( max(source_buffer, dest_buffer) × 8 , 4096 )` frames per channel, drift setpoint `ring/2`. At 48 kHz with 64-sample device buffers the floor dominates — 4096 frames = ~85 ms of headroom. Sized to absorb USB-class source-device delivery jitter (buffers can arrive in bursts with multi-ms gaps), which is the dominant source of underrun on real hardware. The original sizing was `max_buffer × 4` with a 256-frame floor (~5 ms); that was tuned against the synchronous simulated backend and produced sustained underruns on a real Roland V31 → Apollo route during Phase 6 testing.
   - **Low latency (`mode = 1`).** `max( max(source_buffer, dest_buffer) × 3 , 512 )` frames per channel, drift setpoint `ring/2`. At 48 kHz with 64-sample buffers this gives ~10.6 ms of headroom and a ~5 ms residency, cutting the pill in § 2.12 by ~37 ms relative to the Off preset. **Risk:** bursty USB sources may underrun — the UI copy warns about this.
   - **Performance (`mode = 2`).** `max( max(source_buffer, dest_buffer) × 2 , 256 )` frames per channel, drift setpoint `ring/4`. At 48 kHz with 64-sample buffers the ring shrinks to ~5.3 ms and the steady-state residency drops to ~1.3 ms — another ~4 ms off the pill relative to Low. Intended for drum / live monitoring rigs where the user is willing to trade symmetric underrun margin for audible responsiveness. **High underrun risk** on bursty USB sources; the UI copy is explicit about this. The asymmetric setpoint (`ring/4`) means a single below-target source burst can drain the ring to zero; the PI controller will re-prime but the user will hear a click.
3. Construct the `AudioConverter` with source rate → destination rate and `kAudioConverterSampleRateConverterComplexity_Mastering`.
4. If the source device has no input IOProc yet (first route to use it as source), register one and start the device.
5. Same for destination device and output IOProc.
6. Atomically append the route to each device's active-route list (RCU pointer swap).
7. Transition the route to `running`.

**Stop sequence:**

1. Atomically remove the route from each device's active-route list.
2. Wait one buffer period (`usleep(bufferFrameSize / sampleRate × 1.5e6)`) to ensure any in-flight IOProc callback has finished using the route's resources (lightweight RCU grace period).
3. Release the route's ring buffer and AudioConverter.
4. If the source device now has zero active input routes, unregister and stop the input IOProc.
5. Same for destination device.

**All allocations happen before the route goes `running`.** The RT threads never allocate, never call `new`, never touch the heap.

### 2.4 Ring buffer

- **Lock-free SPSC** — single producer (source IOProc), single consumer (destination IOProc).
- **Fixed-size** at route start; pre-allocated; no runtime resize.
- **Interleaved samples** for cache locality. Format: `float` (32-bit), same sample format as Core Audio IOProcs use after normalization.
- **`std::atomic` head and tail indices** with `memory_order_acquire` on reads of the peer's index and `memory_order_release` on writes of our own index. Relaxed ordering for updates of our own side when the peer is not reading — Michael-Scott style.
- Underrun (reader finds buffer empty) and overrun (writer finds buffer full) are logged via the RT-safe log queue, not thrown as exceptions. Recovery: emit silence for underruns (destination), drop oldest samples for overruns (source). Both paths should be rare once drift correction stabilizes.

### 2.5 Resampling with `AudioConverter`

Apple's `AudioConverter` is production-grade, supports variable sample-rate ratio updates, lives in-process, has no license concerns.

**Configuration:**

- `kAudioConverterSampleRateConverterComplexity` = `kAudioConverterSampleRateConverterComplexity_Mastering` (highest quality).
- `kAudioConverterSampleRateConverterQuality` = `kAudioConverterQuality_Max`.
- Input ASBD: source device's sample rate, float interleaved, channel count = route's source-channel count.
- Output ASBD: destination device's sample rate, float interleaved, channel count = `mapping.count` — one converter output slot per edge. Fan-out edges (multiple edges sharing a `src` channel) each get their own slot; the pre-ring scratch copy duplicates the source sample into every such slot.

**Variable-ratio updates:**

The drift tracker computes a small adjustment (typically within ±100 ppm). The control thread writes the proposed new rate into an atomic; the RT output IOProc reads it and — if it crosses the deadband (see below) — applies it via `AudioConverterSetProperty(kAudioConverterCurrentInputStreamDescription, …)` with a fresh input ASBD. (The original spec named `kAudioConverterSampleRate`; that property is not consistently writable on our float-interleaved SRC configuration. The full-ASBD re-set is documented by Apple to propagate as a variable-ratio SRC update. See the Phase 4 deviations in `docs/plan.md`.)

**Apply deadband.** Every `setInputRate` call flushes the converter's polyphase filter state — benign in simulation with synthetic ramps, but on real hardware with dynamic content the flush manifests as audible click artifacts on transients, and the converter consumes ~16 extra input frames per flush while re-priming. At the 100 Hz PI sampling rate, that was ~1600 frames/s draining from the ring with no corresponding output — directly observable as a slow-climbing underrun counter on a V31 → Apollo route. The RT path therefore gates apply decisions through a pure `shouldApplyRate(proposed, last_applied, nominal)` helper (`Sources/JboxEngineC/control/rate_deadband.hpp`): `setInputRate` fires only when the proposed rate differs from the last-applied value by more than 1 ppm of nominal (48 mHz at 48 k — about an order of magnitude below the audible rate-error threshold). Sub-ppm PI noise is silently discarded; real drift corrections accumulate through the integrator until they cross the deadband and get applied. Characterization test: `audio_converter_wrapper_test.cpp` tag `[hypothesis]` pins both the flush-cost signature and the deadband's mitigation.

Even when source and destination sample rates are nominally equal, we run through the converter. At ratio 1.0 it's nearly free, and it's our only hook for drift correction.

### 2.6 Drift correction algorithm

**The problem:** source and destination devices have independent crystal oscillators. Their actual rates deviate from nominal by ±25–100 ppm typically. Over time, the producer generates more or fewer samples than the consumer consumes, and the ring buffer drifts toward overrun or underrun.

**The algorithm — PI control on ring buffer fill level.**

Let:
- `target_fill = capacity / 2` — midpoint of the ring buffer.
- `error(t) = fill_level(t) - target_fill`.
- `integral(t) = integral(t - Δt) + error(t) · Δt`.
- `adjustment(t) = Kp · error(t) + Ki · integral(t)`, clamped to `[-100 ppm, +100 ppm]`.

The control thread samples `fill_level` at ~100 Hz (nothing time-critical about this — it's not on the RT path). Each sample updates the PI state and writes a new proposed rate; the RT output IOProc applies it to `AudioConverter` only when it crosses the § 2.5 deadband.

Empirical starting values: `Kp ≈ 1e-6 per-frame`, `Ki ≈ 1e-8 per-frame-second`. These were chosen conservatively to avoid oscillation on real hardware; real-hardware gain tuning is a deferred Phase 4 exit item (see `docs/plan.md` Phase 4 deviations). They are several orders of magnitude too weak to hold ring fill bounded by themselves; in the simulated-backend integration tests, ring fill was historically bounded by the converter's per-tick flush side-effect rather than by PI authority. With the § 2.5 deadband gating that flush storm, the integration-test excursion bands widen to match open-loop drift accumulation (~750 frames at +50 ppm over 310 s) until real-hardware gain tuning lands.

**Why it works:** the output is sample-accurate and click-free because `AudioConverter` interpolates continuously — as long as we don't force it to flush its filter state on every tick (see § 2.5 deadband). The PI control is slow (response time on the order of seconds), so ordinary fill-level jitter (from context switches, GC-like events) doesn't trigger reactive adjustments.

**Safety:**
- Clamp adjustments to ±100 ppm so a pathological device (reporting clock rates drastically off nominal) can't destabilize the controller.
- If `fill_level` hits the top or bottom of the buffer (indicating we fell behind despite correction), log it and continue — the next correction cycle will pull back toward center.

### 2.7 Per-device coordination when routes share a device

Example: three routes all use V31 as their source. They share one input IOProc on V31.

**Dispatch pattern inside the IOProc:**

```c
void input_ioproc(AudioDeviceID dev, const AudioBufferList* input, ...) {
  RouteList* routes = atomic_load_acquire(&device->activeInputRoutes);
  for (Route* r : *routes) {
    copy_selected_channels(input, r->mapping, r->ringBuffer);
    r->meters.update_from(input, r->mapping);
  }
}
```

The `activeInputRoutes` pointer is swapped atomically (RCU-style) when a route is added or removed. The RT thread never sees a partially-updated list. The old list is freed on the control thread after a grace period.

**Why not lock?** A lock on the RT thread would violate our real-time discipline. Atomic pointer swap with deferred free is the standard RT-safe pattern for "sometimes-mutating list."

**Direct-monitor fast path (same-device routes).** When `source_uid == dest_uid` (typically an aggregate device created in Audio MIDI Setup that wraps two physical interfaces) and the route is in Performance latency mode, the engine takes a dedicated fast path that bypasses the ring buffer, the `AudioConverter`, and even the `DeviceIOMux`. A single duplex `AudioDeviceIOProc` registration receives both input and output `AudioBufferList`s in the same RT callback, copies the mapped source channels directly into the mapped destination channels, and returns. Latency collapses to the HAL round-trip plus one buffer period — sub-5 ms on low-buffer Thunderbolt rigs, drum-monitoring-grade.
Exclusivity: the duplex registration fails if any other IOProc (input, output, or another duplex) is already open on the same device, and the attach path refuses to start if a mux exists for the UID. Performance-mode same-device routes own the device while they run. Cross-device Performance routes (different UIDs) continue to use the mux + ring + SRC path at the tighter sizing described above.
Buffer-size handshake and exclusive ownership: the fast path attempts `IDeviceBackend::claimExclusive` before any buffer changes. That wraps Core Audio's `kAudioDevicePropertyHogMode`: setting it to our PID disconnects every other client of the device for the duration and makes the buffer request authoritative. For an **aggregate device** the claim fans out — each active sub-device (returned by `kAudioAggregateDevicePropertyActiveSubDeviceList`) is hogged independently, because the effective HAL buffer size lives on the members, not the aggregate. At claim time the backend also snapshots each device's pre-claim buffer frame size (aggregate + every sub) and keeps the snapshots keyed by the claimed UID. Claim failure is non-fatal — the route continues on the shared path and accepts whatever buffer size the HAL ends up giving.

`requestBufferFrameSize(uid, 64)` then pushes the target into every sub-device and the aggregate itself, because setting it only on the aggregate is ignored when a member is being held larger by another client. The fast path re-reads the post-change value for the aggregate and folds it into `LatencyComponents::src_buffer_frames` so the pill shows what the HAL actually honored.

The target frame count is the user's override from `RouteConfig.bufferFrames` when supplied (ABI v6+), otherwise the fast-path default of 64 frames. Clients populate the user-visible picker from `jbox_engine_supported_buffer_frame_size_range` — for aggregate devices that returns the narrowest range every active sub-device accepts, so every value offered is uniformly honourable. On stop `releaseExclusive` restores each device to its own snapshotted buffer size (the aggregate and each sub-device independently — critical so a sub that started at, say, 256 doesn't get clobbered with the aggregate's 512 on restore), then releases hog mode on each. Ordering is restore-then-release: while we still own the device no other client is contending, and external apps that reconnect after we release re-read the restored sizes. The UI copy for the Performance tier documents the exclusive-ownership behavior.

**Latency across co-sourced routes.** When two routes share a source but target different destinations, each destination has its own HAL latency (`kAudioDevicePropertyLatency` + `kAudioDevicePropertySafetyOffset`) and its own drift-correction PI loop locking to its own clock. The two outputs therefore carry a fixed delay difference equal to the difference of the two pills (§ 2.12) — small on similarly-buffered destinations, large when one destination is HDMI / AirPlay and the other is a low-latency USB interface. This is audible as a hollow/chorus effect at small offsets and as a clearly-separated digital-delay effect at larger ones. It is not a bug; it is the unavoidable consequence of two independent destination clocks. Users who need phase coherence across destinations should route through an aggregate device; users who only need the relative delay below the chorus threshold should flip `lowLatency` on for both routes to shrink each contribution.

**Mux-path buffer-frame-size negotiation (cross-device routes).** `DeviceIOMux` attaches per-route `requested_buffer_frames`: 0 means "no opinion", non-zero enters the route's request into the mux's refcount. On each attach / detach the mux recomputes the min across currently-attached non-zero requests (spans both input and output directions because `kAudioDevicePropertyBufferFrameSize` is a global device property). On the 0 → 1 transition the mux claims `IDeviceBackend::claimExclusive` on the device so the request actually lands even when another app is holding the device at a larger size; the backend's claim snapshots the pre-claim buffer size internally. On the 1 → 0 transition the mux calls `releaseExclusive`, which restores the snapshot and releases hog mode. When a new route attaches with a smaller target than the current min, the mux re-issues the shrink through the backend. Routes passing 0 (Off tier) neither bump nor affect the refcount; they coexist with an already-shrunk buffer but never themselves trigger a change. Claim failure is non-fatal — the route continues on the shared-client path, accepting whatever buffer size the HAL ends up giving.

After both the source-side and destination-side mux attaches complete, `RouteManager::attemptStart` re-reads `currentBufferFrameSize` for each UID and folds the post-attach values into `LatencyComponents::src_buffer_frames` / `dst_buffer_frames` — the pre-attach values captured at enumeration time are stale, and a HAL-rejected shrink would otherwise look indistinguishable from a successful one. Each side is refreshed independently so an asymmetric outcome (e.g. source honoured at the target, destination clamped back up by another client) surfaces honestly in the pill.

Note: the HAL buffer-frame-size property is visible to every process. Other applications using the same device observe the buffer change for the duration of the route's lifetime; when the route stops and the mux releases exclusive ownership, those apps reconnect and see the restored size. This is documented in the UI copy for the Performance tier.

### 2.8 Metering

- Each route has a `MeterArray` — a small contiguous `std::atomic<float>` buffer sized to `max(sourceChannels.size(), destChannels.size())`.
- RT thread updates peak-over-block values every IOProc callback.
- UI polls the atomics at ~30 Hz via the bridge. Each poll does atomic loads and resets the peaks to zero (single-writer/single-reader atomic read-reset using `exchange`).
- Gray / green / yellow / red thresholds are UI-layer concerns; the engine only exposes linear peak values.

### 2.9 RT-safe logging

- RT code must not call any logging function that allocates or syscalls.
- A lock-free SPSC "log event" queue of pre-allocated fixed-size records (numeric event code, timestamp, small numeric payload).
- A background drainer reads from the queue every ~100 ms and writes to `os_log` and a rotating file in `~/Library/Logs/Jbox/`.
- Log events are sparse — only significant events (underrun, overrun, unexpected device state, drift tracker out of bounds). No per-block telemetry.

### 2.10 RT-safety discipline (enforced by convention and code review)

Rules for any code reachable from an IOProc:

- **No** heap allocations. No `new`, `malloc`, `std::vector::push_back`, `std::string` construction, `std::shared_ptr` construction.
- **No** locks. Lock-free atomics only.
- **No** syscalls. No `pthread_mutex_lock`, `dispatch_async`, `os_log_*` (except the narrow RT-safe subset), `printf`, file I/O.
- **No** Objective-C or Swift calls. No ARC-managed types.
- **Bounded** execution time. No unbounded loops.
- **No** exceptions. RT code compiled with `-fno-exceptions`.

**Enforcement:**

- **Directory discipline.** RT code lives in `Sources/JboxEngineC/rt/`. Non-RT engine code lives in `Sources/JboxEngineC/control/`. Different compile flags.
- **Static scanner** (`scripts/rt_safety_scan.sh`) runs in CI and as a recommended pre-commit hook. It greps `Sources/JboxEngineC/rt/` for banned symbols (`new`, `malloc`, `free`, `std::mutex`, `dispatch_async`, `pthread_mutex_lock`, etc.) and fails the build if any are found.
- **Code review.** Any change to `rt/` is reviewed specifically for RT-safety. A short RT-safety checklist lives in `docs/contributing.md` (to be added before accepting outside contributions).
- **Runtime checks in debug builds.** `-fsanitize=thread` (ThreadSanitizer) enabled for engine integration tests catches any lock or data race on RT threads.

### 2.11 Engine public C API (bridge)

The header `jbox_engine.h` exposes the engine to Swift (and to any future alternative client). Representative signatures:

```c
// Opaque handle
typedef struct jbox_engine jbox_engine_t;

// Lifecycle
jbox_engine_t* jbox_engine_create(const jbox_engine_config_t* config, jbox_error_t* err);
void           jbox_engine_destroy(jbox_engine_t* engine);

// Device enumeration (returns a snapshot, caller frees)
jbox_device_list_t* jbox_engine_enumerate_devices(jbox_engine_t* engine);
void                jbox_device_list_free(jbox_device_list_t* list);

// Route management — all async; status polled separately
jbox_route_id_t jbox_engine_add_route(jbox_engine_t* engine, const jbox_route_config_t* cfg, jbox_error_t* err);
void            jbox_engine_remove_route(jbox_engine_t* engine, jbox_route_id_t route_id);
void            jbox_engine_start_route(jbox_engine_t* engine, jbox_route_id_t route_id);
void            jbox_engine_stop_route(jbox_engine_t* engine, jbox_route_id_t route_id);

// Polling interfaces (called ~30 Hz from UI)
jbox_route_status_t jbox_engine_poll_route_status(jbox_engine_t* engine, jbox_route_id_t route_id);
size_t              jbox_engine_poll_meters(jbox_engine_t* engine, jbox_route_id_t route_id,
                                            float* out_peaks, size_t max_channels);

// ABI version
uint32_t jbox_engine_abi_version(void);
```

All functions are safe to call from any non-RT thread. Return codes / `jbox_error_t` communicate failures; the bridge never throws.

### 2.12 Estimated per-route latency

A route's end-to-end latency is the sum of independently reported components, all available without running the audio:

```
total_frames = src_device_latency + src_safety_offset
             + src_buffer_size              // one input IOProc tick, at src rate
             + ring_target_fill             // ring_capacity_frames / 2
             + converter_prime_input_frames // AudioConverter primeInfo.leadingFrames
             + dst_buffer_size              // one output IOProc tick, at dst rate
             + dst_safety_offset + dst_device_latency
```

Device-side contributions come from `kAudioDevicePropertyLatency` + `kAudioDevicePropertySafetyOffset` (+ `kAudioStreamPropertyLatency` if the device exposes per-stream values). Buffer sizes come from the backend's `buffer_frame_size`. Ring target fill is `ring->usableCapacityFrames() / 2` (matching the drift-sampler setpoint). Converter prime frames come from `AudioConverterGetProperty(kAudioConverterPrimeInfo, …)` — a small handful at mastering quality.

Routes that bridge different device rates compose the sum at each side's native rate and add the two halves in time:

```
src_side_us = (src_device_latency + src_safety_offset + src_buffer_size + ring_target_fill)
            * 1e6 / src_sample_rate
dst_side_us = (converter_prime_input_frames + dst_buffer_size + dst_safety_offset + dst_device_latency)
            * 1e6 / dst_sample_rate
estimated_latency_us = src_side_us + dst_side_us
```

For same-rate routes (the common case) this degenerates to the frame-sum formula above divided by the shared rate.

The engine computes this once at `startRoute` (no RT-thread cost) and surfaces it through the route status snapshot as `jbox_route_status_t::estimated_latency_us` (ABI v2+). Clients that need the per-component breakdown (Advanced / diagnostics UI) call `jbox_engine_poll_route_latency_components` (ABI v4+) which returns the frame counts + sample rates for each contributor alongside the same `total_us`. The UI shows an "~NN ms" estimate on the route row; an expanded breakdown is available in the diagnostics view when the user enables Advanced → "Show engine diagnostics". The number is **indicative, not ground truth** — some drivers (notably USB class-compliant) under-report hardware latency. A loopback-based measurement for authoritative verification is a Phase 9 deliverable.

### 2.13 Deferred to future versions

- **Fan-in / summing mapping** (multiple sources → one destination). Requires mixer-domain decisions (summing attenuation, clipping handling, per-source gain); explicitly out of scope.
- **Per-route gain and mute.** Would require a small DSP block in the engine; deferred until the user sees a concrete need.
- **Internal CPU budget telemetry** (percentage of audio cycle used per callback). Nice to have; not required for v1.
- **Alternative resampler backends** (libsamplerate, SoXr). Apple `AudioConverter` is sufficient; revisit if quality or performance ever disappoints.

---

## Section 3 — Data Model and Persistence

### 3.1 Entities

All entities are defined in the Swift application layer, encoded to / decoded from JSON via `Codable`. The C++ engine does not consume JSON directly; the Swift layer translates configured routes into engine-API calls.

#### 3.1.1 `DeviceReference`

A persistent reference to a device by its Core Audio UID.

```swift
struct DeviceReference: Codable, Equatable {
  let uid: String          // kAudioDevicePropertyDeviceUID; stable across reboots and reconnects
  let lastKnownName: String  // cached human name, e.g. "Roland V31", for UI display when device absent
}
```

- Core Audio's `AudioDeviceID` is **not used** for persistence; it is not stable.
- `lastKnownName` is updated whenever the device is resolved against a current enumeration; used for UI when the device is disconnected.

#### 3.1.2 `ChannelEdge`

A single wire in a route's mapping.

```swift
struct ChannelEdge: Codable, Equatable, Hashable {
  let src: Int   // 0-indexed source channel
  let dst: Int   // 0-indexed destination channel
}
```

0-indexed internally; the UI displays as 1-indexed.

#### 3.1.3 `Route`

The central entity.

```swift
struct Route: Codable, Identifiable, Equatable {
  let id: UUID
  var name: String             // user-visible label
  var isAutoName: Bool         // true → regenerate name on mapping changes; false → user edited
  var sourceDevice: DeviceReference
  var destDevice: DeviceReference
  var mapping: [ChannelEdge]
  let createdAt: Date
  var modifiedAt: Date
}
```

**v1 invariants** on `mapping`:
- Each `dst` appears at most once. (Writing two edges into the same destination channel would be summing / fan-in — deferred per Appendix A.)
- A `src` may appear on multiple edges — 1:N fan-out is allowed. Each such edge gets its own converter output slot; the scratch copy duplicates the sample into every slot.
- Non-empty.
- `mapping.count` is the route's "width" in channels (= converter output channel count).

Runtime state (`stopped` / `waiting` / `running` / `error`) is **not** persisted; it is re-derived at runtime.

#### 3.1.4 `Scene`

A named preset: a group of routes to activate together.

```swift
struct Scene: Codable, Identifiable, Equatable {
  let id: UUID
  var name: String
  var routeIds: [UUID]
  var activationMode: ActivationMode   // .exclusive (default) | .additive
}

enum ActivationMode: String, Codable {
  case exclusive  // activating this scene stops all other routes first
  case additive   // activating this scene starts routeIds without stopping others
}
```

#### 3.1.5 `Preferences`

```swift
struct Preferences: Codable, Equatable {
  var launchAtLogin: Bool                      // default: false
  var bufferSizePolicy: BufferSizePolicy        // default: .useDeviceSetting
  var resamplerQuality: ResamplerQuality        // default: .mastering
  var appearance: AppearanceMode                // default: .system
  var showMetersInMenuBar: Bool                 // default: false
}

enum BufferSizePolicy: Codable, Equatable {
  case useDeviceSetting
  case explicitOverride(frames: Int)
}

enum ResamplerQuality: String, Codable { case mastering, highQuality }
enum AppearanceMode: String, Codable   { case system, light, dark }
```

#### 3.1.6 `AppState`

The root document persisted to disk.

```swift
struct AppState: Codable, Equatable {
  var schemaVersion: Int             // current: 1
  var routes: [Route]
  var scenes: [Scene]
  var preferences: Preferences
  var lastQuittedAt: Date?
}
```

### 3.2 Persistence

**Location.** `~/Library/Application Support/Jbox/state.json`. The directory is created on first launch with mode 0755.

**Format.** JSON via `Codable`. Pretty-printed (2-space indent) so diffs are readable when the user backs up or commits the file.

**Write strategy.**
- Save triggered by any change that mutates persisted state (route add / edit / delete, scene change, preferences change).
- **Debounced at 500 ms.** A burst of edits is coalesced into a single write.
- **Atomic write**: write to `state.json.tmp` in the same directory, `fsync`, rename over `state.json`. Prevents partial-write corruption on crash or power loss.
- Persistence I/O runs on the application-layer's persistence queue; never on main or RT threads.

**Backup.** The previous `state.json` is renamed to `state.json.bak` before each successful write. One-generation backup — enough insurance against bad edits, not a full history.

**Schema migration.**
- `schemaVersion` starts at `1` for initial releases.
- On load, if the read `schemaVersion` is less than the current constant in the app, run a ladder of migration functions: `migrate_v1_to_v2`, `migrate_v2_to_v3`, etc. Each migration is a pure function from the older struct to the newer.
- If `schemaVersion` is **greater** than the app knows (user downgraded), refuse to load. Present an error dialog. Do not silently rewrite the file.

### 3.3 Device matching on launch

When the app launches:

1. Load `AppState` from disk, running migrations if needed.
2. Enumerate current Core Audio devices → build a map from UID to live `AudioDeviceID`.
3. For each route, look up `sourceDevice.uid` and `destDevice.uid`:
   - **Both present** → route is eligible to start. UI shows "idle."
   - **One or both missing** → route is shown with a "device disconnected" indicator. User can click Start anyway, which places the route in `waiting-for-device`.
4. For each present device, update the corresponding `DeviceReference.lastKnownName` from the current Core Audio name (handles user renaming a device in Audio MIDI Setup).

### 3.4 Data explicitly not persisted

- Runtime route state (`stopped` / `running` / `error` / `waiting`) — transient.
- Meter values — transient.
- Device enumeration cache — re-queried on every launch.
- Window position and size — handled by AppKit `NSWindow` autosave using a frame name.
- Log files — separate concern, stored in `~/Library/Logs/Jbox/`, rotated by the logger.

### 3.5 Deferred to future versions

- **iCloud / network sync of `state.json`.** The JSON format is already suitable; a future sync layer can be added without changing the on-disk schema.
- **Multiple configuration profiles** beyond scenes (e.g., separate "Home Studio" vs. "Mobile Rig" full-state documents). Scenes cover the common cases for v1.
- **Schema version downgrade support.** v1 refuses to load future-schema files; a full compatibility dance is deferred.

---

## Section 4 — User Interface

> **Status notice.** This section specifies the v1 SwiftUI UI as a *reference implementation* of the bridge API. The bridge API (not this UI) is the product's public contract. The entire UI may be rewritten or replaced at any time without touching the engine; any such replacement must only re-implement against the same bridge API.

### 4.1 Main window

Two-pane layout — the standard macOS utility pattern, using `NavigationSplitView`.

```
┌────────────────────────────────────────────────────────────────┐
│ [≡] Jbox                                              [ ⚙ ]    │
├──────────────┬─────────────────────────────────────────────────┤
│  SIDEBAR     │  ROUTE LIST                                      │
│              │                                                  │
│  All Routes  │  ● Keys to Console                               │
│  ─────────   │    V31 ch 1,2 → Apollo Virt 1,2     [ ▶ ] [⋯]   │
│  SCENES      │    ▬▬▬▬▬▬▬▬   ▬▬▬▬▬▬                             │
│  · Practice  │                                                  │
│  · Recording │  ○ Mic to Monitors                                │
│  · Jam       │    Interface ch 5 → Interface 1,2   [ ▶ ] [⋯]   │
│  [+ Scene]   │                                                  │
│              │  ! Backup Send (device disconnected)             │
│              │    V31 ch 3,4 → Scarlett 1,2        [⏸ waiting] │
│              │                                  [+ Add Route]  │
└──────────────┴─────────────────────────────────────────────────┘
```

- **Sidebar:** "All Routes" item + list of scenes. `[+]` button to create a scene.
- **Route list:** rows show name, source → destination summary, per-channel meters, start/stop button, `[⋯]` menu for edit / delete / duplicate.
- **Status glyph** per row:
  - `●` (filled circle) — running
  - `○` (open circle) — stopped
  - `⏸` — waiting for device
  - `!` — error
- **[+ Add Route]** floating-style button in the route list's bottom-right.
- **[⚙]** opens Preferences.

**Window sizing.** Minimum 800 × 500; default 1000 × 600; resizable with a draggable splitter between sidebar and main area. Position and size persisted via `NSWindow` autosave.

### 4.2 Menu bar extra

A menu bar icon reflects overall app state:
- **Filled icon** when any route is running.
- **Outline icon** when all routes are stopped.
- **Tinted red** when any route is in error or waiting for a device.

Clicking opens a popover:

```
  Jbox — 2 routes running

  ✔  Keys to Console            ●     (toggle)
  ✔  Mic to Monitors            ●     (toggle)
     Backup Send (waiting)      ⏸     (toggle)
  ─────────────────────────────────
  Scene: Practice                     ▾
  ─────────────────────────────────
  Start All      Stop All
  ─────────────────────────────────
  Open Jbox…
  Preferences…
  Quit
```

No deep editing from the menu bar — just toggles, scene switching, and opening the main window. The menu bar is for "what's running" awareness and quick actions.

### 4.3 Route editor

Opens as a sheet over the main window when the user adds a new route or edits an existing one.

**Fields:**

- **Name** — text field. Auto-generated initially (e.g., "V31 ch 1,2 → Apollo Virt 1,2"); if the user types in it, `isAutoName` flips to `false` and stops regenerating.
- **Source device** — picker (`Picker` dropdown) of all input-capable Core Audio devices currently visible. Disconnected devices referenced by the route are shown with a " (disconnected)" suffix and a warning icon.
- **Source channels** — scrollable multi-select list of the source device's input channels. Shows `"ch N — <label if any>"`. ⌘-click for non-contiguous, shift-click for range.
- **Destination device** — same pattern as source.
- **Destination channels** — same pattern as source.
- **Mapping preview** — a small visual panel showing the edges derived from the two channel selections, in order (a source channel may appear on multiple edges — fan-out):

```
    Source (V31)             Destination (Apollo)
      ch 1  ──────────────→    Virtual 1
      ch 2  ──────────────→    Virtual 2
```

- **Reorderable** — drag items inside either channel list to change which source channel pairs with which destination channel.
- **Validation errors** shown inline: "Source and destination must have the same channel count", "Destination channel already in use" (fan-in is deferred per Appendix A), etc. Duplicate source channels are allowed and produce fan-out.
- **Save** / **Cancel** buttons. No partial saves; the route is committed atomically.

### 4.4 Scene editor

Smaller sheet:
- Name field (required).
- A checkbox list of all routes ("which routes are in this scene").
- Activation-mode segmented control: **Exclusive** (default) / **Additive**.
- Save / Cancel.

Clicking a scene in the sidebar activates it. Activation runs the engine through the necessary start / stop sequence to match the scene's intent: in `exclusive` mode, the app stops all routes not in the scene and starts all that are; in `additive` mode, it starts the scene's routes without touching others.

### 4.5 Meters

- Per-channel, per-route, vertical bar indicator.
- Source-side meter shows input level; destination-side meter shows post-resampling output level.
- Color thresholds:
  - **Gray** — no signal detected (peak < -60 dBFS).
  - **Green** — normal (peak between -60 and -6 dBFS).
  - **Yellow** — near clip (peak between -6 and -3 dBFS).
  - **Red** — clipped (peak ≥ -3 dBFS).
- Thresholds also communicated by bar height and label for color-accessibility.
- Drawn via SwiftUI `Canvas`. A single timer fires at ~30 Hz for the whole app; each meter reads its atomics, no per-frame allocations.

### 4.6 Preferences window

Standard macOS settings window (`SwiftUI.Settings` scene) with three tabs:

- **General** — launch-at-login toggle; appearance picker (System / Light / Dark); "Show meters in menu bar" toggle.
- **Audio** — buffer-size policy: "Use each device's current setting" (default) / "Explicit override" with a numeric field (32 / 64 / 128 / 256 / 512 / 1024 samples) and a warning that the override is device-global. Resampler quality: Mastering (default) / High Quality.
- **Advanced** — "Show engine diagnostics" toggle (off by default; when on, the route row exposes the developer-oriented counters `frames_produced / frames_consumed · u<K>` and the per-side estimated-latency breakdown inside the expanded meter panel). Export Configuration (saves `state.json` to a user-chosen location) / Import Configuration (replaces current state with confirmation) / Reset State (wipes `state.json` with confirmation) / "Open Logs Folder" button.

### 4.7 Key flows

- **First launch.** Empty state in the main window with a "Create your first route" CTA. OS presents microphone permission dialog on first access to any input device.
- **Add route.** `+ Add Route` → editor sheet → fill in → Save → row appears in list, stopped.
- **Start route.** Click `▶` in row. If both devices are present, transitions to `running` in under 1 second. If absent, transitions to `waiting`; `⏸` icon appears.
- **Rename route.** Double-click the route name in the row (or `⌘R`) → inline text field. Engine-side this is a label-only metadata update; the running route is not affected.
- **Edit route mapping.** Double-click elsewhere on the row, or select and `⌘E` → editor sheet prefilled. Mapping changes on a stopped route are applied in place. For a running route the UI performs a stop → reconfigure → start cycle (confirmed in the sheet's "Apply" button tooltip), since mid-flight mapping mutation is not supported in v1.
- **Delete route.** Select and `⌫` → confirmation dialog → removed. Stopped first if running.
- **Create scene.** Sidebar `+` → editor sheet → Save → scene appears in sidebar.
- **Switch scene.** Click scene in sidebar → engine applies → running-state dots update across the route list.

### 4.8 Accessibility

- Standard macOS accessibility: VoiceOver labels on all controls, keyboard navigation for every action, sufficient color contrast.
- Meter state communicated both by color and by numeric dBFS label on focus, so it's distinguishable for users with color-vision deficiency.
- Minimum font size follows the system; no hardcoded small type.

### 4.9 Deferred to future versions

- **Localization.** v1 is English-only.
- **Menu bar quick-route creation** (create a route without opening the main window). Could be a useful speed improvement; not required.
- **Alternative UI shells** (CLI, web UI, AppKit-based UI). Architecturally supported via the bridge API; not implemented in v1.
- **UI-level themes beyond System / Light / Dark** (e.g., high-contrast, custom accent colors).
- **Per-route info / stats overlay** (latency numbers, drift-tracker health, dropout counters). Useful for debugging; postponed until the user wants visibility.

---

## Section 5 — Testing, Build, and Distribution

### 5.1 Testing strategy

**Engine unit tests (C++).** [Catch2 v3](https://github.com/catchorg/Catch2), vendored in-tree as `ThirdParty/Catch2/catch_amalgamated.{hpp,cpp}`. Chosen at Phase 2 kickoff for clean SPM integration (single `.target` declaration, no submodules or external package registries). C++ tests live in `Tests/JboxEngineCxxTests/` as a SPM `.executableTarget`; run them with `swift run JboxEngineCxxTests` (debug config by default, enabling ThreadSanitizer).
- `RingBuffer`: concurrent producer/consumer correctness under stress, wrap-around edges, fill-level tracking, overrun and underrun behavior.
- `DriftTracker`: given synthetic fill-level time series, verify the PI controller converges within N seconds and holds within a target band; bounded under pathological inputs.
- `AudioConverterWrapper`: ratio updates do not glitch; quality setting honored; correct samples-per-block computation.
- `ChannelMapper`: edge-list validation; channel-count matching; destination-uniqueness detection (sources may repeat — fan-out).

**Engine integration tests (C++).** A deterministic simulation harness — a fake Core Audio device that drives the engine with fully controllable timing and clock drift.
- Start / stop a single route end-to-end; verify samples pass through with correct ordering and zero loss.
- Two routes sharing a source device; verify isolation and correct per-route copy.
- Clock-drift simulation: inject a source clock 50 ppm faster than the destination clock; verify the drift tracker converges within 10 seconds and the ring buffer stays within a target band for at least 5 simulated minutes.
- Device disappearance mid-stream: verify the engine transitions to `error`, leaks nothing, and recovers when the device returns.
- Sample-rate mismatch (48 k source, 44.1 k destination): verify the output waveform matches the expected resampled waveform within a quality tolerance (e.g., -60 dB SNR).

**Device-level integration tests (real hardware, manual / release gate).**
- Require actual devices (V31, Apollo, any USB interface, a mic).
- Run before each release.
- **Soak test** — a representative route running for at least 30 minutes; verify zero dropouts in logs, drift tracker within band, meter values plausible.
- **Latency measurement** — loopback test: connect an output channel of the destination device back to an input channel of the source device; inject a test pulse; measure the round-trip time through the engine. Acceptable bound: within ±1 ms of theoretical (source buffer + ring buffer + destination buffer).
- **Stress test** — start / stop routes in rapid succession; unplug / replug devices; verify graceful recovery and no crashes.

**RT-safety enforcement.**
- **Static scanner** (`scripts/rt_safety_scan.sh`) — greps `Sources/JboxEngineC/rt/` for banned symbols. Runs in CI as a required check; documented as a recommended local pre-commit hook.
- **Compile flags** for `rt/`: `-fno-exceptions`, warnings-as-errors for forbidden constructs where compiler supports it.
- **Runtime checks in debug builds:** ThreadSanitizer enabled for engine tests; data races on RT threads fail the test run.

**UI tests (Swift).** Minimal.
- SwiftUI preview-based smoke tests for key views (route row, route editor, sidebar).
- A few XCUITest flows exercising add-route, start-route, switch-scene. Dependent on Xcode to run locally; on CI, they can be skipped without blocking a release.
- Deliberately kept light because the UI is expected to evolve.

### 5.2 Build system

**Swift Package Manager is the single source of truth.** No `.xcodeproj` or `.xcworkspace` is committed. `Package.swift` at the repository root declares:

- `JboxEngineC` — C++ engine target (C++20, mixed-language package with `cxxSettings`).
  - Public header: `Sources/JboxEngineC/include/jbox_engine.h`.
  - Subdirectories `rt/` and `control/` for RT-safe and non-RT code respectively, with different compile flags.
- `JboxEngineSwift` — thin Swift wrapper over the C API, giving ergonomic Swift types to callers.
- `JboxEngineCLI` — standalone headless executable for exercising the engine without the GUI. Useful for engine-level testing on CI runners that can't drive GUI tests, and for the engine's soak tests.
- `JboxApp` — the main macOS GUI executable (SwiftUI `@main` App struct).
- Test targets: `JboxEngineTests`, `JboxEngineIntegrationTests`, `JboxAppTests`.

**Toolchain.** **Xcode.app must be installed** (free — either via the App Store, or via a direct `.xip` download from [developer.apple.com/download/all](https://developer.apple.com/download/all/) using a free Apple Developer account). No paid Apple Developer Program membership is required.

Xcode.app's presence is mandatory because Command Line Tools alone do not include the `XCTest` or `Testing` frameworks; SPM cannot run `swift test` without them. However, **using the Xcode IDE is optional** — once Xcode.app is installed, the Swift toolchain picks up its frameworks automatically and all development can happen from the command line in any editor (VS Code, Cursor, Nova, Vim, etc.). Opening `Package.swift` in Xcode is supported (SwiftUI previews work) but never required.

After installing Xcode.app for the first time, the license must be accepted once — either by opening `Xcode.app` briefly and clicking "Agree", or via `sudo xcodebuild -license`.

**App bundle production.** SPM produces a plain executable. A shell script `scripts/bundle_app.sh` wraps the executable into a valid `Jbox.app`:
1. Create the bundle skeleton: `Jbox.app/Contents/{MacOS,Resources}`.
2. Copy the built executable to `Jbox.app/Contents/MacOS/Jbox` (and the CLI alongside at `Contents/MacOS/JboxEngineCLI`).
3. Generate `Jbox.app/Contents/Info.plist` inline via heredoc with version / build-number substitution and the `NSMicrophoneUsageDescription` key.
4. Emit a minimal `Jbox.entitlements` plist (also inline via heredoc) that claims `com.apple.security.device.audio-input` — required under Hardened Runtime per § 1.5. **Without this entitlement, Core Audio silently delivers zero-filled input buffers: IOProcs still fire, frame counters still advance, and the bug is invisible except through signal meters.**
5. Run `codesign --sign - --force --options runtime --entitlements build/Jbox.entitlements Jbox.app` for ad-hoc signing with the Hardened Runtime flag and the audio-input entitlement attached.
6. Post-sign verification: dump the bundle's attached entitlements via `codesign -d --entitlements -` and fail the script if `com.apple.security.device.audio-input` is absent. This CI-visible guard prevents a future edit to the signing line from reintroducing the silent-buffer bug. Copy the icon asset to `Jbox.app/Contents/Resources/Jbox.icns` if present.

**Dependencies.** v1 uses only the macOS SDK and the C++ standard library. No third-party audio libraries, no Swift packages. If resampler quality or performance ever disappoints, libsamplerate or SoXr can be added as a Swift Package dependency.

### 5.3 Project layout

```
jbox/
├── README.md                  ← project orientation (GitHub landing page)
├── docs/
│   ├── spec.md                ← this file
│   ├── plan.md                ← phased implementation plan
│   └── releases.md            ← release pipeline walk-through
├── Package.swift              ← single SPM manifest, root of the build
├── Makefile                   ← convenience wrappers over scripts/
├── Sources/
│   ├── JboxEngineC/
│   │   ├── include/
│   │   │   └── jbox_engine.h       ← public C API + ABI version
│   │   ├── rt/                     ← real-time code (statically scanned)
│   │   │   ├── ring_buffer.hpp         (header-only; caller owns storage)
│   │   │   ├── rt_log_queue.hpp        (header-only; templated on capacity)
│   │   │   ├── rt_log_codes.hpp        (event-code constants)
│   │   │   ├── atomic_meter.hpp
│   │   │   └── audio_converter_wrapper.{hpp,cpp}
│   │   └── control/                ← non-RT engine (allocations, logging sinks)
│   │       ├── engine.{hpp,cpp}        ← facade; owns DeviceManager + RouteManager + LogDrainer
│   │       ├── bridge_api.cpp          ← implements jbox_engine_*
│   │       ├── channel_mapper.{hpp,cpp}
│   │       ├── device_backend.hpp      ← IDeviceBackend abstraction
│   │       ├── core_audio_backend.{hpp,cpp}
│   │       ├── simulated_backend.{hpp,cpp}   ← deterministic test backend
│   │       ├── device_manager.{hpp,cpp}
│   │       ├── device_io_mux.{hpp,cpp}       ← RCU-style per-device active-route list
│   │       ├── route_manager.{hpp,cpp}
│   │       ├── drift_tracker.{hpp,cpp}       ← PI controller state
│   │       ├── drift_sampler.{hpp,cpp}       ← ~100 Hz sampler thread
│   │       └── log_drainer.{hpp,cpp}         ← consumer thread; os_log sink
│   ├── JboxEngineSwift/
│   │   ├── JboxEngine.swift        ← Swift wrapper over C API
│   │   ├── EngineStore.swift       ← @Observable device + route store (polling, caching)
│   │   ├── ChannelLabel.swift      ← "Ch N · <name>" formatter
│   │   └── JboxLog.swift           ← os.Logger wrappers (app / engine / ui categories)
│   ├── JboxEngineCLI/
│   │   └── main.swift              ← CLI harness (`--list-devices`, `--route`)
│   └── JboxApp/                    ← flat for now; Model/ Views/ Persistence/ land in Phases 6–7
│       ├── JboxApp.swift           ← @main App + engine bootstrap
│       ├── RouteListView.swift
│       └── AddRouteSheet.swift
├── Tests/
│   ├── JboxEngineCxxTests/         ← Catch2-based C++ unit + integration tests (executable target)
│   ├── JboxEngineTests/            ← Swift Testing: bridge + wrapper + EngineStore
│   ├── JboxEngineIntegrationTests/ ← Swift integration tests (placeholder)
│   └── JboxAppTests/               ← XCUITest / UI tests (placeholder)
├── ThirdParty/
│   └── Catch2/                     ← vendored Catch2 v3 amalgamation
├── scripts/
│   ├── rt_safety_scan.sh
│   ├── bundle_app.sh
│   ├── build_release.sh
│   ├── package_unsigned_release.sh
│   ├── run_app.sh
│   └── verify.sh                   ← local mirror of CI
├── .github/
│   └── workflows/
│       ├── ci.yml
│       └── release.yml             ← tag-driven DMG build + draft GitHub Release
├── .gitignore
└── LICENSE
```

Notes that differ from the original spec draft:
- **`ioproc_dispatch.{hpp,cpp}` was never created.** The RCU active-route list landed in Phase 5 as `control/device_io_mux.{hpp,cpp}` — intentionally in `control/` because the mutation work (allocating new lists, deferring reclamation) is control-thread work, while the IOProc trampolines it installs are the only RT-reachable code from that module.
- **`ring_buffer.cpp` was never created.** The ring buffer is header-only; callers provide the backing storage so the class itself stays allocation-free and trivially header-placeable in `rt/`.
- **No `Sources/JboxApp/Resources/Info.plist.in`.** `scripts/bundle_app.sh` emits the `Info.plist` inline via heredoc (see [docs/releases.md](./releases.md) for the version flow). If the plist grows complex, a template file is the natural refactor.
- **`Sources/JboxApp/` is flat.** `Model/`, `Views/`, `Persistence/` appear only when Phases 6 (meters, preferences) and 7 (persistence, scenes) need them.
- **`release.yml`** (tag-driven) lives alongside `ci.yml`; tag-push builds an ad-hoc-signed DMG and creates a draft pre-release GitHub Release.

### 5.4 Continuous integration

**Platform.** GitHub Actions macOS 15 runner (Apple silicon image).

**Per pull request and on `main` push:**
1. `swift build -c release` — compiles all targets in release mode.
2. `swift test` — runs unit and integration tests.
3. `scripts/rt_safety_scan.sh` — required to pass.
4. `clang-tidy` on the C++ targets.
5. `swiftlint` on the Swift targets.

**On release tag `vX.Y.Z`:**
1. Full build in release mode.
2. `scripts/build_release.sh` — runs `bundle_app.sh`, produces ad-hoc-signed `Jbox.app` with the `JboxEngineCLI` executable bundled at `Contents/MacOS/JboxEngineCLI`.
3. `scripts/package_unsigned_release.sh` — wraps the `.app`, the `Uninstall Jbox.command`, and a `READ-THIS-FIRST.txt` into a drag-to-install `Jbox-X.Y.Z.dmg`.
4. Upload `Jbox-X.Y.Z.dmg` to GitHub Releases (draft pre-release by default; promoted manually).

See [docs/releases.md](./releases.md) for the end-to-end release walk-through, including the version-synchronization map.

No CI secrets for code signing / notarization are required — v1 does not use Developer ID.

### 5.5 Distribution

**Primary mode — personal use.**
- User clones, builds, runs `scripts/run_app.sh` (which invokes `bundle_app.sh` and launches `Jbox.app` from the `build/` directory).
- Ad-hoc signature (`codesign --sign -`) is enough for macOS to run the app locally.
- First launch may prompt for microphone/audio access (expected); the user grants it.

**Secondary mode — small-audience distribution, unsigned.**
- `scripts/package_unsigned_release.sh` produces `Jbox-X.Y.Z.dmg` containing:
  - `Jbox.app` (ad-hoc signed; CLI bundled inside at `Contents/MacOS/JboxEngineCLI`)
  - `Applications` symlink (drag-to-install target)
  - `Uninstall Jbox.command` (double-click to remove deployed files)
  - `READ-THIS-FIRST.txt` with Gatekeeper + CLI-usage instructions:
    > To open Jbox for the first time on your Mac:
    > 1. Mount the DMG and drag Jbox.app into the Applications symlink.
    > 2. Right-click (or ⌃-click) Jbox.app in /Applications and choose **Open**.
    > 3. Click **Open** in the Gatekeeper confirmation dialog.
    > 4. You only need to do this once. Subsequent launches work normally.
- Share via GitHub Releases (draft auto-populated on tag push), email attachment, or any other mechanism.
- Recipients must trust the source — there is no notarization chain.

**Future modes (not v1).**
- **Developer ID signed + notarized.** Clean Gatekeeper experience for any audience. Requires Apple Developer Program ($99/year) and a signing identity in CI secrets. A one-hour infrastructure task when the user decides it's worth it.
- **Mac App Store.** **Not viable** for this kind of app — App Sandbox restrictions conflict with HAL-level device control.
- **Auto-update.** Sparkle framework would be the standard retrofit. Clean architecturally — no changes outside the update layer.

### 5.6 Release gates

Before tagging a release:

1. All automated tests passing in CI.
2. RT-safety scan clean.
3. Device-level soak test (≥ 30 minutes with a real hardware setup) passes with zero dropouts.
4. Latency measurement within budget (see Section 2.5 / 2.6).
5. Manual smoke test on a clean macOS 15 user account: mount the DMG, drag the `.app` to Applications, approve Gatekeeper, create a test route, start it, verify audio flows. Covers the full first-run experience end-to-end.

### 5.7 Deferred to future versions

- **Auto-update via Sparkle.**
- **Developer ID signing and Apple notarization** — waiting until there's a real audience to sign for.
- **Mac App Store distribution** — architecturally ruled out.
- **Homebrew cask** — a natural future distribution vector once notarized.
- **Installer package (`.pkg`)** instead of a `.dmg` with a `.app` — a `.pkg` offers a cleaner install experience (fully automated `/Applications` placement, no drag step) but requires signing to be smooth; deferred with signing.

---

## Appendix A — Deferred / Out-of-Scope

Consolidated list of items explicitly deferred from v1, for easy cross-reference:

**Mapping model extensions.**
- Fan-in / summing (multiple sources → one destination). Fan-out (one source → multiple destinations) was **promoted out of deferred** and is supported — see § 3.1 mapping invariants and § 4.3 editor validation.

**Mixer-adjacent features.**
- Per-route gain / trim.
- Per-route mute.
- Per-route pan.
- Global hotkeys.

**Distribution / packaging.**
- Apple Developer Program enrollment.
- Developer ID signing.
- Apple notarization.
- Mac App Store distribution (permanently out of scope; incompatible with HAL access).
- Sparkle auto-update.
- Homebrew cask.
- `.pkg` installer.

**Virtual devices.**
- Creating a virtual Core Audio device (as BlackHole / Loopback do). Jbox routes between existing devices only.
- DriverKit extensions.

**UI scope extensions.**
- Localization.
- Menu bar quick-route creation without opening the main window.
- CLI / web UI / alternative UIs (architecturally supported; not implemented).
- Per-route stats overlay (latency / drift / dropout counters).

**Data / persistence extensions.**
- iCloud / network sync.
- Multiple configuration profiles (separate full-state documents).
- Schema downgrade support.

**Engine extensions.**
- Alternative resampler backends (libsamplerate, SoXr).
- Internal CPU budget telemetry.
- Network audio (Dante, AVB, NDI).
- MIDI routing (different protocol, different problem space).

---

## Appendix B — Glossary

**ABI** — Application Binary Interface; the binary-level contract of the engine's C API.
**Aggregate Device** — a macOS-level composite audio device combining multiple physical devices, with built-in clock drift correction. Used by DAWs for multi-device setups; **not** used by Jbox.
**AudioConverter** — Apple's built-in sample-rate converter / format converter. Supports variable-ratio resampling, used in Jbox for both sample-rate matching and drift correction.
**AudioDeviceID** — a `UInt32` used by Core Audio to identify a device within a session. **Not** stable across reboots; for persistent identification, use the device UID.
**Bridge (C API)** — the stable C-level interface between the C++ engine and its clients (Swift UI layer, CLI, any future client). The product's public contract.
**Core Audio HAL** — the Hardware Abstraction Layer of Apple's Core Audio stack; low-level APIs like `AudioDeviceCreateIOProcIDWithBlock` that Jbox uses for direct device I/O.
**Drift (clock drift)** — the slow divergence in effective sample rate between two independent device clocks. Uncorrected, causes buffer underrun or overrun over time.
**IOProc** — an audio input / output procedure; a callback Core Audio invokes on a real-time thread to deliver input samples and request output samples. Jbox registers one per device per direction.
**Lock-free SPSC** — single-producer / single-consumer, using atomics rather than locks. Jbox's ring buffers between IOProcs are lock-free SPSC.
**PI controller** — Proportional-Integral controller; the feedback-control algorithm used by Jbox's drift tracker.
**ppm** — parts per million. Crystal oscillators in consumer and pro audio gear typically deviate from nominal by ±25–100 ppm.
**RCU (Read-Copy-Update)** — a concurrency pattern where readers see a consistent snapshot via an atomic pointer, and writers publish new versions with deferred reclamation of the old. Jbox uses an RCU-style pattern for per-device active-route lists.
**Ring buffer** — a fixed-size circular queue. Jbox uses lock-free ring buffers to bridge source and destination IOProcs for each route.
**RT-safe** — real-time-safe; code that can execute on an audio callback without allocating, locking, or syscalling. All Jbox code reachable from an IOProc must be RT-safe.
**SPM** — Swift Package Manager; Jbox's build system.
**UID (Audio Device UID)** — a stable string identifier for a Core Audio device. Survives reboots, disconnects, and reconnects. Used by Jbox as the persistent primary key for device references.
