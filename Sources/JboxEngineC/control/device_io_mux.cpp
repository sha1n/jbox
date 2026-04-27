// device_io_mux.cpp — DeviceIOMux implementation.

#include "device_io_mux.hpp"

#include "rt_log_codes.hpp"

#include <atomic>
#include <thread>
#include <utility>

namespace jbox::control {

namespace {

// Push a kLogTeardownFailure event into the borrowed log queue. The
// route_id is 0 because the mux is a per-device resource shared across
// routes — there is no single owning route to attribute the failure to.
// `value_b` carries the IOProcId so an operator can correlate this log
// with whichever route's start emitted the matching IOProcId in its
// own kLogRouteStarted record. Best-effort: drops on a full queue.
inline void pushTeardownFailure(jbox::rt::DefaultRtLogQueue* q,
                                IOProcId failing_id) {
    if (q == nullptr) return;
    static std::atomic<std::uint64_t> seq{0};
    jbox::rt::RtLogEvent ev{};
    ev.timestamp = seq.fetch_add(1, std::memory_order_relaxed) + 1;
    ev.code      = jbox::rt::kLogTeardownFailure;
    ev.route_id  = 0;
    ev.value_a   = 0;  // backend status code (placeholder; engine-side push)
    ev.value_b   = static_cast<std::uint64_t>(failing_id);
    (void)q->tryPush(ev);
}

}  // namespace


DeviceIOMux::DeviceIOMux(IDeviceBackend& backend,
                         std::string uid,
                         std::uint32_t input_channel_count,
                         std::uint32_t output_channel_count,
                         jbox::rt::DefaultRtLogQueue* log_queue)
    : backend_(backend),
      log_queue_(log_queue),
      uid_(std::move(uid)),
      input_channel_count_(input_channel_count),
      output_channel_count_(output_channel_count) {}

DeviceIOMux::~DeviceIOMux() {
    // Null the atomic pointers so any future RT callback observes
    // "no work" and returns immediately. Then drain any in-flight
    // iterations before we let the unique_ptr fields destroy the
    // underlying vectors.
    input_routes_.store(nullptr, std::memory_order_release);
    output_routes_.store(nullptr, std::memory_order_release);
    waitForInputQuiescence();
    waitForOutputQuiescence();

    // closeCallback is synchronous on CoreAudio (AudioDeviceStop waits
    // for the last IOProc execution before returning) and a plain
    // pointer clear on SimulatedBackend; both are safe once the
    // atomic pointers are null and the exit seqs have caught up.
    //
    // 7.6.3 contract: on the destructor path we make a best-effort
    // attempt and log loudly on refusal. There is no further retry
    // surface here — the mux is being destroyed — so a stuck IOProc
    // becomes a logged kernel-side leak that 7.6.4's HAL listeners
    // will surface and recover the next time the device topology
    // changes.
    if (input_ioproc_id_ != kInvalidIOProcId) {
        if (!backend_.closeCallback(input_ioproc_id_)) {
            pushTeardownFailure(log_queue_, input_ioproc_id_);
        }
        input_ioproc_id_ = kInvalidIOProcId;
    }
    if (output_ioproc_id_ != kInvalidIOProcId) {
        if (!backend_.closeCallback(output_ioproc_id_)) {
            pushTeardownFailure(log_queue_, output_ioproc_id_);
        }
        output_ioproc_id_ = kInvalidIOProcId;
    }
    backend_.stopDevice(uid_);
}

bool DeviceIOMux::attachInput(void* key,
                              InputIOProcCallback callback,
                              void* user_data) {
    if (input_channel_count_ == 0 || callback == nullptr) return false;

    auto next = std::make_unique<InputList>();
    if (input_list_) {
        next->reserve(input_list_->size() + 1);
        for (const auto& e : *input_list_) {
            if (e.key == key) return false;  // already attached
            next->push_back(e);
        }
    }
    next->push_back({key, callback, user_data});

    const bool first = (input_ioproc_id_ == kInvalidIOProcId);
    if (first) {
        input_ioproc_id_ = backend_.openInputCallback(
            uid_, &inputTrampoline, this);
        if (input_ioproc_id_ == kInvalidIOProcId) return false;
    }

    input_routes_.store(next.get(), std::memory_order_release);
    std::unique_ptr<InputList> old = std::move(input_list_);
    input_list_ = std::move(next);
    waitForInputQuiescence();
    // `old` deleted here, after any in-flight RT iteration on it exits.

    if (first) {
        // startDevice returns false if the device was already started
        // (e.g., the opposite direction registered first, or something
        // external started it). That's acceptable — the IOProc will
        // receive callbacks either way.
        backend_.startDevice(uid_);
    }
    return true;
}

void DeviceIOMux::detachInput(void* key) {
    if (!input_list_ || input_list_->empty()) return;

    auto next = std::make_unique<InputList>();
    next->reserve(input_list_->size());
    for (const auto& e : *input_list_) {
        if (e.key != key) next->push_back(e);
    }
    if (next->size() == input_list_->size()) return;  // key not found

    const bool now_empty = next->empty();
    const InputList* published = now_empty ? nullptr : next.get();
    input_routes_.store(published, std::memory_order_release);
    std::unique_ptr<InputList> old = std::move(input_list_);
    if (now_empty) {
        input_list_.reset();
    } else {
        input_list_ = std::move(next);
    }
    waitForInputQuiescence();
    // `old` (and `next` if we didn't take it) deleted here.

    if (now_empty && input_ioproc_id_ != kInvalidIOProcId) {
        // 7.6.3: the destroy may refuse on a degraded / hot-unplugged
        // device. On refusal we keep input_ioproc_id_ populated so the
        // mux destructor's best-effort retry — or, transiently, a
        // sibling output detach raising maybeStopDevice — has the
        // handle to retry against. Either way the failure is logged
        // immediately (kLogTeardownFailure with the failing IOProcId).
        if (backend_.closeCallback(input_ioproc_id_)) {
            input_ioproc_id_ = kInvalidIOProcId;
        } else {
            pushTeardownFailure(log_queue_, input_ioproc_id_);
        }
    }
    maybeStopDevice();
}

bool DeviceIOMux::attachOutput(void* key,
                               OutputIOProcCallback callback,
                               void* user_data) {
    if (output_channel_count_ == 0 || callback == nullptr) return false;

    auto next = std::make_unique<OutputList>();
    if (output_list_) {
        next->reserve(output_list_->size() + 1);
        for (const auto& e : *output_list_) {
            if (e.key == key) return false;
            next->push_back(e);
        }
    }
    next->push_back({key, callback, user_data});

    const bool first = (output_ioproc_id_ == kInvalidIOProcId);
    if (first) {
        output_ioproc_id_ = backend_.openOutputCallback(
            uid_, &outputTrampoline, this);
        if (output_ioproc_id_ == kInvalidIOProcId) return false;
    }

    output_routes_.store(next.get(), std::memory_order_release);
    std::unique_ptr<OutputList> old = std::move(output_list_);
    output_list_ = std::move(next);
    waitForOutputQuiescence();

    if (first) {
        backend_.startDevice(uid_);
    }
    return true;
}

void DeviceIOMux::detachOutput(void* key) {
    if (!output_list_ || output_list_->empty()) return;

    auto next = std::make_unique<OutputList>();
    next->reserve(output_list_->size());
    for (const auto& e : *output_list_) {
        if (e.key != key) next->push_back(e);
    }
    if (next->size() == output_list_->size()) return;

    const bool now_empty = next->empty();
    const OutputList* published = now_empty ? nullptr : next.get();
    output_routes_.store(published, std::memory_order_release);
    std::unique_ptr<OutputList> old = std::move(output_list_);
    if (now_empty) {
        output_list_.reset();
    } else {
        output_list_ = std::move(next);
    }
    waitForOutputQuiescence();

    if (now_empty && output_ioproc_id_ != kInvalidIOProcId) {
        if (backend_.closeCallback(output_ioproc_id_)) {
            output_ioproc_id_ = kInvalidIOProcId;
        } else {
            pushTeardownFailure(log_queue_, output_ioproc_id_);
        }
    }
    maybeStopDevice();
}

bool DeviceIOMux::hasAnyInput() const {
    return input_list_ && !input_list_->empty();
}

bool DeviceIOMux::hasAnyOutput() const {
    return output_list_ && !output_list_->empty();
}

void DeviceIOMux::inputTrampoline(const float* samples,
                                  std::uint32_t frame_count,
                                  std::uint32_t channel_count,
                                  void* user_data) {
    auto* self = static_cast<DeviceIOMux*>(user_data);
    self->input_rt_enter_seq_.fetch_add(1, std::memory_order_acq_rel);
    const InputList* list =
        self->input_routes_.load(std::memory_order_acquire);
    if (list != nullptr) {
        for (const auto& e : *list) {
            e.cb(samples, frame_count, channel_count, e.user_data);
        }
    }
    self->input_rt_exit_seq_.fetch_add(1, std::memory_order_acq_rel);
}

void DeviceIOMux::outputTrampoline(float* samples,
                                   std::uint32_t frame_count,
                                   std::uint32_t channel_count,
                                   void* user_data) {
    auto* self = static_cast<DeviceIOMux*>(user_data);
    self->output_rt_enter_seq_.fetch_add(1, std::memory_order_acq_rel);
    const OutputList* list =
        self->output_routes_.load(std::memory_order_acquire);
    if (list != nullptr) {
        // v1 channel-mapping rules disallow two routes writing to the
        // same destination channel on the same device, so per-route
        // writes are to disjoint channel subsets and order doesn't
        // matter.
        for (const auto& e : *list) {
            e.cb(samples, frame_count, channel_count, e.user_data);
        }
    }
    self->output_rt_exit_seq_.fetch_add(1, std::memory_order_acq_rel);
}

void DeviceIOMux::waitForInputQuiescence() {
    const auto target =
        input_rt_enter_seq_.load(std::memory_order_acquire);
    while (input_rt_exit_seq_.load(std::memory_order_acquire) < target) {
        std::this_thread::yield();
    }
}

void DeviceIOMux::waitForOutputQuiescence() {
    const auto target =
        output_rt_enter_seq_.load(std::memory_order_acquire);
    while (output_rt_exit_seq_.load(std::memory_order_acquire) < target) {
        std::this_thread::yield();
    }
}

void DeviceIOMux::maybeStopDevice() {
    if (input_ioproc_id_ == kInvalidIOProcId &&
        output_ioproc_id_ == kInvalidIOProcId) {
        backend_.stopDevice(uid_);
    }
}

}  // namespace jbox::control
