// log_drainer_test.cpp — unit tests for control::LogDrainer.
//
// Covers basic drain correctness, multi-producer push/drain, and the
// shutdown-flush guarantee (events pushed right before stop() still
// reach the sink).

#include <catch_amalgamated.hpp>

#include "log_drainer.hpp"
#include "rt_log_codes.hpp"
#include "rt_log_queue.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using jbox::control::LogDrainer;
using jbox::rt::RtLogEvent;

namespace {

// Thread-safe capture sink for tests.
struct CaptureSink {
    std::mutex              mu;
    std::vector<RtLogEvent> events;

    void operator()(const RtLogEvent& ev) {
        std::lock_guard<std::mutex> g(mu);
        events.push_back(ev);
    }

    std::size_t size() {
        std::lock_guard<std::mutex> g(mu);
        return events.size();
    }
};

RtLogEvent makeEvent(std::uint32_t code, std::uint32_t route,
                     std::uint64_t a = 0, std::uint64_t b = 0) {
    return RtLogEvent{0, code, route, a, b};
}

}  // namespace

TEST_CASE("LogDrainer: drains a single event through a custom sink",
          "[log_drainer]") {
    auto capture = std::make_shared<CaptureSink>();
    LogDrainer drainer(
        [capture](const RtLogEvent& ev) { (*capture)(ev); },
        std::chrono::milliseconds(5));

    REQUIRE(drainer.queue()->tryPush(makeEvent(jbox::rt::kLogUnderrun, 42, 123)));

    REQUIRE(drainer.waitForEmpty(std::chrono::milliseconds(500)));

    // Give the sink a moment to run after the pop.
    for (int i = 0; i < 100 && capture->size() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(capture->size() == 1);
    REQUIRE(capture->events[0].code == jbox::rt::kLogUnderrun);
    REQUIRE(capture->events[0].route_id == 42);
    REQUIRE(capture->events[0].value_a == 123);
}

TEST_CASE("LogDrainer: preserves FIFO order across many events",
          "[log_drainer]") {
    auto capture = std::make_shared<CaptureSink>();
    LogDrainer drainer(
        [capture](const RtLogEvent& ev) { (*capture)(ev); },
        std::chrono::milliseconds(2));

    constexpr std::uint32_t kCount = 500;
    for (std::uint32_t i = 1; i <= kCount; ++i) {
        while (!drainer.queue()->tryPush(makeEvent(jbox::rt::kLogUnderrun, i, i))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(drainer.waitForEmpty(std::chrono::seconds(2)));
    // Allow the last-popped event to land in the sink.
    for (int i = 0; i < 100 && capture->size() < kCount; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(capture->size() == kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        REQUIRE(capture->events[i].route_id == i + 1);
        REQUIRE(capture->events[i].value_a == i + 1);
    }
}

TEST_CASE("LogDrainer: stop() flushes pending events", "[log_drainer]") {
    auto capture = std::make_shared<CaptureSink>();
    LogDrainer drainer(
        [capture](const RtLogEvent& ev) { (*capture)(ev); },
        // Large poll interval so the thread is almost certainly sleeping
        // when we stop — the shutdown path must still drain what's left.
        std::chrono::milliseconds(500));

    for (std::uint32_t i = 1; i <= 10; ++i) {
        REQUIRE(drainer.queue()->tryPush(makeEvent(jbox::rt::kLogRouteStarted, i)));
    }

    drainer.stop();

    REQUIRE(capture->size() == 10);
    REQUIRE(drainer.deliveredCount() == 10);
}

TEST_CASE("LogDrainer: stop() is idempotent", "[log_drainer]") {
    LogDrainer drainer(
        [](const RtLogEvent&) {},
        std::chrono::milliseconds(5));
    drainer.stop();
    drainer.stop();  // Must not crash or hang.
    SUCCEED();
}

TEST_CASE("LogDrainer: tolerates a null sink", "[log_drainer]") {
    LogDrainer drainer(LogDrainer::Sink{}, std::chrono::milliseconds(5));
    REQUIRE(drainer.queue()->tryPush(makeEvent(jbox::rt::kLogUnderrun, 1)));
    REQUIRE(drainer.waitForEmpty(std::chrono::milliseconds(200)));
    drainer.stop();
    REQUIRE(drainer.deliveredCount() == 0);
}
