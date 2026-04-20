// bridge_api_test.cpp — C-bridge smoke and integration tests.
//
// The C bridge forwards to the Engine class. Production jbox_engine_create
// uses CoreAudioBackend; these tests exercise the real bridge against
// the system's audio devices for enumerate/create/destroy. Full route
// lifecycle tests via the bridge use SimulatedBackend through the
// test-only internal helper jbox::internal::createEngineWithBackend.

#include <catch_amalgamated.hpp>

#include "device_backend.hpp"
#include "jbox_engine.h"
#include "simulated_backend.hpp"

#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

// Forward-declare the test-only helpers from bridge_api.cpp. The
// drainer defaults to false in tests to avoid spawning a consumer
// thread (and os_log noise) unless a test specifically exercises it.
namespace jbox::internal {
jbox_engine_t* createEngineWithBackend(
    std::unique_ptr<jbox::control::IDeviceBackend> backend,
    bool spawn_sampler_thread,
    bool spawn_log_drainer = false);
void tickDriftOnce(jbox_engine_t* engine, double dt_seconds);
}  // namespace jbox::internal

using jbox::control::BackendDeviceInfo;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::SimulatedBackend;

// -----------------------------------------------------------------------------
// Name-getters and free (pure utility — no engine needed)
// -----------------------------------------------------------------------------

TEST_CASE("bridge: ABI version is exposed and positive", "[bridge_api]") {
    REQUIRE(jbox_engine_abi_version() == JBOX_ENGINE_ABI_VERSION);
    REQUIRE(jbox_engine_abi_version() > 0);
}

TEST_CASE("bridge: error code names are defined for all values", "[bridge_api]") {
    const jbox_error_code_t codes[] = {
        JBOX_OK, JBOX_ERR_INVALID_ARGUMENT, JBOX_ERR_DEVICE_NOT_FOUND,
        JBOX_ERR_MAPPING_INVALID, JBOX_ERR_RESOURCE_EXHAUSTED,
        JBOX_ERR_DEVICE_BUSY, JBOX_ERR_NOT_IMPLEMENTED, JBOX_ERR_INTERNAL,
    };
    for (auto code : codes) {
        const char* name = jbox_error_code_name(code);
        REQUIRE(name != nullptr);
        REQUIRE(std::strlen(name) > 0);
    }
}

TEST_CASE("bridge: route state names are defined for all values", "[bridge_api]") {
    const jbox_route_state_t states[] = {
        JBOX_ROUTE_STATE_STOPPED, JBOX_ROUTE_STATE_WAITING,
        JBOX_ROUTE_STATE_STARTING, JBOX_ROUTE_STATE_RUNNING,
        JBOX_ROUTE_STATE_ERROR,
    };
    for (auto state : states) {
        const char* name = jbox_route_state_name(state);
        REQUIRE(name != nullptr);
        REQUIRE(std::strlen(name) > 0);
    }
}

TEST_CASE("bridge: device_list_free is safe with NULL", "[bridge_api]") {
    jbox_device_list_free(nullptr);
}

// -----------------------------------------------------------------------------
// Invalid-argument guards (no engine state required)
// -----------------------------------------------------------------------------

TEST_CASE("bridge: null engine arguments return INVALID_ARGUMENT", "[bridge_api]") {
    REQUIRE(jbox_engine_start_route(nullptr, 1) == JBOX_ERR_INVALID_ARGUMENT);
    REQUIRE(jbox_engine_stop_route(nullptr, 1)  == JBOX_ERR_INVALID_ARGUMENT);
    REQUIRE(jbox_engine_remove_route(nullptr, 1) == JBOX_ERR_INVALID_ARGUMENT);

    jbox_route_status_t status{};
    REQUIRE(jbox_engine_poll_route_status(nullptr, 1, &status) == JBOX_ERR_INVALID_ARGUMENT);

    jbox_error_t err{};
    REQUIRE(jbox_engine_enumerate_devices(nullptr, &err) == nullptr);
    REQUIRE(err.code == JBOX_ERR_INVALID_ARGUMENT);

    jbox_route_config_t cfg{};
    REQUIRE(jbox_engine_add_route(nullptr, &cfg, &err) == JBOX_INVALID_ROUTE_ID);
}

TEST_CASE("bridge: engine_destroy is safe with NULL", "[bridge_api]") {
    jbox_engine_destroy(nullptr);  // no-op, no crash
}

// -----------------------------------------------------------------------------
// Integration: full bridge path via SimulatedBackend
// -----------------------------------------------------------------------------

namespace {

BackendDeviceInfo makeDev(const std::string& uid, std::uint32_t dir,
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

TEST_CASE("bridge: enumerate via SimulatedBackend returns our devices",
          "[bridge_api][integration]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeDev("src", kBackendDirectionInput, 2, 0));
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));

    jbox_engine_t* e = jbox::internal::createEngineWithBackend(std::move(backend),
                                                                /*spawn_sampler_thread=*/false);
    REQUIRE(e != nullptr);

    jbox_error_t err{};
    jbox_device_list_t* list = jbox_engine_enumerate_devices(e, &err);
    REQUIRE(list != nullptr);
    REQUIRE(list->count == 2);

    bool saw_src = false, saw_dst = false;
    for (std::size_t i = 0; i < list->count; ++i) {
        if (std::string_view{list->devices[i].uid} == "src") saw_src = true;
        if (std::string_view{list->devices[i].uid} == "dst") saw_dst = true;
    }
    REQUIRE(saw_src);
    REQUIRE(saw_dst);

    jbox_device_list_free(list);
    jbox_engine_destroy(e);
}

TEST_CASE("bridge: add / start / poll / stop / remove route via C API",
          "[bridge_api][integration]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend = backend_holder.get();
    backend->addDevice(makeDev("src", kBackendDirectionInput,  2, 0));
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));

    jbox_engine_t* e = jbox::internal::createEngineWithBackend(std::move(backend_holder),
                                                                /*spawn_sampler_thread=*/false);
    REQUIRE(e != nullptr);

    // Enumerate — this populates DeviceManager's UID index via
    // Engine's constructor-time refresh. Call explicitly for clarity.
    jbox_error_t err{};
    if (auto* l = jbox_engine_enumerate_devices(e, &err)) {
        jbox_device_list_free(l);
    }

    const jbox_channel_edge_t mapping[] = {{0, 0}, {1, 1}};
    jbox_route_config_t rcfg{
        .source_uid = "src",
        .dest_uid   = "dst",
        .mapping    = mapping,
        .mapping_count = 2,
        .name = "bridge-test"
    };

    const jbox_route_id_t id = jbox_engine_add_route(e, &rcfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(jbox_engine_start_route(e, id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(jbox_engine_poll_route_status(e, id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // Drive one buffer cycle to see counters update.
    const float in[] = {1.0f, 2.0f, 3.0f, 4.0f};   // 2 frames × 2 channels
    float out[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    backend->deliverBuffer("src", 2, in, nullptr);
    backend->deliverBuffer("dst", 2, nullptr, out);

    REQUIRE(jbox_engine_poll_route_status(e, id, &status) == JBOX_OK);
    REQUIRE(status.frames_produced == 2);
    REQUIRE(status.frames_consumed == 2);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
    REQUIRE(out[2] == 3.0f);
    REQUIRE(out[3] == 4.0f);

    REQUIRE(jbox_engine_stop_route(e, id) == JBOX_OK);
    REQUIRE(jbox_engine_poll_route_status(e, id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_STOPPED);

    REQUIRE(jbox_engine_remove_route(e, id) == JBOX_OK);
    REQUIRE(jbox_engine_remove_route(e, id) == JBOX_ERR_INVALID_ARGUMENT);

    jbox_engine_destroy(e);
}

TEST_CASE("bridge: add_route rejects NULL mapping with mapping_count>0",
          "[bridge_api]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeDev("src", kBackendDirectionInput,  2, 0));
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));
    jbox_engine_t* e = jbox::internal::createEngineWithBackend(std::move(backend),
                                                                /*spawn_sampler_thread=*/false);

    jbox_route_config_t rcfg{
        .source_uid = "src",
        .dest_uid   = "dst",
        .mapping    = nullptr,
        .mapping_count = 2,  // claims 2 edges but pointer is null
        .name = nullptr
    };
    jbox_error_t err{};
    REQUIRE(jbox_engine_add_route(e, &rcfg, &err) == JBOX_INVALID_ROUTE_ID);
    REQUIRE(err.code == JBOX_ERR_INVALID_ARGUMENT);

    jbox_engine_destroy(e);
}

// -----------------------------------------------------------------------------
// Live-system smoke check (runs on CI with default built-in devices)
// -----------------------------------------------------------------------------

TEST_CASE("bridge: create/destroy via default backend", "[bridge_api][integration]") {
    jbox_engine_config_t cfg{};
    jbox_error_t err{};
    jbox_engine_t* e = jbox_engine_create(&cfg, &err);
    REQUIRE(e != nullptr);

    if (auto* list = jbox_engine_enumerate_devices(e, &err)) {
        REQUIRE(list->count > 0);
        jbox_device_list_free(list);
    }

    jbox_engine_destroy(e);
}

TEST_CASE("bridge: struct sizes document the ABI shape", "[bridge_api]") {
    REQUIRE(sizeof(jbox_channel_edge_t) == 8);
    REQUIRE(sizeof(jbox_device_info_t) >= 256 + 256 + 4 + 4 + 4 + 8 + 4);
}
