// device_change_watcher.hpp — control-thread queue of device-topology events.
//
// Wraps an IDeviceBackend's setDeviceChangeListener: the watcher
// installs itself on construction, captures every event the backend
// emits into a mutex-protected queue, and exposes drain() for the
// control thread to consume them on its tick.
//
// Why the mutex
// -------------
// Backends emit events from whatever thread observes the change. In
// production that's a HAL property-listener thread (Apple-managed
// pool); in tests, it's the test thread driving simulateDevice*. The
// drainer always runs on the engine's control thread. The mutex
// makes producer/consumer race-free without forcing an SPSC queue
// (events are sparse — sample-rate cascade bursts hit a handful per
// second, hot-plug + sleep/wake cycles a handful per minute — so
// lock contention is a non-issue).
//
// Why no debounce inside the watcher
// ----------------------------------
// 7.6.4 keeps coalescing in the *reaction* layer (RouteManager::
// handleDeviceChanges): an already-WAITING route ignores subsequent
// kDeviceIsNotAlive events on the same UID; bursts of
// kDeviceListChanged events during a sample-rate cascade collapse to
// at most a few dm_.refresh() calls per drain. A timer-based debounce
// inside the watcher would need an injectable clock for tests; the
// idempotent-reaction approach keeps tests deterministic without it.
// A future refinement can add std::chrono dedup at this layer if real-
// hardware traces show it's worth the complexity.
//
// See docs/plan.md § Phase 7.6.4.

#ifndef JBOX_CONTROL_DEVICE_CHANGE_WATCHER_HPP
#define JBOX_CONTROL_DEVICE_CHANGE_WATCHER_HPP

#include "device_backend.hpp"

#include <deque>
#include <mutex>
#include <vector>

namespace jbox::control {

class DeviceChangeWatcher {
public:
    // Registers a static thunk on `backend.setDeviceChangeListener`.
    // The backend reference must outlive the watcher.
    explicit DeviceChangeWatcher(IDeviceBackend& backend);

    // Clears the listener registration on the backend so future
    // events stop being routed here. The backend keeps living.
    ~DeviceChangeWatcher();

    DeviceChangeWatcher(const DeviceChangeWatcher&) = delete;
    DeviceChangeWatcher& operator=(const DeviceChangeWatcher&) = delete;

    // Drain pending events. Returns events in arrival order; the
    // queue is empty after this call. Control-thread only — but it
    // is safe to call concurrently with the producer-side onEvent
    // (the mutex serialises them).
    std::vector<DeviceChangeEvent> drain();

    // True when no events are pending. Approximate but useful for
    // tests; always exact when the producer thread is quiescent.
    bool empty() const;

private:
    // Static trampoline registered with the backend; forwards to the
    // owning instance's onEvent.
    static void onBackendEvent(const DeviceChangeEvent& ev, void* user_data);

    void onEvent(const DeviceChangeEvent& ev);

    IDeviceBackend& backend_;
    mutable std::mutex mutex_;
    std::deque<DeviceChangeEvent> events_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_DEVICE_CHANGE_WATCHER_HPP
