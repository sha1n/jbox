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

    // Remove a simulated device. Equivalent to "unplugging" it:
    // its callbacks are quietly dropped and started state is cleared.
    // Safe to call with an unknown UID (no-op).
    void removeDevice(const std::string& uid);

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
    IOProcId openInputCallback(const std::string& uid,
                               InputIOProcCallback callback,
                               void* user_data) override;
    IOProcId openOutputCallback(const std::string& uid,
                                OutputIOProcCallback callback,
                                void* user_data) override;
    void closeCallback(IOProcId id) override;
    bool startDevice(const std::string& uid) override;
    void stopDevice(const std::string& uid) override;

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
    };

    std::unordered_map<std::string, DeviceSlot> devices_;
    IOProcId next_id_ = 1;  // 0 is reserved as kInvalidIOProcId.

    // Reusable working buffer for output callbacks, to avoid per-cycle
    // allocation in tests. Grown as needed.
    std::vector<float> output_scratch_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_SIMULATED_BACKEND_HPP
