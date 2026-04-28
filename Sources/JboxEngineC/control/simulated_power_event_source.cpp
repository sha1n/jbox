// simulated_power_event_source.cpp — SimulatedPowerEventSource impl.

#include "simulated_power_event_source.hpp"

namespace jbox::control {

void SimulatedPowerEventSource::setPowerStateListener(PowerStateListener cb,
                                                     void* user_data) {
    cb_   = cb;
    user_ = user_data;
}

void SimulatedPowerEventSource::acknowledgeSleep() {
    ++ack_count_;
}

namespace {
inline void fire(PowerStateListener cb, void* ud, PowerStateEvent::Kind kind) {
    if (cb == nullptr) return;
    PowerStateEvent ev{kind};
    cb(ev, ud);
}
}  // namespace

void SimulatedPowerEventSource::simulateWillSleep() {
    fire(cb_, user_, PowerStateEvent::kWillSleep);
}

void SimulatedPowerEventSource::simulatePoweredOn() {
    fire(cb_, user_, PowerStateEvent::kPoweredOn);
}

}  // namespace jbox::control
