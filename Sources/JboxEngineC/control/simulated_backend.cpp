// simulated_backend.cpp — deterministic SimulatedBackend implementation.

#include "simulated_backend.hpp"

#include <algorithm>
#include <cstring>

namespace jbox::control {

void SimulatedBackend::addDevice(const BackendDeviceInfo& info) {
    DeviceSlot slot;
    slot.info = info;
    devices_[info.uid] = slot;
}

void SimulatedBackend::removeDevice(const std::string& uid) {
    devices_.erase(uid);
}

void SimulatedBackend::setChannelNames(const std::string& uid,
                                       std::uint32_t direction,
                                       std::vector<std::string> names) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return;
    if (direction == kBackendDirectionInput) {
        it->second.input_channel_names = std::move(names);
    } else if (direction == kBackendDirectionOutput) {
        it->second.output_channel_names = std::move(names);
    }
}

std::vector<std::string> SimulatedBackend::channelNames(
    const std::string& uid,
    std::uint32_t direction) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return {};
    if (direction == kBackendDirectionInput) {
        return it->second.input_channel_names;
    }
    if (direction == kBackendDirectionOutput) {
        return it->second.output_channel_names;
    }
    return {};
}

void SimulatedBackend::deliverBuffer(const std::string& uid,
                                     std::uint32_t frame_count,
                                     const float* input_source,
                                     float* output_capture) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return;
    DeviceSlot& slot = it->second;
    if (!slot.started) return;
    if (frame_count == 0) return;

    // Input direction.
    if (slot.input_cb != nullptr && input_source != nullptr) {
        slot.input_cb(input_source,
                      frame_count,
                      slot.info.input_channel_count,
                      slot.input_ud);
    }

    // Output direction.
    if (slot.output_cb != nullptr) {
        const std::size_t total_samples =
            static_cast<std::size_t>(frame_count) * slot.info.output_channel_count;
        if (output_scratch_.size() < total_samples) {
            output_scratch_.assign(total_samples, 0.0f);
        } else {
            std::fill_n(output_scratch_.begin(), total_samples, 0.0f);
        }
        slot.output_cb(output_scratch_.data(),
                       frame_count,
                       slot.info.output_channel_count,
                       slot.output_ud);
        if (output_capture != nullptr) {
            std::memcpy(output_capture,
                        output_scratch_.data(),
                        total_samples * sizeof(float));
        }
    }
}

std::vector<BackendDeviceInfo> SimulatedBackend::enumerate() {
    std::vector<BackendDeviceInfo> out;
    out.reserve(devices_.size());
    for (const auto& [uid, slot] : devices_) {
        out.push_back(slot.info);
    }
    return out;
}

IOProcId SimulatedBackend::openInputCallback(const std::string& uid,
                                             InputIOProcCallback callback,
                                             void* user_data) {
    if (callback == nullptr) return kInvalidIOProcId;
    auto it = devices_.find(uid);
    if (it == devices_.end()) return kInvalidIOProcId;
    DeviceSlot& slot = it->second;
    if ((slot.info.direction & kBackendDirectionInput) == 0) return kInvalidIOProcId;
    if (slot.input_cb != nullptr) return kInvalidIOProcId;  // already registered
    slot.input_cb = callback;
    slot.input_ud = user_data;
    slot.input_id = next_id_++;
    return slot.input_id;
}

IOProcId SimulatedBackend::openOutputCallback(const std::string& uid,
                                              OutputIOProcCallback callback,
                                              void* user_data) {
    if (callback == nullptr) return kInvalidIOProcId;
    auto it = devices_.find(uid);
    if (it == devices_.end()) return kInvalidIOProcId;
    DeviceSlot& slot = it->second;
    if ((slot.info.direction & kBackendDirectionOutput) == 0) return kInvalidIOProcId;
    if (slot.output_cb != nullptr) return kInvalidIOProcId;  // already registered
    slot.output_cb = callback;
    slot.output_ud = user_data;
    slot.output_id = next_id_++;
    return slot.output_id;
}

void SimulatedBackend::closeCallback(IOProcId id) {
    if (id == kInvalidIOProcId) return;
    for (auto& [uid, slot] : devices_) {
        if (slot.input_id == id) {
            slot.input_cb = nullptr;
            slot.input_ud = nullptr;
            slot.input_id = kInvalidIOProcId;
            return;
        }
        if (slot.output_id == id) {
            slot.output_cb = nullptr;
            slot.output_ud = nullptr;
            slot.output_id = kInvalidIOProcId;
            return;
        }
    }
    // Unknown id — silent no-op.
}

bool SimulatedBackend::startDevice(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return false;
    DeviceSlot& slot = it->second;
    if (slot.started) return false;  // already started
    slot.started = true;
    return true;
}

void SimulatedBackend::stopDevice(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return;
    it->second.started = false;
}

std::uint32_t SimulatedBackend::currentBufferFrameSize(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return 0;
    return it->second.info.buffer_frame_size;
}

std::uint32_t SimulatedBackend::requestBufferFrameSize(
    const std::string& uid, std::uint32_t frames) {
    auto it = devices_.find(uid);
    if (it == devices_.end() || frames == 0) {
        buffer_size_requests_.push_back({uid, frames, 0});
        return 0;
    }
    // No range clamping in the simulated backend — tests supply the
    // exact frame count they want recorded. The HAL-side clamping
    // lives in CoreAudioBackend::requestBufferFrameSize.
    it->second.info.buffer_frame_size = frames;
    buffer_size_requests_.push_back({uid, frames, frames});
    return frames;
}

}  // namespace jbox::control
