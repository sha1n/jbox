// multi_route_test.cpp — Phase 5 multi-route and shared-device tests.
//
// These integration tests drive the RouteManager through scenarios
// that require the DeviceIOMux's RCU-style dispatch: two routes on a
// shared source, two on a shared destination, three concurrent routes
// with overlap, and add/remove-while-running.

#include <catch_amalgamated.hpp>

#include "device_manager.hpp"
#include "route_manager.hpp"
#include "simulated_backend.hpp"

#include <cstdint>
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

BackendDeviceInfo makeInput(const std::string& uid,
                            std::uint32_t channels,
                            std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput;
    info.input_channel_count = channels;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = buf;
    return info;
}

BackendDeviceInfo makeOutput(const std::string& uid,
                             std::uint32_t channels,
                             std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionOutput;
    info.output_channel_count = channels;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = buf;
    return info;
}

}  // namespace

TEST_CASE("Multi-route: two routes share a source device",
          "[multi_route][integration]") {
    // Both routes take input from "src" (4 channels) and write to
    // disjoint channel pairs on different output devices. One
    // DeviceIOMux on "src" dispatches to both routes; each destination
    // has its own mux.
    auto backend_owned = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_owned.get();
    backend->addDevice(makeInput("src", 4));
    backend->addDevice(makeOutput("dst_a", 2));
    backend->addDevice(makeOutput("dst_b", 2));
    DeviceManager dm(std::move(backend_owned));
    dm.refresh();
    RouteManager rm(dm);

    jbox_error_t err{};
    const auto id_a = rm.addRoute({"src", "dst_a",
                                   {{0, 0}, {1, 1}}, "a"}, &err);
    const auto id_b = rm.addRoute({"src", "dst_b",
                                   {{2, 0}, {3, 1}}, "b"}, &err);
    REQUIRE(rm.startRoute(id_a) == JBOX_OK);
    REQUIRE(rm.startRoute(id_b) == JBOX_OK);

    // 32 frames of input on src: ch 0 = 1.0, ch 1 = 2.0, ch 2 = 3.0, ch 3 = 4.0.
    constexpr std::uint32_t kFrames = 32;
    std::vector<float> in(kFrames * 4);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        in[f * 4 + 0] = 1.0f;
        in[f * 4 + 1] = 2.0f;
        in[f * 4 + 2] = 3.0f;
        in[f * 4 + 3] = 4.0f;
    }
    std::vector<float> out_a(kFrames * 2, 0.0f);
    std::vector<float> out_b(kFrames * 2, 0.0f);

    // One delivery on the shared source — the mux dispatches to both
    // routes; each writes into its own ring buffer.
    backend->deliverBuffer("src", kFrames, in.data(), nullptr);
    backend->deliverBuffer("dst_a", kFrames, nullptr, out_a.data());
    backend->deliverBuffer("dst_b", kFrames, nullptr, out_b.data());

    for (std::uint32_t f = 0; f < kFrames; ++f) {
        REQUIRE(out_a[f * 2 + 0] == 1.0f);
        REQUIRE(out_a[f * 2 + 1] == 2.0f);
        REQUIRE(out_b[f * 2 + 0] == 3.0f);
        REQUIRE(out_b[f * 2 + 1] == 4.0f);
    }

    jbox_route_status_t sa{}, sb{};
    rm.pollStatus(id_a, &sa);
    rm.pollStatus(id_b, &sb);
    REQUIRE(sa.frames_produced == kFrames);
    REQUIRE(sb.frames_produced == kFrames);
    REQUIRE(sa.underrun_count == 0);
    REQUIRE(sb.underrun_count == 0);
}

TEST_CASE("Multi-route: two routes share a destination device",
          "[multi_route][integration]") {
    // Input comes from two separate source devices; both routes write
    // into disjoint channels on the same destination. The destination
    // has one DeviceIOMux serving both output callbacks.
    auto backend_owned = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_owned.get();
    backend->addDevice(makeInput("src_a", 2));
    backend->addDevice(makeInput("src_b", 2));
    backend->addDevice(makeOutput("dst", 4));
    DeviceManager dm(std::move(backend_owned));
    dm.refresh();
    RouteManager rm(dm);

    jbox_error_t err{};
    const auto id_a = rm.addRoute({"src_a", "dst",
                                   {{0, 0}, {1, 1}}, "a"}, &err);
    const auto id_b = rm.addRoute({"src_b", "dst",
                                   {{0, 2}, {1, 3}}, "b"}, &err);
    REQUIRE(rm.startRoute(id_a) == JBOX_OK);
    REQUIRE(rm.startRoute(id_b) == JBOX_OK);

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> in_a(kFrames * 2, 0.0f);
    std::vector<float> in_b(kFrames * 2, 0.0f);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        in_a[f * 2 + 0] = 0.1f;
        in_a[f * 2 + 1] = 0.2f;
        in_b[f * 2 + 0] = 0.3f;
        in_b[f * 2 + 1] = 0.4f;
    }
    std::vector<float> out(kFrames * 4, 0.0f);

    backend->deliverBuffer("src_a", kFrames, in_a.data(), nullptr);
    backend->deliverBuffer("src_b", kFrames, in_b.data(), nullptr);
    backend->deliverBuffer("dst",   kFrames, nullptr, out.data());

    for (std::uint32_t f = 0; f < kFrames; ++f) {
        REQUIRE(out[f * 4 + 0] == Catch::Approx(0.1f));
        REQUIRE(out[f * 4 + 1] == Catch::Approx(0.2f));
        REQUIRE(out[f * 4 + 2] == Catch::Approx(0.3f));
        REQUIRE(out[f * 4 + 3] == Catch::Approx(0.4f));
    }
}

TEST_CASE("Multi-route: three routes, shared source and shared destination",
          "[multi_route][integration]") {
    // Scenario from docs/plan.md § Phase 5 exit criteria:
    //   A: src1 → dst1  (shared source with B, shared destination with C)
    //   B: src1 → dst2  (shared source with A)
    //   C: src2 → dst1  (shared destination with A)
    auto backend_owned = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_owned.get();
    backend->addDevice(makeInput("src1", 4));
    backend->addDevice(makeInput("src2", 2));
    backend->addDevice(makeOutput("dst1", 4));
    backend->addDevice(makeOutput("dst2", 2));
    DeviceManager dm(std::move(backend_owned));
    dm.refresh();
    RouteManager rm(dm);

    jbox_error_t err{};
    const auto a = rm.addRoute({"src1", "dst1",
                                {{0, 0}, {1, 1}}, "a"}, &err);
    const auto b = rm.addRoute({"src1", "dst2",
                                {{2, 0}, {3, 1}}, "b"}, &err);
    const auto c = rm.addRoute({"src2", "dst1",
                                {{0, 2}, {1, 3}}, "c"}, &err);
    REQUIRE(rm.startRoute(a) == JBOX_OK);
    REQUIRE(rm.startRoute(b) == JBOX_OK);
    REQUIRE(rm.startRoute(c) == JBOX_OK);

    constexpr std::uint32_t kFrames = 16;
    std::vector<float> in1(kFrames * 4);
    std::vector<float> in2(kFrames * 2);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        in1[f * 4 + 0] = 10.0f;
        in1[f * 4 + 1] = 11.0f;
        in1[f * 4 + 2] = 20.0f;
        in1[f * 4 + 3] = 21.0f;
        in2[f * 2 + 0] = 30.0f;
        in2[f * 2 + 1] = 31.0f;
    }
    std::vector<float> out1(kFrames * 4, 0.0f);
    std::vector<float> out2(kFrames * 2, 0.0f);

    backend->deliverBuffer("src1", kFrames, in1.data(), nullptr);
    backend->deliverBuffer("src2", kFrames, in2.data(), nullptr);
    backend->deliverBuffer("dst1", kFrames, nullptr, out1.data());
    backend->deliverBuffer("dst2", kFrames, nullptr, out2.data());

    for (std::uint32_t f = 0; f < kFrames; ++f) {
        // dst1 ch 0/1 ← src1 ch 0/1 (route a); ch 2/3 ← src2 ch 0/1 (route c).
        REQUIRE(out1[f * 4 + 0] == Catch::Approx(10.0f));
        REQUIRE(out1[f * 4 + 1] == Catch::Approx(11.0f));
        REQUIRE(out1[f * 4 + 2] == Catch::Approx(30.0f));
        REQUIRE(out1[f * 4 + 3] == Catch::Approx(31.0f));
        // dst2 ch 0/1 ← src1 ch 2/3 (route b).
        REQUIRE(out2[f * 2 + 0] == Catch::Approx(20.0f));
        REQUIRE(out2[f * 2 + 1] == Catch::Approx(21.0f));
    }

    // Removing one route does not disturb the others.
    REQUIRE(rm.removeRoute(b) == JBOX_OK);

    // Deliver again and confirm routes a and c still work.
    std::fill(out1.begin(), out1.end(), 0.0f);
    std::fill(out2.begin(), out2.end(), 0.0f);
    backend->deliverBuffer("src1", kFrames, in1.data(), nullptr);
    backend->deliverBuffer("src2", kFrames, in2.data(), nullptr);
    backend->deliverBuffer("dst1", kFrames, nullptr, out1.data());
    backend->deliverBuffer("dst2", kFrames, nullptr, out2.data());
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        REQUIRE(out1[f * 4 + 0] == Catch::Approx(10.0f));
        REQUIRE(out1[f * 4 + 2] == Catch::Approx(30.0f));
        // dst2 is no longer wired up — output stays at the value the
        // simulated backend wrote (zero, since it zeros before calling).
        REQUIRE(out2[f * 2 + 0] == 0.0f);
        REQUIRE(out2[f * 2 + 1] == 0.0f);
    }
}

TEST_CASE("Multi-route: add-while-running keeps the first route flowing",
          "[multi_route][integration]") {
    // Start route A, run some audio, then start route B on the same
    // source. Drive one more cycle and confirm both routes have
    // advanced.
    auto backend_owned = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_owned.get();
    backend->addDevice(makeInput("src", 2));
    backend->addDevice(makeOutput("dst_a", 2));
    backend->addDevice(makeOutput("dst_b", 2));
    DeviceManager dm(std::move(backend_owned));
    dm.refresh();
    RouteManager rm(dm);

    jbox_error_t err{};
    const auto a = rm.addRoute({"src", "dst_a",
                                {{0, 0}, {1, 1}}, "a"}, &err);
    REQUIRE(rm.startRoute(a) == JBOX_OK);

    constexpr std::uint32_t kFrames = 16;
    std::vector<float> in(kFrames * 2, 0.5f);
    std::vector<float> out_a(kFrames * 2, 0.0f);
    std::vector<float> out_b(kFrames * 2, 0.0f);

    backend->deliverBuffer("src",   kFrames, in.data(), nullptr);
    backend->deliverBuffer("dst_a", kFrames, nullptr, out_a.data());
    for (float v : out_a) REQUIRE(v == Catch::Approx(0.5f));

    jbox_route_status_t sa0{};
    rm.pollStatus(a, &sa0);
    REQUIRE(sa0.frames_produced == kFrames);

    // Add route B mid-run; the mux on "src" swaps its dispatch list
    // atomically.
    const auto b = rm.addRoute({"src", "dst_b",
                                {{0, 0}, {1, 1}}, "b"}, &err);
    REQUIRE(rm.startRoute(b) == JBOX_OK);

    backend->deliverBuffer("src",   kFrames, in.data(), nullptr);
    backend->deliverBuffer("dst_a", kFrames, nullptr, out_a.data());
    backend->deliverBuffer("dst_b", kFrames, nullptr, out_b.data());

    for (float v : out_a) REQUIRE(v == Catch::Approx(0.5f));
    for (float v : out_b) REQUIRE(v == Catch::Approx(0.5f));

    jbox_route_status_t sa1{}, sb1{};
    rm.pollStatus(a, &sa1);
    rm.pollStatus(b, &sb1);
    REQUIRE(sa1.frames_produced == 2 * kFrames);
    REQUIRE(sb1.frames_produced == kFrames);
}

TEST_CASE("Multi-route: remove-while-running stops one without disturbing peers",
          "[multi_route][integration]") {
    auto backend_owned = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_owned.get();
    backend->addDevice(makeInput("src", 4));
    backend->addDevice(makeOutput("dst", 4));
    DeviceManager dm(std::move(backend_owned));
    dm.refresh();
    RouteManager rm(dm);

    jbox_error_t err{};
    const auto a = rm.addRoute({"src", "dst",
                                {{0, 0}, {1, 1}}, "a"}, &err);
    const auto b = rm.addRoute({"src", "dst",
                                {{2, 2}, {3, 3}}, "b"}, &err);
    REQUIRE(rm.startRoute(a) == JBOX_OK);
    REQUIRE(rm.startRoute(b) == JBOX_OK);

    constexpr std::uint32_t kFrames = 16;
    std::vector<float> in(kFrames * 4);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        in[f * 4 + 0] = 1.0f;
        in[f * 4 + 1] = 2.0f;
        in[f * 4 + 2] = 3.0f;
        in[f * 4 + 3] = 4.0f;
    }
    std::vector<float> out(kFrames * 4, 0.0f);

    backend->deliverBuffer("src", kFrames, in.data(), nullptr);
    backend->deliverBuffer("dst", kFrames, nullptr, out.data());
    // Both routes active — all four channels carry their values.
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        REQUIRE(out[f * 4 + 0] == Catch::Approx(1.0f));
        REQUIRE(out[f * 4 + 1] == Catch::Approx(2.0f));
        REQUIRE(out[f * 4 + 2] == Catch::Approx(3.0f));
        REQUIRE(out[f * 4 + 3] == Catch::Approx(4.0f));
    }

    // Stop route B; route A must continue untouched.
    REQUIRE(rm.stopRoute(b) == JBOX_OK);

    std::fill(out.begin(), out.end(), 0.0f);
    backend->deliverBuffer("src", kFrames, in.data(), nullptr);
    backend->deliverBuffer("dst", kFrames, nullptr, out.data());
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        REQUIRE(out[f * 4 + 0] == Catch::Approx(1.0f));
        REQUIRE(out[f * 4 + 1] == Catch::Approx(2.0f));
        // B's channels are no longer written (simulated backend zero-
        // initialised the output buffer before the callback).
        REQUIRE(out[f * 4 + 2] == 0.0f);
        REQUIRE(out[f * 4 + 3] == 0.0f);
    }

    jbox_route_status_t sa{}, sb{};
    rm.pollStatus(a, &sa);
    rm.pollStatus(b, &sb);
    REQUIRE(sa.state == JBOX_ROUTE_STATE_RUNNING);
    REQUIRE(sb.state == JBOX_ROUTE_STATE_STOPPED);
}
