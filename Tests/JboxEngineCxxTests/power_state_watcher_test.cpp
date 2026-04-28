// power_state_watcher_test.cpp — sleep/wake event plumbing for the
// Phase 7.6.5 PowerStateWatcher.
//
// Two layers under test:
//   - SimulatedPowerEventSource (the fixture): simulateWillSleep /
//     simulatePoweredOn fire the listener synchronously;
//     acknowledgeSleep increments a test-visible counter.
//   - PowerStateWatcher (next commit): kWillSleep invokes the
//     registered sleep handler synchronously and acks regardless;
//     kPoweredOn queues onto the drain.
//
// RouteManager-side reaction tests live in route_manager_test.cpp's
// [route_manager][sleep_wake] group.

#include <catch_amalgamated.hpp>

#include "power_event_source.hpp"
#include "simulated_power_event_source.hpp"

#include <vector>

using jbox::control::PowerStateEvent;
using jbox::control::SimulatedPowerEventSource;

TEST_CASE("SimulatedPowerEventSource: simulateWillSleep fires kWillSleep through the listener",
          "[sim_power]") {
    SimulatedPowerEventSource source;
    std::vector<PowerStateEvent> captured;
    source.setPowerStateListener(
        +[](const PowerStateEvent& ev, void* ud) {
            static_cast<std::vector<PowerStateEvent>*>(ud)->push_back(ev);
        },
        &captured);

    source.simulateWillSleep();
    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].kind == PowerStateEvent::kWillSleep);
}

TEST_CASE("SimulatedPowerEventSource: simulatePoweredOn fires kPoweredOn through the listener",
          "[sim_power]") {
    SimulatedPowerEventSource source;
    std::vector<PowerStateEvent> captured;
    source.setPowerStateListener(
        +[](const PowerStateEvent& ev, void* ud) {
            static_cast<std::vector<PowerStateEvent>*>(ud)->push_back(ev);
        },
        &captured);

    source.simulatePoweredOn();
    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].kind == PowerStateEvent::kPoweredOn);
}

TEST_CASE("SimulatedPowerEventSource: acknowledgeSleep increments the test counter",
          "[sim_power]") {
    SimulatedPowerEventSource source;
    REQUIRE(source.ackCount() == 0);
    source.acknowledgeSleep();
    source.acknowledgeSleep();
    REQUIRE(source.ackCount() == 2);
}
