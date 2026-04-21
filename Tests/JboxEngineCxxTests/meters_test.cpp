// meters_test.cpp — per-route source/dest signal-peak meters (Phase 6
// Slice A). Drives the engine via SimulatedBackend and polls meters
// through the RouteManager API the bridge will eventually forward to.

#include <catch_amalgamated.hpp>

#include "device_manager.hpp"
#include "jbox_engine.h"
#include "route_manager.hpp"
#include "simulated_backend.hpp"

#include <cmath>
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

struct Fixture {
    SimulatedBackend* backend = nullptr;
    std::unique_ptr<DeviceManager> dm;
    std::unique_ptr<RouteManager>  rm;

    Fixture(std::uint32_t src_channels, std::uint32_t dst_channels,
            std::uint32_t buf_frames = 32) {
        auto b = std::make_unique<SimulatedBackend>();
        backend = b.get();
        b->addDevice(makeInputDevice("src", src_channels, buf_frames));
        b->addDevice(makeOutputDevice("dst", dst_channels, buf_frames));
        dm = std::make_unique<DeviceManager>(std::move(b));
        dm->refresh();
        rm = std::make_unique<RouteManager>(*dm);
    }
};

jbox_route_id_t addAndStart(Fixture& f,
                            const std::vector<ChannelEdge>& mapping,
                            const std::string& name = "m") {
    RouteManager::RouteConfig cfg{"src", "dst", mapping, name};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);
    return id;
}

}  // namespace

TEST_CASE("pollMeters: unknown route id returns 0", "[meters]") {
    Fixture f(2, 2);
    float peaks[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    REQUIRE(f.rm->pollMeters(12345, JBOX_METER_SIDE_SOURCE, peaks, 4) == 0);
    // Buffer must not be touched.
    REQUIRE(peaks[0] == 9.0f);
    REQUIRE(peaks[1] == 9.0f);
}

TEST_CASE("pollMeters: stopped route returns 0", "[meters]") {
    Fixture f(2, 2);
    RouteManager::RouteConfig cfg{"src", "dst", {{0, 0}, {1, 1}}, "stopped"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    float peaks[2] = {};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 2) == 0);
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_DEST,   peaks, 2) == 0);
}

TEST_CASE("pollMeters: null buffer or zero max returns 0", "[meters]") {
    Fixture f(2, 2);
    const auto id = addAndStart(f, {{0, 0}, {1, 1}});
    float peaks[2] = {};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, nullptr, 2) == 0);
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 0)   == 0);
}

TEST_CASE("pollMeters: source side captures per-channel peak from input IOProc",
          "[meters][integration]") {
    // 2-channel route: src ch 0 → dst ch 0, src ch 1 → dst ch 1.
    // Drive input with distinct per-channel peaks.
    Fixture f(/*src*/ 2, /*dst*/ 2, /*buf*/ 32);
    const auto id = addAndStart(f, {{0, 0}, {1, 1}});

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2, 0.0f);
    // Ch 0 peak at +0.3; ch 1 peak at -0.8 (we expect abs).
    input[10 * 2 + 0] = 0.30f;
    input[15 * 2 + 1] = -0.80f;

    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);

    float peaks[2] = {};
    const auto n = f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 2);
    REQUIRE(n == 2);
    REQUIRE(peaks[0] == Catch::Approx(0.30f));
    REQUIRE(peaks[1] == Catch::Approx(0.80f));
}

TEST_CASE("pollMeters: dest side captures per-channel peak after convert",
          "[meters][integration]") {
    Fixture f(/*src*/ 2, /*dst*/ 4, /*buf*/ 32);
    // Map to dst channels 2 and 3 to also prove the peak is keyed by
    // route-internal channel index (0, 1), not device channel index.
    const auto id = addAndStart(f, {{0, 2}, {1, 3}});

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2, 0.0f);
    input[5  * 2 + 0] = 0.10f;  // route ch 0 peak
    input[20 * 2 + 1] = 0.55f;  // route ch 1 peak

    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);

    std::vector<float> output(kFrames * 4, 0.0f);
    f.backend->deliverBuffer("dst", kFrames, nullptr, output.data());

    float peaks[2] = {};
    const auto n = f.rm->pollMeters(id, JBOX_METER_SIDE_DEST, peaks, 2);
    REQUIRE(n == 2);
    REQUIRE(peaks[0] == Catch::Approx(0.10f).margin(1e-4f));
    REQUIRE(peaks[1] == Catch::Approx(0.55f).margin(1e-4f));
}

TEST_CASE("pollMeters: read-and-reset — second call without new samples is zero",
          "[meters][integration]") {
    Fixture f(2, 2, /*buf*/ 32);
    const auto id = addAndStart(f, {{0, 0}, {1, 1}});

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2, 0.0f);
    input[3 * 2 + 0] = 0.42f;
    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);

    float peaks[2] = {};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 2) == 2);
    REQUIRE(peaks[0] == Catch::Approx(0.42f));

    // No new input — meters should read as zero now (the peak was reset).
    peaks[0] = peaks[1] = 9.0f;
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 2) == 2);
    REQUIRE(peaks[0] == 0.0f);
    REQUIRE(peaks[1] == 0.0f);
}

TEST_CASE("pollMeters: max_channels smaller than route width truncates",
          "[meters][integration]") {
    Fixture f(4, 4, /*buf*/ 32);
    const auto id = addAndStart(f, {{0, 0}, {1, 1}, {2, 2}, {3, 3}});

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 4, 0.0f);
    // Give ch 2 a clear peak so we can prove truncation didn't silently skip it.
    input[7 * 4 + 2] = 0.66f;
    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);

    // Only ask for the first 2 channels.
    float peaks[2] = {9.0f, 9.0f};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 2) == 2);
    REQUIRE(peaks[0] == 0.0f);
    REQUIRE(peaks[1] == 0.0f);
    // Truncation must not clobber unrequested channels (ch 2's peak
    // should still be readable when we ask for 4).
    float peaks4[4] = {};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks4, 4) == 4);
    REQUIRE(peaks4[2] == Catch::Approx(0.66f));
}

TEST_CASE("pollMeters: meters reset on restart", "[meters][integration]") {
    Fixture f(2, 2, /*buf*/ 32);
    const auto id = addAndStart(f, {{0, 0}, {1, 1}});

    constexpr std::uint32_t kFrames = 32;
    std::vector<float> input(kFrames * 2, 0.0f);
    input[1 * 2 + 0] = 0.9f;
    f.backend->deliverBuffer("src", kFrames, input.data(), nullptr);

    // Restart without draining.
    REQUIRE(f.rm->stopRoute(id) == JBOX_OK);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    float peaks[2] = {9.0f, 9.0f};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, peaks, 2) == 2);
    REQUIRE(peaks[0] == 0.0f);
    REQUIRE(peaks[1] == 0.0f);
}

TEST_CASE("pollMeters: invalid side returns 0", "[meters]") {
    Fixture f(2, 2);
    const auto id = addAndStart(f, {{0, 0}, {1, 1}});

    float peaks[2] = {7.0f, 7.0f};
    // 42 is not a valid side; cast through int to smuggle past the enum.
    const auto bogus = static_cast<jbox_meter_side_t>(42);
    REQUIRE(f.rm->pollMeters(id, bogus, peaks, 2) == 0);
    REQUIRE(peaks[0] == 7.0f);
    REQUIRE(peaks[1] == 7.0f);
}
