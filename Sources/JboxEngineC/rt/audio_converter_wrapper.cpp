// audio_converter_wrapper.cpp

#include "audio_converter_wrapper.hpp"

#include <AudioToolbox/AudioConverter.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>

#include <cstring>
#include <stdexcept>

namespace jbox::rt {

namespace {

AudioStreamBasicDescription makeFloatASBD(double rate, std::uint32_t channels) {
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket   = sizeof(float) * channels;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = sizeof(float) * channels;
    asbd.mChannelsPerFrame = channels;
    asbd.mBitsPerChannel   = 32;
    return asbd;
}

// Trampoline state passed through AudioConverterFillComplexBuffer.
struct TrampolineCtx {
    AudioConverterWrapper::PullInputFn pull;
    void*                              user;
    std::uint32_t                      channels;

    // Scratch storage for one chunk. Sized at construction so the
    // callback path is allocation-free. Sized to 4096 frames; if the
    // converter asks for more in a single callback (uncommon at our
    // rates and buffer sizes), we return what we have and the outer
    // call loops.
    static constexpr std::size_t   kMaxChunkFrames = 4096;
    static constexpr std::uint32_t kMaxChannels    = 8;
    float chunk[kMaxChunkFrames * kMaxChannels];
    std::size_t chunk_valid_bytes = 0;
};

OSStatus acInputCallback(AudioConverterRef /*inConverter*/,
                         UInt32* ioNumberDataPackets,
                         AudioBufferList* ioData,
                         AudioStreamPacketDescription** /*outDataPacketDescription*/,
                         void* inUserData) {
    auto* ctx = static_cast<TrampolineCtx*>(inUserData);
    const UInt32 requested = *ioNumberDataPackets;
    const std::size_t max_chunk = TrampolineCtx::kMaxChunkFrames;
    const std::size_t to_request = (requested < max_chunk) ? requested : max_chunk;
    const std::size_t produced =
        ctx->pull(ctx->chunk, to_request, ctx->user);

    ctx->chunk_valid_bytes = produced * ctx->channels * sizeof(float);
    ioData->mNumberBuffers = 1;
    ioData->mBuffers[0].mNumberChannels = ctx->channels;
    ioData->mBuffers[0].mDataByteSize   = static_cast<UInt32>(ctx->chunk_valid_bytes);
    ioData->mBuffers[0].mData           = ctx->chunk;
    *ioNumberDataPackets = static_cast<UInt32>(produced);
    return noErr;
}

}  // namespace

AudioConverterWrapper::AudioConverterWrapper(double src_rate, double dst_rate,
                                             std::uint32_t channels)
    : channels_(channels),
      current_input_rate_(src_rate) {
    if (channels == 0 || channels > TrampolineCtx::kMaxChannels) {
        throw std::runtime_error(
            "AudioConverterWrapper: channels must be in [1, 8]");
    }
    const auto in  = makeFloatASBD(src_rate, channels);
    const auto out = makeFloatASBD(dst_rate, channels);
    AudioConverterRef ac = nullptr;
    const OSStatus status = AudioConverterNew(&in, &out, &ac);
    if (status != noErr || ac == nullptr) {
        throw std::runtime_error("AudioConverterNew failed");
    }
    const UInt32 complexity = kAudioConverterSampleRateConverterComplexity_Mastering;
    AudioConverterSetProperty(ac,
                              kAudioConverterSampleRateConverterComplexity,
                              sizeof(complexity), &complexity);
    const UInt32 quality = kAudioConverterQuality_Max;
    AudioConverterSetProperty(ac,
                              kAudioConverterSampleRateConverterQuality,
                              sizeof(quality), &quality);
    converter_ = ac;
}

AudioConverterWrapper::~AudioConverterWrapper() {
    if (converter_ != nullptr) {
        AudioConverterDispose(static_cast<AudioConverterRef>(converter_));
        converter_ = nullptr;
    }
}

void AudioConverterWrapper::setInputRate(double rate) noexcept {
    if (rate <= 0.0 || converter_ == nullptr) return;
    if (rate == current_input_rate_) return;
    // Apple AudioConverter's kAudioConverterSampleRate property (named
    // in docs/spec.md § 2.5) is not consistently writable on our
    // float-interleaved SRC configuration. We instead
    // re-set the full input stream description, which is documented to
    // propagate as a variable-ratio SRC update. The no-op fast-path
    // above means we pay the cost only when the rate actually changes;
    // the drift controller is slow (PI response in seconds) so most
    // 100 Hz ticks short-circuit.
    AudioStreamBasicDescription asbd = makeFloatASBD(rate, channels_);
    AudioConverterSetProperty(static_cast<AudioConverterRef>(converter_),
                              kAudioConverterCurrentInputStreamDescription,
                              sizeof(asbd), &asbd);
    current_input_rate_ = rate;
}

void AudioConverterWrapper::reset() noexcept {
    if (converter_ == nullptr) return;
    AudioConverterReset(static_cast<AudioConverterRef>(converter_));
}

std::uint32_t AudioConverterWrapper::primeLeadingFrames() const noexcept {
    if (converter_ == nullptr) return 0;
    AudioConverterPrimeInfo info{};
    UInt32 size = sizeof(info);
    const OSStatus status = AudioConverterGetProperty(
        static_cast<AudioConverterRef>(converter_),
        kAudioConverterPrimeInfo,
        &size, &info);
    if (status != noErr) return 0;
    return static_cast<std::uint32_t>(info.leadingFrames);
}

std::size_t AudioConverterWrapper::convert(float* out,
                                           std::size_t frames_requested,
                                           PullInputFn pull_input,
                                           void* user) noexcept {
    if (converter_ == nullptr || pull_input == nullptr || out == nullptr) return 0;

    // Not value-initialized: only the fields below are read by
    // acInputCallback. The chunk[] array is written by pull_input and
    // its valid byte count is set inside the callback, so leaving it
    // uninitialized avoids zeroing 128 KiB per IOProc.
    TrampolineCtx ctx;
    ctx.pull              = pull_input;
    ctx.user              = user;
    ctx.channels          = channels_;
    ctx.chunk_valid_bytes = 0;

    AudioBufferList out_list{};
    out_list.mNumberBuffers = 1;
    out_list.mBuffers[0].mNumberChannels = channels_;
    out_list.mBuffers[0].mDataByteSize =
        static_cast<UInt32>(frames_requested * channels_ * sizeof(float));
    out_list.mBuffers[0].mData = out;

    UInt32 io_frames = static_cast<UInt32>(frames_requested);
    const OSStatus status = AudioConverterFillComplexBuffer(
        static_cast<AudioConverterRef>(converter_),
        &acInputCallback,
        &ctx,
        &io_frames,
        &out_list,
        nullptr);
    (void)status;  // short reads are conveyed via io_frames; non-error.
    return static_cast<std::size_t>(io_frames);
}

}  // namespace jbox::rt
