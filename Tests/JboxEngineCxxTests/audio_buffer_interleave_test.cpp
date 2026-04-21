// audio_buffer_interleave_test.cpp — direct unit tests for the
// AudioBufferList ↔ interleaved-scratch helpers used by the duplex
// fast-path trampoline.
//
// These tests construct synthetic `AudioBufferList` instances by
// hand so we can cover three layouts that are otherwise only
// exercised on real hardware:
//   1. single-buffer, all-channels-interleaved (the hot path)
//   2. per-channel planar (N buffers, 1 channel each)
//   3. aggregate-device layout: multiple buffers where each buffer
//      carries one member device's full channel group in its own
//      interleaved format. The pre-Phase-6 helper misread this
//      case and produced silent output; the regression lives here.

#include <catch_amalgamated.hpp>

#include "audio_buffer_interleave.hpp"

#include <array>
#include <cstring>
#include <vector>

using jbox::control::readInputInterleaved;
using jbox::control::writeOutputFromInterleaved;

namespace {

// A poor-man's AudioBufferList builder. Core Audio's struct has a
// flexible-array of AudioBuffer at the end; we back it with a vector
// and keep pointers stable.
struct BufferList {
    std::vector<AudioBufferList> storage;  // single element sized for N buffers
    std::vector<std::vector<float>> bufs;

    BufferList(std::initializer_list<std::uint32_t> channels_per_buffer,
               std::uint32_t frames) {
        const std::size_t n_buffers = channels_per_buffer.size();
        const std::size_t abl_bytes =
            offsetof(AudioBufferList, mBuffers) +
            n_buffers * sizeof(AudioBuffer);
        storage.resize((abl_bytes + sizeof(AudioBufferList) - 1) /
                       sizeof(AudioBufferList));
        auto* abl = reinterpret_cast<AudioBufferList*>(storage.data());
        abl->mNumberBuffers = static_cast<UInt32>(n_buffers);

        bufs.reserve(n_buffers);
        std::size_t i = 0;
        for (std::uint32_t ch : channels_per_buffer) {
            bufs.emplace_back(frames * ch, 0.0f);
            abl->mBuffers[i].mNumberChannels = ch;
            abl->mBuffers[i].mDataByteSize   =
                static_cast<UInt32>(frames * ch * sizeof(float));
            abl->mBuffers[i].mData           = bufs.back().data();
            ++i;
        }
    }

    AudioBufferList* abl() {
        return reinterpret_cast<AudioBufferList*>(storage.data());
    }
};

}  // namespace

TEST_CASE("readInputInterleaved: single fully-interleaved buffer memcpys",
          "[audio_buffer_interleave]") {
    constexpr std::uint32_t kFrames = 8;
    constexpr std::uint32_t kChannels = 4;
    BufferList in({kChannels}, kFrames);
    // Fill buffer 0 with a signature: channel c at frame f = 100*c + f.
    auto* p = in.bufs[0].data();
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            p[f * kChannels + c] = 100.0f * static_cast<float>(c) +
                                   static_cast<float>(f);
        }
    }

    std::array<float, kFrames * kChannels> scratch{};
    const auto written =
        readInputInterleaved(in.abl(), kChannels, scratch.data());
    REQUIRE(written == kFrames);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            REQUIRE(scratch[f * kChannels + c] ==
                    100.0f * static_cast<float>(c) + static_cast<float>(f));
        }
    }
}

TEST_CASE("readInputInterleaved: per-channel planar (N buffers × 1 channel)",
          "[audio_buffer_interleave]") {
    constexpr std::uint32_t kFrames = 8;
    BufferList in({1, 1, 1}, kFrames);  // 3 channels, one per buffer
    for (std::uint32_t c = 0; c < 3; ++c) {
        for (std::uint32_t f = 0; f < kFrames; ++f) {
            in.bufs[c][f] = 100.0f * static_cast<float>(c) +
                            static_cast<float>(f);
        }
    }

    std::array<float, kFrames * 3> scratch{};
    const auto written =
        readInputInterleaved(in.abl(), /*channels*/ 3, scratch.data());
    REQUIRE(written == kFrames);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 3; ++c) {
            REQUIRE(scratch[f * 3 + c] ==
                    100.0f * static_cast<float>(c) + static_cast<float>(f));
        }
    }
}

TEST_CASE("readInputInterleaved: aggregate layout — multi-buffer × multi-channel",
          "[audio_buffer_interleave][aggregate]") {
    // Regression: aggregate devices present each member's channel
    // group as its own interleaved buffer. The pre-Phase-6 code
    // assumed one channel per buffer and misread this layout,
    // producing near-silent output. Here buffer 0 carries 4 channels
    // (member A) and buffer 1 carries 2 channels (member B), total
    // 6 logical channels on the aggregate.
    constexpr std::uint32_t kFrames = 5;
    BufferList in({4, 2}, kFrames);
    // Buffer 0: channels 0..3 of the aggregate. value = 100*c + f.
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 4; ++c) {
            in.bufs[0][f * 4 + c] = 100.0f * static_cast<float>(c) +
                                    static_cast<float>(f);
        }
    }
    // Buffer 1: channels 4..5 of the aggregate.
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            in.bufs[1][f * 2 + c] =
                100.0f * static_cast<float>(c + 4) + static_cast<float>(f);
        }
    }

    std::array<float, kFrames * 6> scratch{};
    const auto written =
        readInputInterleaved(in.abl(), /*channels*/ 6, scratch.data());
    REQUIRE(written == kFrames);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 6; ++c) {
            REQUIRE(scratch[f * 6 + c] ==
                    100.0f * static_cast<float>(c) + static_cast<float>(f));
        }
    }
}

TEST_CASE("writeOutputFromInterleaved: single fully-interleaved buffer memcpys",
          "[audio_buffer_interleave]") {
    constexpr std::uint32_t kFrames = 8;
    constexpr std::uint32_t kChannels = 4;
    BufferList out({kChannels}, kFrames);

    std::array<float, kFrames * kChannels> scratch{};
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            scratch[f * kChannels + c] =
                100.0f * static_cast<float>(c) + static_cast<float>(f);
        }
    }
    writeOutputFromInterleaved(out.abl(), kChannels, kFrames, scratch.data());
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            REQUIRE(out.bufs[0][f * kChannels + c] ==
                    100.0f * static_cast<float>(c) + static_cast<float>(f));
        }
    }
}

TEST_CASE("writeOutputFromInterleaved: aggregate layout round-trips correctly",
          "[audio_buffer_interleave][aggregate]") {
    // Same scenario as the read test, reversed: scratch has 6
    // interleaved channels; output is two buffers of (4, 2) channels.
    constexpr std::uint32_t kFrames = 5;
    BufferList out({4, 2}, kFrames);

    std::array<float, kFrames * 6> scratch{};
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 6; ++c) {
            scratch[f * 6 + c] =
                100.0f * static_cast<float>(c) + static_cast<float>(f);
        }
    }
    writeOutputFromInterleaved(out.abl(), 6, kFrames, scratch.data());

    // First buffer: channels 0..3.
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 4; ++c) {
            REQUIRE(out.bufs[0][f * 4 + c] ==
                    100.0f * static_cast<float>(c) + static_cast<float>(f));
        }
    }
    // Second buffer: channels 4..5.
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            REQUIRE(out.bufs[1][f * 2 + c] ==
                    100.0f * static_cast<float>(c + 4) +
                    static_cast<float>(f));
        }
    }
}

TEST_CASE("readInputInterleaved: empty / invalid inputs are silent no-ops",
          "[audio_buffer_interleave]") {
    float scratch = 0.0f;
    REQUIRE(readInputInterleaved(nullptr, 2, &scratch) == 0);
    // Empty list.
    AudioBufferList empty{};
    empty.mNumberBuffers = 0;
    REQUIRE(readInputInterleaved(&empty, 2, &scratch) == 0);
}
