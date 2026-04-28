// device_change_watcher.cpp — DeviceChangeWatcher implementation.

#include "device_change_watcher.hpp"

namespace jbox::control {

DeviceChangeWatcher::DeviceChangeWatcher(IDeviceBackend& backend)
    : backend_(backend) {
    backend_.setDeviceChangeListener(&DeviceChangeWatcher::onBackendEvent,
                                     this);
}

DeviceChangeWatcher::~DeviceChangeWatcher() {
    // Clear the listener so the backend stops invoking our trampoline
    // even if it outlives us (CoreAudioBackend's HAL listener thread
    // could otherwise call into a destroyed instance).
    backend_.setDeviceChangeListener(nullptr, nullptr);
}

void DeviceChangeWatcher::onBackendEvent(const DeviceChangeEvent& ev,
                                         void* user_data) {
    auto* self = static_cast<DeviceChangeWatcher*>(user_data);
    if (self == nullptr) return;
    self->onEvent(ev);
}

void DeviceChangeWatcher::onEvent(const DeviceChangeEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(ev);
}

std::vector<DeviceChangeEvent> DeviceChangeWatcher::drain() {
    std::vector<DeviceChangeEvent> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(events_.size());
    while (!events_.empty()) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return out;
}

bool DeviceChangeWatcher::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_.empty();
}

}  // namespace jbox::control
