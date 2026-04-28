// route_manager_gain_test.cpp — Task 5 + Task 6 of the Route Gain +
// Mixer-Strip implementation plan.
//
// Verifies that `outputIOProcCallback` (Task 5, split path) and
// `duplexIOProcCallback` (Task 6, direct-monitor fast path) both
// apply the smoothed master * trim per-channel gain before writing
// destination samples. Behaviours pinned here:
//
//   1. Default (zero-init) RouteConfig still passes audio through
//      unchanged (master = 0 dB unity, all trims = 0 dB unity).
//   2. master_gain_db = -6 dB attenuates the output by ~×0.5012 once
//      the smoother has settled.
//   3. Mute target ramps the output down to perceptual silence
//      (≤ -40 dB) within the 50 ms window the spec promises.
//   4. Per-channel trims affect only the indexed channel — channel 1
//      stays at unity when channel 0 is trimmed -6 dB.
//   5. The smoother is primed at attemptStart so the very first
//      output block already runs at the configured gain (no startup
//      ramp from unity).
//
// Plus two edge cases beyond the plan:
//   6. Source meter stays pre-fader regardless of master gain.
//   7. Dest meter reflects the post-fader signal (because the IOProc
//      tracks peaks after multiplying by master * trim).
//
// Task 6 mirrors the same checks against the duplex fast path
// (single device, latency_mode = 2, source_uid == dest_uid):
//   8. -12 dB master attenuation lands on the very first duplex block
//      (smoother primed; no ring + converter pre-roll on this path).
//   9. Source meter is pre-gain, dest meter is post-gain on duplex.
//   10. Mute settles to ≤ -40 dB within the 50 ms / 5τ horizon on
//       duplex (priming sets `current` to the configured master, NOT
//       to 0 — the muted-from-start case still has a smoother decay,
//       it just runs from primed-master toward 0).
//
// Tagged `[route_gain]` for the whole suite, with `[duplex]` added
// to the Task 6 cases so they can be filtered separately:
//   swift run JboxEngineCxxTests '[route_gain]'
//   swift run JboxEngineCxxTests '[duplex]'

#include <catch_amalgamated.hpp>

#include "device_backend.hpp"
#include "device_manager.hpp"
#include "route_manager.hpp"
#include "simulated_backend.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::ChannelEdge;
using jbox::control::DeviceManager;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::RouteManager;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeInputDevice(const std::string& uid,
                                  std::uint32_t channels,
                                  std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput;
    info.input_channel_count = channels;
    info.output_channel_count = 0;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = buf;
    return info;
}

BackendDeviceInfo makeOutputDevice(const std::string& uid,
                                   std::uint32_t channels,
                                   std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionOutput;
    info.input_channel_count = 0;
    info.output_channel_count = channels;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = buf;
    return info;
}

// Fixture: 2-channel src → 2-channel dst at 48 kHz, 64-frame buffers.
// Mirrors the lightweight pattern used in route_manager_test.cpp /
// multi_route_test.cpp (no DeviceIOMux trickery — just one route).
struct Fixture {
    SimulatedBackend* backend = nullptr;
    std::unique_ptr<DeviceManager> dm;
    std::unique_ptr<RouteManager>  rm;

    Fixture(std::uint32_t buf_frames = 64) {
        auto b = std::make_unique<SimulatedBackend>();
        backend = b.get();
        b->addDevice(makeInputDevice("src", 2, buf_frames));
        b->addDevice(makeOutputDevice("dst", 2, buf_frames));
        dm = std::make_unique<DeviceManager>(std::move(b));
        dm->refresh();
        rm = std::make_unique<RouteManager>(*dm);
    }
};

// Convert dB to linear amplitude — same formula as RouteManager's
// internal helper (keeps test expectations honest by mirroring the
// production conversion). Tests only call this with finite dB values
// in the [-12, 0] range so the corner cases handled in addRoute (NaN
// / -inf collapse, +12 dB cap) are out of scope here.
inline float dbToAmp(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// One block size = 64 frames @ 48 kHz ≈ 1.33 ms. With a 10 ms time
// constant the smoother is at ~95 % settling after roughly 3 *
// (10 / 1.33) ≈ 22.5 blocks. We use 64 warm-up blocks to be well
// past 99 % settling on the -6 dB / -12 dB / trim tests where the
// smoother starts at unity (the default RouteConfig zero-inits to
// 0 dB master + 0 dB trims, so master_smoother / trim_smoothers are
// primed to 1.0). For the explicitly-primed test (#5) we drive a
// single block instead.
constexpr std::uint32_t kFrames           = 64;
constexpr std::uint32_t kSettleBlocks     = 64;
constexpr std::uint32_t kConverterPreroll = 4;  // a couple of blocks
                                                 // to flush polyphase

// Drive `n` paired input + output blocks through the route at value
// `level` on both source channels, capturing the very last output
// block in `out_block`.
void driveBlocks(Fixture& f,
                 std::uint32_t n,
                 float level,
                 std::vector<float>& out_block) {
    std::vector<float> in(kFrames * 2, level);
    out_block.assign(kFrames * 2, 0.0f);
    for (std::uint32_t b = 0; b < n; ++b) {
        f.backend->deliverBuffer("src", kFrames, in.data(), nullptr);
        f.backend->deliverBuffer("dst", kFrames, nullptr, out_block.data());
    }
}

}  // namespace

TEST_CASE("route gain: unity master + unity trims pass audio unchanged",
          "[route_gain]") {
    // Default RouteConfig zero-inits master_gain_db = 0 (unity) and
    // channel_trims_db empty (defaults to 0 dB per channel), so the
    // smoother is primed at 1.0 and the IOProc multiplies each sample
    // by 1.0 * 1.0. Output should match input modulo polyphase
    // pre-roll.
    Fixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "unity"};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    std::vector<float> out;
    driveBlocks(f, /*n*/ kSettleBlocks, /*level*/ 0.5f, out);
    for (std::uint32_t fr = 0; fr < kFrames; ++fr) {
        // 1e-4 tolerance — AudioConverterWrapper introduces a small
        // amount of polyphase ringing that we can't fully eliminate.
        REQUIRE(out[fr * 2 + 0] == Catch::Approx(0.5f).margin(1e-4f));
        REQUIRE(out[fr * 2 + 1] == Catch::Approx(0.5f).margin(1e-4f));
    }
}

TEST_CASE("route gain: master gain at -6 dB halves output amplitude",
          "[route_gain]") {
    Fixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "minus-6db"};
    cfg.master_gain_db = -6.0f;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    // Smoother is primed to 10^(-6/20) ≈ 0.5012 at attemptStart, so
    // there is no settling delay needed — but we still drive several
    // blocks to let the converter pre-roll out.
    std::vector<float> out;
    driveBlocks(f, /*n*/ kConverterPreroll * 4, /*level*/ 0.5f, out);

    const float expected = 0.5f * dbToAmp(-6.0f);  // ~0.2506
    for (std::uint32_t fr = 0; fr < kFrames; ++fr) {
        REQUIRE(out[fr * 2 + 0] == Catch::Approx(expected).margin(1e-3f));
        REQUIRE(out[fr * 2 + 1] == Catch::Approx(expected).margin(1e-3f));
    }
}

TEST_CASE("route gain: mute target ramps to silence within ~50 ms",
          "[route_gain]") {
    // muted = true forces the master_target read in outputIOProcCallback
    // to 0; the smoother decays toward 0 with tau = 10 ms. 50 ms = 5τ,
    // which leaves residual e^-5 ≈ 0.0067 of the original amplitude.
    // 0.5 * 0.0067 ≈ 3.4e-3 → comfortably below the 1e-2 threshold
    // ( -40 dB, perceptually inaudible) the spec promises.
    Fixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "muted"};
    cfg.muted = true;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    // 50 ms at 48 kHz is 2400 frames; with 64-frame blocks that's
    // ~38 paired input/output deliveries. Round up to 40 to stay
    // safely past the 5τ horizon.
    constexpr std::uint32_t kBlocksFor50ms = 40;
    std::vector<float> out;
    driveBlocks(f, kBlocksFor50ms, /*level*/ 0.5f, out);

    // After the ramp every sample in the final block should be at or
    // below -40 dB. We assert that the *peak* of the last block
    // satisfies the bound, which is stricter than checking individual
    // samples one by one.
    float peak = 0.0f;
    for (float s : out) {
        const float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    REQUIRE(peak <= 1e-2f);
}

TEST_CASE("route gain: per-channel trim attenuates only the indexed channel",
          "[route_gain]") {
    // Edge case beyond the plan: channel 0 carries a -6 dB trim;
    // channel 1 stays at unity. The output IOProc multiplies each
    // converted sample by master * trim[i], so channel 0 ends up at
    // ~0.2506 while channel 1 stays at 0.5.
    Fixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "trim-ch0"};
    cfg.channel_trims_db = {-6.0f, 0.0f};
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    // Trim smoothers are primed at attemptStart, so a few blocks of
    // converter pre-roll is enough.
    std::vector<float> out;
    driveBlocks(f, /*n*/ kConverterPreroll * 4, /*level*/ 0.5f, out);

    const float expected_ch0 = 0.5f * dbToAmp(-6.0f);  // ~0.2506
    const float expected_ch1 = 0.5f;
    for (std::uint32_t fr = 0; fr < kFrames; ++fr) {
        REQUIRE(out[fr * 2 + 0] ==
                Catch::Approx(expected_ch0).margin(1e-3f));
        REQUIRE(out[fr * 2 + 1] ==
                Catch::Approx(expected_ch1).margin(1e-3f));
    }
}

TEST_CASE("route gain: smoother runs from primed value, no startup ramp",
          "[route_gain]") {
    // Edge case beyond the plan: with master = -12 dB (~×0.2512), the
    // *first* output block must already reflect the full attenuation
    // — no audible "ramp-up from unity" at route start. attemptStart
    // primes both master_smoother.current and trim_smoothers[i].current
    // to their respective targets so the very first IOProc tick
    // multiplies samples by the configured gain.
    Fixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "primed"};
    cfg.master_gain_db = -12.0f;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    // We still need a couple of blocks of converter pre-roll before
    // the polyphase ringing dies out — but no smoother settling time
    // because the smoother starts AT the target, not below it.
    // Without priming, the first block would be near 0.5 (smoother
    // current = 1.0, target ≈ 0.2512; per-block recurrence gives
    // ~0.5 * (pole * 1 + (1 - pole) * 0.2512) ≈ 0.5 * 0.97 ≈ 0.485).
    std::vector<float> out;
    driveBlocks(f, /*n*/ kConverterPreroll, /*level*/ 0.5f, out);

    const float expected = 0.5f * dbToAmp(-12.0f);  // ~0.1256
    for (std::uint32_t fr = 0; fr < kFrames; ++fr) {
        REQUIRE(out[fr * 2 + 0] == Catch::Approx(expected).margin(1e-3f));
        REQUIRE(out[fr * 2 + 1] == Catch::Approx(expected).margin(1e-3f));
    }
}

TEST_CASE("route gain: source meter is pre-fader, dest meter is post-fader",
          "[route_gain]") {
    // Edge case beyond the plan but worth pinning: the input IOProc
    // tracks peaks BEFORE writing into the ring (pre-fader); the
    // output IOProc tracks peaks AFTER multiplying by master * trim
    // (post-fader). With master = -12 dB (~×0.2512), source meter
    // should still report ~0.5 and dest meter ~0.1256.
    Fixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{"src", "dst", m, "meter-pre-post"};
    cfg.master_gain_db = -12.0f;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    std::vector<float> out;
    driveBlocks(f, /*n*/ kConverterPreroll * 4, /*level*/ 0.5f, out);

    float src_peaks[2] = {};
    float dst_peaks[2] = {};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, src_peaks, 2) == 2);
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_DEST,   dst_peaks, 2) == 2);

    // Source meter sees the raw 0.5 input.
    REQUIRE(src_peaks[0] == Catch::Approx(0.5f).margin(1e-4f));
    REQUIRE(src_peaks[1] == Catch::Approx(0.5f).margin(1e-4f));

    // Dest meter sees the post-fader value.
    const float expected = 0.5f * dbToAmp(-12.0f);  // ~0.1256
    REQUIRE(dst_peaks[0] == Catch::Approx(expected).margin(1e-3f));
    REQUIRE(dst_peaks[1] == Catch::Approx(expected).margin(1e-3f));
}

// -----------------------------------------------------------------------------
// Task 6: duplex (direct-monitor) fast path
//
// When source_uid == dest_uid AND latency_mode = 2 (Performance), the
// engine bypasses the ring + AudioConverter and copies input straight
// to output in a single duplex IOProc. The gain multiply must happen
// in that path too, with the same priming + smoothing dynamics.
// -----------------------------------------------------------------------------

namespace {

// Duplex fixture: a single device with both input AND output channels,
// driven via a single deliverBuffer call carrying both pointers (the
// SimulatedBackend dispatches that to the duplex callback — see
// simulated_backend.cpp deliverBuffer body and `RouteManager: duplex
// fast path routes same-device Performance directly` for the pattern).
struct DuplexFixture {
    SimulatedBackend* backend = nullptr;
    std::unique_ptr<DeviceManager> dm;
    std::unique_ptr<RouteManager>  rm;

    DuplexFixture(std::uint32_t buf_frames = 64) {
        auto b = std::make_unique<SimulatedBackend>();
        backend = b.get();
        BackendDeviceInfo info;
        info.uid = "aggregate";
        info.name = "aggregate";
        info.direction =
            kBackendDirectionInput | kBackendDirectionOutput;
        info.input_channel_count  = 2;
        info.output_channel_count = 2;
        info.nominal_sample_rate  = 48000.0;
        info.buffer_frame_size    = buf_frames;
        b->addDevice(info);
        dm = std::make_unique<DeviceManager>(std::move(b));
        dm->refresh();
        rm = std::make_unique<RouteManager>(*dm);
    }
};

// Single duplex tick. The simulated backend dispatches to the duplex
// callback when both pointers are non-null; one call per IOProc tick
// (no separate input + output deliveries on the duplex path).
void driveDuplexBlock(DuplexFixture& f, float level,
                      std::vector<float>& out_block) {
    std::vector<float> in(kFrames * 2, level);
    out_block.assign(kFrames * 2, 0.0f);
    f.backend->deliverBuffer("aggregate", kFrames,
                             in.data(), out_block.data());
}

void driveDuplexBlocks(DuplexFixture& f, std::uint32_t n, float level,
                       std::vector<float>& out_block) {
    for (std::uint32_t b = 0; b < n; ++b) {
        driveDuplexBlock(f, level, out_block);
    }
}

}  // namespace

TEST_CASE("route gain (duplex): -12 dB master attenuation lands on first block",
          "[route_gain][duplex]") {
    // Direct-monitor fast path with master = -12 dB. attemptStart
    // primes the smoother to ~0.2512 for both paths, and the duplex
    // path has no AudioConverter polyphase pre-roll, so the very first
    // duplex tick already produces fully-attenuated output. Tolerance
    // 5e-3 because the smoother is exact-on-target at priming time.
    DuplexFixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-minus-12db",
        /*latency_mode*/ 2};
    cfg.master_gain_db = -12.0f;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    std::vector<float> out;
    driveDuplexBlock(f, /*level*/ 1.0f, out);

    const float expected = 1.0f * dbToAmp(-12.0f);  // ~0.2512
    for (std::uint32_t fr = 0; fr < kFrames; ++fr) {
        REQUIRE(out[fr * 2 + 0] == Catch::Approx(expected).margin(5e-3f));
        REQUIRE(out[fr * 2 + 1] == Catch::Approx(expected).margin(5e-3f));
    }
}

TEST_CASE("route gain (duplex): source meter is pre-gain, dest meter is post-gain",
          "[route_gain][duplex]") {
    // The duplex callback updates BOTH meters in its per-channel loop:
    // source_meter sees the raw input sample `s`, dest_meter sees the
    // post-multiply sample `o = s * gain`. With master = -12 dB and a
    // 0.5 input, source should report ~0.5 and dest ~0.1256.
    DuplexFixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-meter-pre-post",
        /*latency_mode*/ 2};
    cfg.master_gain_db = -12.0f;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    std::vector<float> out;
    // A handful of duplex ticks so the meter has a stable peak; the
    // smoother is primed so each tick lands at the target.
    driveDuplexBlocks(f, /*n*/ 4, /*level*/ 0.5f, out);

    float src_peaks[2] = {};
    float dst_peaks[2] = {};
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_SOURCE, src_peaks, 2) == 2);
    REQUIRE(f.rm->pollMeters(id, JBOX_METER_SIDE_DEST,   dst_peaks, 2) == 2);

    REQUIRE(src_peaks[0] == Catch::Approx(0.5f).margin(1e-4f));
    REQUIRE(src_peaks[1] == Catch::Approx(0.5f).margin(1e-4f));

    const float expected = 0.5f * dbToAmp(-12.0f);  // ~0.1256
    REQUIRE(dst_peaks[0] == Catch::Approx(expected).margin(5e-3f));
    REQUIRE(dst_peaks[1] == Catch::Approx(expected).margin(5e-3f));
}

TEST_CASE("route gain (duplex): muted-from-start settles to silence within ~50 ms",
          "[route_gain][duplex]") {
    // Subtle: priming at attemptStart sets master_smoother.current to
    // target_master_gain.load(), NOT to 0 — even when muted = true
    // (the mute override happens inside the IOProc, not at priming).
    // So the very first duplex block of a muted-at-start route is NOT
    // silent; it runs the smoother from primed master (1.0 here) toward
    // 0 with τ = 10 ms. For τ=10 ms at 48 kHz with 64-frame blocks the
    // per-block pole is ≈ 0.876 and the first-block multiplier is
    // ≈ 0.876, i.e. ~88 % of unity — the test asserts the 50 ms /
    // 5τ horizon (residual ≤ -40 dB ≡ 1e-2), NOT first-block silence.
    DuplexFixture f;
    std::vector<ChannelEdge> m{{0, 0}, {1, 1}};
    RouteManager::RouteConfig cfg{
        "aggregate", "aggregate", m, "duplex-muted",
        /*latency_mode*/ 2};
    cfg.muted = true;
    jbox_error_t err{};
    const auto id = f.rm->addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(f.rm->startRoute(id) == JBOX_OK);

    // 50 ms at 48 kHz is 2400 frames; with 64-frame blocks that's
    // ~38 ticks. Round up to 40 to stay safely past the 5τ horizon.
    constexpr std::uint32_t kBlocksFor50ms = 40;
    std::vector<float> out;
    driveDuplexBlocks(f, kBlocksFor50ms, /*level*/ 0.5f, out);

    // After the ramp every sample in the final block should be at or
    // below -40 dB.
    float peak = 0.0f;
    for (float s : out) {
        const float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    REQUIRE(peak <= 1e-2f);
}
