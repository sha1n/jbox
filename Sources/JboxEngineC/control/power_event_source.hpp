// power_event_source.hpp — abstract interface over the system power
// management notifications.
//
// Two concrete implementations:
//   - MacosPowerEventSource (production) — wraps IORegisterForSystem
//     Power; will-sleep messages fire kIOMessageSystemWillSleep, wake
//     messages fire kIOMessageSystemHasPoweredOn.
//   - SimulatedPowerEventSource (tests) — drives the listener
//     synchronously through `simulateWillSleep` / `simulatePoweredOn`.
//
// Threading
// ---------
// The listener fires on whichever thread the source uses. Production
// fires from a dedicated IOPM dispatch queue; the simulator fires
// synchronously on the test thread.
//
// Synchronous-ack contract for kWillSleep
// ---------------------------------------
// macOS waits for the app to call `IOAllowPowerChange()` before the
// system actually transitions to sleep. Listener implementations must
// therefore complete their teardown work (or schedule it on a
// suitably fast path) and then call `acknowledgeSleep()` from inside
// the listener invocation, before returning. macOS allows ~30 s before
// it gives up and forces sleep, but the contract says "ack as soon as
// you can".
//
// kPoweredOn has no such constraint — the system is already awake;
// recovery work can be queued for the control-thread tick.
//
// See docs/plan.md § Phase 7.6.5.

#ifndef JBOX_CONTROL_POWER_EVENT_SOURCE_HPP
#define JBOX_CONTROL_POWER_EVENT_SOURCE_HPP

#include <cstdint>

namespace jbox::control {

struct PowerStateEvent {
    enum Kind : std::uint32_t {
        // The system is preparing to sleep. The listener MUST complete
        // synchronous work and call acknowledgeSleep() before returning.
        kWillSleep  = 0,
        // The system has just woken from sleep. Devices may not be
        // fully re-enumerated yet; recovery should be retried with
        // backoff.
        kPoweredOn  = 1,
    };
    Kind kind;
};

using PowerStateListener = void(*)(const PowerStateEvent& event,
                                   void* user_data);

class IPowerEventSource {
public:
    virtual ~IPowerEventSource() = default;

    // Install (or clear, with nullptr) the single PowerStateListener.
    // Re-registering replaces the previous callback. Production sources
    // wire this through IORegisterForSystemPower; simulated sources
    // simply store the pointer and fire it from simulate*.
    virtual void setPowerStateListener(PowerStateListener cb,
                                       void* user_data) = 0;

    // Release macOS's hold on the sleep transition. Listener
    // implementations call this exactly once per kWillSleep event,
    // typically as the last step before returning from the listener.
    // No-op on simulated sources after they record the call (tests
    // can inspect the count via SimulatedPowerEventSource::ack
    // Count()).
    virtual void acknowledgeSleep() = 0;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_POWER_EVENT_SOURCE_HPP
