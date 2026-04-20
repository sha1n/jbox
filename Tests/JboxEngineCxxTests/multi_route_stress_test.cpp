// multi_route_stress_test.cpp — concurrent attach/detach under sustained
// dispatch. Designed to exercise the RCU-style list swap in DeviceIOMux
// and in RouteManager via the mux; intended to be run under
// ThreadSanitizer.
//
// The scenarios use an "anchor" route/callback that keeps the backend
// IOProc registered throughout the concurrent phase, so the simulated
// backend's callback-slot state (which is not thread-safe against
// open/close) never changes while the RT thread is active.

#include <catch_amalgamated.hpp>

#include "device_io_mux.hpp"
#include "device_manager.hpp"
#include "route_manager.hpp"
#include "simulated_backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::ChannelEdge;
using jbox::control::DeviceIOMux;
using jbox::control::DeviceManager;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::RouteManager;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeInput(const std::string& uid, std::uint32_t ch) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput;
    info.input_channel_count = ch;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    return info;
}

BackendDeviceInfo makeOutput(const std::string& uid, std::uint32_t ch) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionOutput;
    info.output_channel_count = ch;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    return info;
}

void countingCallback(const float* /*samples*/,
                      std::uint32_t /*frames*/,
                      std::uint32_t /*channels*/,
                      void* user) {
    auto* counter = static_cast<std::atomic<std::uint64_t>*>(user);
    counter->fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

TEST_CASE("Multi-route stress: mux concurrent attach/detach under dispatch",
          "[multi_route][stress]") {
    SimulatedBackend backend;
    backend.addDevice(makeInput("src", 2));

    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);

    std::atomic<std::uint64_t> anchor_calls{0};
    REQUIRE(mux.attachInput(&anchor_calls, &countingCallback, &anchor_calls));

    std::atomic<bool> stop{false};
    std::vector<float> buf(64 * 2, 0.25f);

    std::thread rt_thread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            backend.deliverBuffer("src", 64, buf.data(), nullptr);
            std::this_thread::yield();
        }
    });

    constexpr int kCycles = 200;
    std::vector<std::unique_ptr<std::atomic<std::uint64_t>>> transient_counters;
    transient_counters.reserve(kCycles);

    for (int i = 0; i < kCycles; ++i) {
        auto counter = std::make_unique<std::atomic<std::uint64_t>>(0);
        void* key = counter.get();
        REQUIRE(mux.attachInput(key, &countingCallback, key));
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        mux.detachInput(key);
        transient_counters.push_back(std::move(counter));
    }

    stop.store(true, std::memory_order_relaxed);
    rt_thread.join();

    // Anchor must have received many callbacks across the run.
    REQUIRE(anchor_calls.load() > 0);

    // Most transient attachments should have received at least one
    // callback — the mux is definitely alive; a few could miss a slot
    // if scheduling is unlucky, but a total of zero would be a
    // correctness red flag.
    std::uint64_t total_transient = 0;
    for (const auto& c : transient_counters) {
        total_transient += c->load(std::memory_order_relaxed);
    }
    REQUIRE(total_transient > 0);

    mux.detachInput(&anchor_calls);
}

TEST_CASE("Multi-route stress: rapid route start/stop while RT is dispatching",
          "[multi_route][stress]") {
    // Anchor route keeps the mux's IOProcs registered throughout the
    // concurrent phase so that SimulatedBackend's slot state never
    // mutates while the RT thread is reading it.
    auto backend_owned = std::make_unique<SimulatedBackend>();
    SimulatedBackend* backend = backend_owned.get();
    backend->addDevice(makeInput("src", 4));
    backend->addDevice(makeOutput("dst", 4));
    DeviceManager dm(std::move(backend_owned));
    dm.refresh();
    RouteManager rm(dm);

    jbox_error_t err{};
    const auto anchor = rm.addRoute(
        {"src", "dst", {{0, 0}}, "anchor"}, &err);
    REQUIRE(rm.startRoute(anchor) == JBOX_OK);

    std::atomic<bool> stop{false};
    std::vector<float> in(64 * 4, 0.125f);
    std::vector<float> out(64 * 4, 0.0f);

    std::thread rt_thread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            backend->deliverBuffer("src", 64, in.data(), nullptr);
            backend->deliverBuffer("dst", 64, nullptr, out.data());
            std::this_thread::yield();
        }
    });

    constexpr int kCycles = 30;
    for (int i = 0; i < kCycles; ++i) {
        const auto id = rm.addRoute(
            {"src", "dst", {{1, 1}}, "transient"}, &err);
        REQUIRE(id != JBOX_INVALID_ROUTE_ID);
        REQUIRE(rm.startRoute(id) == JBOX_OK);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        REQUIRE(rm.stopRoute(id) == JBOX_OK);
        REQUIRE(rm.removeRoute(id) == JBOX_OK);
    }

    stop.store(true, std::memory_order_relaxed);
    rt_thread.join();

    jbox_route_status_t status{};
    REQUIRE(rm.pollStatus(anchor, &status) == JBOX_OK);
    REQUIRE(status.state == JBOX_ROUTE_STATE_RUNNING);
    REQUIRE(status.frames_produced > 0);
    REQUIRE(status.frames_consumed > 0);
}
