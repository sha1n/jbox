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

void SimulatedBackend::addAggregateDevice(const BackendDeviceInfo& info,
                                          std::vector<std::string> sub_device_uids) {
    DeviceSlot slot;
    slot.info = info;
    slot.sub_device_uids = std::move(sub_device_uids);
    devices_[info.uid] = std::move(slot);
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

    // Duplex direction. Runs once with both directions' buffers so
    // tests can exercise the direct-monitor fast path against the
    // simulated backend. The output buffer is zero-initialised before
    // the callback — matching Core Audio's IOProc contract.
    if (slot.duplex_cb != nullptr && input_source != nullptr) {
        const std::size_t out_total =
            static_cast<std::size_t>(frame_count) * slot.info.output_channel_count;
        if (output_scratch_.size() < out_total) {
            output_scratch_.assign(out_total, 0.0f);
        } else {
            std::fill_n(output_scratch_.begin(), out_total, 0.0f);
        }
        slot.duplex_cb(input_source,
                       frame_count,
                       slot.info.input_channel_count,
                       output_scratch_.data(),
                       frame_count,
                       slot.info.output_channel_count,
                       slot.duplex_ud);
        if (output_capture != nullptr) {
            std::memcpy(output_capture,
                        output_scratch_.data(),
                        out_total * sizeof(float));
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

IOProcId SimulatedBackend::openDuplexCallback(const std::string& uid,
                                              DuplexIOProcCallback callback,
                                              void* user_data) {
    if (callback == nullptr) return kInvalidIOProcId;
    auto it = devices_.find(uid);
    if (it == devices_.end()) return kInvalidIOProcId;
    DeviceSlot& slot = it->second;
    const bool duplex_capable =
        (slot.info.direction & kBackendDirectionInput)  != 0 &&
        (slot.info.direction & kBackendDirectionOutput) != 0;
    if (!duplex_capable) return kInvalidIOProcId;
    if (slot.duplex_cb != nullptr) return kInvalidIOProcId;  // already registered
    if (slot.input_cb  != nullptr) return kInvalidIOProcId;
    if (slot.output_cb != nullptr) return kInvalidIOProcId;
    slot.duplex_cb = callback;
    slot.duplex_ud = user_data;
    slot.duplex_id = next_id_++;
    return slot.duplex_id;
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
        if (slot.duplex_id == id) {
            slot.duplex_cb = nullptr;
            slot.duplex_ud = nullptr;
            slot.duplex_id = kInvalidIOProcId;
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

bool SimulatedBackend::claimExclusive(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return false;

    ExclusiveSnapshot snap;

    // Fan out to sub-devices first (aggregate semantics), then self.
    // A member that's already hogged by someone else returns false on
    // macOS; we mirror that.
    for (const auto& sub_uid : it->second.sub_device_uids) {
        auto sub = devices_.find(sub_uid);
        if (sub == devices_.end() || sub->second.exclusive_claimed) {
            // Roll back already-hogged members before failing.
            for (const auto& other : it->second.sub_device_uids) {
                if (other == sub_uid) break;
                auto o = devices_.find(other);
                if (o != devices_.end()) o->second.exclusive_claimed = false;
            }
            return false;
        }
        sub->second.exclusive_claimed = true;
        snap.sub_devices.push_back({sub_uid, sub->second.info.buffer_frame_size});
    }
    if (it->second.exclusive_claimed) {
        // Self already claimed — roll back subs and bail.
        for (const auto& sub_uid : it->second.sub_device_uids) {
            auto sub = devices_.find(sub_uid);
            if (sub != devices_.end()) sub->second.exclusive_claimed = false;
        }
        return false;
    }
    it->second.exclusive_claimed = true;
    snap.self = {uid, it->second.info.buffer_frame_size};
    exclusive_state_[uid] = std::move(snap);
    return true;
}

void SimulatedBackend::releaseExclusive(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return;

    // Restore buffer sizes from the snapshot, then release the hog
    // flag on self + sub-devices. Mirrors CoreAudioBackend's ordering.
    auto snap_it = exclusive_state_.find(uid);
    if (snap_it != exclusive_state_.end()) {
        if (snap_it->second.self.original_buffer_frames > 0) {
            it->second.info.buffer_frame_size =
                snap_it->second.self.original_buffer_frames;
        }
        for (const auto& sub : snap_it->second.sub_devices) {
            auto sub_it = devices_.find(sub.uid);
            if (sub_it != devices_.end() && sub.original_buffer_frames > 0) {
                sub_it->second.info.buffer_frame_size = sub.original_buffer_frames;
            }
        }
        exclusive_state_.erase(snap_it);
    }

    it->second.exclusive_claimed = false;
    for (const auto& sub_uid : it->second.sub_device_uids) {
        auto sub = devices_.find(sub_uid);
        if (sub != devices_.end()) sub->second.exclusive_claimed = false;
    }
}

IDeviceBackend::BufferFrameSizeRange
SimulatedBackend::supportedBufferFrameSizeRange(const std::string& uid) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return {};
    BufferFrameSizeRange r{it->second.buffer_frame_size_min,
                           it->second.buffer_frame_size_max};
    for (const auto& sub_uid : it->second.sub_device_uids) {
        auto sub = devices_.find(sub_uid);
        if (sub == devices_.end()) continue;
        if (sub->second.buffer_frame_size_min > r.minimum) {
            r.minimum = sub->second.buffer_frame_size_min;
        }
        if (sub->second.buffer_frame_size_max < r.maximum) {
            r.maximum = sub->second.buffer_frame_size_max;
        }
    }
    if (r.maximum > 0 && r.minimum > r.maximum) return {};
    return r;
}

void SimulatedBackend::setBufferFrameSizeRange(const std::string& uid,
                                               std::uint32_t minimum,
                                               std::uint32_t maximum) {
    auto it = devices_.find(uid);
    if (it == devices_.end()) return;
    it->second.buffer_frame_size_min = minimum;
    it->second.buffer_frame_size_max = maximum;
}

std::uint32_t SimulatedBackend::requestBufferFrameSize(
    const std::string& uid, std::uint32_t frames) {
    auto it = devices_.find(uid);
    if (it == devices_.end() || frames == 0) {
        buffer_size_requests_.push_back({uid, frames, 0});
        return 0;
    }
    // Aggregate semantics: fan out to each sub-device (recording each
    // request so tests can assert it happened), then apply to the
    // aggregate itself. No range clamping in the simulated backend —
    // tests supply the exact frame count they want recorded. HAL-side
    // clamping lives in CoreAudioBackend::requestBufferFrameSize.
    for (const auto& sub_uid : it->second.sub_device_uids) {
        auto sub = devices_.find(sub_uid);
        if (sub == devices_.end()) {
            buffer_size_requests_.push_back({sub_uid, frames, 0});
            continue;
        }
        sub->second.info.buffer_frame_size = frames;
        buffer_size_requests_.push_back({sub_uid, frames, frames});
    }
    it->second.info.buffer_frame_size = frames;
    buffer_size_requests_.push_back({uid, frames, frames});
    return frames;
}

}  // namespace jbox::control
