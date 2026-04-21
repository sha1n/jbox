// logging_pipeline_test.cpp — exercises the RT and control-thread log
// producers through RouteManager, verifying the right events land in
// the right order at the right time.
//
// No drainer, no consumer thread, no timing: RouteManager pushes
// synchronously into a test-owned RtLogQueue, and the test pops from
// it directly. This isolates the producer contract from drainer and
// sink machinery (those are covered separately).

#include <catch_amalgamated.hpp>

#include "device_manager.hpp"
#include "route_manager.hpp"
#include "rt_log_codes.hpp"
#include "rt_log_queue.hpp"
#include "simulated_backend.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::ChannelEdge;
using jbox::control::DeviceManager;
using jbox::control::RouteManager;
using jbox::control::SimulatedBackend;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::rt::DefaultRtLogQueue;
using jbox::rt::RtLogEvent;

namespace {

BackendDeviceInfo makeDev(const std::string& uid, std::uint32_t dir,
                          std::uint32_t ic, std::uint32_t oc,
                          std::uint32_t buf = 32) {
    BackendDeviceInfo d;
    d.uid = uid;
    d.name = uid;
    d.direction = dir;
    d.input_channel_count = ic;
    d.output_channel_count = oc;
    d.nominal_sample_rate = 48000.0;
    d.buffer_frame_size = buf;
    return d;
}

std::vector<RtLogEvent> drainAll(DefaultRtLogQueue& q) {
    std::vector<RtLogEvent> out;
    RtLogEvent ev{};
    while (q.tryPop(ev)) out.push_back(ev);
    return out;
}

const RtLogEvent* findCode(const std::vector<RtLogEvent>& events,
                           jbox::rt::RtLogCode code) {
    for (const auto& e : events) {
        if (e.code == code) return &e;
    }
    return nullptr;
}

std::size_t countCode(const std::vector<RtLogEvent>& events,
                      jbox::rt::RtLogCode code) {
    std::size_t n = 0;
    for (const auto& e : events) if (e.code == code) ++n;
    return n;
}

// Convenience fixture: ready-to-run route on 2x2 simulated devices with
// a small ring buffer. Use .rm to drive operations, .queue to inspect
// events, .backend to drive deliverBuffer cycles.
struct RouteFixture {
    std::unique_ptr<SimulatedBackend> backend_owner;
    SimulatedBackend* backend = nullptr;
    std::unique_ptr<DeviceManager> dm;
    DefaultRtLogQueue queue;
    std::unique_ptr<RouteManager> rm;
    jbox_route_id_t id = JBOX_INVALID_ROUTE_ID;

    // Build a pair of 2-channel devices and one route mapping 0->0, 1->1.
    void setup() {
        backend_owner = std::make_unique<SimulatedBackend>();
        backend = backend_owner.get();
        backend->addDevice(makeDev("src", kBackendDirectionInput,  2, 0));
        backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));
        dm = std::make_unique<DeviceManager>(std::move(backend_owner));
        dm->refresh();
        rm = std::make_unique<RouteManager>(*dm, &queue);

        RouteManager::RouteConfig cfg;
        cfg.source_uid = "src";
        cfg.dest_uid   = "dst";
        cfg.mapping    = {ChannelEdge{0, 0}, ChannelEdge{1, 1}};

        jbox_error_t err{};
        id = rm->addRoute(cfg, &err);
        REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    }

    // Start the device so deliverBuffer() actually fires callbacks.
    // RouteManager::startRoute already starts the device mux, but for
    // tests that stopped the route and want to continue delivering,
    // we expose this directly.
    void startBackendDevices() {
        backend->startDevice("src");
        backend->startDevice("dst");
    }
};

}  // namespace

// -----------------------------------------------------------------------------
// Happy path: state transitions produce the expected lifecycle events.
// -----------------------------------------------------------------------------

TEST_CASE("logging: start/stop cycle emits route_started then route_stopped",
          "[logging][pipeline]") {
    RouteFixture f;
    f.setup();

    REQUIRE(f.rm->startRoute(f.id) == JBOX_OK);
    {
        const auto events = drainAll(f.queue);
        const auto* started = findCode(events, jbox::rt::kLogRouteStarted);
        REQUIRE(started != nullptr);
        REQUIRE(started->route_id == f.id);
        REQUIRE(started->value_a == 2);  // source channel count
        REQUIRE(started->value_b == 2);  // dest channel count
        REQUIRE(findCode(events, jbox::rt::kLogRouteStopped) == nullptr);
    }

    REQUIRE(f.rm->stopRoute(f.id) == JBOX_OK);
    {
        const auto events = drainAll(f.queue);
        const auto* stopped = findCode(events, jbox::rt::kLogRouteStopped);
        REQUIRE(stopped != nullptr);
        REQUIRE(stopped->route_id == f.id);
        // No second "started" should have been emitted on its own.
        REQUIRE(findCode(events, jbox::rt::kLogRouteStarted) == nullptr);
    }
}

// -----------------------------------------------------------------------------
// Waiting: starting against a missing device reports which side is gone.
// -----------------------------------------------------------------------------

TEST_CASE("logging: route with missing destination emits route_waiting",
          "[logging][pipeline]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeDev("src", kBackendDirectionInput, 2, 0));
    // No "dst" device; route will sit in WAITING on start.
    DeviceManager dm(std::move(backend));
    dm.refresh();

    DefaultRtLogQueue queue;
    RouteManager rm(dm, &queue);

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "src";
    cfg.dest_uid   = "missing";
    cfg.mapping    = {ChannelEdge{0, 0}};

    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);

    // startRoute returns OK even when devices are missing; the route
    // transitions to WAITING (not ERROR).
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    const auto events = drainAll(queue);
    const auto* waiting = findCode(events, jbox::rt::kLogRouteWaiting);
    REQUIRE(waiting != nullptr);
    REQUIRE(waiting->route_id == id);
    REQUIRE(waiting->value_a == 0);  // src present
    REQUIRE(waiting->value_b == 1);  // dst missing
    REQUIRE(findCode(events, jbox::rt::kLogRouteStarted) == nullptr);
}

TEST_CASE("logging: route with missing source flags value_a=1",
          "[logging][pipeline]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));
    DeviceManager dm(std::move(backend));
    dm.refresh();

    DefaultRtLogQueue queue;
    RouteManager rm(dm, &queue);

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "missing";
    cfg.dest_uid   = "dst";
    cfg.mapping    = {ChannelEdge{0, 0}};

    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(rm.startRoute(id) == JBOX_OK);

    const auto events = drainAll(queue);
    const auto* waiting = findCode(events, jbox::rt::kLogRouteWaiting);
    REQUIRE(waiting != nullptr);
    REQUIRE(waiting->value_a == 1);  // src missing
    REQUIRE(waiting->value_b == 0);
}

// -----------------------------------------------------------------------------
// Underrun: edge-triggered — only the first since (re)start is logged.
// -----------------------------------------------------------------------------

TEST_CASE("logging: underrun is edge-triggered (one event per start cycle)",
          "[logging][pipeline]") {
    RouteFixture f;
    f.setup();
    REQUIRE(f.rm->startRoute(f.id) == JBOX_OK);
    (void)drainAll(f.queue);  // discard the "started" event; we want underrun isolated.

    // Drive the output side with an empty ring (no input delivered yet).
    // AudioConverter will return fewer frames than requested → underrun.
    float out[4] = {};
    f.backend->deliverBuffer("dst", 2, nullptr, out);

    {
        const auto events = drainAll(f.queue);
        REQUIRE(countCode(events, jbox::rt::kLogUnderrun) == 1);
        const auto* u = findCode(events, jbox::rt::kLogUnderrun);
        REQUIRE(u != nullptr);
        REQUIRE(u->route_id == f.id);
        // value_a is the running total; first underrun => 1 or more.
        REQUIRE(u->value_a >= 1);
    }

    // Drive several more output cycles with no input. Each fires the
    // underrun path again, but the edge trigger must suppress the log.
    for (int i = 0; i < 5; ++i) {
        f.backend->deliverBuffer("dst", 2, nullptr, out);
    }
    {
        const auto events = drainAll(f.queue);
        REQUIRE(countCode(events, jbox::rt::kLogUnderrun) == 0);
    }
}

TEST_CASE("logging: edge-trigger flags reset on re-start",
          "[logging][pipeline]") {
    RouteFixture f;
    f.setup();
    REQUIRE(f.rm->startRoute(f.id) == JBOX_OK);
    (void)drainAll(f.queue);

    float out[4] = {};
    f.backend->deliverBuffer("dst", 2, nullptr, out);
    REQUIRE(countCode(drainAll(f.queue), jbox::rt::kLogUnderrun) == 1);

    // Stop + start: flags should clear, next underrun must log again.
    REQUIRE(f.rm->stopRoute(f.id) == JBOX_OK);
    (void)drainAll(f.queue);  // discard stopped event
    REQUIRE(f.rm->startRoute(f.id) == JBOX_OK);
    (void)drainAll(f.queue);  // discard started event

    f.backend->deliverBuffer("dst", 2, nullptr, out);
    REQUIRE(countCode(drainAll(f.queue), jbox::rt::kLogUnderrun) == 1);
}

// -----------------------------------------------------------------------------
// Overrun: edge-triggered the same way. Requires flooding the ring
// buffer past its capacity. Current sizing (see route_manager.cpp) is
// max(max_buffer_frame_size × 8, 4096 floor); the test fixture uses
// 32-frame device buffers so the ring is capped at the 4096 floor.
// Pick kFlood comfortably above that to force the overrun.
// -----------------------------------------------------------------------------

TEST_CASE("logging: overrun is edge-triggered",
          "[logging][pipeline]") {
    RouteFixture f;
    f.setup();
    REQUIRE(f.rm->startRoute(f.id) == JBOX_OK);
    (void)drainAll(f.queue);

    // Deliver more input frames than the ring can hold without a
    // concurrent output drain — forces the writeFrames short-write
    // path in inputIOProcCallback and bumps overrun_count.
    constexpr std::uint32_t kFlood = 8192;
    std::vector<float> input(static_cast<std::size_t>(kFlood) * 2, 0.5f);
    f.backend->deliverBuffer("src", kFlood, input.data(), nullptr);

    {
        const auto events = drainAll(f.queue);
        REQUIRE(countCode(events, jbox::rt::kLogOverrun) == 1);
        const auto* o = findCode(events, jbox::rt::kLogOverrun);
        REQUIRE(o != nullptr);
        REQUIRE(o->route_id == f.id);
        REQUIRE(o->value_a >= 1);
    }

    // Flood again — no new event (edge trigger).
    f.backend->deliverBuffer("src", kFlood, input.data(), nullptr);
    {
        const auto events = drainAll(f.queue);
        REQUIRE(countCode(events, jbox::rt::kLogOverrun) == 0);
    }
}

// -----------------------------------------------------------------------------
// Null queue: RouteManager built without a queue must not crash.
// -----------------------------------------------------------------------------

TEST_CASE("logging: RouteManager without a queue is a no-op",
          "[logging][pipeline]") {
    auto backend = std::make_unique<SimulatedBackend>();
    backend->addDevice(makeDev("src", kBackendDirectionInput,  2, 0));
    backend->addDevice(makeDev("dst", kBackendDirectionOutput, 0, 2));
    DeviceManager dm(std::move(backend));
    dm.refresh();

    RouteManager rm(dm, /*log_queue=*/nullptr);

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "src";
    cfg.dest_uid   = "dst";
    cfg.mapping    = {ChannelEdge{0, 0}};

    jbox_error_t err{};
    const auto id = rm.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(rm.startRoute(id) == JBOX_OK);
    REQUIRE(rm.stopRoute(id) == JBOX_OK);
    // Nothing to assert — the test proves only that none of the push
    // sites crashed on a null queue. If any of them dereferenced the
    // pointer instead of checking it, this test would segfault.
    SUCCEED();
}

// -----------------------------------------------------------------------------
// Channel-mismatch (intentionally uncovered here): the path is triggered
// when Core Audio invokes the IOProc with a different channel count
// than the RouteRecord was configured for (format change mid-stream).
// The SimulatedBackend's deliverBuffer always passes the device's
// configured channel count, so this scenario cannot be reproduced
// without a test-only seam that deliberately lies to the callback.
// The code path itself is trivial (one atomic exchange, one tryPush)
// and is reviewed manually. If a failure mode for channel mismatch
// ever appears in production, a dedicated seam is the natural place
// to add deterministic coverage.
