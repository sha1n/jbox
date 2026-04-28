// power_state_watcher.hpp — control-thread bridge for sleep/wake events.
//
// Splits the IPowerEventSource's listener into two distinct call
// shapes that match how the engine actually wants to consume them:
//
//   - kWillSleep is *synchronous*: macOS waits for IOAllowPowerChange
//     before transitioning. The watcher invokes a caller-registered
//     `setSleepHandler` callback synchronously and then calls
//     `source.acknowledgeSleep()` — even if the handler throws (we
//     swallow exceptions so the system always gets its ack).
//
//   - kPoweredOn is *asynchronous*: the system is already awake, so
//     the watcher just appends the event to a mutex-protected queue
//     for the engine's 10 Hz tick to drain via `drain()`.
//
// Why split the two
// -----------------
// One generic callback for both kinds would either force every
// listener to handle the synchronous-ack contract (boilerplate at
// every call site) or push the kWillSleep event onto a queue that
// the tick can't drain in time. The split keeps the concerns
// separated: production code wires `setSleepHandler` to
// `RouteManager::prepareForSleep`; the wake-recovery loop pops kPoweredOn
// events off the drain queue and calls `RouteManager::recoverFromWake`.
//
// See docs/plan.md § Phase 7.6.5.

#ifndef JBOX_CONTROL_POWER_STATE_WATCHER_HPP
#define JBOX_CONTROL_POWER_STATE_WATCHER_HPP

#include "power_event_source.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace jbox::control {

class PowerStateWatcher {
public:
    explicit PowerStateWatcher(IPowerEventSource& source);
    ~PowerStateWatcher();

    PowerStateWatcher(const PowerStateWatcher&) = delete;
    PowerStateWatcher& operator=(const PowerStateWatcher&) = delete;

    // Register the synchronous handler that runs on every kWillSleep
    // event before the watcher acknowledges sleep to the source.
    // Re-registration replaces. Pass an empty std::function to clear.
    using SleepHandler = std::function<void()>;
    void setSleepHandler(SleepHandler handler);

    // Drain pending kPoweredOn events. Returns events in arrival
    // order; the queue is empty after this call. Control-thread only,
    // but safe to call concurrently with the producer-side onEvent.
    std::vector<PowerStateEvent> drain();

    // True when no wake events are pending. (Sleep events never queue.)
    bool empty() const;

private:
    static void onSourceEvent(const PowerStateEvent& ev, void* user_data);
    void onEvent(const PowerStateEvent& ev);

    IPowerEventSource& source_;
    SleepHandler       sleep_handler_;
    mutable std::mutex mutex_;
    std::deque<PowerStateEvent> wake_events_;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_POWER_STATE_WATCHER_HPP
