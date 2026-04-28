// device_change_watcher_test.cpp — control-thread queue mechanics for
// the Phase 7.6.4 hot-plug listener path. Pairs with the simulator's
// simulateDevice* seams (also exercised here) so we can drive
// device-topology changes deterministically without real Core Audio
// listeners. Reaction-layer tests live in route_manager_test.cpp's
// [route_manager][device_loss] group.

#include <catch_amalgamated.hpp>

#include "device_backend.hpp"
#include "simulated_backend.hpp"

#include <string>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::DeviceChangeEvent;
using jbox::control::SimulatedBackend;
using jbox::control::kBackendDirectionInput;

namespace {

BackendDeviceInfo makeInputDevice(const std::string& uid,
                                  std::uint32_t channels = 2) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput;
    info.input_channel_count = channels;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    return info;
}

}  // namespace

TEST_CASE("SimulatedBackend: simulateDeviceRemoval fires IsNotAlive then ListChanged",
          "[sim_backend][device_change]") {
    // Lock in the seam contract: simulateDeviceRemoval must fire two
    // events in order and update the simulator's enumerable state
    // between them. By the time the listener observes the second
    // event, the device is gone from enumerate().
    SimulatedBackend backend;
    backend.addDevice(makeInputDevice("doomed"));

    std::vector<DeviceChangeEvent> captured;
    backend.setDeviceChangeListener(
        +[](const DeviceChangeEvent& ev, void* ud) {
            static_cast<std::vector<DeviceChangeEvent>*>(ud)->push_back(ev);
        },
        &captured);

    REQUIRE(backend.enumerate().size() == 1);
    backend.simulateDeviceRemoval("doomed");

    REQUIRE(captured.size() == 2);
    REQUIRE(captured[0].kind == DeviceChangeEvent::kDeviceIsNotAlive);
    REQUIRE(captured[0].uid  == "doomed");
    REQUIRE(captured[1].kind == DeviceChangeEvent::kDeviceListChanged);
    REQUIRE(captured[1].uid  == "doomed");
    REQUIRE(backend.enumerate().empty());
}

TEST_CASE("SimulatedBackend: simulateDeviceReappearance adds device + fires ListChanged",
          "[sim_backend][device_change]") {
    SimulatedBackend backend;
    std::vector<DeviceChangeEvent> captured;
    backend.setDeviceChangeListener(
        +[](const DeviceChangeEvent& ev, void* ud) {
            static_cast<std::vector<DeviceChangeEvent>*>(ud)->push_back(ev);
        },
        &captured);

    REQUIRE(backend.enumerate().empty());
    backend.simulateDeviceReappearance(makeInputDevice("returns"));

    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].kind == DeviceChangeEvent::kDeviceListChanged);
    REQUIRE(captured[0].uid  == "returns");
    REQUIRE(backend.enumerate().size() == 1);
}

TEST_CASE("SimulatedBackend: simulateAggregateMembersChanged fires the matching kind",
          "[sim_backend][device_change]") {
    SimulatedBackend backend;
    std::vector<DeviceChangeEvent> captured;
    backend.setDeviceChangeListener(
        +[](const DeviceChangeEvent& ev, void* ud) {
            static_cast<std::vector<DeviceChangeEvent>*>(ud)->push_back(ev);
        },
        &captured);

    backend.simulateAggregateMembersChanged("agg");

    REQUIRE(captured.size() == 1);
    REQUIRE(captured[0].kind == DeviceChangeEvent::kAggregateMembersChanged);
    REQUIRE(captured[0].uid  == "agg");
}
