// device_backend.hpp — abstract interface over the audio device system.
//
// Why an abstraction
// ------------------
// The engine talks to the hardware through this interface, not directly
// to Core Audio. Two concrete implementations exist:
//   - CoreAudioBackend (production, Sources/.../control/core_audio_backend.*)
//   - SimulatedBackend (tests and CI, Sources/.../control/simulated_backend.*)
//
// The simulated backend lets us drive the engine deterministically in
// CI — no real hardware, no sleep, no flakiness — and is the fixture
// Phase 4's drift-correction simulation needs anyway.
//
// Threading
// ---------
// Non-RT methods (enumerate, open*, close*, start/stop) are called on
// the engine's control thread. IOProc callbacks fire on whichever
// thread the backend uses for audio delivery — in production that is
// the Core Audio RT thread, in the simulated backend it is whatever
// thread calls deliverBuffer(). Callback implementations must be
// RT-safe: no allocation, no locking, no syscalls.
//
// One IOProc per direction per device. The engine is responsible for
// multiplexing multiple routes that share a device behind a single
// registered callback (see docs/spec.md § 2.7, Phase 5).
//
// See docs/spec.md §§ 2.2, 2.7.

#ifndef JBOX_CONTROL_DEVICE_BACKEND_HPP
#define JBOX_CONTROL_DEVICE_BACKEND_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace jbox::control {

// Mirror of jbox_device_direction_t (bitmask).
enum BackendDeviceDirection : std::uint32_t {
    kBackendDirectionNone   = 0,
    kBackendDirectionInput  = 1u << 0,
    kBackendDirectionOutput = 1u << 1,
};

// Backend-internal view of a device. Same fields as the public
// jbox_device_info_t, but using std::string for ergonomics on the
// C++ side.
struct BackendDeviceInfo {
    std::string   uid;
    std::string   name;
    std::uint32_t direction = kBackendDirectionNone;  // bitmask
    std::uint32_t input_channel_count  = 0;
    std::uint32_t output_channel_count = 0;
    double        nominal_sample_rate  = 0.0;
    std::uint32_t buffer_frame_size    = 0;

    // Phase-6 refinement #3: static latency components reported by the
    // HAL (kAudioDevicePropertyLatency + kAudioDevicePropertySafetyOffset)
    // on each scope that has channels. Used by the per-route latency
    // estimator (docs/spec.md § 2.12). All values are in frames at
    // nominal_sample_rate; 0 means the HAL either did not expose the
    // property or the device has no channels in that direction.
    std::uint32_t input_device_latency_frames  = 0;
    std::uint32_t input_safety_offset_frames  = 0;
    std::uint32_t output_device_latency_frames = 0;
    std::uint32_t output_safety_offset_frames = 0;
};

// Opaque handle for a registered IOProc. 0 is reserved as invalid.
using IOProcId = std::uint64_t;
inline constexpr IOProcId kInvalidIOProcId = 0;

// Phase 7.6.4: device topology change events.
//
// Backends emit these via the `setDeviceChangeListener` callback when
// the HAL (or, in tests, the simulator's simulate-* seams) signals a
// topology shift. The watcher / control-thread tick is responsible
// for any debouncing or coalescing — backends just report.
//
// The listener fires from whatever thread the backend uses to observe
// the change. CoreAudioBackend invokes from a HAL property-listener
// thread; SimulatedBackend invokes synchronously from the test thread.
// Listeners must therefore be thread-safe internally (the
// DeviceChangeWatcher uses a mutex-protected queue).
struct DeviceChangeEvent {
    enum Kind {
        // The top-level enumerable device list changed (a device was
        // added, removed, or its identity flipped). `uid` is the
        // affected device when known; empty when the kind is a
        // bare "list changed" notification with no specific subject.
        kDeviceListChanged       = 0,
        // A specific device's IsAlive property went to 0 — the device
        // is no longer usable even if it still appears in enumeration.
        kDeviceIsNotAlive        = 1,
        // An aggregate's active sub-device list changed (a member was
        // added or removed). The aggregate's UID is in `uid`.
        kAggregateMembersChanged = 2,
    };
    Kind        kind;
    std::string uid;
};

using DeviceChangeListener = void(*)(const DeviceChangeEvent& event,
                                     void* user_data);

// Callback invoked by the backend with a buffer of input samples.
// Samples are interleaved: samples[frame * channels + ch].
// Callback MUST be RT-safe: no allocation, no locks, no syscalls.
using InputIOProcCallback = void (*)(const float* samples,
                                     std::uint32_t frame_count,
                                     std::uint32_t channel_count,
                                     void* user_data);

// Callback invoked by the backend to fill output samples. The
// destination buffer is zero-initialised before the call. Samples
// are interleaved. Callback MUST be RT-safe.
using OutputIOProcCallback = void (*)(float* samples,
                                      std::uint32_t frame_count,
                                      std::uint32_t channel_count,
                                      void* user_data);

// Callback invoked by the backend once per RT tick on a duplex-
// configured device (typically an aggregate device, or any device
// opened for both input and output). `input_samples` and
// `output_samples` are interleaved. `output_samples` is zero-
// initialised on entry so the callback only needs to write channels
// it actually fills. Used by the Phase 6 direct-monitor fast path
// to skip the ring buffer and AudioConverter entirely when a route's
// source and destination devices are the same UID and the user has
// opted into Performance latency mode. MUST be RT-safe.
using DuplexIOProcCallback = void (*)(const float* input_samples,
                                      std::uint32_t input_frame_count,
                                      std::uint32_t input_channel_count,
                                      float*        output_samples,
                                      std::uint32_t output_frame_count,
                                      std::uint32_t output_channel_count,
                                      void* user_data);

class IDeviceBackend {
public:
    virtual ~IDeviceBackend() = default;

    // Snapshot of currently-available devices. No ownership transfer.
    virtual std::vector<BackendDeviceInfo> enumerate() = 0;

    // Per-channel names for the given device + direction, as macOS /
    // the device driver exposes them via Core Audio's
    // `kAudioObjectPropertyElementName`. The returned vector has one
    // entry per channel (index 0 = channel 1). Entries may be empty
    // strings when the driver does not provide a label — callers
    // should fall back to numeric labels in that case.
    //
    // `direction` must be exactly one of kBackendDirectionInput or
    // kBackendDirectionOutput; any other value returns an empty vector.
    // An unknown `uid` also returns an empty vector.
    virtual std::vector<std::string> channelNames(
        const std::string& uid,
        std::uint32_t direction) = 0;

    // Register an input IOProc on the device identified by `uid`.
    // Returns kInvalidIOProcId on failure (unknown device, no input
    // channels, or an input IOProc is already registered).
    virtual IOProcId openInputCallback(const std::string& uid,
                                       InputIOProcCallback callback,
                                       void* user_data) = 0;

    // Register an output IOProc on the device identified by `uid`.
    // Returns kInvalidIOProcId on failure.
    virtual IOProcId openOutputCallback(const std::string& uid,
                                        OutputIOProcCallback callback,
                                        void* user_data) = 0;

    // Register a duplex IOProc that handles both directions in one
    // RT callback. Used by the direct-monitor fast path on aggregate
    // devices. Fails (returns kInvalidIOProcId) if the device has no
    // input channels, no output channels, or already has an IOProc
    // registered in either direction.
    virtual IOProcId openDuplexCallback(const std::string& uid,
                                        DuplexIOProcCallback callback,
                                        void* user_data) = 0;

    // Unregister a previously-registered callback.
    //
    // Returns true on success: the callback was destroyed, or it was
    // already gone / unknown / kInvalidIOProcId (all benign no-ops).
    // Callers may clear their stored IOProcId on a true return.
    //
    // Returns false when the destroy attempt failed (e.g. macOS
    // returned non-noErr from AudioDeviceDestroyIOProcID on a
    // hot-unplugged or otherwise degraded device). On false, the
    // backend retains the IOProc bookkeeping so a subsequent
    // closeCallback(id) retries the destroy. Callers MUST keep their
    // stored IOProcId (do NOT reset to kInvalidIOProcId) so that the
    // next teardown opportunity — another stop, removeRoute, or
    // hot-plug-driven recovery — can re-invoke this method with the
    // same id.
    virtual bool closeCallback(IOProcId id) = 0;

    // Start device audio flow. After this call, registered callbacks
    // begin firing on the backend's audio thread (or, for the
    // simulated backend, when the test calls deliverBuffer). Returns
    // false if the device is unknown or already started.
    virtual bool startDevice(const std::string& uid) = 0;

    // Stop device audio flow. Registered callbacks continue to be
    // registered but no longer fire. Safe to call on a non-started
    // device (no-op).
    virtual void stopDevice(const std::string& uid) = 0;

    // Read the device's current buffer frame size as Core Audio
    // reports it. Returns 0 if the device is unknown or the query
    // failed. Non-RT; call from the control thread only.
    virtual std::uint32_t currentBufferFrameSize(const std::string& uid) = 0;

    // Phase 7.6.4: install a single `DeviceChangeListener` that fires
    // whenever the backend observes a device-topology change. Pass a
    // null callback to clear. At most one listener at a time —
    // re-registration replaces the previous callback.
    //
    // Production backends (CoreAudioBackend) wire HAL property
    // listeners under the hood: kAudioHardwarePropertyDevices,
    // kAudioDevicePropertyDeviceIsAlive on each enumerated device,
    // and kAudioAggregateDevicePropertyActiveSubDeviceList on each
    // aggregate. Simulated backends fire the callback synchronously
    // from their `simulateDevice*` test seams.
    //
    // The callback runs on whatever thread observed the change (HAL
    // thread in production, test thread in simulation). Listener
    // implementations must be thread-safe.
    virtual void setDeviceChangeListener(DeviceChangeListener cb,
                                         void* user_data) = 0;

    // Express a per-device preference for the HAL buffer-frame-size,
    // exactly as Superior Drummer / Reaper / any other Core Audio
    // client would: a single `AudioObjectSetPropertyData
    // (kAudioDevicePropertyBufferFrameSize)` write. This does NOT
    // claim hog mode and does NOT evict other clients. macOS
    // resolves the actual buffer with `max-across-clients` — if
    // another app (Music, a video call, a DAW) is asking for a
    // larger buffer, the actual value will be the larger one
    // until that app stops asking.
    //
    // For an aggregate device the call enumerates the active
    // sub-devices via `kAudioAggregateDevicePropertyActive
    // SubDeviceList` and writes to each member directly. The
    // aggregate's own property is also written; macOS coordinates
    // the rest. No fan-out via the aggregate driver — each member
    // write goes through that member's own HAL plugin, the same
    // way SD-style clients do it.
    //
    // `frames == 0` is a no-op (used by callers that route the
    // "no override" case through the same code path).
    //
    // Non-RT; control thread only.
    virtual void setBufferFrameSize(const std::string& uid,
                                    std::uint32_t frames) = 0;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DEVICE_BACKEND_HPP
