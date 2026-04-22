// audio_converter_wrapper.hpp — thin RT-friendly wrapper around Apple's
// AudioConverter, configured once per route.
//
// Always in the output path (including at ratio 1.0). The wrapper owns
// an AudioConverterRef, exposes a pull-style convert() that takes a
// caller-supplied RT-safe input callback, and allows setInputRate() to
// be called from the RT thread immediately before convert() to apply
// drift-correction rate adjustments without glitching.
//
// The underlying AudioConverter is not documented as thread-safe; only
// the RT thread that calls convert() may also call setInputRate(). The
// control thread must publish new rates through an atomic that the RT
// thread reads — see RouteRecord::target_input_rate in route_manager.
//
// Quality: two presets selectable at construction time (docs/spec.md
// § 2.5 and § 4.6). `Mastering` uses
// `kAudioConverterSampleRateConverterComplexity_Mastering` +
// `kAudioConverterQuality_Max` — the default, highest-fidelity setting
// we have used since Phase 4. `HighQuality` uses `_Normal` complexity +
// `Quality_High`, trading a small amount of SRC transparency for
// noticeably less CPU on high-channel-count / multi-route sessions.
// The preset is chosen per-construction; changing the engine-wide
// preference affects new routes only (existing routes keep the quality
// their converter was built with).

#ifndef JBOX_RT_AUDIO_CONVERTER_WRAPPER_HPP
#define JBOX_RT_AUDIO_CONVERTER_WRAPPER_HPP

#include <cstddef>
#include <cstdint>

namespace jbox::rt {

enum class ResamplerQuality : std::uint8_t {
    // kAudioConverterSampleRateConverterComplexity_Mastering +
    // kAudioConverterQuality_Max. Default. Highest-fidelity preset.
    Mastering   = 0,
    // _Normal complexity + Quality_High. Cheaper; still well above the
    // Core Audio default quality. Pick this when CPU headroom matters
    // more than SRC transparency.
    HighQuality = 1,
};

class AudioConverterWrapper {
public:
    // Construct with nominal rates and channel count. Throws std::runtime_error
    // on AudioConverterNew failure (control-thread only — construction is
    // never called on the RT thread). `quality` selects the SRC quality
    // preset; defaulting to Mastering preserves the pre-preferences
    // behaviour at call sites that have not yet been updated.
    AudioConverterWrapper(double src_rate, double dst_rate,
                          std::uint32_t channels,
                          ResamplerQuality quality = ResamplerQuality::Mastering);
    ~AudioConverterWrapper();

    AudioConverterWrapper(const AudioConverterWrapper&) = delete;
    AudioConverterWrapper& operator=(const AudioConverterWrapper&) = delete;

    // Pull N output frames. `pull_input` supplies source samples on demand
    // and MUST be RT-safe. Returns frames actually produced; a short count
    // signals input starvation (pull_input returned fewer frames than the
    // converter asked for).
    using PullInputFn = std::size_t (*)(float* dst, std::size_t frames,
                                        void* user);

    std::size_t convert(float* out, std::size_t frames_requested,
                        PullInputFn pull_input, void* user) noexcept;

    // Adjust the effective input sample rate. Safe to call from the RT
    // thread immediately before convert(). Glitch-free per AudioConverter
    // semantics (docs/spec.md § 2.5).
    void setInputRate(double rate) noexcept;
    double inputRate() const noexcept { return current_input_rate_; }

    // Discard any internal buffering. NOT RT-safe — call from the
    // control thread only (e.g., on route stop).
    void reset() noexcept;

    // Leading prime frames reported by AudioConverter — the number of
    // input frames the polyphase SRC holds in its filter tail at any
    // point (per kAudioConverterPrimeInfo). Used by the per-route
    // latency estimator (docs/spec.md § 2.12). Property read is NOT
    // RT-safe and must be called from the control thread only.
    std::uint32_t primeLeadingFrames() const noexcept;

    std::uint32_t channels() const noexcept { return channels_; }

private:
    void*         converter_{nullptr};  // AudioConverterRef (opaque here).
    std::uint32_t channels_;
    double        current_input_rate_{0.0};  // last rate pushed to AudioConverter; ctor sets to src_rate
};

}  // namespace jbox::rt

#endif  // JBOX_RT_AUDIO_CONVERTER_WRAPPER_HPP
