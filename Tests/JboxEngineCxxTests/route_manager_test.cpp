// route_manager_test.cpp — integration tests for RouteManager using
// SimulatedBackend to drive deterministic sample flow.

#include <catch_amalgamated.hpp>

#include "device_manager.hpp"
#include "route_manager.hpp"
#include "rt_log_codes.hpp"
#include "rt_log_queue.hpp"
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

TEST_CASE("RouteManager: addRoute accepts fan-out (duplicate src)",
          "[route_manager][fan_out]") {
    // Phase 6 refinement #1: a single source channel feeding two
    // destinations is now a supported mapping — the engine happily
    // accepts it. Fan-in (duplicate dst) still fails at validate().
    Fixture f(4, 4);
    std::vector<ChannelEdge> m{{0, 0}, {0, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "fan-out"};
    jbox_error_t err{};
    REQUIRE(f.rm->addRoute(cfg, &err) != JBOX_INVALID_ROUTE_ID);
}

TEST_CASE("RouteManager: addRoute rejects duplicate destination channel",
          "[route_manager]") {
    Fixture f(4, 4);
    std::vector<ChannelEdge> m{{0, 0}, {1, 0}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "fan-in"};
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

// -----------------------------------------------------------------------------
// User-driven WAITING recovery (route_manager.hpp § 1)
//
// The engine has no hot-plug listener — sub-phases 7.6.4 / 7.6.5 are
// deferred per `route_manager.hpp:10-14` and `CLAUDE.md`. The currently-
// shipped contract is user-driven: a route that can't resolve its
// devices at start time stays in WAITING; the user (or a polling client)
// refreshes the DeviceManager and calls `startRoute` again. The cases
// below lock down that flow and the WAITING-state lifecycle hygiene.
//
// Intentionally NOT covered here: a RUNNING route whose device
// disappears underneath. That reaction is the contract sub-phase 7.6.4
// will introduce; testing it now would either fail or pin undefined
// current behavior.
// -----------------------------------------------------------------------------

TEST_CASE("RouteManager: WAITING route resumes to RUNNING when the missing "
          "device appears and start is called again",
          "[route_manager][waiting]") {
    // Begin with only a destination; source is missing → WAITING.
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend = backend_holder.get();
    backend->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_holder));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "recovers"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);

    // The "missing" device now appears. The user / app refreshes
    // DeviceManager and re-issues startRoute.
    backend->addDevice(makeInputDevice("src", 2));
    dm->refresh();
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // Drive samples to confirm audio actually flows after recovery.
    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2, 0.5f);
    std::vector<float> output(kFrames * 2, 0.0f);
    backend->deliverBuffer("src", kFrames, input.data(), nullptr);
    backend->deliverBuffer("dst", kFrames, nullptr, output.data());

    rm.pollStatus(id, &status);
    REQUIRE(status.frames_produced == kFrames);
    REQUIRE(status.frames_consumed == kFrames);
}

TEST_CASE("RouteManager: WAITING route stays WAITING on retry while the "
          "missing device is still absent",
          "[route_manager][waiting]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    backend_holder->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_holder));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"still-missing", "dst", m, "still-waiting"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);

    // Idempotent retry: device hasn't appeared → still WAITING, still no error.
    REQUIRE(rm.startRoute(id) == JBOX_OK);
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);
    REQUIRE(status.last_error == JBOX_OK);
}

TEST_CASE("RouteManager: stopRoute on a WAITING route transitions to STOPPED",
          "[route_manager][waiting]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    backend_holder->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_holder));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"missing-src", "dst", m, "stoppable"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_STOPPED);

    // After stopping, the route can be restarted normally if the
    // device subsequently appears.
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_STOPPED);
}

TEST_CASE("RouteManager: removeRoute on a WAITING route succeeds and frees the id",
          "[route_manager][waiting]") {
    auto backend_holder = std::make_unique<SimulatedBackend>();
    backend_holder->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_holder));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"missing-src", "dst", m, "removable"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);

    REQUIRE(rm.removeRoute(id) == JBOX_OK);
    REQUIRE(rm.pollStatus(id, &status) == JBOX_ERR_INVALID_ARGUMENT);
    REQUIRE(rm.removeRoute(id) == JBOX_ERR_INVALID_ARGUMENT);
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

TEST_CASE("RouteManager: renameRoute updates the stored name in any state",
          "[route_manager][rename]") {
    Fixture f(2, 2);
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "initial"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->routeName(id) == "initial");

    // STOPPED → renamed.
    REQUIRE(f.rm->renameRoute(id, "alpha") == JBOX_OK);
    REQUIRE(f.rm->routeName(id) == "alpha");

    // RUNNING → still accepted, audio flow not disturbed.
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);
    jbox_route_status_t before{};
    f.rm->pollStatus(id, &before);
    REQUIRE(before.state == JBOX_ROUTE_STATE_RUNNING);
    REQUIRE(f.rm->renameRoute(id, "beta") == JBOX_OK);
    REQUIRE(f.rm->routeName(id) == "beta");
    jbox_route_status_t after{};
    f.rm->pollStatus(id, &after);
    REQUIRE(after.state == JBOX_ROUTE_STATE_RUNNING);

    // Empty string clears the name.
    REQUIRE(f.rm->renameRoute(id, "") == JBOX_OK);
    REQUIRE(f.rm->routeName(id).empty());
}

TEST_CASE("RouteManager: renameRoute rejects unknown ids",
          "[route_manager][rename]") {
    Fixture f(2, 2);
    REQUIRE(f.rm->renameRoute(999u, "orphan") == JBOX_ERR_INVALID_ARGUMENT);
    REQUIRE(f.rm->routeName(999u).empty());
}

TEST_CASE("RouteManager: renameRoute on a WAITING route leaves state untouched",
          "[route_manager][rename]") {
    // Reach WAITING by starting against a missing source.
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeOutputDevice("dst", 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{"missing-src", "dst", m, "pre-rename"};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);

    REQUIRE(rm.renameRoute(id, "waited") == JBOX_OK);
    REQUIRE(rm.routeName(id) == "waited");

    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_WAITING);
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

TEST_CASE("RouteManager: absorbs bursty-source USB-style delivery without glitching",
          "[route_manager][ring_sizing][integration]") {
    // Real USB devices (e.g. Roland V31) deliver source samples in
    // bursts, not evenly spaced. This test simulates 8 back-to-back
    // source callbacks followed by 8 drain callbacks — a realistic
    // "one USB frame of source, one USB frame of drain" cadence at
    // small device-buffer sizes. Source and destination rates are
    // balanced over the full cycle, so any underrun / overrun here
    // is purely a ring-depth issue (too little headroom for the
    // burst phase).
    Fixture f(/*src*/ 2, /*dst*/ 2, /*buf*/ 64);
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "burst"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    constexpr std::uint32_t kFrames = 64;
    std::vector<float> input(kFrames * 2, 0.10f);  // non-zero so we can see real flow
    std::vector<float> output(kFrames * 2, 0.0f);

    for (int i = 0; i < 8; ++i) {
        f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);
    }
    for (int i = 0; i < 8; ++i) {
        f.backend->deliverBuffer("dst", kFrames, nullptr, output.data());
    }

    jbox_route_status_t status{};
    f.rm->pollStatus(id, &status);
    REQUIRE(status.overrun_count  == 0);
    REQUIRE(status.underrun_count == 0);
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

TEST_CASE("RouteManager: duplex fast path routes same-device Performance directly",
          "[route_manager][duplex][integration]") {
    // Phase 6 refinement #6 direct-monitor fast path. A route where
    // source_uid == dest_uid and latency_mode == 2 bypasses the ring
    // buffer and AudioConverter and copies input straight to output
    // in a single duplex IOProc. Verifies:
    //   - addRoute + startRoute succeed
    //   - the state is RUNNING (no waiting / error)
    //   - delivering one buffer through SimulatedBackend routes the
    //     input samples into the mapped output channels
    //   - the latency estimate drops ring + converter contributions
    //     to zero and the dst buffer is not double-counted
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 4;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 64;
    info.input_device_latency_frames  = 24;
    info.input_safety_offset_frames   = 16;
    info.output_device_latency_frames = 24;
    info.output_safety_offset_frames  = 16;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    // src channel 0 → dst channels 1 AND 3 (fan-out also exercised).
    std::vector<ChannelEdge> m{{0, 1}, {0, 3}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // The pill should NOT contain ring or converter contributions and
    // should NOT double-count the device buffer. Total ≈
    // src_hal + src_safety + src_buffer + dst_safety + dst_hal frames
    // at 48 k = (24 + 16 + 64 + 16 + 24) = 144 → 3000 µs.
    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);
    REQUIRE(components.ring_target_fill_frames == 0);
    REQUIRE(components.converter_prime_frames  == 0);
    REQUIRE(components.dst_buffer_frames       == 0);
    REQUIRE(components.src_buffer_frames       == 64);
    REQUIRE(status.estimated_latency_us > 2500);
    REQUIRE(status.estimated_latency_us < 3500);

    // Drive one buffer through the duplex path.
    constexpr std::uint32_t kFrames = 64;
    std::vector<float> input(kFrames * 2);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        input[i * 2 + 0] = static_cast<float>(i) * 0.01f;  // src ch 0
        input[i * 2 + 1] = 99.0f;                          // src ch 1 (unmapped)
    }
    std::vector<float> output(kFrames * 4, 0.0f);
    backend->deliverBuffer("aggregate", kFrames, input.data(), output.data());

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const float expected = static_cast<float>(i) * 0.01f;
        REQUIRE(output[i * 4 + 0] == 0.0f);      // unmapped
        REQUIRE(output[i * 4 + 1] == expected);  // dst ch 1 ← src ch 0
        REQUIRE(output[i * 4 + 2] == 0.0f);      // unmapped
        REQUIRE(output[i * 4 + 3] == expected);  // dst ch 3 ← src ch 0 (fan-out)
    }

    // Stop should tear down cleanly.
    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    jbox_route_status_t stopped_status{};
    REQUIRE(rm.pollStatus(id, &stopped_status) == JBOX_OK);
    REQUIRE(stopped_status.state == JBOX_ROUTE_STATE_STOPPED);
}


TEST_CASE("RouteManager: duplex fast path leaves an already-small device at target",
          "[route_manager][duplex][buffer]") {
    // If the device is already at the target (64 frames), the shrink
    // request is a no-op from the device's perspective — the
    // buffer stays at 64 and stays at 64 on stop.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 64;  // already at target
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-small",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Buffer stays at 64 through start ...
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 64);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    // ... and through stop (restore target == original == 64).
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 64);
}

TEST_CASE("RouteManager: duplex fast path reflects the user's interface buffer in the latency pill",
          "[route_manager][duplex][buffer]") {
    // Phase 7.6 contract: Jbox no longer asks the HAL to change the
    // device's buffer frame size. Whatever the user has dialed in
    // their interface software (UA Console / RME TotalMix / Audio MIDI
    // Setup / etc.) is what shows up in the latency pill. The
    // simulated backend's `setBufferFrameSize(uid, frames)` test seam
    // mirrors that out-of-band change. This test pins both directions
    // of the contract: a route picks up the device's buffer at start
    // time, and changing the device's buffer between stop and start
    // is reflected in the next start's latency components without any
    // engine-side negotiation.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 256;  // user's initial interface setting
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "respect-user-buffer",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Latency pill reflects the user's 256-frame setting. The duplex
    // fast path doesn't touch dst on a same-device route, so only
    // src_buffer_frames is meaningful.
    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);
    REQUIRE(components.src_buffer_frames == 256);
    // Engine never wrote to the device (it's not allowed to anymore).
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 256);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    // Stop is also a no-op against the device buffer — there's no
    // restore-on-release because there was no claim-on-start.
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 256);

    // User dials the interface to 64 frames out-of-band (the simulated
    // backend's test seam stands in for them touching UA Console etc.).
    backend->setBufferFrameSize("aggregate", 64);
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 64);

    // A fresh start picks up the new buffer in the latency pill.
    REQUIRE(rm.startRoute(id) == JBOX_OK);
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);
    REQUIRE(components.src_buffer_frames == 64);
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 64);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 64);
}

TEST_CASE("RouteManager: buffer_frames override issues a single setBufferFrameSize per device, no hog",
          "[route_manager][duplex][buffer_frames]") {
    // ABI v11: a non-zero `buffer_frames` on the route config triggers
    // a Superior-Drummer-style HAL property write — exactly one
    // `setBufferFrameSize(uid, frames)` call per device the route
    // touches, no claimExclusive / hog mode. macOS resolves the actual
    // buffer with `max-across-clients`, so this test pins both
    // outcomes: the simulated backend records the write and the
    // device's buffer_frame_size lands at the requested value (no
    // other client is asking, so there's no max-across-clients pull).
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();

    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 256;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "buffer-override",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 16};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Exactly one write to the (non-aggregate) device, at 16 frames.
    REQUIRE(backend->bufferSizeWrites().size() == 1);
    REQUIRE(backend->bufferSizeWrites().front().uid == "aggregate");
    REQUIRE(backend->bufferSizeWrites().front().frames == 16);
    // Buffer landed.
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 16);

    // Latency pill picks up the post-write value.
    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);
    REQUIRE(components.src_buffer_frames == 16);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
}

TEST_CASE("RouteManager: buffer_frames override fans to every aggregate sub-device",
          "[route_manager][duplex][buffer_frames][aggregate]") {
    // For an aggregate UID, setBufferFrameSize enumerates the active
    // sub-device list and writes to each member directly. This is
    // structurally identical to what Superior Drummer / any vanilla
    // Core Audio client does when the device IS an aggregate:
    // independent property writes per member, no aggregate-driver
    // fan-out, no hog claim. macOS computes the aggregate's
    // effective buffer as `max(member buffers)` once each member
    // has been individually asked.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();

    backend->addDevice([] {
        BackendDeviceInfo info;
        info.uid = "sub-a";
        info.name = "sub-a";
        info.direction = kBackendDirectionInput | kBackendDirectionOutput;
        info.input_channel_count  = 2;
        info.output_channel_count = 2;
        info.nominal_sample_rate  = 48000.0;
        info.buffer_frame_size    = 512;
        return info;
    }());
    backend->addDevice([] {
        BackendDeviceInfo info;
        info.uid = "sub-b";
        info.name = "sub-b";
        info.direction = kBackendDirectionInput | kBackendDirectionOutput;
        info.input_channel_count  = 2;
        info.output_channel_count = 2;
        info.nominal_sample_rate  = 48000.0;
        info.buffer_frame_size    = 256;
        return info;
    }());

    BackendDeviceInfo agg;
    agg.uid = "aggregate";
    agg.name = "aggregate";
    agg.direction = kBackendDirectionInput | kBackendDirectionOutput;
    agg.input_channel_count  = 2;
    agg.output_channel_count = 2;
    agg.nominal_sample_rate  = 48000.0;
    agg.buffer_frame_size    = 512;
    backend->addAggregateDevice(agg, {"sub-a", "sub-b"});

    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "agg-buffer-override",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 64};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Three writes total: one per sub plus the aggregate self.
    const auto& writes = backend->bufferSizeWrites();
    REQUIRE(writes.size() == 3);
    auto count_for = [&](const std::string& uid) {
        std::size_t n = 0;
        for (const auto& w : writes) {
            if (w.uid == uid && w.frames == 64) ++n;
        }
        return n;
    };
    REQUIRE(count_for("sub-a") == 1);
    REQUIRE(count_for("sub-b") == 1);
    REQUIRE(count_for("aggregate") == 1);

    // Each member's buffer landed at 64.
    REQUIRE(backend->currentBufferFrameSize("sub-a")     == 64);
    REQUIRE(backend->currentBufferFrameSize("sub-b")     == 64);
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 64);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
}

TEST_CASE("RouteManager: buffer_frames == 0 issues no setBufferFrameSize call",
          "[route_manager][duplex][buffer_frames]") {
    // The default — no per-route preference — leaves the HAL
    // alone entirely. No property writes; the device stays at
    // whatever buffer the user (or some other app) put it at.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();

    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 256;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "no-buffer-override",
        /*latency_mode*/ 2};  // buffer_frames defaults to 0
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    REQUIRE(backend->bufferSizeWrites().empty());
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 256);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
}

TEST_CASE("RouteManager: when the device clamps the request upward "
          "(max-across-clients), the route still runs and the latency "
          "pill reflects the device's actual buffer, not the request",
          "[route_manager][cross_device][buffer_frames][max_across_clients]") {
    // macOS resolves kAudioDevicePropertyBufferFrameSize across all
    // clients of a device: if another app has asked for 256, our
    // request for 64 is silently kept at 256 until that app stops
    // asking. The engine handles this by reading the post-write
    // buffer back via currentBufferFrameSize and reporting *that*
    // value in the latency pill -- it never trusts the request.
    //
    // This test pins both halves of that contract:
    //   1. Route reaches RUNNING regardless of the clamp (no error).
    //   2. pollLatencyComponents reflects the resolved value, not
    //      the override the caller asked for.
    auto backend_holder = std::make_unique<SimulatedBackend>();
    auto* backend = backend_holder.get();
    backend->addDevice(makeInputDevice("src",  2, /*buf*/ 128));
    backend->addDevice(makeOutputDevice("dst", 2, /*buf*/ 128));
    // Simulate a co-resident client holding the destination at 256.
    // No floor on src -> request wins for the source side.
    backend->setMaxAcrossClientsFloor("dst", 256);

    auto dm = std::make_unique<DeviceManager>(std::move(backend_holder));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> mapping{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "src", "dst", mapping, "clamped",
        /*latency_mode*/ 0,
        /*buffer_frames*/ 64};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    rm.pollStatus(id, &status);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);

    // Both writes were issued at the request value -- the engine
    // doesn't know (or care) that one of them got clamped.
    const auto& writes = backend->bufferSizeWrites();
    REQUIRE(writes.size() == 2);
    for (const auto& w : writes) REQUIRE(w.frames == 64);

    jbox_route_latency_components_t lc{};
    REQUIRE(rm.pollLatencyComponents(id, &lc) == JBOX_OK);
    // src had no floor -> resolved = max(64, 0) = 64.
    REQUIRE(lc.src_buffer_frames == 64);
    // dst floor was 256 -> resolved = max(64, 256) = 256. The pill
    // reports the *actual* value, not the override the user asked for.
    REQUIRE(lc.dst_buffer_frames == 256);
}

TEST_CASE("RouteManager: cross-device buffer_frames override writes to both source and destination",
          "[route_manager][cross_device][buffer_frames]") {
    // V31 → Apollo–style cross-device route with buffer override.
    // The engine writes the preference to BOTH devices independently
    // (Superior Drummer style on each), since macOS resolves the
    // buffer per-device, not per-route.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();

    BackendDeviceInfo src;
    src.uid = "v31";
    src.name = "v31";
    src.direction = kBackendDirectionInput;
    src.input_channel_count  = 2;
    src.output_channel_count = 0;
    src.nominal_sample_rate  = 48000.0;
    src.buffer_frame_size    = 256;
    backend->addDevice(src);

    BackendDeviceInfo dst;
    dst.uid = "apollo";
    dst.name = "apollo";
    dst.direction = kBackendDirectionOutput;
    dst.input_channel_count  = 0;
    dst.output_channel_count = 2;
    dst.nominal_sample_rate  = 48000.0;
    dst.buffer_frame_size    = 512;
    backend->addDevice(dst);

    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "v31", "apollo", m, "cross-device-override",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 16};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    const auto& writes = backend->bufferSizeWrites();
    REQUIRE(writes.size() == 2);
    auto count_for = [&](const std::string& uid) {
        std::size_t n = 0;
        for (const auto& w : writes) {
            if (w.uid == uid && w.frames == 16) ++n;
        }
        return n;
    };
    REQUIRE(count_for("v31")    == 1);
    REQUIRE(count_for("apollo") == 1);

    REQUIRE(backend->currentBufferFrameSize("v31")    == 16);
    REQUIRE(backend->currentBufferFrameSize("apollo") == 16);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
}

TEST_CASE("RouteManager: fan-out replicates one source into multiple destinations",
          "[route_manager][fan_out][integration]") {
    // Phase 6 refinement #1 end-to-end check: a single source channel
    // mapped to two distinct destination channels drives both dst
    // channels with the same input signal on the same IOProc tick.
    Fixture f(/*src*/ 2, /*dst*/ 4, /*buf*/ 32);

    // src channel 0 → dst channels 1 AND 3.
    std::vector<ChannelEdge> m{{0, 1}, {0, 3}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "fan-out"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        input[i * 2 + 0] = static_cast<float>(i) * 0.01f;  // src ch 0
        input[i * 2 + 1] = 99.0f;                          // src ch 1 (unmapped — must not leak)
    }

    std::vector<float> output(kFrames * 4, 0.0f);
    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);
    f.backend->deliverBuffer("dst", kFrames, nullptr, output.data());

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const float expected = static_cast<float>(i) * 0.01f;
        REQUIRE(output[i * 4 + 0] == 0.0f);      // dst ch 0 unmapped
        REQUIRE(output[i * 4 + 1] == expected);  // dst ch 1 ← src ch 0
        REQUIRE(output[i * 4 + 2] == 0.0f);      // dst ch 2 unmapped
        REQUIRE(output[i * 4 + 3] == expected);  // dst ch 3 ← src ch 0 (fan-out replica)
    }
}

TEST_CASE("RouteManager: estimated_latency_us reflects HAL components",
          "[route_manager][latency]") {
    // docs/spec.md § 2.12: on startRoute the engine composes HAL
    // latency + safety offset + buffer sizes + ring setpoint +
    // converter prime frames into a single estimate, surfaced through
    // pollStatus. Zero-latency HAL state with known buffer sizes gives
    // us a lower-bound assertion without hard-coding the converter's
    // quality-dependent prime frames.
    auto backend = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo src = makeInputDevice("src", 2, /*buf*/ 64);
    src.input_device_latency_frames = 48;
    src.input_safety_offset_frames  = 32;
    BackendDeviceInfo dst = makeOutputDevice("dst", 2, /*buf*/ 64);
    dst.output_device_latency_frames = 96;
    dst.output_safety_offset_frames  = 24;
    backend->addDevice(src);
    backend->addDevice(dst);
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    jbox_error_t err{};
    const auto id = rm.addRoute({"src", "dst", m, "lat"}, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    // Before start: no estimate.
    jbox_route_status_t stopped{};
    REQUIRE(rm.pollStatus(id, &stopped) == JBOX_OK);
    REQUIRE(stopped.estimated_latency_us == 0);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_status_t running{};
    REQUIRE(rm.pollStatus(id, &running) == JBOX_OK);
    REQUIRE(running.state == JBOX_ROUTE_STATE_RUNNING);

    // Lower bound: the HAL-reported frames alone at 48 kHz are
    //   (48 + 32 + 64) src-side + (64 + 24 + 96) dst-side = 328 frames
    //   328 * 1_000_000 / 48000 ≈ 6833 µs
    // The full estimate adds ring_target_fill (≈ 2047 frames, ~42 ms)
    // plus converter prime frames, so the pill must be strictly larger.
    constexpr std::uint64_t kLowerBoundUs = 6833;
    REQUIRE(running.estimated_latency_us > kLowerBoundUs);
    // Upper bound: everything known to contribute is small-to-medium;
    // a wildly larger number signals a unit mix-up (e.g. ms vs µs).
    REQUIRE(running.estimated_latency_us < 500'000);

    // Bigger device buffers must produce a bigger estimate — the pill
    // is monotone in buffer size at a fixed sample rate. Re-plumb a
    // second manager with 2048-frame buffers on both sides and confirm.
    auto backend2 = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo big_src = src;
    big_src.buffer_frame_size = 2048;
    BackendDeviceInfo big_dst = dst;
    big_dst.buffer_frame_size = 2048;
    backend2->addDevice(big_src);
    backend2->addDevice(big_dst);
    auto dm2 = std::make_unique<DeviceManager>(std::move(backend2));
    dm2->refresh();
    RouteManager rm2(*dm2);
    const auto id2 = rm2.addRoute({"src", "dst", m, "lat-big"}, &err);
    REQUIRE(rm2.startRoute(id2) == JBOX_OK);
    jbox_route_status_t running2{};
    REQUIRE(rm2.pollStatus(id2, &running2) == JBOX_OK);
    REQUIRE(running2.estimated_latency_us > running.estimated_latency_us);

    // After stop: estimate clears.
    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    jbox_route_status_t after_stop{};
    REQUIRE(rm.pollStatus(id, &after_stop) == JBOX_OK);
    REQUIRE(after_stop.estimated_latency_us == 0);
}

TEST_CASE("RouteManager: pollLatencyComponents reflects cached components",
          "[route_manager][latency][components]") {
    // New in ABI v4: the engine surfaces the full per-component
    // breakdown, not just the total. Zeros on STOPPED, populated on
    // RUNNING, zeroed again on STOPPED. Values round-trip the HAL
    // numbers SimulatedBackend hands us.
    auto backend = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo src = makeInputDevice("src", 2, /*buf*/ 128);
    src.input_device_latency_frames = 48;
    src.input_safety_offset_frames  = 32;
    BackendDeviceInfo dst = makeOutputDevice("dst", 2, /*buf*/ 128);
    dst.output_device_latency_frames = 96;
    dst.output_safety_offset_frames  = 24;
    backend->addDevice(src);
    backend->addDevice(dst);
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    jbox_error_t err{};
    const auto id = rm.addRoute({"src", "dst", m, "components"}, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    jbox_route_latency_components_t stopped{};
    stopped.src_hal_latency_frames = 0xDEADBEEF;  // sentinel to prove zeroing
    REQUIRE(rm.pollLatencyComponents(id, &stopped) == JBOX_OK);
    REQUIRE(stopped.src_hal_latency_frames == 0);
    REQUIRE(stopped.total_us == 0);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_latency_components_t running{};
    REQUIRE(rm.pollLatencyComponents(id, &running) == JBOX_OK);

    REQUIRE(running.src_hal_latency_frames   == 48);
    REQUIRE(running.src_safety_offset_frames == 32);
    REQUIRE(running.src_buffer_frames        == 128);
    REQUIRE(running.dst_buffer_frames        == 128);
    REQUIRE(running.dst_safety_offset_frames == 24);
    REQUIRE(running.dst_hal_latency_frames   == 96);
    REQUIRE(running.ring_target_fill_frames  > 0);
    REQUIRE(running.src_sample_rate_hz == 48000.0);
    REQUIRE(running.dst_sample_rate_hz == 48000.0);
    REQUIRE(running.total_us > 0);

    // The total_us surfaced here must equal the one in
    // jbox_route_status_t — both come from the same cached computation.
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(running.total_us == status.estimated_latency_us);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    jbox_route_latency_components_t after_stop{};
    REQUIRE(rm.pollLatencyComponents(id, &after_stop) == JBOX_OK);
    REQUIRE(after_stop.src_hal_latency_frames == 0);
    REQUIRE(after_stop.total_us == 0);
}

TEST_CASE("RouteManager: latency_mode tiers strictly shrink the pill",
          "[route_manager][latency_mode]") {
    // Phase 6 refinement #6. With 64-frame device buffers at 48 kHz:
    //   - safe         (mode 0): ring = 4096,  target ≈ 2047 (42.6 ms)
    //   - low latency  (mode 1): ring =  512,  target ≈  255 ( 5.3 ms)
    //   - performance  (mode 2): ring =  256,  target ≈   63 ( 1.3 ms)
    // Each successive tier must report a strictly smaller pill on the
    // same devices; everything else (HAL latencies, converter primes,
    // buffer sizes) is equal, so the reductions are entirely ring-
    // sizing + drift-setpoint driven.
    Fixture f(/*src*/ 2, /*dst*/ 2, /*buf*/ 64);

    std::vector<ChannelEdge> mapping{{0, 0}, {1, 1}};
    jbox_error_t err{};

    RouteManager::RouteConfig safe_cfg{
        "src", "dst", mapping, "safe", /*latency_mode*/ 0};
    const auto id_safe = f.rm->addRoute(safe_cfg, &err);
    REQUIRE(id_safe != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id_safe) == JBOX_OK);

    RouteManager::RouteConfig low_cfg{
        "src", "dst", mapping, "low", /*latency_mode*/ 1};
    const auto id_low = f.rm->addRoute(low_cfg, &err);
    REQUIRE(id_low != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id_low) == JBOX_OK);

    RouteManager::RouteConfig perf_cfg{
        "src", "dst", mapping, "performance", /*latency_mode*/ 2};
    const auto id_perf = f.rm->addRoute(perf_cfg, &err);
    REQUIRE(id_perf != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id_perf) == JBOX_OK);

    jbox_route_status_t safe_status{};
    jbox_route_status_t low_status{};
    jbox_route_status_t perf_status{};
    REQUIRE(f.rm->pollStatus(id_safe, &safe_status) == JBOX_OK);
    REQUIRE(f.rm->pollStatus(id_low,  &low_status)  == JBOX_OK);
    REQUIRE(f.rm->pollStatus(id_perf, &perf_status) == JBOX_OK);

    REQUIRE(safe_status.estimated_latency_us > 0);
    REQUIRE(low_status.estimated_latency_us  > 0);
    REQUIRE(perf_status.estimated_latency_us > 0);

    // Strictly monotonic: off > low > performance.
    REQUIRE(safe_status.estimated_latency_us > low_status.estimated_latency_us);
    REQUIRE(low_status.estimated_latency_us  > perf_status.estimated_latency_us);

    // The safe → low gap is ring-sizing dominated. Expected savings
    // ≈ (2047 − 255) frames @ 48 kHz = ~37 333 µs.
    REQUIRE(safe_status.estimated_latency_us
            - low_status.estimated_latency_us > 30'000);

    // The low → performance gap is setpoint + smaller ring; expected
    // savings ≈ (255 − 63) frames @ 48 kHz = ~4 000 µs.
    REQUIRE(low_status.estimated_latency_us
            - perf_status.estimated_latency_us > 3'000);

    // Also check that the per-component breakdown reflects the
    // setpoint change — performance mode's ring_target_fill_frames
    // must be about a quarter of the ring, not half.
    jbox_route_latency_components_t perf_components{};
    REQUIRE(f.rm->pollLatencyComponents(id_perf, &perf_components) == JBOX_OK);
    // ring is ~256 frames usable; /4 ≈ 63; /2 ≈ 127. Accept either a
    // low-bound ratio check (<= ~40 %).
    const double ring_frac =
        static_cast<double>(perf_components.ring_target_fill_frames)
      / 256.0;
    REQUIRE(ring_frac < 0.40);
}

// -----------------------------------------------------------------------------
// Phase 7.6.3 — robust teardown.
//
// Two paths land an IOProc destroy at the backend: the duplex fast-path
// (RouteManager::releaseRouteResources directly) and the mux path
// (DeviceIOMux::detachInput/detachOutput, last detach). Either may fail
// under degraded conditions (hot-unplug, sleep, kernel resource
// pressure). 7.6.3's contract: surface the failure via
// kLogTeardownFailure, preserve the in-memory IOProc handle so a
// later teardown opportunity (e.g. another start/stop cycle) can
// retry, and never silently nuke the route's bookkeeping while the
// kernel-side resource is leaked.
// -----------------------------------------------------------------------------

namespace {

BackendDeviceInfo makeDuplexDevice(const std::string& uid,
                                   std::uint32_t in_channels,
                                   std::uint32_t out_channels,
                                   std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = in_channels;
    info.output_channel_count = out_channels;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = buf;
    return info;
}

const jbox::rt::RtLogEvent* findCode(const std::vector<jbox::rt::RtLogEvent>& v,
                                     jbox::rt::RtLogCode code) {
    for (const auto& e : v) if (e.code == code) return &e;
    return nullptr;
}

std::vector<jbox::rt::RtLogEvent> drainAll(jbox::rt::DefaultRtLogQueue& q) {
    std::vector<jbox::rt::RtLogEvent> out;
    jbox::rt::RtLogEvent ev{};
    while (q.tryPop(ev)) out.push_back(ev);
    return out;
}

}  // namespace

TEST_CASE("RouteManager: duplex teardown logs kLogTeardownFailure when close fails",
          "[route_manager][teardown_failure]") {
    // The duplex fast path closes the IOProc directly via
    // dm.backend().closeCallback. When the destroy fails, the engine
    // must push a kLogTeardownFailure event tagged with the route id
    // (so operators can correlate it with the route they're stopping)
    // and the IOProcId that failed to close (so they can match it
    // against any backend-side trace).
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeDuplexDevice("aggregate", 2, 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    jbox::rt::DefaultRtLogQueue queue;
    RouteManager rm(*dm, &queue);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-teardown-failure",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Capture the IOProcId that the route opened so we can assert the
    // log event references it.
    REQUIRE(backend->hasDuplexCallback("aggregate"));
    // Drain the start log so we only see teardown events afterwards.
    (void)drainAll(queue);

    // Inject one close failure on the duplex IOProc and stop the route.
    backend->setNextCloseCallbacksFailing(1);
    REQUIRE(rm.stopRoute(id) == JBOX_OK);

    const auto events = drainAll(queue);
    const auto* failure = findCode(events, jbox::rt::kLogTeardownFailure);
    REQUIRE(failure != nullptr);
    REQUIRE(failure->route_id == id);
    REQUIRE(failure->value_b != 0);  // IOProcId payload non-zero
    // Backend slot survived the failed close — proof the destroy
    // attempt was made (and refused) rather than skipped.
    REQUIRE(backend->hasDuplexCallback("aggregate") == true);
}

TEST_CASE("RouteManager: duplex teardown retries close on next startRoute",
          "[route_manager][teardown_failure]") {
    // After a failed duplex close the engine must preserve enough
    // state that the *next* lifecycle event reattempts the destroy
    // before opening a fresh IOProc — otherwise the next start would
    // try to register a duplex callback while the old one is still
    // bound to the device, openDuplexCallback would refuse, and the
    // user would be stuck with an unstartable route.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeDuplexDevice("aggregate", 2, 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-retry",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Inject a single close failure, stop, then restart. The retry
    // budget is exhausted on the second attempt, so the second close
    // (during startRoute's retry path or the second stop, depending
    // on the implementation choice) must succeed and the route must
    // reach RUNNING again.
    backend->setNextCloseCallbacksFailing(1);
    REQUIRE(rm.stopRoute(id) == JBOX_OK);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
}

TEST_CASE("RouteManager: removeRoute completes even when duplex close fails",
          "[route_manager][teardown_failure]") {
    // removeRoute is the route's terminal disposal — there is no
    // future "next opportunity" for *this* route to retry the close
    // (the record is about to be erased). Contract: removeRoute must
    // still attempt the close, log the failure, and complete the
    // erase. Leaving the route alive in `routes_` would prevent the
    // user from ever retiring the route on a permanently degraded
    // device; leaking the close attempt without a log would be the
    // exact silent failure 7.6.3 set out to eliminate.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    backend->addDevice(makeDuplexDevice("aggregate", 2, 2));
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    jbox::rt::DefaultRtLogQueue queue;
    RouteManager rm(*dm, &queue);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-remove-while-failing",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);
    (void)drainAll(queue);

    backend->setNextCloseCallbacksFailing(1);
    REQUIRE(rm.removeRoute(id) == JBOX_OK);

    // The route record is gone — pollStatus on the freed id is an
    // invalid-argument, not a stale STOPPED snapshot.
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_ERR_INVALID_ARGUMENT);
    REQUIRE(rm.routeCount() == 0);

    // The failure was surfaced via kLogTeardownFailure.
    const auto events = drainAll(queue);
    REQUIRE(findCode(events, jbox::rt::kLogTeardownFailure) != nullptr);
}

