// simulated_backend.hpp — deterministic test backend for IDeviceBackend.
//
// Drives the engine without any real audio hardware. Tests register
// devices via addDevice(), then drive audio by calling deliverBuffer()
// once per "buffer cycle" — no threads, no timers, no flakiness.
//
// Threading: single-threaded from the test's point of view. All
// methods should be called from the test thread that owns the
// backend instance.
//
// See docs/plan.md § Phase 3.

#ifndef JBOX_CONTROL_SIMULATED_BACKEND_HPP
#define JBOX_CONTROL_SIMULATED_BACKEND_HPP

#include "device_backend.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace jbox::control {

class SimulatedBackend final : public IDeviceBackend {
public:
    SimulatedBackend() = default;
    ~SimulatedBackend() override = default;

    SimulatedBackend(const SimulatedBackend&) = delete;
    SimulatedBackend& operator=(const SimulatedBackend&) = delete;

    // Register a simulated device. If a device with the same UID
    // already exists, its configuration is replaced (callbacks and
    // started state are reset).
    void addDevice(const BackendDeviceInfo& info);

    // Register a simulated aggregate device — a device that wraps
    // one or more previously-added member UIDs. Aggregate semantics
    // are the same as macOS's Audio MIDI Setup aggregates:
    // hog-mode and buffer-size requests on the aggregate fan out to
    // each member. Members must already exist (addDevice'd) at the
    // time of registration.
    void addAggregateDevice(const BackendDeviceInfo& info,
                            std::vector<std::string> sub_device_uids);

    // Remove a simulated device. Equivalent to "unplugging" it:
    // its callbacks are quietly dropped and started state is cleared.
    // Safe to call with an unknown UID (no-op).
    void removeDevice(const std::string& uid);

    // ----- Phase 7.6.4 hot-plug simulation (test-only seams) -----
    //
    // Mirror the device-topology events macOS's HAL emits. Each method
    // synchronously fires the registered DeviceChangeListener (if any)
    // with the appropriate event kind. State changes (slot removal,
    // slot insertion) are applied as part of the call, so a subsequent
    // enumerate() reflects the new topology.
    //
    // simulateDeviceRemoval fires kDeviceIsNotAlive followed by
    // kDeviceListChanged — matching the order a HAL plugin emits when
    // a device is yanked. The slot is erased between the two events.
    //
    // simulateDeviceReappearance adds the device back (replacing any
    // existing slot with the same UID) and fires kDeviceListChanged.
    //
    // simulateAggregateMembersChanged fires kAggregateMembersChanged
    // for the named aggregate UID; the simulator does not modify the
    // aggregate's sub-device list, so test fixtures can pre-arrange
    // the new shape via addAggregateDevice / removeDevice before
    // calling.
    void simulateDeviceRemoval(const std::string& uid);
    void simulateDeviceReappearance(const BackendDeviceInfo& info);
    void simulateAggregateMembersChanged(const std::string& uid);

    // Seed the names reported by `channelNames()` for a given device
    // and direction. Must be called after `addDevice` for that UID.
    // Names are stored by value; the vector length is not required
    // to match the device's channel count (tests pick the contract
    // they want to exercise).
    void setChannelNames(const std::string& uid,
                         std::uint32_t direction,
                         std::vector<std::string> names);

    // Drive one buffer cycle for the device identified by `uid`.
    //
    // If an input callback is registered AND the device is started:
    //   - `input_source` must point to `frame_count * info.input_channel_count`
    //     interleaved floats; the callback is invoked with those samples.
    //   - `input_source` may be nullptr to signal "no input this cycle"
    //     (callback is skipped).
    //
    // If an output callback is registered AND the device is started:
    //   - The callback is invoked with a zero-initialised working buffer.
    //   - On return, the working buffer is copied into `output_capture`
    //     (if non-null). `output_capture` must point to
    //     `frame_count * info.output_channel_count` floats.
    //
    // deliverBuffer is a no-op if the device doesn't exist or hasn't
    // been started.
    void deliverBuffer(const std::string& uid,
                       std::uint32_t frame_count,
                       const float* input_source,
                       float* output_capture);

    // IDeviceBackend.
    std::vector<BackendDeviceInfo> enumerate() override;
    std::vector<std::string> channelNames(const std::string& uid,
                                          std::uint32_t direction) override;
    IOProcId openInputCallback(const std::string& uid,
                               InputIOProcCallback callback,
                               void* user_data) override;
    IOProcId openOutputCallback(const std::string& uid,
                                OutputIOProcCallback callback,
                                void* user_data) override;
    IOProcId openDuplexCallback(const std::string& uid,
                                DuplexIOProcCallback callback,
                                void* user_data) override;
    bool closeCallback(IOProcId id) override;
    bool startDevice(const std::string& uid) override;
    void stopDevice(const std::string& uid) override;
    std::uint32_t currentBufferFrameSize(const std::string& uid) override;
    void setBufferFrameSize(const std::string& uid,
                            std::uint32_t frames) override;
    void setDeviceChangeListener(DeviceChangeListener cb,
                                 void* user_data) override;

    // ----- Phase 7.6.3 teardown-failure injection (test-only seams) -----
    //
    // The next `count` `closeCallback` invocations on this backend
    // will refuse to destroy the matching IOProc: closeCallback returns
    // false and the backend retains the slot's callback bookkeeping
    // (input_cb / input_id / etc. stay populated) so a subsequent
    // closeCallback(id) call retries the destroy. Once `count` is
    // exhausted, closeCallback resumes its normal success path.
    //
    // Injection state is global across IOProcs unless scoped via
    // `setCloseCallbackFailing(id, count)` — that variant only fails
    // when closing the named IOProcId, leaving siblings on the same
    // device unaffected. Per-id budgets are checked before the
    // global budget; an unknown / kInvalidIOProcId is never failed.
    void setNextCloseCallbacksFailing(std::uint32_t count);
    void setCloseCallbackFailing(IOProcId id, std::uint32_t count);

    // True when the named IOProcId is still registered to some slot
    // (input / output / duplex). After a normal closeCallback the
    // callback is gone and this returns false; after a failure-injected
    // close the bookkeeping survives and this returns true. False for
    // kInvalidIOProcId.
    bool isCallbackOpen(IOProcId id) const;

    // Slot-level introspection: any input / output / duplex callback
    // currently registered on `uid`. Lets tests assert mux teardown
    // residuals (failed close on the mux's IOProc keeps the slot
    // populated) without exposing the mux's private IOProc id.
    bool hasInputCallback(const std::string& uid) const;
    bool hasOutputCallback(const std::string& uid) const;
    bool hasDuplexCallback(const std::string& uid) const;

    // Test introspection: every `setBufferFrameSize` call (after
    // aggregate fan-out, one entry per uid) is recorded so tests
    // can assert which UIDs were touched and with what frame count.
    // The recorded `frames` is the request value passed to the API
    // -- not the post-resolution effective value (see
    // `setMaxAcrossClientsFloor` below) -- mirroring real macOS,
    // where the SetPropertyData call carries the request and the
    // post-call readback may report a different number.
    struct BufferSizeWrite {
        std::string   uid;
        std::uint32_t frames = 0;
    };
    const std::vector<BufferSizeWrite>& bufferSizeWrites() const {
        return buffer_size_writes_;
    }

    // Simulate macOS's `max-across-clients` resolution for the HAL
    // buffer-frame-size property. After this is set on `uid`,
    // subsequent `setBufferFrameSize(uid, request)` calls update
    // the device's effective buffer to `max(request, floor)` --
    // mimicking a co-resident client holding the device at `floor`
    // while it runs. The recorded `bufferSizeWrites()` entry still
    // carries the request value (matching what the API caller saw),
    // but `currentBufferFrameSize(uid)` returns the resolved value.
    // Default floor is 0 (request always wins). Persistent across
    // multiple writes; pass 0 to clear.
    void setMaxAcrossClientsFloor(const std::string& uid,
                                  std::uint32_t frames);

private:
    struct DeviceSlot {
        BackendDeviceInfo info;
        bool started = false;

        InputIOProcCallback  input_cb  = nullptr;
        void*                input_ud  = nullptr;
        IOProcId             input_id  = kInvalidIOProcId;

        OutputIOProcCallback output_cb = nullptr;
        void*                output_ud = nullptr;
        IOProcId             output_id = kInvalidIOProcId;

        DuplexIOProcCallback duplex_cb = nullptr;
        void*                duplex_ud = nullptr;
        IOProcId             duplex_id = kInvalidIOProcId;

        // Non-empty iff this device is an aggregate; each UID
        // references another device in `devices_`. Matches macOS
        // aggregate-device semantics.
        std::vector<std::string> sub_device_uids;

        // Test-seeded per-channel names; empty until the test populates
        // them via setChannelNames(). Element index i corresponds to
        // channel (i+1) — same 0-indexed convention we use everywhere
        // else, just rendered as 1-indexed in the UI.
        std::vector<std::string> input_channel_names;
        std::vector<std::string> output_channel_names;
    };

    std::unordered_map<std::string, DeviceSlot> devices_;
    IOProcId next_id_ = 1;  // 0 is reserved as kInvalidIOProcId.

    // Reusable working buffer for output callbacks, to avoid per-cycle
    // allocation in tests. Grown as needed.
    std::vector<float> output_scratch_;

    // Test introspection storage; populated by every
    // `setBufferFrameSize` call (one entry per uid touched, including
    // each aggregate sub-device fan-out target).
    std::vector<BufferSizeWrite> buffer_size_writes_;

    // Per-UID `max-across-clients` floor. Empty by default; populated
    // by `setMaxAcrossClientsFloor`. Each `setBufferFrameSize` lookup
    // resolves the effective buffer to `max(request, floor)`.
    std::unordered_map<std::string, std::uint32_t> mac_floor_;

    // Phase 7.6.3 teardown-failure injection state. `next_close_failures_
    // remaining_` is a global budget consumed by any closeCallback;
    // `per_id_close_failures_` scopes a budget to a specific IOProcId
    // (checked first). Both default to zero / empty so production
    // tests remain unaffected.
    std::uint32_t                                  next_close_failures_remaining_ = 0;
    std::unordered_map<IOProcId, std::uint32_t>    per_id_close_failures_;

    // Phase 7.6.4: stored device-change listener. simulateDevice*
    // helpers fire it synchronously; production tests can register
    // a watcher's static thunk here.
    DeviceChangeListener device_change_cb_      = nullptr;
    void*                device_change_user_    = nullptr;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_SIMULATED_BACKEND_HPP
