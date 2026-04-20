// route_manager_test.cpp — integration tests for RouteManager using
// SimulatedBackend to drive deterministic sample flow.

#include <catch_amalgamated.hpp>

#include "device_manager.hpp"
#include "route_manager.hpp"
#include "simulated_backend.hpp"

#include <memory>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::ChannelEdge;
using jbox::control::DeviceManager;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::RouteManager;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeInputDevice(const std::string& uid,
                                  std::uint32_t channels,
                                  std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput;
    info.input_channel_count = channels;
    info.output_channel_count = 0;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = buf;
    return info;
}

BackendDeviceInfo makeOutputDevice(const std::string& uid,
                                   std::uint32_t channels,
                                   std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionOutput;
    info.input_channel_count = 0;
    info.output_channel_count = channels;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = buf;
    return info;
}

// Fixture: sets up a DeviceManager + RouteManager with a simulated
// backend that has a source device and a destination device.
struct Fixture {
    SimulatedBackend* backend = nullptr;
    std::unique_ptr<DeviceManager> dm;
    std::unique_ptr<RouteManager>  rm;

    Fixture(std::uint32_t src_channels, std::uint32_t dst_channels,
            std::uint32_t buf_frames = 64) {
        auto b = std::make_unique<SimulatedBackend>();
        backend = b.get();
        b->addDevice(makeInputDevice("src", src_channels, buf_frames));
        b->addDevice(makeOutputDevice("dst", dst_channels, buf_frames));
        dm = std::make_unique<DeviceManager>(std::move(b));
        dm->refresh();
        rm = std::make_unique<RouteManager>(*dm);
    }
};

}  // namespace

TEST_CASE("RouteManager: addRoute rejects empty mapping", "[route_manager]") {
    Fixture f(4, 4);
    RouteManager::RouteConfig cfg{"src", "dst", {}, "empty"};
    jbox_error_t err{};
    REQUIRE(f.rm->addRoute(cfg, &err) == JBOX_INVALID_ROUTE_ID);
    REQUIRE(err.code == JBOX_ERR_MAPPING_INVALID);
}

TEST_CASE("RouteManager: addRoute rejects duplicate source channel", "[route_manager]") {
    Fixture f(4, 4);
    std::vector<ChannelEdge> m{{0, 0}, {0, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "dup"};
    jbox_error_t err{};
    REQUIRE(f.rm->addRoute(cfg, &err) == JBOX_INVALID_ROUTE_ID);
    REQUIRE(err.code == JBOX_ERR_MAPPING_INVALID);
}

TEST_CASE("RouteManager: addRoute succeeds and status is STOPPED", "[route_manager]") {
    Fixture f(4, 4);
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "ok"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    jbox_route_status_t status{};
    REQUIRE(f.rm->pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_STOPPED);
}

TEST_CASE("RouteManager: startRoute with missing device transitions to WAITING",
          "[route_manager]") {
    // Build manager with only a destination device; source UID won't resolve.
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"missing-source", "dst", m, "waiting"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);
}

TEST_CASE("RouteManager: startRoute with invalid channel index goes to ERROR",
          "[route_manager]") {
    Fixture f(2, 2);
    // Route references dst channel 5 which does not exist (dst has 2 channels).
    std::vector<ChannelEdge> m{{0, 5}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "bad-dst"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(f.rm->startRoute(id) == JBOX_ERR_MAPPING_INVALID);
    jbox_route_status_t status{};
    f.rm->pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_ERROR);
    REQUIRE(status.last_error == JBOX_ERR_MAPPING_INVALID);
}

TEST_CASE("RouteManager: end-to-end sample flow through the engine",
          "[route_manager][integration]") {
    // 2-channel src (indices 0, 1) → 4-channel dst, routed to dst channels 2 and 3.
    // Drive input samples; verify the same samples appear on the right
    // output channels of the destination device.
    Fixture f(/*src*/ 2, /*dst*/ 4, /*buf*/ 32);

    std::vector<ChannelEdge> m{{0, 2}, {1, 3}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "flow"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    f.rm->pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // Input: 32 frames × 2 channels (src ch 0, src ch 1).
    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        input[f * 2 + 0] = static_cast<float>(f) * 0.01f;        // src ch 0
        input[f * 2 + 1] = static_cast<float>(f) * 0.01f + 1.0f; // src ch 1
    }

    // Output capture: 32 frames × 4 channels, zero-init.
    std::vector<float> output(kFrames * 4, 0.0f);

    // Drive the source device (produces the input samples) — input
    // callback writes into the ring buffer.
    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);
    // Drive the destination device — output callback reads from the
    // ring buffer and writes to selected dst channels.
    f.backend->deliverBuffer("dst", kFrames, nullptr, output.data());

    // Verify: dst ch 2 receives src ch 0's samples; dst ch 3 receives src ch 1's.
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        const float* out_frame = output.data() + frame * 4;
        REQUIRE(out_frame[0] == 0.0f);  // unmapped → zero
        REQUIRE(out_frame[1] == 0.0f);  // unmapped → zero
        REQUIRE(out_frame[2] == static_cast<float>(frame) * 0.01f);
        REQUIRE(out_frame[3] == static_cast<float>(frame) * 0.01f + 1.0f);
    }

    // Counters updated.
    f.rm->pollStatus(id, &status);
    REQUIRE(status.frames_produced == kFrames);
    REQUIRE(status.frames_consumed == kFrames);
    REQUIRE(status.underrun_count == 0);
    REQUIRE(status.overrun_count == 0);
}

TEST_CASE("RouteManager: consumer without producer produces underrun count",
          "[route_manager][integration]") {
    Fixture f(2, 2, /*buf*/ 16);
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "underrun"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    // Drive only the output side: ring buffer is empty, so each
    // deliverBuffer on dst should register one underrun.
    std::vector<float> output(16 * 2, 99.0f);
    for (int i = 0; i < 5; ++i) {
        f.backend->deliverBuffer("dst", 16, nullptr, output.data());
    }

    jbox_route_status_t status{};
    f.rm->pollStatus(id, &status);
    REQUIRE(status.underrun_count == 5);
    REQUIRE(status.frames_consumed == 0);
    // Output should be zero-filled on the mapped channels (no data).
    for (std::uint32_t i = 0; i < 16 * 2; ++i) {
        REQUIRE(output[i] == 0.0f);
    }
}

TEST_CASE("RouteManager: stopRoute returns to STOPPED and releases resources",
          "[route_manager]") {
    Fixture f(2, 2);
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "stop"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    f.rm->pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    REQUIRE(f.rm->stopRoute(id) == JBOX_OK);
    f.rm->pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_STOPPED);

    // Subsequent stopRoute is a no-op.
    REQUIRE(f.rm->stopRoute(id) == JBOX_OK);
}

TEST_CASE("RouteManager: Phase 5 lets two routes share a device",
          "[route_manager]") {
    // Two routes on the same {src, dst} pair — Phase 3 rejected this
    // with JBOX_ERR_DEVICE_BUSY, Phase 5 allows it via DeviceIOMux.
    // Channel sets are disjoint per the v1 uniqueness invariants.
    Fixture f(4, 4);
    std::vector<ChannelEdge> m1{{0, 0}, {1, 1}};
    std::vector<ChannelEdge> m2{{2, 2}, {3, 3}};

    jbox_error_t err{};
    const auto id1 = f.rm->addRoute({"src", "dst", m1, "r1"}, &err);
    REQUIRE(f.rm->startRoute(id1) == JBOX_OK);

    const auto id2 = f.rm->addRoute({"src", "dst", m2, "r2"}, &err);
    REQUIRE(f.rm->startRoute(id2) == JBOX_OK);

    jbox_route_status_t status{};
    f.rm->pollStatus(id1, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
    f.rm->pollStatus(id2, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
}

TEST_CASE("RouteManager: removeRoute stops running routes and frees slots",
          "[route_manager]") {
    Fixture f(2, 2);
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    jbox_error_t err{};
    auto id1 = f.rm->addRoute({"src", "dst", m, "r1"}, &err);
    REQUIRE(f.rm->startRoute(id1) == JBOX_OK);

    REQUIRE(f.rm->removeRoute(id1) == JBOX_OK);
    REQUIRE(f.rm->routeCount() == 0);

    // Now a new route can use the same devices.
    auto id2 = f.rm->addRoute({"src", "dst", m, "r2"}, &err);
    REQUIRE(f.rm->startRoute(id2) == JBOX_OK);
}

TEST_CASE("RouteManager: non-contiguous channel selection",
          "[route_manager][integration]") {
    // 4-channel source; route only channels 0 and 3. 4-channel dst;
    // deliver to channels 0 and 1. Shows arbitrary 1:1 mapping works.
    Fixture f(4, 4, /*buf*/ 8);
    std::vector<ChannelEdge> m{{0, 0}, {3, 1}};
    jbox_error_t err{};
    auto id = f.rm->addRoute({"src", "dst", m, "non-contig"}, &err);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    std::vector<float> in(8 * 4);
    for (std::uint32_t fr = 0; fr < 8; ++fr) {
        in[fr * 4 + 0] = static_cast<float>(fr);         // src ch 0
        in[fr * 4 + 1] = 0.0f;
        in[fr * 4 + 2] = 0.0f;
        in[fr * 4 + 3] = static_cast<float>(fr) + 100.0f; // src ch 3
    }
    std::vector<float> out(8 * 4, 0.0f);

    f.backend->deliverBuffer("src", 8, in.data(), nullptr);
    f.backend->deliverBuffer("dst", 8, nullptr, out.data());

    for (std::uint32_t fr = 0; fr < 8; ++fr) {
        REQUIRE(out[fr * 4 + 0] == static_cast<float>(fr));          // dst ch 0 ← src ch 0
        REQUIRE(out[fr * 4 + 1] == static_cast<float>(fr) + 100.0f); // dst ch 1 ← src ch 3
        REQUIRE(out[fr * 4 + 2] == 0.0f);
        REQUIRE(out[fr * 4 + 3] == 0.0f);
    }
}
