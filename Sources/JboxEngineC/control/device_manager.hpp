// device_manager.hpp — engine-side UID-keyed device registry.
//
// Wraps an IDeviceBackend so the rest of the engine can look devices
// up by their stable UID without caring about the AudioDeviceID or
// backend-specific bookkeeping. Caches the most recent enumeration
// snapshot; refresh() re-polls the backend and updates the cache.
//
// A DeviceManager owns its backend (taking a unique_ptr on
// construction). Swap out the implementation by passing a different
// IDeviceBackend subclass: SimulatedBackend for tests, CoreAudioBackend
// for production.
//
// Thread model: single-threaded from the engine control thread. Not
// safe to call concurrently from multiple threads.
//
// Phase 3 scope: enumeration + lookup + backend access for
// RouteManager. Hot-plug property listeners and refcounted IOProc
// sharing are deferred (see docs/spec.md §§ 2.2, 2.7 and Phase 5).

#ifndef JBOX_CONTROL_DEVICE_MANAGER_HPP
#define JBOX_CONTROL_DEVICE_MANAGER_HPP

#include "device_backend.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jbox::control {

class DeviceManager {
public:
    // Takes ownership of the backend. The backend remains valid for
    // the lifetime of this DeviceManager.
    explicit DeviceManager(std::unique_ptr<IDeviceBackend> backend);

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    ~DeviceManager() = default;

    // Re-poll the backend. Updates the cached snapshot. Returns a
    // const reference to the same vector exposed by devices().
    const std::vector<BackendDeviceInfo>& refresh();

    // Current cached snapshot. May be stale if the caller has not
    // called refresh() since a device topology change.
    const std::vector<BackendDeviceInfo>& devices() const { return devices_; }

    // Look up a device by UID in the cached snapshot. Returns nullptr
    // if no device with that UID is currently known.
    const BackendDeviceInfo* findByUid(const std::string& uid) const;

    // Whether a device with the given UID is present in the cached
    // snapshot. Equivalent to findByUid(uid) != nullptr.
    bool isPresent(const std::string& uid) const {
        return findByUid(uid) != nullptr;
    }

    // Phase 7.6.6 helper: when `uid` belongs to an aggregate device in
    // the cached snapshot, push each of its active sub-device UIDs
    // onto `out`. Pure read; no-op for non-aggregate / unknown UIDs.
    // Used by RouteManager::attemptStart to expand a route's watched-
    // UID set to include sub-devices whose loss must fail the route.
    void appendAggregateMembers(std::vector<std::string>& out,
                                const std::string&        uid) const;

    // Backend access for RouteManager / other engine components that
    // need to register IOProcs or drive start/stop directly.
    IDeviceBackend& backend() { return *backend_; }

private:
    std::unique_ptr<IDeviceBackend> backend_;
    std::vector<BackendDeviceInfo> devices_;
    std::unordered_map<std::string, std::size_t> uid_index_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DEVICE_MANAGER_HPP
