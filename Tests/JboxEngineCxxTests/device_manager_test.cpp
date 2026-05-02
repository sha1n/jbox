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

TEST_CASE("DeviceManager: aggregate device exposes its active sub-device UIDs",
          "[device_manager][aggregate_members]") {
    using jbox::control::BackendDeviceInfo;
    using jbox::control::DeviceManager;
    using jbox::control::SimulatedBackend;
    using jbox::control::kBackendDirectionInput;

    auto b = std::make_unique<SimulatedBackend>();

    BackendDeviceInfo member_a;
    member_a.uid = "member-a";
    member_a.name = "member-a";
    member_a.direction = kBackendDirectionInput;
    member_a.input_channel_count = 2;
    member_a.nominal_sample_rate = 48000.0;
    member_a.buffer_frame_size = 64;
    b->addDevice(member_a);

    BackendDeviceInfo member_b = member_a;
    member_b.uid = "member-b";
    member_b.name = "member-b";
    b->addDevice(member_b);

    BackendDeviceInfo agg;
    agg.uid = "agg";
    agg.name = "agg";
    agg.direction = kBackendDirectionInput;
    agg.input_channel_count = 4;
    agg.nominal_sample_rate = 48000.0;
    agg.buffer_frame_size = 64;
    b->addAggregateDevice(agg, {"member-a", "member-b"});

    DeviceManager dm(std::move(b));
    dm.refresh();

    const auto* info = dm.findByUid("agg");
    REQUIRE(info != nullptr);
    REQUIRE(info->is_aggregate);
    REQUIRE(info->aggregate_member_uids ==
            std::vector<std::string>{"member-a", "member-b"});

    const auto* member_info = dm.findByUid("member-a");
    REQUIRE(member_info != nullptr);
    REQUIRE_FALSE(member_info->is_aggregate);
    REQUIRE(member_info->aggregate_member_uids.empty());
}

TEST_CASE("DeviceManager: appendAggregateMembers is a no-op for non-aggregate UIDs",
          "[device_manager][aggregate_members]") {
    using jbox::control::BackendDeviceInfo;
    using jbox::control::DeviceManager;
    using jbox::control::SimulatedBackend;
    using jbox::control::kBackendDirectionInput;

    auto b = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo info;
    info.uid = "plain";
    info.name = "plain";
    info.direction = kBackendDirectionInput;
    info.input_channel_count = 2;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    b->addDevice(info);
    DeviceManager dm(std::move(b));
    dm.refresh();

    std::vector<std::string> out{"pre-existing"};
    dm.appendAggregateMembers(out, "plain");
    REQUIRE(out == std::vector<std::string>{"pre-existing"});

    dm.appendAggregateMembers(out, "unknown-uid");
    REQUIRE(out == std::vector<std::string>{"pre-existing"});
}

TEST_CASE("DeviceManager: appendAggregateMembers appends each active sub-device UID",
          "[device_manager][aggregate_members]") {
    using jbox::control::BackendDeviceInfo;
    using jbox::control::DeviceManager;
    using jbox::control::SimulatedBackend;
    using jbox::control::kBackendDirectionInput;

    auto b = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo m;
    m.direction = kBackendDirectionInput;
    m.input_channel_count = 2;
    m.nominal_sample_rate = 48000.0;
    m.buffer_frame_size = 64;
    m.uid = "a"; m.name = "a"; b->addDevice(m);
    m.uid = "b"; m.name = "b"; b->addDevice(m);
    BackendDeviceInfo agg = m;
    agg.uid = "agg"; agg.name = "agg"; agg.input_channel_count = 4;
    b->addAggregateDevice(agg, {"a", "b"});
    DeviceManager dm(std::move(b));
    dm.refresh();

    std::vector<std::string> out;
    dm.appendAggregateMembers(out, "agg");
    REQUIRE(out == std::vector<std::string>{"a", "b"});

    // Idempotent on repeat — appends the same set again.
    dm.appendAggregateMembers(out, "agg");
    REQUIRE(out.size() == 4);
}
