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
    DeviceIOMux(IDeviceBackend& backend,
                std::string uid,
                std::uint32_t input_channel_count,
                std::uint32_t output_channel_count);

    ~DeviceIOMux();

    DeviceIOMux(const DeviceIOMux&) = delete;
    DeviceIOMux& operator=(const DeviceIOMux&) = delete;

    // Attach a per-route input callback identified by `key`. Fails and
    // returns false if: `key` is already attached, the device has no
    // input channels, `callback` is null, or the backend refuses to
    // open the input IOProc on the first attach.
    //
    // `requested_buffer_frames` lets the route participate in the
    // mux's device-buffer negotiation. 0 means "no opinion" — the
    // route neither shrinks the buffer nor prevents another route's
    // shrink request. Non-zero enters this request into the mux's
    // min-across-requests refcount.
    //
    // `share_device` (Phase 7.5): when true, the attach participates
    // in the RT dispatch and in the buffer-size min, but does not
    // count toward the `non_sharing_attached` refcount that drives
    // hog-mode acquisition. Mixed-mode attachments on the same
    // device see hog mode claimed only while at least one
    // non-sharing route is attached; when the last non-sharing
    // route detaches the claim is released even if sharing routes
    // remain active.
    bool attachInput(void* key,
                     InputIOProcCallback callback,
                     void* user_data,
                     std::uint32_t requested_buffer_frames = 0,
                     bool share_device = false);

    // Detach the input callback previously registered with `key`.
    // No-op if `key` is not attached. Blocks the caller for one grace
    // period after the atomic swap; on return, `user_data` and any
    // resources it references are safe to destroy.
    void detachInput(void* key);

    bool attachOutput(void* key,
                      OutputIOProcCallback callback,
                      void* user_data,
                      std::uint32_t requested_buffer_frames = 0,
                      bool share_device = false);
    void detachOutput(void* key);

    bool hasAnyInput() const;
    bool hasAnyOutput() const;

    const std::string& uid() const { return uid_; }

private:
    struct InputEntry {
        void*               key = nullptr;
        InputIOProcCallback cb  = nullptr;
        void*               user_data = nullptr;
        // 0 = route has no opinion on buffer size. Non-zero takes
        // part in the mux's min-across-requests refcount.
        std::uint32_t       requested_buffer_frames = 0;
        // Phase 7.5: true = route opted out of hog-mode; this entry
        // does not contribute to `non_sharing_attached_`.
        bool                share_device = false;
    };
    struct OutputEntry {
        void*                key = nullptr;
        OutputIOProcCallback cb  = nullptr;
        void*                user_data = nullptr;
        std::uint32_t        requested_buffer_frames = 0;
        bool                 share_device = false;
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

    // Recompute the min across currently-attached non-zero requests
    // and push the result through to the backend — spans both input
    // and output directions because the HAL buffer-frame-size is a
    // global device property, not per-scope. On the 0→1 transition
    // the mux claims exclusive (hog-mode) ownership so the request
    // actually lands even when another app is holding the device at
    // a larger size. On the 1→0 transition it releases, which
    // restores the original buffer size from the backend's snapshot.
    void updateBufferRequest();
    std::uint32_t currentMinBufferRequest() const;

    IDeviceBackend&  backend_;
    std::string      uid_;
    std::uint32_t    input_channel_count_  = 0;
    std::uint32_t    output_channel_count_ = 0;

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

    // Buffer-negotiation state — control thread only.
    // `exclusive_claimed_` tracks whether we currently hold the
    // backend's exclusive (hog-mode) claim. The backend keeps the
    // per-device buffer snapshot internally under its claim state,
    // so the mux doesn't duplicate that storage.
    bool          exclusive_claimed_      = false;
    // Last buffer size we asked the backend for. Used to suppress
    // redundant calls when the min hasn't changed across attaches.
    std::uint32_t last_requested_frames_  = 0;
    // Phase 7.5: count of currently-attached entries with
    // `share_device == false`. Spans both input and output
    // directions because hog-mode is a per-device property.
    // claimExclusive fires on the 0→1 edge of this counter;
    // releaseExclusive fires on the 1→0 edge. Buffer-size min
    // negotiation stays independent of this counter (sharing
    // routes still participate in the buffer min).
    std::uint32_t non_sharing_attached_   = 0;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DEVICE_IO_MUX_HPP
