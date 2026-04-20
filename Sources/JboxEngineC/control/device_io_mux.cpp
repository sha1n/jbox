// device_io_mux.cpp — DeviceIOMux implementation.

#include "device_io_mux.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace jbox::control {

DeviceIOMux::DeviceIOMux(IDeviceBackend& backend,
                         std::string uid,
                         std::uint32_t input_channel_count,
                         std::uint32_t output_channel_count,
                         double grace_period_seconds)
    : backend_(backend),
      uid_(std::move(uid)),
      input_channel_count_(input_channel_count),
      output_channel_count_(output_channel_count),
      grace_period_seconds_(grace_period_seconds) {}

DeviceIOMux::~DeviceIOMux() {
    // Null the atomic pointers so any future RT callback observes
    // "no work" and returns immediately.
    input_routes_.store(nullptr, std::memory_order_release);
    output_routes_.store(nullptr, std::memory_order_release);
    waitGrace();

    // closeCallback is synchronous on CoreAudio (AudioDeviceStop waits
    // for the last IOProc execution before returning) and a plain
    // pointer clear on SimulatedBackend, so no extra grace is needed
    // after this point.
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
    waitGrace();
    // `old` deleted here.

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
    waitGrace();
    // `old` (and `next` if we didn't take it) deleted here.

    if (now_empty && input_ioproc_id_ != kInvalidIOProcId) {
        backend_.closeCallback(input_ioproc_id_);
        input_ioproc_id_ = kInvalidIOProcId;
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
    waitGrace();

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
    waitGrace();

    if (now_empty && output_ioproc_id_ != kInvalidIOProcId) {
        backend_.closeCallback(output_ioproc_id_);
        output_ioproc_id_ = kInvalidIOProcId;
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
    const InputList* list =
        self->input_routes_.load(std::memory_order_acquire);
    if (list == nullptr) return;
    for (const auto& e : *list) {
        e.cb(samples, frame_count, channel_count, e.user_data);
    }
}

void DeviceIOMux::outputTrampoline(float* samples,
                                   std::uint32_t frame_count,
                                   std::uint32_t channel_count,
                                   void* user_data) {
    auto* self = static_cast<DeviceIOMux*>(user_data);
    const OutputList* list =
        self->output_routes_.load(std::memory_order_acquire);
    if (list == nullptr) return;
    // v1 channel-mapping invariants forbid two routes writing to the
    // same destination channel on the same device, so per-route writes
    // are to disjoint channel subsets and order doesn't matter.
    for (const auto& e : *list) {
        e.cb(samples, frame_count, channel_count, e.user_data);
    }
}

void DeviceIOMux::waitGrace() const {
    if (grace_period_seconds_ <= 0.0) return;
    const auto ns =
        static_cast<std::int64_t>(grace_period_seconds_ * 1e9);
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

void DeviceIOMux::maybeStopDevice() {
    if (input_ioproc_id_ == kInvalidIOProcId &&
        output_ioproc_id_ == kInvalidIOProcId) {
        backend_.stopDevice(uid_);
    }
}

}  // namespace jbox::control
