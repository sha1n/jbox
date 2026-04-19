// engine_test.cpp — tests for the Engine facade class using SimulatedBackend.

#include <catch_amalgamated.hpp>

#include "engine.hpp"
#include "simulated_backend.hpp"

#include <memory>

using jbox::control::BackendDeviceInfo;
using jbox::control::ChannelEdge;
using jbox::control::Engine;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::RouteManager;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeDev(const std::string& uid,
                          std::uint32_t dir,
                          std::uint32_t ic, std::uint32_t oc) {
    BackendDeviceInfo d;
    d.uid = uid;
    d.name = uid;
    d.direction = dir;
    d.input_channel_count = ic;
    d.output_channel_count = oc;
    d.nominal_sample_rate = 48000.0;
    d.buffer_frame_size = 32;
    return d;
}

}  // namespace

TEST_CASE("Engine: construction populates device snapshot", "[engine]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeDev("src", kBackendDirectionInput, 2, 0));
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));

    Engine e(std::move(backend), /*spawn_sampler_thread=*/false);
    REQUIRE(e.deviceManager().devices().size() == 2);
}

TEST_CASE("Engine: enumerateDevices re-polls the backend", "[engine]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend = backend_holder.get();
    backend->addDevice(makeDev("src", kBackendDirectionInput, 2, 0));

    Engine e(std::move(backend_holder), /*spawn_sampler_thread=*/false);
    REQUIRE(e.enumerateDevices().size() == 1);

    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));
    REQUIRE(e.enumerateDevices().size() == 2);
}

TEST_CASE("Engine: full route lifecycle via facade", "[engine][integration]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend = backend_holder.get();
    backend->addDevice(makeDev("src", kBackendDirectionInput,  2, 0));
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));

    Engine e(std::move(backend_holder), /*spawn_sampler_thread=*/false);

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "src";
    cfg.dest_uid   = "dst";
    cfg.mapping    = {{0, 0}, {1, 1}};
    cfg.name       = "engine-test";

    jbox_error_t err{};
    const auto id = e.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(e.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(e.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // Simple sample-flow sanity check.
    const float in[] = {0.1f, 0.2f, 0.3f, 0.4f};  // 2 frames × 2 channels
    float out[4] = {};
    backend->deliverBuffer("src", 2, in, nullptr);
    backend->deliverBuffer("dst", 2, nullptr, out);
    REQUIRE(out[0] == 0.1f);
    REQUIRE(out[1] == 0.2f);
    REQUIRE(out[2] == 0.3f);
    REQUIRE(out[3] == 0.4f);

    REQUIRE(e.stopRoute(id)   == JBOX_OK);
    REQUIRE(e.removeRoute(id) == JBOX_OK);
}
