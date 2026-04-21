// Unit tests for AudioConverterWrapper.
//
// Focus: lifecycle, unity-ratio fidelity, setInputRate, partial-input
// frame accounting. SNR-style fidelity checks use a windowed sine.

#include "audio_converter_wrapper.hpp"

#include "rate_deadband.hpp"

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

// Hypothesis test: setInputRate fired on every convert() with distinct
// (PI-noise-scale) rate deltas forces Apple's AudioConverter to re-prime
// its polyphase filter state, which we hypothesize is the real-hardware
// cause of the audible clicks and the slow-growing underrun counter on
// the V31 → Apollo route (see docs/spec.md § 2.6 drift notes and
// drift_tracker.cpp header). The observable signature is extra input
// consumption per unit of output, because the converter keeps re-filling
// its filter state after each flush.
//
// This test drives the REAL Apple AudioConverter — not a mock — with a
// continuous sine, under two scenarios:
//   A) baseline         : setInputRate called once at construction.
//   B) per-call flush   : setInputRate called before every convert() at
//                         a distinct rate differing from the previous
//                         by ~1e-4 ppm (the regime the legacy naive-!=
//                         comparison admitted).
// If the hypothesis holds, scenario B consumes materially more input
// frames than scenario A for the same total output. If it does not,
// the deadband fix is unjustified and we must look elsewhere.
TEST_CASE("AudioConverterWrapper: per-call setInputRate at PI-noise ppm "
          "forces extra input consumption",
          "[audio_converter_wrapper][hypothesis]") {
    constexpr double      kRate       = 48000.0;
    constexpr std::size_t kBlockFrames = 256;
    constexpr int         kBlocks      = 400;  // ~2.1 s of audio

    struct CountingSine {
        double       phase   = 0.0;
        double       freq_hz = 997.0;
        double       rate_hz = kRate;
        std::size_t  pulled  = 0;
        static std::size_t pull(float* dst, std::size_t frames, void* user) {
            auto* s = static_cast<CountingSine*>(user);
            const double step = 2.0 * std::numbers::pi * s->freq_hz / s->rate_hz;
            for (std::size_t f = 0; f < frames; ++f) {
                dst[f] = static_cast<float>(std::sin(s->phase));
                s->phase += step;
            }
            s->pulled += frames;
            return frames;
        }
    };

    auto runScenario = [&](bool flush_every_call) -> std::pair<std::size_t, std::size_t> {
        jbox::rt::AudioConverterWrapper w(kRate, kRate, 1);
        CountingSine src;
        std::vector<float> out(kBlockFrames, 0.0f);
        std::size_t total_out = 0;
        for (int i = 0; i < kBlocks; ++i) {
            if (flush_every_call) {
                // 1e-4 ppm per call, alternating sign. Each call differs
                // from the previous one by a float-distinguishable amount
                // but by less than the 1 ppm deadband threshold, matching
                // the real-hardware PI-noise regime.
                const double ppm = (i % 2 == 0 ? 1.0 : -1.0) * 1e-4;
                w.setInputRate(kRate * (1.0 + ppm * 1e-6));
            }
            total_out += w.convert(out.data(), out.size(),
                                   &CountingSine::pull, &src);
        }
        return {src.pulled, total_out};
    };

    const auto [baseline_in, baseline_out] = runScenario(false);
    const auto [flushed_in,  flushed_out]  = runScenario(true);

    INFO("baseline pulled="  << baseline_in << " emitted=" << baseline_out);
    INFO("flushed  pulled="  << flushed_in  << " emitted=" << flushed_out);

    // Both scenarios should emit the same amount of output.
    REQUIRE(baseline_out == flushed_out);
    REQUIRE(baseline_out == static_cast<std::size_t>(kBlocks * kBlockFrames));

    // Core hypothesis: the flushed scenario consumes extra input. Margin
    // is generous — if Apple's converter primes only a handful of extra
    // frames per flush this still shows up as thousands of extra frames
    // over 400 blocks. A tight margin would be flaky across macOS
    // releases; we just need non-noise evidence that per-call flushing
    // is materially worse than steady-state.
    REQUIRE(flushed_in > baseline_in);
    const std::size_t extra = flushed_in - baseline_in;
    INFO("extra input frames caused by per-call flushing: " << extra);
    REQUIRE(extra >= 256u);  // at least one block's worth over 400 blocks
}

// Companion to the hypothesis test: wrap each per-call setInputRate in
// the rate_deadband decision we wired into RouteManager. The sub-ppm
// PI-noise pattern from scenario B now falls below the 1 ppm threshold
// on every tick after the first one, so setInputRate should fire at
// most once — matching the baseline's input consumption rather than the
// flushed scenario's. This is the end-to-end evidence that the fix in
// route_manager.cpp closes the real-world input-consumption gap.
TEST_CASE("AudioConverterWrapper: deadband-gated per-call setInputRate "
          "matches baseline input consumption",
          "[audio_converter_wrapper][hypothesis]") {
    constexpr double      kRate        = 48000.0;
    constexpr std::size_t kBlockFrames = 256;
    constexpr int         kBlocks      = 400;

    struct CountingSine {
        double      phase   = 0.0;
        double      freq_hz = 997.0;
        double      rate_hz = kRate;
        std::size_t pulled  = 0;
        static std::size_t pull(float* dst, std::size_t frames, void* user) {
            auto* s = static_cast<CountingSine*>(user);
            const double step = 2.0 * std::numbers::pi * s->freq_hz / s->rate_hz;
            for (std::size_t f = 0; f < frames; ++f) {
                dst[f] = static_cast<float>(std::sin(s->phase));
                s->phase += step;
            }
            s->pulled += frames;
            return frames;
        }
    };

    // Baseline: no rate updates after construction.
    auto runBaseline = [&]() -> std::size_t {
        jbox::rt::AudioConverterWrapper w(kRate, kRate, 1);
        CountingSine src;
        std::vector<float> out(kBlockFrames, 0.0f);
        for (int i = 0; i < kBlocks; ++i) {
            w.convert(out.data(), out.size(), &CountingSine::pull, &src);
        }
        return src.pulled;
    };

    // Deadband-wrapped: mirrors the exact apply-or-skip logic from
    // route_manager.cpp. Sub-ppm PI-noise proposals must not reach
    // setInputRate. Also exercise one supra-threshold proposal partway
    // through to prove the deadband still lets real drift through.
    auto runDeadbanded = [&]() -> std::pair<std::size_t, int> {
        jbox::rt::AudioConverterWrapper w(kRate, kRate, 1);
        CountingSine src;
        std::vector<float> out(kBlockFrames, 0.0f);
        double last_applied = 0.0;
        int applies = 0;
        for (int i = 0; i < kBlocks; ++i) {
            // Sub-ppm PI-noise (~1e-4 ppm) around a baseline that steps
            // from 0 ppm to +5 ppm at i=200 — matches a real PI
            // controller slowly tracking a new drift. The deadband must
            // apply once at cold-start and once at the step, and gate
            // every noisy tick on either side.
            const double baseline_ppm = (i < 200 ? 0.0 : 5.0);
            const double noise_ppm    = (i % 2 == 0 ? 1e-4 : -1e-4);
            const double proposed     =
                kRate * (1.0 + (baseline_ppm + noise_ppm) * 1e-6);
            if (jbox::control::shouldApplyRate(proposed, last_applied, kRate)) {
                w.setInputRate(proposed);
                last_applied = proposed;
                ++applies;
            }
            w.convert(out.data(), out.size(), &CountingSine::pull, &src);
        }
        return {src.pulled, applies};
    };

    const std::size_t baseline_in               = runBaseline();
    const auto        [deadbanded_in, applies]  = runDeadbanded();

    INFO("baseline pulled="   << baseline_in);
    INFO("deadbanded pulled=" << deadbanded_in << " applies=" << applies);

    // Deadband must have gated almost all proposals: one for the first
    // non-zero target from cold start, one for the +5 ppm step at
    // block 200. Any more means sub-ppm noise is leaking through.
    REQUIRE(applies == 2);

    // Flush-storm signature (~6400 extra frames over 400 blocks) is
    // gone. Allow a tiny margin for the two legitimate applies; those
    // cost ~16 frames each per the hypothesis test.
    REQUIRE(deadbanded_in <= baseline_in + 128u);
}
