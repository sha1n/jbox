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
// Grace-period discipline: after each atomic list swap the control
// thread waits until any RT callback already in flight on the
// just-retired list has returned. This is done via a pair of
// monotonically-increasing sequence counters (`*_rt_enter_seq_` and
// `*_rt_exit_seq_`) that the trampolines bump on entry and exit. The
// control thread snapshots the current enter-seq and spins on
// `std::this_thread::yield()` until exit-seq catches up. This gives
// TSan-visible release/acquire synchronisation between the RT thread's
// last use of the old list and the control thread's subsequent delete.
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
#include "rt_log_queue.hpp"

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
    // `log_queue` is borrowed; pass nullptr to disable mux-side logging
    // (kLogTeardownFailure on a refused IOProc destroy is the only
    // event today). Lifetime must outlive the mux.
    DeviceIOMux(IDeviceBackend& backend,
                std::string uid,
                std::uint32_t input_channel_count,
                std::uint32_t output_channel_count,
                jbox::rt::DefaultRtLogQueue* log_queue = nullptr);

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

    void waitForInputQuiescence();
    void waitForOutputQuiescence();
    void maybeStopDevice();

    IDeviceBackend&             backend_;
    jbox::rt::DefaultRtLogQueue* log_queue_ = nullptr;
    std::string                 uid_;
    std::uint32_t               input_channel_count_  = 0;
    std::uint32_t               output_channel_count_ = 0;

    IOProcId         input_ioproc_id_  = kInvalidIOProcId;
    IOProcId         output_ioproc_id_ = kInvalidIOProcId;

    // Published lists. The RT trampoline loads these with
    // memory_order_acquire; control-thread mutations store the new
    // pointer with memory_order_release, then wait for any in-flight
    // RT iteration on the old list to exit before dropping it.
    std::atomic<const InputList*>  input_routes_{nullptr};
    std::atomic<const OutputList*> output_routes_{nullptr};

    // Sequence counters for the RT-side grace period. The trampolines
    // bump enter on entry and exit on exit; the control thread spins
    // on a yield-loop until exit catches up with a prior enter
    // snapshot. Acq/rel on the fetch_adds is what gives TSan a
    // happens-before edge between the RT thread's last use of the old
    // list and the control thread's subsequent delete.
    std::atomic<std::uint64_t> input_rt_enter_seq_{0};
    std::atomic<std::uint64_t> input_rt_exit_seq_{0};
    std::atomic<std::uint64_t> output_rt_enter_seq_{0};
    std::atomic<std::uint64_t> output_rt_exit_seq_{0};

    // Owning storage for the currently-published lists.
    std::unique_ptr<InputList>  input_list_;
    std::unique_ptr<OutputList> output_list_;

};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DEVICE_IO_MUX_HPP
