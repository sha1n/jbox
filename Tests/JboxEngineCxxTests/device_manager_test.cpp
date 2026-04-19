// device_manager_test.cpp — unit tests for DeviceManager.

#include <catch_amalgamated.hpp>

#include "device_manager.hpp"
#include "simulated_backend.hpp"

#include <memory>
#include <string>

using jbox::control::BackendDeviceInfo;
using jbox::control::DeviceManager;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeInfo(const std::string& uid,
                           const std::string& name,
                           std::uint32_t direction,
                           std::uint32_t ic,
                           std::uint32_t oc) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = name;
    info.direction = direction;
    info.input_channel_count = ic;
    info.output_channel_count = oc;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    return info;
}

}  // namespace

TEST_CASE("DeviceManager: fresh manager has empty snapshot", "[device_manager]") {
    auto backend = std::make_unique<SimulatedBackend>();
    DeviceManager dm(std::move(backend));
    REQUIRE(dm.devices().empty());
    REQUIRE(dm.findByUid("anything") == nullptr);
    REQUIRE_FALSE(dm.isPresent("anything"));
}

TEST_CASE("DeviceManager: refresh populates snapshot from backend",
          "[device_manager]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeInfo("in-uid", "Input Device", kBackendDirectionInput, 2, 0));
    backend->addDevice(makeInfo("out-uid", "Output Device", kBackendDirectionOutput, 0, 2));

    DeviceManager dm(std::move(backend));
    const auto& snapshot = dm.refresh();

    REQUIRE(snapshot.size() == 2);
    REQUIRE(dm.isPresent("in-uid"));
    REQUIRE(dm.isPresent("out-uid"));
    REQUIRE_FALSE(dm.isPresent("missing"));
}

TEST_CASE("DeviceManager: findByUid returns the right device",
          "[device_manager]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeInfo("uid-A", "Alpha", kBackendDirectionInput, 2, 0));
    backend->addDevice(makeInfo("uid-B", "Bravo", kBackendDirectionOutput, 0, 4));

    DeviceManager dm(std::move(backend));
    dm.refresh();

    const auto* a = dm.findByUid("uid-A");
    REQUIRE(a != nullptr);
    REQUIRE(a->name == "Alpha");
    REQUIRE(a->input_channel_count == 2);

    const auto* b = dm.findByUid("uid-B");
    REQUIRE(b != nullptr);
    REQUIRE(b->name == "Bravo");
    REQUIRE(b->output_channel_count == 4);
}

TEST_CASE("DeviceManager: refresh reflects subsequent backend changes",
          "[device_manager]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    // Keep a raw pointer for post-construction mutation — unusual
    // pattern but fine in this test since we control the lifetime.
    auto* backend_raw = backend_holder.get();

    DeviceManager dm(std::move(backend_holder));
    REQUIRE(dm.refresh().empty());

    backend_raw->addDevice(makeInfo("new-uid", "New", kBackendDirectionInput, 1, 0));
    REQUIRE(dm.refresh().size() == 1);
    REQUIRE(dm.isPresent("new-uid"));

    backend_raw->removeDevice("new-uid");
    REQUIRE(dm.refresh().empty());
    REQUIRE_FALSE(dm.isPresent("new-uid"));
}

TEST_CASE("DeviceManager: cached snapshot does not change until refresh",
          "[device_manager]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend_raw = backend_holder.get();

    backend_raw->addDevice(makeInfo("uid", "D", kBackendDirectionInput, 1, 0));
    DeviceManager dm(std::move(backend_holder));
    REQUIRE(dm.refresh().size() == 1);

    // Add another without refreshing — cached snapshot is unchanged.
    backend_raw->addDevice(makeInfo("uid2", "D2", kBackendDirectionInput, 1, 0));
    REQUIRE(dm.devices().size() == 1);
    REQUIRE_FALSE(dm.isPresent("uid2"));

    // Refresh — now it appears.
    REQUIRE(dm.refresh().size() == 2);
    REQUIRE(dm.isPresent("uid2"));
}

TEST_CASE("DeviceManager: backend() returns the owned backend",
          "[device_manager]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend_raw = backend_holder.get();

    DeviceManager dm(std::move(backend_holder));
    REQUIRE(&dm.backend() == backend_raw);
}
