// audio_buffer_interleave.cpp — see header.

#include "audio_buffer_interleave.hpp"

#include <cstring>

namespace jbox::control {

std::uint32_t readInputInterleaved(const AudioBufferList* input,
                                   std::uint32_t channels,
                                   float* scratch) {
    if (input == nullptr || input->mNumberBuffers == 0 || channels == 0) {
        return 0;
    }

    // Hot path: single fully-interleaved buffer.
    if (input->mNumberBuffers == 1 &&
        input->mBuffers[0].mNumberChannels == channels) {
        const std::uint32_t frame_count =
            input->mBuffers[0].mDataByteSize /
            (sizeof(float) * channels);
        std::memcpy(scratch, input->mBuffers[0].mData,
                    static_cast<std::size_t>(frame_count) * channels * sizeof(float));
        return frame_count;
    }

    // General case: walk each buffer, use its own mNumberChannels.
    // Frame count is whatever the first buffer reports (every buffer
    // in an `AudioBufferList` carries the same frame count).
    const AudioBuffer& first = input->mBuffers[0];
    const std::uint32_t first_channels =
        first.mNumberChannels == 0 ? 1 : first.mNumberChannels;
    const std::uint32_t frame_count =
        first.mDataByteSize / (sizeof(float) * first_channels);

    std::uint32_t dest_ch = 0;
    for (UInt32 b = 0;
         b < input->mNumberBuffers && dest_ch < channels; ++b) {
        const AudioBuffer& buf = input->mBuffers[b];
        const std::uint32_t buf_channels =
            buf.mNumberChannels == 0 ? 1 : buf.mNumberChannels;
        const auto* src = static_cast<const float*>(buf.mData);
        if (src == nullptr) {
            dest_ch += buf_channels;
            continue;
        }
        for (std::uint32_t ch_in_buf = 0;
             ch_in_buf < buf_channels && dest_ch < channels;
             ++ch_in_buf, ++dest_ch) {
            for (std::uint32_t f = 0; f < frame_count; ++f) {
                scratch[f * channels + dest_ch] =
                    src[f * buf_channels + ch_in_buf];
            }
        }
    }
    return frame_count;
}

void writeOutputFromInterleaved(AudioBufferList* output,
                                std::uint32_t channels,
                                std::uint32_t frame_count,
                                const float* scratch) {
    if (output == nullptr || output->mNumberBuffers == 0 || channels == 0) {
        return;
    }

    // Hot path: single fully-interleaved buffer.
    if (output->mNumberBuffers == 1 &&
        output->mBuffers[0].mNumberChannels == channels) {
        std::memcpy(output->mBuffers[0].mData, scratch,
                    static_cast<std::size_t>(frame_count) * channels * sizeof(float));
        return;
    }

    // General case: walk each buffer, de-interleave into its native
    // `mNumberChannels`-wide layout.
    std::uint32_t src_ch = 0;
    for (UInt32 b = 0;
         b < output->mNumberBuffers && src_ch < channels; ++b) {
        AudioBuffer& buf = output->mBuffers[b];
        const std::uint32_t buf_channels =
            buf.mNumberChannels == 0 ? 1 : buf.mNumberChannels;
        auto* dst = static_cast<float*>(buf.mData);
        if (dst == nullptr) {
            src_ch += buf_channels;
            continue;
        }
        for (std::uint32_t ch_in_buf = 0;
             ch_in_buf < buf_channels && src_ch < channels;
             ++ch_in_buf, ++src_ch) {
            for (std::uint32_t f = 0; f < frame_count; ++f) {
                dst[f * buf_channels + ch_in_buf] =
                    scratch[f * channels + src_ch];
            }
        }
    }
}

}  // namespace jbox::control
