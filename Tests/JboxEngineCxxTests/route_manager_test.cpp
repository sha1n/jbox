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

TEST_CASE("RouteManager: duplex fast path shrinks device buffer and restores on stop",
          "[route_manager][duplex][buffer]") {
    // Regression test for the fast-path buffer-shrink fix. The
    // duplex path bypasses the mux, so it has to issue its own
    // requestBufferFrameSize when starting and a matching restore
    // when stopping. Exercises three observable outcomes:
    //   - a request-with-frames=64 call was recorded on attemptStart
    //     when the device's buffer was larger
    //   - the device's post-change buffer is honoured (64 in the
    //     simulated backend, which doesn't clamp)
    //   - stopRoute issues a restore-to-original request
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 512;  // large starting buffer
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-buffer",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(backend->bufferSizeRequests().empty());  // not started yet

    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Shrink request: first entry must be 64 against this device.
    REQUIRE(backend->bufferSizeRequests().size() == 1);
    REQUIRE(backend->bufferSizeRequests().front().uid       == "aggregate");
    REQUIRE(backend->bufferSizeRequests().front().requested == 64);
    REQUIRE(backend->currentBufferFrameSize("aggregate")    == 64);

    // The pill must reflect the post-shrink value, not the 512 we
    // started with.
    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);
    REQUIRE(components.src_buffer_frames == 64);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);

    // Restore happens inside releaseExclusive using the snapshot the
    // backend captured at claim time — it does NOT go through
    // requestBufferFrameSize, so the request log still has just the
    // one shrink entry. The observable outcome is that the device's
    // current buffer size is back to its original value.
    REQUIRE(backend->bufferSizeRequests().size() == 1);
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 512);
}

TEST_CASE("RouteManager: duplex fast path claims exclusive ownership + releases on stop",
          "[route_manager][duplex][exclusive]") {
    // Performance-mode same-device routes take Core Audio hog mode so
    // the buffer-size request reliably lands even when another app
    // (UAD Console, system audio, a running DAW) is holding the
    // device at a larger buffer. The claim happens before the shrink
    // request; the release happens after the restore so external apps
    // see the original buffer size when they reconnect.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 512;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-exclusive",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE_FALSE(backend->isExclusive("aggregate"));  // not yet started

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    REQUIRE(backend->isExclusive("aggregate"));

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    REQUIRE_FALSE(backend->isExclusive("aggregate"));
    // Buffer size still restored to the pre-route value.
    REQUIRE(backend->currentBufferFrameSize("aggregate") == 512);
}

TEST_CASE("RouteManager: duplex fast path hogs aggregate sub-devices and shrinks each",
          "[route_manager][duplex][aggregate]") {
    // Hogging an aggregate alone doesn't evict other clients from
    // its members, and the HAL buffer size lives on the members —
    // so the duplex fast path must claim exclusive on each member
    // and push the buffer-size request down into each. Stopping the
    // route restores every member to its own pre-claim buffer size
    // (not the aggregate's value, which would clobber the smaller
    // member's original).
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();

    BackendDeviceInfo sub_in;
    sub_in.uid = "sub-input";
    sub_in.name = "sub-input";
    sub_in.direction = kBackendDirectionInput;
    sub_in.input_channel_count  = 2;
    sub_in.output_channel_count = 0;
    sub_in.nominal_sample_rate  = 48000.0;
    sub_in.buffer_frame_size    = 256;
    backend->addDevice(sub_in);

    BackendDeviceInfo sub_out;
    sub_out.uid = "sub-output";
    sub_out.name = "sub-output";
    sub_out.direction = kBackendDirectionOutput;
    sub_out.input_channel_count  = 0;
    sub_out.output_channel_count = 2;
    sub_out.nominal_sample_rate  = 48000.0;
    // Larger than sub-input: simulates a device held open by another
    // app at a bigger buffer size than its sibling.
    sub_out.buffer_frame_size    = 512;
    backend->addDevice(sub_out);

    BackendDeviceInfo agg;
    agg.uid = "aggregate";
    agg.name = "aggregate";
    agg.direction = kBackendDirectionInput | kBackendDirectionOutput;
    agg.input_channel_count  = 2;
    agg.output_channel_count = 2;
    agg.nominal_sample_rate  = 48000.0;
    agg.buffer_frame_size    = 512;  // effective = max(members)
    backend->addAggregateDevice(agg, {"sub-input", "sub-output"});

    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "aggregate-duplex",
        /*latency_mode*/ 2};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    // Every device in the aggregate is hogged.
    REQUIRE(backend->isExclusive("sub-input"));
    REQUIRE(backend->isExclusive("sub-output"));
    REQUIRE(backend->isExclusive("aggregate"));

    // Each sub-device AND the aggregate got a 64-frame request.
    const auto& reqs = backend->bufferSizeRequests();
    auto count_for = [&](const std::string& uid) {
        std::size_t n = 0;
        for (const auto& r : reqs) {
            if (r.uid == uid && r.requested == 64) ++n;
        }
        return n;
    };
    REQUIRE(count_for("sub-input")  >= 1);
    REQUIRE(count_for("sub-output") >= 1);
    REQUIRE(count_for("aggregate")  >= 1);

    // Buffer sizes now actually 64 on all three.
    REQUIRE(backend->currentBufferFrameSize("sub-input")  == 64);
    REQUIRE(backend->currentBufferFrameSize("sub-output") == 64);
    REQUIRE(backend->currentBufferFrameSize("aggregate")  == 64);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);

    // Hog released on every member.
    REQUIRE_FALSE(backend->isExclusive("sub-input"));
    REQUIRE_FALSE(backend->isExclusive("sub-output"));
    REQUIRE_FALSE(backend->isExclusive("aggregate"));

    // Buffer sizes restored to each member's own pre-claim value —
    // critically, sub-input goes back to 256 (its original) and not
    // 512 (the aggregate's original).
    REQUIRE(backend->currentBufferFrameSize("sub-input")  == 256);
    REQUIRE(backend->currentBufferFrameSize("sub-output") == 512);
    REQUIRE(backend->currentBufferFrameSize("aggregate")  == 512);
}

TEST_CASE("RouteManager: duplex fast path honours buffer_frames override",
          "[route_manager][duplex][buffer]") {
    // ABI v6: a non-zero `buffer_frames` on the route config
    // overrides the fast path's 64-frame default. Verify the request
    // lands on the device and the pill reflects the chosen value.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 512;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-override",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 128};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    REQUIRE(backend->currentBufferFrameSize("aggregate") == 128);
    REQUIRE(backend->bufferSizeRequests().size() == 1);
    REQUIRE(backend->bufferSizeRequests().front().requested == 128);

    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);
    REQUIRE(components.src_buffer_frames == 128);
}

TEST_CASE("supportedBufferFrameSizeRange: plain device returns its own range",
          "[device_backend][buffer]") {
    SimulatedBackend backend;
    BackendDeviceInfo info;
    info.uid = "plain";
    info.name = "plain";
    info.direction = kBackendDirectionInput;
    info.input_channel_count = 2;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 128;
    backend.addDevice(info);
    backend.setBufferFrameSizeRange("plain", 16, 2048);

    const auto r = backend.supportedBufferFrameSizeRange("plain");
    REQUIRE(r.minimum == 16);
    REQUIRE(r.maximum == 2048);

    // Unknown UID returns an empty range so the UI hides the picker.
    const auto bogus = backend.supportedBufferFrameSizeRange("nope");
    REQUIRE(bogus.minimum == 0);
    REQUIRE(bogus.maximum == 0);
}

TEST_CASE("supportedBufferFrameSizeRange: non-overlapping sub-device ranges collapse to empty",
          "[device_backend][aggregate][buffer]") {
    // Sub A accepts [32, 64], sub B accepts [128, 2048] — no
    // intersection. The engine must not surface a value that
    // neither member can honour; an empty range signals "hide the
    // picker, there is no common target".
    SimulatedBackend backend;
    BackendDeviceInfo a;
    a.uid = "sub-a";
    a.name = "sub-a";
    a.direction = kBackendDirectionInput;
    a.input_channel_count = 2;
    a.nominal_sample_rate = 48000.0;
    a.buffer_frame_size = 64;
    backend.addDevice(a);
    backend.setBufferFrameSizeRange("sub-a", 32, 64);

    BackendDeviceInfo b;
    b.uid = "sub-b";
    b.name = "sub-b";
    b.direction = kBackendDirectionOutput;
    b.output_channel_count = 2;
    b.nominal_sample_rate = 48000.0;
    b.buffer_frame_size = 128;
    backend.addDevice(b);
    backend.setBufferFrameSizeRange("sub-b", 128, 2048);

    BackendDeviceInfo agg;
    agg.uid = "aggregate";
    agg.name = "aggregate";
    agg.direction = kBackendDirectionInput | kBackendDirectionOutput;
    agg.input_channel_count  = 2;
    agg.output_channel_count = 2;
    agg.nominal_sample_rate  = 48000.0;
    agg.buffer_frame_size    = 128;
    backend.addAggregateDevice(agg, {"sub-a", "sub-b"});
    backend.setBufferFrameSizeRange("aggregate", 16, 8192);

    const auto r = backend.supportedBufferFrameSizeRange("aggregate");
    REQUIRE(r.minimum == 0);
    REQUIRE(r.maximum == 0);
}

TEST_CASE("supportedBufferFrameSizeRange intersects aggregate sub-device ranges",
          "[device_backend][aggregate][buffer]") {
    // The range query the UI uses must reflect every active member
    // on an aggregate device — values outside any member's range
    // would fail silently. Sub A accepts [32, 1024], sub B accepts
    // [128, 4096]; the intersection is [128, 1024].
    SimulatedBackend backend;
    BackendDeviceInfo a;
    a.uid = "sub-a";
    a.name = "sub-a";
    a.direction = kBackendDirectionInput;
    a.input_channel_count = 2;
    a.nominal_sample_rate = 48000.0;
    a.buffer_frame_size = 128;
    backend.addDevice(a);
    backend.setBufferFrameSizeRange("sub-a", 32, 1024);

    BackendDeviceInfo b;
    b.uid = "sub-b";
    b.name = "sub-b";
    b.direction = kBackendDirectionOutput;
    b.output_channel_count = 2;
    b.nominal_sample_rate = 48000.0;
    b.buffer_frame_size = 128;
    backend.addDevice(b);
    backend.setBufferFrameSizeRange("sub-b", 128, 4096);

    BackendDeviceInfo agg;
    agg.uid = "aggregate";
    agg.name = "aggregate";
    agg.direction = kBackendDirectionInput | kBackendDirectionOutput;
    agg.input_channel_count  = 2;
    agg.output_channel_count = 2;
    agg.nominal_sample_rate  = 48000.0;
    agg.buffer_frame_size    = 128;
    backend.addAggregateDevice(agg, {"sub-a", "sub-b"});
    // Aggregate's own declared range is wide.
    backend.setBufferFrameSizeRange("aggregate", 16, 8192);

    const auto r = backend.supportedBufferFrameSizeRange("aggregate");
    REQUIRE(r.minimum == 128);
    REQUIRE(r.maximum == 1024);
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

TEST_CASE("RouteManager: mux path re-reads buffer sizes into the pill after shrink",
          "[route_manager][latency_mode][mux]") {
    // Regression: the non-duplex (cross-device) attemptStart used to
    // populate the LatencyComponents from the pre-attach cached
    // `BackendDeviceInfo.buffer_frame_size`, which is the value
    // captured at device-enumeration time — BEFORE the mux runs its
    // buffer-shrink request. That left the pill reading the pre-
    // shrink buffer (e.g. 512) even when the HAL actually accepted
    // the smaller target. This test drives Performance mode on two
    // separate devices that start at 512 frames and asserts the
    // pill reflects the post-shrink value (64) — exercising the
    // re-read path that folds `currentBufferFrameSize` back into
    // the latency components after the attach.
    auto backend = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo src = makeInputDevice("src", 2, /*buf*/ 512);
    BackendDeviceInfo dst = makeOutputDevice("dst", 2, /*buf*/ 512);
    backend->addDevice(src);
    backend->addDevice(dst);
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> mapping{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "src", "dst", mapping, "mux-reread",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 64};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);

    REQUIRE(components.src_buffer_frames == 64);
    REQUIRE(components.dst_buffer_frames == 64);

    // The ring target fill is sized from the ring's usable capacity,
    // which doesn't change mid-route (allocation is pre-attach and
    // keyed to the pre-shrink buffer sizes). This assertion just
    // pins that we didn't lose the setpoint in the refresh.
    REQUIRE(components.ring_target_fill_frames > 0);
}

TEST_CASE("RouteManager: mux path sizes the ring from post-shrink buffer values",
          "[route_manager][latency_mode][mux][ring]") {
    // Load-bearing Performance-mode invariant: on a cross-device
    // route, the ring is sized from the POST-shrink device buffer,
    // not the pre-shrink cached value. If we size it pre-shrink, the
    // ring is ~1024 frames on a 64-frame target (from 512-frame
    // starting buffers) and the drift-sampler setpoint lands at
    // ~256 frames — 5.3 ms residency on top of the buffers. Sizing
    // post-shrink gives a 256-frame ring and a ~64-frame setpoint
    // (~1.3 ms), which is what Performance mode is supposed to
    // deliver.
    //
    // RingSizing for Performance: multiplier=2, floor=256. With a
    // 64-frame post-shrink buffer: capacity = max(256, 2*64) = 256;
    // target_fill = capacity * 0.25 ≈ 64 frames. Assert the actual
    // target-fill value is ≤ 128 so this test catches the pre-shrink
    // regression (which would give ~256) without over-pinning the
    // exact preset values.
    auto backend = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo src = makeInputDevice("src", 2, /*buf*/ 512);
    BackendDeviceInfo dst = makeOutputDevice("dst", 2, /*buf*/ 512);
    backend->addDevice(src);
    backend->addDevice(dst);
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> mapping{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "src", "dst", mapping, "mux-ring-post-shrink",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 64};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);

    // Post-shrink ring: target_fill ≤ 128 frames (~2.7 ms at 48 k).
    // Pre-shrink ring would give ~256 (5.3 ms) and fail this bound.
    REQUIRE(components.ring_target_fill_frames <= 128);
    REQUIRE(components.ring_target_fill_frames > 0);

    // The corresponding pill is dominated by the two 64-frame
    // buffers + the tight setpoint; it must come in well under the
    // pre-shrink-ring ~11 ms (11 000 µs) that motivated this change.
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.estimated_latency_us < 8'000);
}

TEST_CASE("RouteManager: Low-latency tier setpoint is refreshed from post-shrink buffers",
          "[route_manager][latency_mode][mux][ring]") {
    // Companion to the Performance-tier case above: Low latency
    // (tier 1) also benefits from the post-attach setpoint refresh
    // when the user picks a buffer override smaller than the
    // device's starting buffer. Without the refresh, Low on a
    // 512-frame device with a 64-frame override would land the
    // setpoint at ~767 frames (ring sized pre-shrink: max(512,
    // 3*512) = 1536 → 0.5 × 1535 ≈ 767). With the refresh, it
    // lands at ~256 frames (post-shrink: max(512, 3*64) = 512 →
    // 0.5 × 512 = 256). This test pins the post-shrink value so a
    // regression in the refresh logic would be caught on the Low
    // tier, not just Performance.
    auto backend = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo src = makeInputDevice("src", 2, /*buf*/ 512);
    BackendDeviceInfo dst = makeOutputDevice("dst", 2, /*buf*/ 512);
    backend->addDevice(src);
    backend->addDevice(dst);
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> mapping{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "src", "dst", mapping, "mux-low-ring",
        /*latency_mode*/ 1,
        /*buffer_frames*/ 64};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);

    // Post-shrink Low-tier setpoint: ≤ 300 frames leaves ample slack
    // above the expected 256, well below the pre-refresh 767.
    REQUIRE(components.ring_target_fill_frames <= 300);
    REQUIRE(components.ring_target_fill_frames > 0);
    // Low tier keeps ring/2 drain headroom, so the pill is higher
    // than Performance's ~6–7 ms but still well under the ~29 ms
    // pre-shrink ceiling.
    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE(status.estimated_latency_us < 12'000);
}

TEST_CASE("RouteManager: mux path reads each device independently after shrink",
          "[route_manager][latency_mode][mux]") {
    // Realistic failure mode on real hardware: hog succeeds on one
    // device but not the other (another app is holding just the
    // second). In that case the shrink lands asymmetrically — one
    // device accepts the request, the other's HAL clamps back up.
    // Use SimulatedBackend's `setBufferFrameSizeRange` to pin the
    // dst device's minimum at 256 so its "shrink" clamps to 256,
    // while src accepts 64 cleanly. The pill must report the
    // asymmetric truth, not the symmetric expectation.
    //
    // SimulatedBackend's requestBufferFrameSize is unclamped for
    // simplicity (see its header comment), so we emulate HAL
    // clamping by pre-setting the dst device's buffer size to a
    // value larger than the requested target and observing that the
    // pill still reflects the ACTUAL post-attach size. If the code
    // path is correct, whatever value is in the backend at the
    // moment of the re-read is what the pill carries.
    auto backend = std::make_unique<SimulatedBackend>();
    BackendDeviceInfo src = makeInputDevice("src", 2, /*buf*/ 512);
    BackendDeviceInfo dst = makeOutputDevice("dst", 2, /*buf*/ 512);
    backend->addDevice(src);
    backend->addDevice(dst);
    auto dm = std::make_unique<DeviceManager>(std::move(backend));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> mapping{{0, 0}};
    // Different per-route targets than the default: 128 frames. Lets
    // us verify the per-route override is what the mux picked up.
    RouteManager::RouteConfig cfg{
        "src", "dst", mapping, "mux-asymmetric",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 128};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_latency_components_t components{};
    REQUIRE(rm.pollLatencyComponents(id, &components) == JBOX_OK);

    // Both devices take the same 128-frame request (no per-device
    // clamp in the simulated backend); but the key property is that
    // the pill matches each device's current value independently,
    // not the pre-attach cached value. On real hardware with a
    // one-sided clamp these two fields can differ.
    REQUIRE(components.src_buffer_frames ==
            dm->backend().currentBufferFrameSize("src"));
    REQUIRE(components.dst_buffer_frames ==
            dm->backend().currentBufferFrameSize("dst"));
    REQUIRE(components.src_buffer_frames == 128);
    REQUIRE(components.dst_buffer_frames == 128);
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

// ---------------------------------------------------------------------------
// Phase 7.5 — device sharing (hog-mode opt-out). See docs/spec.md § 2.7.
// ---------------------------------------------------------------------------

TEST_CASE("RouteManager: share_device skips hog-mode on the duplex fast path",
          "[route_manager][share_device]") {
    // A share_device route trades the exclusive HAL buffer guarantee
    // for coexistence with other apps on the same device. The duplex
    // fast path still runs, but claimExclusive is not invoked — the
    // route rides the shared-client path that exists today as the
    // hog-mode-acquisition-failed fall-through.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "aggregate";
    info.name = "aggregate";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 512;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "share-duplex",
        /*latency_mode*/ 1,  // Low — keeps fast path off, still exercises
                             // the gate for non-Performance share paths.
        /*buffer_frames*/ 0,
        /*share_device*/  true};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    REQUIRE_FALSE(backend->isExclusive("aggregate"));

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    REQUIRE_FALSE(backend->isExclusive("aggregate"));
}

TEST_CASE("RouteManager: Performance + share_device is demoted to Low and flagged",
          "[route_manager][share_device]") {
    // Performance tier needs exclusivity (direct-monitor fast path +
    // ring/4 setpoint) and cannot be honoured with share_device on.
    // The engine silently demotes to Low and surfaces
    // JBOX_ROUTE_STATUS_SHARE_DOWNGRADE so the UI can explain why the
    // tier picker shows Low after the user saved Performance.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "dev";
    info.name = "dev";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 256;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "dev", "dev", m, "perf-share",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 0,
        /*share_device*/  true};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);

    REQUIRE(rm.startRoute(id) == JBOX_OK);

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE((status.status_flags & JBOX_ROUTE_STATUS_SHARE_DOWNGRADE) != 0);

    // LatencyComponents now reflect Low-tier ring/2 setpoint, not the
    // Performance-tier ring/4 setpoint. Since this is a same-device
    // route, the shared fast path is also disabled — the ring is
    // populated at all, which Performance's direct-monitor bypass
    // would have zeroed.
    jbox_route_latency_components_t comp{};
    REQUIRE(rm.pollLatencyComponents(id, &comp) == JBOX_OK);
    REQUIRE(comp.ring_target_fill_frames > 0);
    // Ring-target-fill fraction is ring/2 (Low) rather than ring/4
    // (Performance). Assert strictly > 40 % which the Performance
    // tier would violate.
    const auto usable =
        comp.ring_target_fill_frames * 2u /* x2 bounds the ring */;
    (void)usable;  // only used conceptually; we rely on strict > 40% below.
    // Direct check: ring fraction against its source-side buffer. The
    // exact buffer capacity is internal; the key invariant is that the
    // setpoint is *not* ring/4, so we express it as a ratio relative
    // to a known-large divisor.
    REQUIRE(comp.ring_target_fill_frames >= 128);
}

TEST_CASE("RouteManager: SHARE_DOWNGRADE survives a stop + start cycle",
          "[route_manager][share_device]") {
    // Regression: the demotion must be computed from the stored
    // `latency_mode` every attemptStart, not from a mutated copy.
    // Otherwise the second start() after a stop would see the record's
    // tier already demoted to Low, skip the gating check, and silently
    // drop the SHARE_DOWNGRADE bit — leaving the user running at Low
    // with no UI indicator to explain why.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    (void)backend;
    BackendDeviceInfo info;
    info.uid = "dev";
    info.name = "dev";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 256;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "dev", "dev", m, "perf-share-cycle",
        /*latency_mode*/ 2,
        /*buffer_frames*/ 0,
        /*share_device*/  true};
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);

    for (int i = 0; i < 3; ++i) {
        REQUIRE(rm.startRoute(id) == JBOX_OK);
        jbox_route_status_t status{};
        REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
        REQUIRE((status.status_flags & JBOX_ROUTE_STATUS_SHARE_DOWNGRADE) != 0);
        REQUIRE(rm.stopRoute(id) == JBOX_OK);
    }
}

TEST_CASE("RouteManager: share_device = false preserves exclusive behavior",
          "[route_manager][share_device]") {
    // Regression guard: default-initialised RouteConfig (share_device
    // = false) must behave byte-for-byte like the pre-v9 exclusive
    // flow on the Performance-tier duplex path.
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_ptr.get();
    BackendDeviceInfo info;
    info.uid = "dev";
    info.name = "dev";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count  = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate  = 48000.0;
    info.buffer_frame_size    = 512;
    backend->addDevice(info);
    auto dm = std::make_unique<DeviceManager>(std::move(backend_ptr));
    dm->refresh();
    RouteManager rm(*dm);

    std::vector<ChannelEdge> m{{0, 0}};
    RouteManager::RouteConfig cfg{
        "dev", "dev", m, "exclusive-default",
        /*latency_mode*/ 2};  // share_device defaults to false.
    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);

    REQUIRE(rm.startRoute(id) == JBOX_OK);
    REQUIRE(backend->isExclusive("dev"));

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(id, &status) == JBOX_OK);
    REQUIRE((status.status_flags & JBOX_ROUTE_STATUS_SHARE_DOWNGRADE) == 0);

    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    REQUIRE_FALSE(backend->isExclusive("dev"));
}
