// Unit tests for AudioConverterWrapper.
//
// Focus: lifecycle, unity-ratio fidelity, setInputRate, partial-input
// frame accounting. SNR-style fidelity checks use a windowed sine.

#include "audio_converter_wrapper.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <vector>

namespace {

// Simple sine generator used as the input source. Frequency is in Hz
// relative to the source sample rate passed into the wrapper.
struct SineSource {
    double phase{0.0};
    double freq_hz{440.0};
    double rate_hz{48000.0};
    std::uint32_t channels{1};

    // PullInputFn-compatible trampoline.
    static std::size_t pull(float* dst, std::size_t frames, void* user) {
        auto* s = static_cast<SineSource*>(user);
        const double step = 2.0 * std::numbers::pi * s->freq_hz / s->rate_hz;
        for (std::size_t f = 0; f < frames; ++f) {
            const float v = static_cast<float>(std::sin(s->phase));
            for (std::uint32_t c = 0; c < s->channels; ++c) {
                dst[f * s->channels + c] = v;
            }
            s->phase += step;
        }
        return frames;
    }
};

// Compute RMS of a buffer.
double rms(const float* buf, std::size_t count) {
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) sum += buf[i] * buf[i];
    return std::sqrt(sum / std::max<std::size_t>(count, 1));
}

}  // namespace

TEST_CASE("AudioConverterWrapper constructs and destroys cleanly at unity",
          "[audio_converter_wrapper]") {
    jbox::rt::AudioConverterWrapper w(48000.0, 48000.0, 2);
    REQUIRE(w.channels() == 2u);
    REQUIRE(w.inputRate() == Catch::Approx(48000.0));
}

TEST_CASE("AudioConverterWrapper passes unity-ratio sine with high fidelity",
          "[audio_converter_wrapper]") {
    jbox::rt::AudioConverterWrapper w(48000.0, 48000.0, 1);
    SineSource src{.phase = 0.0, .freq_hz = 440.0, .rate_hz = 48000.0, .channels = 1};

    std::vector<float> out(4800, 0.0f);  // 100 ms
    const std::size_t produced = w.convert(out.data(), out.size(),
                                           &SineSource::pull, &src);
    REQUIRE(produced == out.size());

    // Amplitude should be close to 1 / sqrt(2) for a full-scale sine.
    const double r = rms(out.data(), out.size());
    REQUIRE(r == Catch::Approx(1.0 / std::sqrt(2.0)).margin(0.02));
}

TEST_CASE("AudioConverterWrapper setInputRate increases input consumption",
          "[audio_converter_wrapper]") {
    // For a fixed output request, a higher effective input rate means
    // the converter has to consume more source frames to fill the same
    // output buffer. We count input frames pulled; the second convert
    // (post-setInputRate at +1%) must consume more than the first.
    jbox::rt::AudioConverterWrapper w(44100.0, 48000.0, 1);

    struct CountingSource {
        double phase = 0.0;
        double freq_hz = 440.0;
        double rate_hz = 44100.0;
        std::size_t pulled = 0;
        static std::size_t pull(float* dst, std::size_t frames, void* user) {
            auto* s = static_cast<CountingSource*>(user);
            const double step = 2.0 * std::numbers::pi * s->freq_hz / s->rate_hz;
            for (std::size_t f = 0; f < frames; ++f) {
                dst[f] = static_cast<float>(std::sin(s->phase));
                s->phase += step;
            }
            s->pulled += frames;
            return frames;
        }
    };

    CountingSource src;
    std::vector<float> out(4800, 0.0f);

    const std::size_t first_out = w.convert(out.data(), out.size(),
                                            &CountingSource::pull, &src);
    REQUIRE(first_out == out.size());
    const std::size_t first_pulled = src.pulled;
    REQUIRE(first_pulled > 0u);

    // Bump effective input rate by 1%. For the same 4800-frame output
    // request, the converter must pull more input now.
    w.setInputRate(44100.0 * 1.01);
    const std::size_t second_out = w.convert(out.data(), out.size(),
                                             &CountingSource::pull, &src);
    REQUIRE(second_out == out.size());
    const std::size_t second_pulled = src.pulled - first_pulled;

    REQUIRE(second_pulled > first_pulled);
}

TEST_CASE("AudioConverterWrapper handles partial input correctly",
          "[audio_converter_wrapper]") {
    jbox::rt::AudioConverterWrapper w(48000.0, 48000.0, 1);

    struct Limited {
        std::size_t remaining = 100;
        static std::size_t pull(float* dst, std::size_t frames, void* user) {
            auto* l = static_cast<Limited*>(user);
            const std::size_t n = std::min(frames, l->remaining);
            std::memset(dst, 0, n * sizeof(float));
            l->remaining -= n;
            return n;
        }
    };
    Limited l;
    std::vector<float> out(1000, 1.0f);  // pre-fill to detect writes
    const std::size_t produced = w.convert(out.data(), out.size(),
                                           &Limited::pull, &l);
    REQUIRE(produced <= 100u);
    REQUIRE(produced > 0u);
}

TEST_CASE("AudioConverterWrapper reset clears internal state",
          "[audio_converter_wrapper]") {
    jbox::rt::AudioConverterWrapper w(48000.0, 48000.0, 1);
    SineSource src{.phase = 0.0, .freq_hz = 440.0, .rate_hz = 48000.0, .channels = 1};
    std::vector<float> out(256, 0.0f);
    w.convert(out.data(), out.size(), &SineSource::pull, &src);
    w.reset();  // must not crash or leak.
    const std::size_t produced = w.convert(out.data(), out.size(),
                                           &SineSource::pull, &src);
    REQUIRE(produced == out.size());
}
