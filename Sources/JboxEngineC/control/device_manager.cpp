// device_manager.cpp — UID-keyed registry over IDeviceBackend.

#include "device_manager.hpp"

#include <utility>

namespace jbox::control {

DeviceManager::DeviceManager(std::unique_ptr<IDeviceBackend> backend)
    : backend_(std::move(backend)) {}

const std::vector<BackendDeviceInfo>& DeviceManager::refresh() {
    devices_ = backend_->enumerate();
    uid_index_.clear();
    uid_index_.reserve(devices_.size());
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        uid_index_[devices_[i].uid] = i;
    }
    return devices_;
}

const BackendDeviceInfo* DeviceManager::findByUid(const std::string& uid) const {
    auto it = uid_index_.find(uid);
    if (it == uid_index_.end()) return nullptr;
    return &devices_[it->second];
}

void DeviceManager::appendAggregateMembers(std::vector<std::string>& out,
                                            const std::string&        uid) const {
    const BackendDeviceInfo* info = findByUid(uid);
    if (info == nullptr || !info->is_aggregate) return;
    for (const auto& member_uid : info->aggregate_member_uids) {
        out.push_back(member_uid);
    }
}

}  // namespace jbox::control
