// bridge_api_test.cpp — smoke tests for the public C bridge surface.
//
// Covers the parts that are implemented in Phase 3 commit #1:
//   - ABI version round-trip
//   - error-code and route-state name-getters (all enum values covered)
//   - device-list free with NULL
//   - consistent NOT_IMPLEMENTED behaviour for stubs
//
// Real functional tests of create/enumerate/add_route/etc. land in
// Phase 3 commit #6 (bridge implementation) and #8 (integration tests).

#include <catch_amalgamated.hpp>

#include "jbox_engine.h"

#include <cstring>
#include <string_view>

TEST_CASE("bridge: ABI version is exposed and positive", "[bridge_api]") {
    REQUIRE(jbox_engine_abi_version() == JBOX_ENGINE_ABI_VERSION);
    REQUIRE(jbox_engine_abi_version() > 0);
}

TEST_CASE("bridge: error code names are defined for all values", "[bridge_api]") {
    const jbox_error_code_t codes[] = {
        JBOX_OK,
        JBOX_ERR_INVALID_ARGUMENT,
        JBOX_ERR_DEVICE_NOT_FOUND,
        JBOX_ERR_MAPPING_INVALID,
        JBOX_ERR_RESOURCE_EXHAUSTED,
        JBOX_ERR_DEVICE_BUSY,
        JBOX_ERR_NOT_IMPLEMENTED,
        JBOX_ERR_INTERNAL,
    };
    for (auto code : codes) {
        const char* name = jbox_error_code_name(code);
        REQUIRE(name != nullptr);
        REQUIRE(std::strlen(name) > 0);
    }
    REQUIRE(std::string_view{jbox_error_code_name(JBOX_OK)} == "ok");
}

TEST_CASE("bridge: route state names are defined for all values", "[bridge_api]") {
    const jbox_route_state_t states[] = {
        JBOX_ROUTE_STATE_STOPPED,
        JBOX_ROUTE_STATE_WAITING,
        JBOX_ROUTE_STATE_STARTING,
        JBOX_ROUTE_STATE_RUNNING,
        JBOX_ROUTE_STATE_ERROR,
    };
    for (auto state : states) {
        const char* name = jbox_route_state_name(state);
        REQUIRE(name != nullptr);
        REQUIRE(std::strlen(name) > 0);
    }
    REQUIRE(std::string_view{jbox_route_state_name(JBOX_ROUTE_STATE_RUNNING)} == "running");
}

TEST_CASE("bridge: device_list_free is safe with NULL", "[bridge_api]") {
    // Should not crash.
    jbox_device_list_free(nullptr);
}

TEST_CASE("bridge: stubs return NOT_IMPLEMENTED uniformly", "[bridge_api][stub]") {
    // Until the engine lands in commit #6, every non-trivial entry
    // point must report JBOX_ERR_NOT_IMPLEMENTED cleanly and without
    // undefined behaviour.
    jbox_error_t err{};

    SECTION("jbox_engine_create") {
        jbox_engine_config_t cfg{};
        jbox_engine_t* engine = jbox_engine_create(&cfg, &err);
        REQUIRE(engine == nullptr);
        REQUIRE(err.code == JBOX_ERR_NOT_IMPLEMENTED);
        REQUIRE(err.message != nullptr);

        // Destroy must tolerate the NULL handle.
        jbox_engine_destroy(engine);
    }

    SECTION("jbox_engine_enumerate_devices") {
        jbox_device_list_t* list = jbox_engine_enumerate_devices(nullptr, &err);
        REQUIRE(list == nullptr);
        REQUIRE(err.code == JBOX_ERR_NOT_IMPLEMENTED);
    }

    SECTION("jbox_engine_add_route") {
        jbox_route_config_t cfg{};
        jbox_route_id_t id = jbox_engine_add_route(nullptr, &cfg, &err);
        REQUIRE(id == JBOX_INVALID_ROUTE_ID);
        REQUIRE(err.code == JBOX_ERR_NOT_IMPLEMENTED);
    }

    SECTION("jbox_engine_start_route") {
        REQUIRE(jbox_engine_start_route(nullptr, 0) == JBOX_ERR_NOT_IMPLEMENTED);
    }

    SECTION("jbox_engine_stop_route") {
        REQUIRE(jbox_engine_stop_route(nullptr, 0) == JBOX_ERR_NOT_IMPLEMENTED);
    }

    SECTION("jbox_engine_remove_route") {
        REQUIRE(jbox_engine_remove_route(nullptr, 0) == JBOX_ERR_NOT_IMPLEMENTED);
    }

    SECTION("jbox_engine_poll_route_status") {
        jbox_route_status_t out{};
        REQUIRE(jbox_engine_poll_route_status(nullptr, 0, &out) == JBOX_ERR_NOT_IMPLEMENTED);
    }
}

TEST_CASE("bridge: NULL error out-parameter is tolerated", "[bridge_api][stub]") {
    // Passing nullptr for the err argument should be safe; callers
    // who don't need details just see the sentinel return values.
    jbox_engine_config_t cfg{};
    REQUIRE(jbox_engine_create(&cfg, nullptr) == nullptr);
    REQUIRE(jbox_engine_enumerate_devices(nullptr, nullptr) == nullptr);

    jbox_route_config_t rcfg{};
    REQUIRE(jbox_engine_add_route(nullptr, &rcfg, nullptr) == JBOX_INVALID_ROUTE_ID);
}

TEST_CASE("bridge: struct sizes document the ABI shape", "[bridge_api]") {
    // These REQUIREs are not correctness tests — they pin the sizes
    // of the POD structs so that an accidental field insertion in
    // the middle becomes a compile-time or test-time signal. If a
    // field is legitimately added at the end, bump JBOX_ENGINE_ABI_VERSION
    // (MINOR) and update the expected size here.
    REQUIRE(sizeof(jbox_channel_edge_t) == 8);
    // 256 (uid) + 256 (name) + 4 (direction) + 4 + 4 (chan counts) +
    // 8 (sample rate, aligned) + 4 (buffer frame size) + 4 (padding)
    REQUIRE(sizeof(jbox_device_info_t) >= 256 + 256 + 4 + 4 + 4 + 8 + 4);
}
