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
#include "power_state_watcher.hpp"
#include "simulated_power_event_source.hpp"

#include <vector>

using jbox::control::PowerStateEvent;
using jbox::control::PowerStateWatcher;
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

TEST_CASE("PowerStateWatcher: kWillSleep invokes the sleep handler then acks the source",
          "[power_state_watcher]") {
    SimulatedPowerEventSource source;
    PowerStateWatcher watcher(source);

    int handler_calls = 0;
    watcher.setSleepHandler([&]() { ++handler_calls; });

    source.simulateWillSleep();

    REQUIRE(handler_calls == 1);
    REQUIRE(source.ackCount() == 1);
    // Sleep is synchronous — nothing queued.
    REQUIRE(watcher.empty() == true);
    REQUIRE(watcher.drain().empty());
}

TEST_CASE("PowerStateWatcher: kWillSleep acks even when no sleep handler is registered",
          "[power_state_watcher]") {
    // The contract: "macOS gets an ack regardless of what the
    // handler does (or doesn't do)" — so a missing handler must
    // not stall the sleep transition.
    SimulatedPowerEventSource source;
    PowerStateWatcher watcher(source);

    source.simulateWillSleep();
    REQUIRE(source.ackCount() == 1);
}

TEST_CASE("PowerStateWatcher: kPoweredOn drains via drain() and does NOT invoke the sleep handler",
          "[power_state_watcher]") {
    SimulatedPowerEventSource source;
    PowerStateWatcher watcher(source);

    int handler_calls = 0;
    watcher.setSleepHandler([&]() { ++handler_calls; });

    source.simulatePoweredOn();
    source.simulatePoweredOn();

    REQUIRE(handler_calls == 0);
    REQUIRE(source.ackCount() == 0);

    const auto events = watcher.drain();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].kind == PowerStateEvent::kPoweredOn);
    REQUIRE(events[1].kind == PowerStateEvent::kPoweredOn);
    REQUIRE(watcher.empty() == true);
}
