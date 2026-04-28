// simulated_power_event_source.hpp — deterministic sleep/wake events
// for tests.
//
// Drives the registered PowerStateListener synchronously, matching
// the shape MacosPowerEventSource will produce in production once
// the IORegisterForSystemPower wiring lands.

#ifndef JBOX_CONTROL_SIMULATED_POWER_EVENT_SOURCE_HPP
#define JBOX_CONTROL_SIMULATED_POWER_EVENT_SOURCE_HPP

#include "power_event_source.hpp"

#include <cstdint>

namespace jbox::control {

class SimulatedPowerEventSource final : public IPowerEventSource {
public:
    SimulatedPowerEventSource() = default;
    ~SimulatedPowerEventSource() override = default;

    SimulatedPowerEventSource(const SimulatedPowerEventSource&) = delete;
    SimulatedPowerEventSource& operator=(const SimulatedPowerEventSource&) = delete;

    // IPowerEventSource.
    void setPowerStateListener(PowerStateListener cb, void* user_data) override;
    void acknowledgeSleep() override;

    // Test-only: fire the listener synchronously with kWillSleep /
    // kPoweredOn. No-op when no listener is registered.
    void simulateWillSleep();
    void simulatePoweredOn();

    // Test introspection: how many times acknowledgeSleep has been
    // called since construction (or since the last reset).
    std::uint32_t ackCount() const { return ack_count_; }

private:
    PowerStateListener cb_      = nullptr;
    void*              user_    = nullptr;
    std::uint32_t      ack_count_ = 0;
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_SIMULATED_POWER_EVENT_SOURCE_HPP
