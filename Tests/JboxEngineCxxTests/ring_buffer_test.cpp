// ring_buffer_test.cpp — single-thread and concurrent-stress tests
// for RingBuffer.

#include <catch_amalgamated.hpp>

#include "ring_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using jbox::rt::RingBuffer;

namespace {

// Helper: allocate backing storage for a RingBuffer of a given frame
// capacity and channel count. Returned vector outlives the ring.
std::vector<float> makeStorage(std::size_t capacity_frames, std::size_t channels) {
    return std::vector<float>(capacity_frames * channels, 0.0f);
}

// Helper: build a frame of monotonic values; frame f, channel c → f*channels + c.
void fillFrame(float* frame, std::size_t channels, std::size_t frame_index) {
    for (std::size_t c = 0; c < channels; ++c) {
        frame[c] = static_cast<float>(frame_index * channels + c);
    }
}

// Helper: verify a frame matches the fillFrame scheme.
bool checkFrame(const float* frame, std::size_t channels, std::size_t frame_index) {
    for (std::size_t c = 0; c < channels; ++c) {
        if (frame[c] != static_cast<float>(frame_index * channels + c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("RingBuffer: fresh buffer is empty", "[ring_buffer]") {
    auto storage = makeStorage(8, 2);
    RingBuffer rb(storage.data(), 8, 2);

    REQUIRE(rb.framesAvailableForRead() == 0);
    REQUIRE(rb.framesAvailableForWrite() == rb.usableCapacityFrames());

    float scratch[4]{};
    REQUIRE(rb.readFrames(scratch, 2) == 0);
}

TEST_CASE("RingBuffer: channel count and capacity reporting", "[ring_buffer]") {
    auto storage = makeStorage(16, 4);
    RingBuffer rb(storage.data(), 16, 4);

    REQUIRE(rb.channels() == 4);
    REQUIRE(rb.capacityFrames() == 16);
    REQUIRE(rb.usableCapacityFrames() == 15);
}

TEST_CASE("RingBuffer: write then read returns the same samples (2ch)", "[ring_buffer]") {
    auto storage = makeStorage(32, 2);
    RingBuffer rb(storage.data(), 32, 2);

    constexpr std::size_t kFrames = 10;
    std::vector<float> in(kFrames * 2);
    for (std::size_t f = 0; f < kFrames; ++f) {
        fillFrame(in.data() + f * 2, 2, f);
    }

    REQUIRE(rb.writeFrames(in.data(), kFrames) == kFrames);
    REQUIRE(rb.framesAvailableForRead() == kFrames);

    std::vector<float> out(kFrames * 2);
    REQUIRE(rb.readFrames(out.data(), kFrames) == kFrames);
    REQUIRE(out == in);
    REQUIRE(rb.framesAvailableForRead() == 0);
}

TEST_CASE("RingBuffer: mono (1ch) write/read round-trip", "[ring_buffer]") {
    auto storage = makeStorage(8, 1);
    RingBuffer rb(storage.data(), 8, 1);

    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(rb.writeFrames(in, 4) == 4);

    float out[4]{};
    REQUIRE(rb.readFrames(out, 4) == 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out[i] == in[i]);
}

TEST_CASE("RingBuffer: 8-channel write/read preserves per-channel interleaving", "[ring_buffer]") {
    auto storage = makeStorage(16, 8);
    RingBuffer rb(storage.data(), 16, 8);

    constexpr std::size_t kFrames = 5;
    std::vector<float> in(kFrames * 8);
    for (std::size_t f = 0; f < kFrames; ++f) {
        fillFrame(in.data() + f * 8, 8, f);
    }

    REQUIRE(rb.writeFrames(in.data(), kFrames) == kFrames);
    std::vector<float> out(kFrames * 8);
    REQUIRE(rb.readFrames(out.data(), kFrames) == kFrames);

    for (std::size_t f = 0; f < kFrames; ++f) {
        REQUIRE(checkFrame(out.data() + f * 8, 8, f));
    }
}

TEST_CASE("RingBuffer: writing past capacity returns a short count", "[ring_buffer]") {
    auto storage = makeStorage(4, 1);
    RingBuffer rb(storage.data(), 4, 1);
    // Usable capacity is 3 frames.

    float in[5] = {1, 2, 3, 4, 5};
    REQUIRE(rb.writeFrames(in, 5) == 3);
    REQUIRE(rb.framesAvailableForRead() == 3);
    // Further writes on a full buffer return 0.
    REQUIRE(rb.writeFrames(in + 3, 2) == 0);
}

TEST_CASE("RingBuffer: reading past available returns a short count", "[ring_buffer]") {
    auto storage = makeStorage(8, 2);
    RingBuffer rb(storage.data(), 8, 2);

    float in[4] = {1, 2, 3, 4};  // 2 frames of 2 channels
    REQUIRE(rb.writeFrames(in, 2) == 2);

    float out[10]{};
    // Ask for 5 frames; only 2 available.
    REQUIRE(rb.readFrames(out, 5) == 2);
    REQUIRE(rb.framesAvailableForRead() == 0);
}

TEST_CASE("RingBuffer: wrap-around with contiguous writes and reads", "[ring_buffer]") {
    auto storage = makeStorage(4, 1);
    RingBuffer rb(storage.data(), 4, 1);
    // Usable capacity is 3. Write 2, read 2, write 3 (wraps), read 3.

    float in1[2] = {1, 2};
    REQUIRE(rb.writeFrames(in1, 2) == 2);
    float out1[2]{};
    REQUIRE(rb.readFrames(out1, 2) == 2);
    REQUIRE(out1[0] == 1);
    REQUIRE(out1[1] == 2);

    float in2[3] = {10, 20, 30};
    REQUIRE(rb.writeFrames(in2, 3) == 3);  // this wraps past index 0
    float out2[3]{};
    REQUIRE(rb.readFrames(out2, 3) == 3);
    REQUIRE(out2[0] == 10);
    REQUIRE(out2[1] == 20);
    REQUIRE(out2[2] == 30);
}

TEST_CASE("RingBuffer: many interleaved cycles through the same slots",
          "[ring_buffer]") {
    // Small buffer (capacity 4 frames, usable 3) — force many wrap
    // cycles and verify data integrity after thousands of round trips.
    auto storage = makeStorage(4, 2);
    RingBuffer rb(storage.data(), 4, 2);

    constexpr std::size_t kCycles = 5'000;
    for (std::size_t f = 0; f < kCycles; ++f) {
        float in[2];
        fillFrame(in, 2, f);
        REQUIRE(rb.writeFrames(in, 1) == 1);

        float out[2]{};
        REQUIRE(rb.readFrames(out, 1) == 1);
        REQUIRE(checkFrame(out, 2, f));
    }
    REQUIRE(rb.framesAvailableForRead() == 0);
}

TEST_CASE("RingBuffer: partial write then partial read advances correctly", "[ring_buffer]") {
    auto storage = makeStorage(8, 2);
    RingBuffer rb(storage.data(), 8, 2);
    // Usable capacity: 7 frames. Write 5, read 3, write 5 more (some wrap), read 7.

    std::vector<float> in(5 * 2);
    for (std::size_t f = 0; f < 5; ++f) fillFrame(in.data() + f * 2, 2, f);
    REQUIRE(rb.writeFrames(in.data(), 5) == 5);

    std::vector<float> out(3 * 2);
    REQUIRE(rb.readFrames(out.data(), 3) == 3);
    for (std::size_t f = 0; f < 3; ++f) {
        REQUIRE(checkFrame(out.data() + f * 2, 2, f));
    }

    // Now 2 frames remaining in buffer; write 5 more (frames 5..9),
    // wrapping around the physical end of storage.
    std::vector<float> in2(5 * 2);
    for (std::size_t f = 0; f < 5; ++f) fillFrame(in2.data() + f * 2, 2, f + 5);
    REQUIRE(rb.writeFrames(in2.data(), 5) == 5);
    REQUIRE(rb.framesAvailableForRead() == 7);

    // Drain all 7 frames (indexes 3..9).
    std::vector<float> out2(7 * 2);
    REQUIRE(rb.readFrames(out2.data(), 7) == 7);
    for (std::size_t f = 0; f < 7; ++f) {
        REQUIRE(checkFrame(out2.data() + f * 2, 2, f + 3));
    }
}

TEST_CASE("RingBuffer: zero-count operations are no-ops", "[ring_buffer]") {
    auto storage = makeStorage(4, 1);
    RingBuffer rb(storage.data(), 4, 1);

    float buf[1] = {0};
    REQUIRE(rb.writeFrames(buf, 0) == 0);
    REQUIRE(rb.readFrames(buf, 0) == 0);
    REQUIRE(rb.framesAvailableForRead() == 0);
}

TEST_CASE("RingBuffer: SPSC concurrent write/read preserves sample ordering",
          "[ring_buffer][stress]") {
    // Classical producer/consumer stress. Producer writes monotonic
    // frames in batches of up to 16; consumer reads in batches of up
    // to 16. Verify every frame is received in order with no loss.
    const std::size_t channels = 2;
    const std::size_t capacity = 256;  // 255 usable
    auto storage = makeStorage(capacity, channels);
    RingBuffer rb(storage.data(), capacity, channels);

    constexpr std::size_t kTotalFrames = 200'000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        std::vector<float> batch(16 * channels);
        std::size_t frame_index = 0;
        while (frame_index < kTotalFrames) {
            const std::size_t batch_frames =
                std::min<std::size_t>(16, kTotalFrames - frame_index);
            for (std::size_t f = 0; f < batch_frames; ++f) {
                fillFrame(batch.data() + f * channels, channels, frame_index + f);
            }
            std::size_t written = 0;
            while (written < batch_frames) {
                const std::size_t n = rb.writeFrames(
                    batch.data() + written * channels,
                    batch_frames - written);
                if (n == 0) {
                    std::this_thread::yield();
                } else {
                    written += n;
                }
            }
            frame_index += batch_frames;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::size_t next_expected = 0;
    std::thread consumer([&]() {
        std::vector<float> batch(16 * channels);
        while (next_expected < kTotalFrames) {
            const std::size_t n = rb.readFrames(batch.data(), 16);
            if (n == 0) {
                if (producer_done.load(std::memory_order_acquire)) {
                    // One last drain in case producer finished between
                    // our read and done-flag check.
                    const std::size_t n2 = rb.readFrames(batch.data(), 16);
                    if (n2 == 0) break;
                    for (std::size_t f = 0; f < n2; ++f) {
                        REQUIRE(checkFrame(batch.data() + f * channels,
                                           channels, next_expected + f));
                    }
                    next_expected += n2;
                    continue;
                }
                std::this_thread::yield();
                continue;
            }
            for (std::size_t f = 0; f < n; ++f) {
                REQUIRE(checkFrame(batch.data() + f * channels,
                                   channels, next_expected + f));
            }
            next_expected += n;
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(next_expected == kTotalFrames);
    REQUIRE(rb.framesAvailableForRead() == 0);
}

TEST_CASE("RingBuffer: consumer-faster-than-producer yields short reads, no corruption",
          "[ring_buffer][stress]") {
    // Consumer spins reading; producer writes at a slower pace.
    // Verify reads always succeed in order (possibly with short
    // counts) and never deliver stale or corrupted data.
    const std::size_t channels = 2;
    const std::size_t capacity = 64;
    auto storage = makeStorage(capacity, channels);
    RingBuffer rb(storage.data(), capacity, channels);

    constexpr std::size_t kTotal = 50'000;
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        std::vector<float> one(channels);
        for (std::size_t f = 0; f < kTotal; ++f) {
            fillFrame(one.data(), channels, f);
            while (rb.writeFrames(one.data(), 1) == 0) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::size_t next_expected = 0;
    std::thread consumer([&]() {
        std::vector<float> one(channels);
        while (next_expected < kTotal) {
            const std::size_t n = rb.readFrames(one.data(), 1);
            if (n == 1) {
                REQUIRE(checkFrame(one.data(), channels, next_expected));
                ++next_expected;
            } else if (producer_done.load(std::memory_order_acquire) &&
                       rb.framesAvailableForRead() == 0) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(next_expected == kTotal);
}
