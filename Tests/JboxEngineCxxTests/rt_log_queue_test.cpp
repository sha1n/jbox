// rt_log_queue_test.cpp — unit and concurrent tests for RtLogQueue.

#include <catch_amalgamated.hpp>

#include "rt_log_queue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using jbox::rt::DefaultRtLogQueue;
using jbox::rt::RtLogEvent;
using jbox::rt::RtLogQueue;

namespace {

RtLogEvent makeEvent(std::uint64_t ts, std::uint32_t code,
                     std::uint32_t route = 0,
                     std::uint64_t a = 0, std::uint64_t b = 0) {
    return RtLogEvent{ts, code, route, a, b};
}

}  // namespace

TEST_CASE("RtLogQueue: fresh queue is empty", "[rt_log_queue]") {
    RtLogQueue<16> q;
    RtLogEvent out;
    REQUIRE_FALSE(q.tryPop(out));
    REQUIRE(q.approxSize() == 0);
}

TEST_CASE("RtLogQueue: push then pop returns the same record", "[rt_log_queue]") {
    RtLogQueue<16> q;
    RtLogEvent in = makeEvent(1234, 7, 42, 0xAA, 0xBB);
    REQUIRE(q.tryPush(in));

    RtLogEvent out{};
    REQUIRE(q.tryPop(out));
    REQUIRE(out.timestamp == in.timestamp);
    REQUIRE(out.code == in.code);
    REQUIRE(out.route_id == in.route_id);
    REQUIRE(out.value_a == in.value_a);
    REQUIRE(out.value_b == in.value_b);
}

TEST_CASE("RtLogQueue: FIFO ordering preserved", "[rt_log_queue]") {
    RtLogQueue<16> q;
    for (std::uint32_t i = 1; i <= 5; ++i) {
        REQUIRE(q.tryPush(makeEvent(i, i)));
    }
    for (std::uint32_t expected = 1; expected <= 5; ++expected) {
        RtLogEvent out{};
        REQUIRE(q.tryPop(out));
        REQUIRE(out.code == expected);
    }
}

TEST_CASE("RtLogQueue: full queue rejects further pushes", "[rt_log_queue]") {
    RtLogQueue<8> q;  // capacity 8 → usable 7
    for (std::uint32_t i = 0; i < RtLogQueue<8>::usableCapacity(); ++i) {
        REQUIRE(q.tryPush(makeEvent(i, i)));
    }
    // Queue should now be full; next push must fail.
    REQUIRE_FALSE(q.tryPush(makeEvent(999, 999)));
    REQUIRE(q.approxSize() == RtLogQueue<8>::usableCapacity());
}

TEST_CASE("RtLogQueue: drain restores capacity", "[rt_log_queue]") {
    RtLogQueue<8> q;
    // Fill, drain, fill again — verifies wrap-around indexing.
    for (std::uint32_t i = 0; i < RtLogQueue<8>::usableCapacity(); ++i) {
        REQUIRE(q.tryPush(makeEvent(i, i)));
    }
    RtLogEvent out{};
    while (q.tryPop(out)) { /* drain */ }
    REQUIRE(q.approxSize() == 0);

    // Push & pop again - should work because head/tail wrapped cleanly.
    REQUIRE(q.tryPush(makeEvent(100, 42)));
    REQUIRE(q.tryPop(out));
    REQUIRE(out.code == 42);
}

TEST_CASE("RtLogQueue: wrap-around across many push/pop cycles", "[rt_log_queue]") {
    RtLogQueue<4> q;  // tiny capacity, large number of items
    RtLogEvent out{};

    for (std::uint32_t i = 0; i < 10'000; ++i) {
        REQUIRE(q.tryPush(makeEvent(i, i)));
        REQUIRE(q.tryPop(out));
        REQUIRE(out.code == i);
    }
}

TEST_CASE("RtLogQueue: usableCapacity is one less than Capacity", "[rt_log_queue]") {
    REQUIRE(RtLogQueue<2>::usableCapacity() == 1);
    REQUIRE(RtLogQueue<16>::usableCapacity() == 15);
    REQUIRE(RtLogQueue<1024>::usableCapacity() == 1023);
}

TEST_CASE("RtLogQueue: DefaultRtLogQueue capacity is 1023", "[rt_log_queue]") {
    REQUIRE(DefaultRtLogQueue::usableCapacity() == 1023);
}

TEST_CASE("RtLogQueue: SPSC concurrent producer/consumer preserves ordering",
          "[rt_log_queue][stress]") {
    RtLogQueue<64> q;
    constexpr std::uint32_t kMessages = 100'000;

    std::thread producer([&]() {
        for (std::uint32_t i = 0; i < kMessages; ++i) {
            // Spin until there's space; mirrors what the drainer would
            // see in real use if the queue briefly fills up.
            while (!q.tryPush(makeEvent(i, i, /*route=*/0, /*a=*/i, /*b=*/0))) {
                std::this_thread::yield();
            }
        }
    });

    std::uint32_t next_expected = 0;
    std::thread consumer([&]() {
        RtLogEvent out{};
        while (next_expected < kMessages) {
            if (q.tryPop(out)) {
                REQUIRE(out.code == next_expected);
                REQUIRE(out.timestamp == next_expected);
                REQUIRE(out.value_a == next_expected);
                ++next_expected;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(next_expected == kMessages);
    // After the run, queue should be empty.
    RtLogEvent out{};
    REQUIRE_FALSE(q.tryPop(out));
}

TEST_CASE("RtLogQueue: slow consumer causes producer drops but never corruption",
          "[rt_log_queue][stress]") {
    // Producer pushes faster than consumer drains. Some events get
    // dropped (tryPush returns false); none that were accepted get
    // corrupted; consumer sees a monotonically non-decreasing
    // sequence of codes (strictly increasing among accepted ones).
    RtLogQueue<16> q;
    constexpr std::uint32_t kAttempts = 50'000;
    std::atomic<std::uint32_t> produced_ok{0};
    std::atomic<std::uint32_t> produced_dropped{0};

    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (std::uint32_t i = 0; i < kAttempts; ++i) {
            if (q.tryPush(makeEvent(i, i))) {
                produced_ok.fetch_add(1, std::memory_order_relaxed);
            } else {
                produced_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint32_t last_seen = 0;
    bool first = true;
    std::uint32_t consumed = 0;
    std::thread consumer([&]() {
        RtLogEvent out{};
        while (true) {
            // Snapshot producer's status BEFORE draining so we don't
            // race against a final post-snapshot push.
            const bool producer_finished =
                producer_done.load(std::memory_order_acquire);

            // Drain everything currently queued via the inner loop —
            // consuming here (not in the outer condition) so every
            // popped event is counted.
            while (q.tryPop(out)) {
                if (!first) {
                    // Codes accepted by the producer are strictly
                    // increasing (0, 1, 2, ...). After the queue drops
                    // some, the consumer skips ahead — but later codes
                    // are still strictly greater than earlier ones.
                    REQUIRE(out.code > last_seen);
                }
                first = false;
                last_seen = out.code;
                ++consumed;
                // Brief backoff so the producer can outrun us.
                std::this_thread::yield();
            }

            if (producer_finished) {
                // One final drain to collect anything pushed between
                // the snapshot and the now-empty inner loop exit.
                while (q.tryPop(out)) {
                    if (!first) {
                        REQUIRE(out.code > last_seen);
                    }
                    first = false;
                    last_seen = out.code;
                    ++consumed;
                }
                break;
            }

            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(produced_ok.load() + produced_dropped.load() == kAttempts);
    REQUIRE(consumed == produced_ok.load());
    // Producer should have been fast enough to fill + drop at least
    // occasionally at this test's pacing.
    REQUIRE(produced_dropped.load() > 0);
}
