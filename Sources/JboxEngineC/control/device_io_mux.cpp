// device_io_mux.cpp — DeviceIOMux implementation.

#include "device_io_mux.hpp"

#include <thread>
#include <utility>

namespace jbox::control {


DeviceIOMux::DeviceIOMux(IDeviceBackend& backend,
                         std::string uid,
                         std::uint32_t input_channel_count,
                         std::uint32_t output_channel_count)
    : backend_(backend),
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
    if (input_ioproc_id_ != kInvalidIOProcId) {
        backend_.closeCallback(input_ioproc_id_);
        input_ioproc_id_ = kInvalidIOProcId;
    }
    if (output_ioproc_id_ != kInvalidIOProcId) {
        backend_.closeCallback(output_ioproc_id_);
        output_ioproc_id_ = kInvalidIOProcId;
    }
    backend_.stopDevice(uid_);
}

bool DeviceIOMux::attachInput(void* key,
                              InputIOProcCallback callback,
                              void* user_data,
                              std::uint32_t requested_buffer_frames) {
    if (input_channel_count_ == 0 || callback == nullptr) return false;

    auto next = std::make_unique<InputList>();
    if (input_list_) {
        next->reserve(input_list_->size() + 1);
        for (const auto& e : *input_list_) {
            if (e.key == key) return false;  // already attached
            next->push_back(e);
        }
    }
    next->push_back({key, callback, user_data, requested_buffer_frames});

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
    updateBufferRequest();
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
        backend_.closeCallback(input_ioproc_id_);
        input_ioproc_id_ = kInvalidIOProcId;
    }
    maybeStopDevice();
    updateBufferRequest();
}

bool DeviceIOMux::attachOutput(void* key,
                               OutputIOProcCallback callback,
                               void* user_data,
                               std::uint32_t requested_buffer_frames) {
    if (output_channel_count_ == 0 || callback == nullptr) return false;

    auto next = std::make_unique<OutputList>();
    if (output_list_) {
        next->reserve(output_list_->size() + 1);
        for (const auto& e : *output_list_) {
            if (e.key == key) return false;
            next->push_back(e);
        }
    }
    next->push_back({key, callback, user_data, requested_buffer_frames});

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
    updateBufferRequest();
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
        backend_.closeCallback(output_ioproc_id_);
        output_ioproc_id_ = kInvalidIOProcId;
    }
    maybeStopDevice();
    updateBufferRequest();
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

std::uint32_t DeviceIOMux::currentMinBufferRequest() const {
    std::uint32_t min = 0;
    auto take = [&min](std::uint32_t v) {
        if (v == 0) return;
        if (min == 0 || v < min) min = v;
    };
    if (input_list_) {
        for (const auto& e : *input_list_) take(e.requested_buffer_frames);
    }
    if (output_list_) {
        for (const auto& e : *output_list_) take(e.requested_buffer_frames);
    }
    return min;
}

void DeviceIOMux::updateBufferRequest() {
    const std::uint32_t target = currentMinBufferRequest();

    if (target == 0) {
        // No active requester — release exclusive ownership if we
        // had claimed it. Backend's releaseExclusive restores the
        // original buffer size.
        if (exclusive_claimed_) {
            backend_.releaseExclusive(uid_);
            exclusive_claimed_ = false;
        }
        last_requested_frames_ = 0;
        return;
    }

    // At least one route wants a specific target. Claim exclusive on
    // the 0→1 transition so the buffer-size request actually lands
    // (the backend's max-across-clients policy would otherwise let
    // another app's larger preference win). Claim failure is non-
    // fatal; the route continues on the shared-client path and
    // accepts whatever the HAL ends up giving.
    if (!exclusive_claimed_) {
        exclusive_claimed_ = backend_.claimExclusive(uid_);
    }

    // Re-apply only if the min changed, to avoid churning the HAL
    // every time a new route attaches with the same target.
    if (target != last_requested_frames_) {
        (void)backend_.requestBufferFrameSize(uid_, target);
        last_requested_frames_ = target;
    }
}

}  // namespace jbox::control
