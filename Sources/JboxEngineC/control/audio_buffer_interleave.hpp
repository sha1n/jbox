// audio_buffer_interleave.hpp — Core Audio AudioBufferList ↔
// interleaved-scratch conversion for the RT trampoline.
//
// Core Audio presents audio to an IOProc as an `AudioBufferList`
// whose layout depends on the device configuration. Jbox's duplex
// fast path reads from the input list and writes to the output list
// in the same RT callback, and needs a uniformly interleaved scratch
// representation in the middle for simple channel-mapped copies.
// The two functions here convert between the HAL's native layout
// and that scratch.
//
// Three layouts we must handle:
//   1. Single interleaved buffer carrying all N channels
//      (`mNumberBuffers == 1`, `mBuffers[0].mNumberChannels == N`).
//      Direct memcpy hot path.
//   2. Per-channel planar layout (N buffers, each with one channel).
//      The classic macOS per-channel AudioBufferList.
//   3. Aggregate-device layout: multiple buffers, each holding one
//      sub-device's interleaved channels (e.g. buffer 0 = member A's
//      4 channels, buffer 1 = member B's 18 channels). This is the
//      layout an Audio MIDI Setup aggregate produces, and the layout
//      the pre-Phase-6 code did NOT handle — producing silent output
//      on the fast path. Handled here by walking each buffer's
//      `mNumberChannels` independently.
//
// Both functions are RT-safe: no allocation, no syscalls, bounded
// work. The callers pass a pre-allocated scratch buffer sized for
// `channels × max_frames`.

#ifndef JBOX_CONTROL_AUDIO_BUFFER_INTERLEAVE_HPP
#define JBOX_CONTROL_AUDIO_BUFFER_INTERLEAVE_HPP

#include <CoreAudioTypes/CoreAudioBaseTypes.h>

#include <cstdint>

namespace jbox::control {

// Interleave every audio frame from `input` into `scratch`. Returns
// the number of frames actually written (derived from the first
// buffer's byte size). Stops walking buffers once `channels` channels
// have been laid down; extra buffers are ignored. Missing buffers
// leave the corresponding scratch slots untouched — callers should
// memset the scratch to zero before the call if they care.
std::uint32_t readInputInterleaved(const AudioBufferList* input,
                                   std::uint32_t channels,
                                   float* scratch);

// De-interleave `frame_count` frames from `scratch` into `output`'s
// native layout. If the device exposes a single interleaved buffer
// with the full channel count, uses a direct memcpy. Otherwise walks
// each output buffer (which may itself carry multiple channels) and
// scatters the scratch samples across each slot.
void writeOutputFromInterleaved(AudioBufferList* output,
                                std::uint32_t channels,
                                std::uint32_t frame_count,
                                const float* scratch);

}  // namespace jbox::control

#endif  // JBOX_CONTROL_AUDIO_BUFFER_INTERLEAVE_HPP
