// device_io_mux.hpp — per-device IOProc multiplexer (RCU-style dispatch).
//
// Owns at most one backend input IOProc and one output IOProc on a
// single device. Internally publishes an atomic snapshot of attached
// per-route callbacks; the RT trampoline loads the snapshot with
// acquire ordering and dispatches each attached callback in turn. This
// is how several routes can share a source or destination device
// behind a single registered Core Audio IOProc (see docs/spec.md § 2.7).
//
// Threading:
//   * attachInput / detachInput / attachOutput / detachOutput run on
//     the engine's control thread and must NOT be called from the RT
//     thread.
//   * inputTrampoline / outputTrampoline run on the backend's RT thread
//     (Core Audio IOProc thread in production, whichever thread calls
//     SimulatedBackend::deliverBuffer in tests).
//
// After every atomic list swap the mux sleeps one grace period on the
// control thread (`grace_period_seconds`) so any callback still running
// on the just-retired list has returned before the old list is freed.
// This is the "lightweight RCU grace period" from docs/spec.md § 2.3;
// 1.5 × device buffer period is a reasonable default.
//
// Lifecycle:
//   * First attach in a direction: backend openInputCallback /
//     openOutputCallback plus startDevice.
//   * Last detach in a direction: backend closeCallback; stopDevice
//     once both directions are empty.
//   * Destructor: forces detach and releases the IOProcs / stops the
//     device cleanly.

#ifndef JBOX_CONTROL_DEVICE_IO_MUX_HPP
#define JBOX_CONTROL_DEVICE_IO_MUX_HPP

#include "device_backend.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jbox::control {

class DeviceIOMux {
public:
    // `input_channel_count` / `output_channel_count` are copied from
    // the backend's enumeration for this device. Pass 0 for a
    // direction the device does not support — the corresponding
    // attach*() call will then refuse with `false`.
    //
    // `grace_period_seconds` is the sleep duration applied on the
    // control thread after each atomic list swap and before the old
    // list is released. Pick a value ≥ 1.5× the device's buffer period
    // so that a callback already in flight on the retired list has
    // time to return. Values ≤ 0 skip the sleep (useful for tests that
    // drive the backend on the same thread as the control code).
    DeviceIOMux(IDeviceBackend& backend,
                std::string uid,
                std::uint32_t input_channel_count,
                std::uint32_t output_channel_count,
                double grace_period_seconds);

    ~DeviceIOMux();

    DeviceIOMux(const DeviceIOMux&) = delete;
    DeviceIOMux& operator=(const DeviceIOMux&) = delete;

    // Attach a per-route input callback identified by `key`. Fails and
    // returns false if: `key` is already attached, the device has no
    // input channels, `callback` is null, or the backend refuses to
    // open the input IOProc on the first attach.
    bool attachInput(void* key,
                     InputIOProcCallback callback,
                     void* user_data);

    // Detach the input callback previously registered with `key`.
    // No-op if `key` is not attached. Blocks the caller for one grace
    // period after the atomic swap; on return, `user_data` and any
    // resources it references are safe to destroy.
    void detachInput(void* key);

    bool attachOutput(void* key,
                      OutputIOProcCallback callback,
                      void* user_data);
    void detachOutput(void* key);

    bool hasAnyInput() const;
    bool hasAnyOutput() const;

    const std::string& uid() const { return uid_; }

private:
    struct InputEntry {
        void*               key = nullptr;
        InputIOProcCallback cb  = nullptr;
        void*               user_data = nullptr;
    };
    struct OutputEntry {
        void*                key = nullptr;
        OutputIOProcCallback cb  = nullptr;
        void*                user_data = nullptr;
    };
    using InputList  = std::vector<InputEntry>;
    using OutputList = std::vector<OutputEntry>;

    static void inputTrampoline(const float* samples,
                                std::uint32_t frame_count,
                                std::uint32_t channel_count,
                                void* user_data);
    static void outputTrampoline(float* samples,
                                 std::uint32_t frame_count,
                                 std::uint32_t channel_count,
                                 void* user_data);

    void waitGrace() const;
    void maybeStopDevice();

    IDeviceBackend&  backend_;
    std::string      uid_;
    std::uint32_t    input_channel_count_  = 0;
    std::uint32_t    output_channel_count_ = 0;
    double           grace_period_seconds_ = 0.0;

    IOProcId         input_ioproc_id_  = kInvalidIOProcId;
    IOProcId         output_ioproc_id_ = kInvalidIOProcId;

    // Published lists. The RT trampoline loads these with
    // memory_order_acquire; control-thread mutations store the new
    // pointer with memory_order_release and then wait one grace period
    // before dropping the old list.
    std::atomic<const InputList*>  input_routes_{nullptr};
    std::atomic<const OutputList*> output_routes_{nullptr};

    // Owning storage for the currently-published lists.
    std::unique_ptr<InputList>  input_list_;
    std::unique_ptr<OutputList> output_list_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DEVICE_IO_MUX_HPP
