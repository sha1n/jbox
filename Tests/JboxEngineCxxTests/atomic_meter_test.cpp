// atomic_meter_test.cpp — unit and concurrent-stress tests for AtomicMeter.

#include <catch_amalgamated.hpp>

#include "atomic_meter.hpp"

#include <atomic>
#include <thread>
#include <vector>

using jbox::rt::AtomicMeter;
using jbox::rt::kAtomicMeterInvalidChannel;
using jbox::rt::kAtomicMeterMaxChannels;

TEST_CASE("AtomicMeter: default-constructed reads zero on all channels", "[atomic_meter]") {
    AtomicMeter meter;
    for (std::size_t ch = 0; ch < kAtomicMeterMaxChannels; ++ch) {
        REQUIRE(meter.peek(ch) == 0.0f);
    }
}

TEST_CASE("AtomicMeter: updateMax then peek returns the value", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(0, 0.5f);
    REQUIRE(meter.peek(0) == 0.5f);
}

TEST_CASE("AtomicMeter: readAndReset returns peak and zeros it", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(3, 0.75f);
    REQUIRE(meter.readAndReset(3) == 0.75f);
    REQUIRE(meter.peek(3) == 0.0f);
    REQUIRE(meter.readAndReset(3) == 0.0f);
}

TEST_CASE("AtomicMeter: increasing updates raise the stored peak", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(0, 0.1f);
    meter.updateMax(0, 0.3f);
    meter.updateMax(0, 0.8f);
    meter.updateMax(0, 0.9f);
    REQUIRE(meter.peek(0) == 0.9f);
}

TEST_CASE("AtomicMeter: decreasing updates do not lower the stored peak", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(0, 0.9f);
    meter.updateMax(0, 0.5f);
    meter.updateMax(0, 0.1f);
    REQUIRE(meter.peek(0) == 0.9f);
}

TEST_CASE("AtomicMeter: mixed updates keep the absolute max", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(0, 0.2f);
    meter.updateMax(0, 0.7f);
    meter.updateMax(0, 0.4f);
    meter.updateMax(0, 0.9f);
    meter.updateMax(0, 0.3f);
    REQUIRE(meter.peek(0) == 0.9f);
}

TEST_CASE("AtomicMeter: channels are independent", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(2, 0.4f);
    meter.updateMax(5, 0.8f);
    meter.updateMax(7, 0.2f);

    REQUIRE(meter.peek(2) == 0.4f);
    REQUIRE(meter.peek(5) == 0.8f);
    REQUIRE(meter.peek(7) == 0.2f);
    REQUIRE(meter.peek(0) == 0.0f);
    REQUIRE(meter.peek(4) == 0.0f);
    REQUIRE(meter.peek(10) == 0.0f);
}

TEST_CASE("AtomicMeter: readAndReset leaves other channels untouched", "[atomic_meter]") {
    AtomicMeter meter;
    meter.updateMax(2, 0.4f);
    meter.updateMax(5, 0.8f);

    REQUIRE(meter.readAndReset(2) == 0.4f);
    REQUIRE(meter.peek(5) == 0.8f);
}

TEST_CASE("AtomicMeter: out-of-range channel is a safe no-op", "[atomic_meter]") {
    AtomicMeter meter;
    const std::size_t bad = kAtomicMeterMaxChannels;  // one past the end

    // updateMax on an out-of-range channel does nothing and does not crash.
    meter.updateMax(bad, 0.5f);

    // peek and readAndReset return the sentinel value.
    REQUIRE(meter.peek(bad) == kAtomicMeterInvalidChannel);
    REQUIRE(meter.readAndReset(bad) == kAtomicMeterInvalidChannel);

    // In-range channels are unaffected.
    REQUIRE(meter.peek(0) == 0.0f);
}

TEST_CASE("AtomicMeter: capacity reports the compiled-in maximum", "[atomic_meter]") {
    REQUIRE(AtomicMeter::capacity() == kAtomicMeterMaxChannels);
}

TEST_CASE("AtomicMeter: concurrent updates preserve the global maximum", "[atomic_meter][stress]") {
    AtomicMeter meter;
    constexpr int kThreads = 8;
    constexpr int kIterationsPerThread = 20'000;

    // Each thread writes increasing values from its own base. The
    // global maximum across all threads is the overall highest value
    // attempted. After the threads join, peek must equal that maximum.
    auto worker = [&meter](int thread_index) {
        float base = static_cast<float>(thread_index) * 0.01f;
        for (int i = 0; i < kIterationsPerThread; ++i) {
            float value = base + static_cast<float>(i) * 0.0001f;
            meter.updateMax(0, value);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Expected global max: the final iteration of the highest-base thread.
    float expected =
        static_cast<float>(kThreads - 1) * 0.01f +
        static_cast<float>(kIterationsPerThread - 1) * 0.0001f;

    REQUIRE(meter.peek(0) == Catch::Approx(expected));
}

TEST_CASE("AtomicMeter: single-producer / single-consumer interleaving", "[atomic_meter][stress]") {
    // Simulates the real usage pattern: one "audio thread" writing
    // peaks, one "UI thread" polling via readAndReset. The UI may
    // occasionally read-and-reset a value below a concurrent peak
    // that's arriving; that peak should still land and become the
    // next-observed value, not be silently lost.
    AtomicMeter meter;
    std::atomic<bool> stop{false};
    constexpr int kWrites = 200'000;
    float observed_max = 0.0f;

    std::thread producer([&]() {
        for (int i = 0; i < kWrites; ++i) {
            float v = static_cast<float>(i) * 0.0001f;
            meter.updateMax(1, v);
        }
        stop.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            float v = meter.readAndReset(1);
            if (v > observed_max) {
                observed_max = v;
            }
        }
        // Final drain after producer stops.
        float tail = meter.readAndReset(1);
        if (tail > observed_max) {
            observed_max = tail;
        }
    });

    producer.join();
    consumer.join();

    float expected = static_cast<float>(kWrites - 1) * 0.0001f;
    // Observed max may be just below expected due to interleaved resets,
    // but must be within one iteration step. We tolerate that by
    // requiring observed_max to be at least within a small window.
    REQUIRE(observed_max >= expected - 0.001f);
    REQUIRE(observed_max <= expected + 1e-6f);
}
