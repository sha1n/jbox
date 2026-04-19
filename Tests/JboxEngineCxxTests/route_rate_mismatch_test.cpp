// Integration test: a route with source 48k -> destination 44.1k
// produces a non-empty, bounded-amplitude output. Full spectral fidelity
// is covered by unit tests on AudioConverterWrapper; this test proves
// the route carries mismatched rates end-to-end.

#include "engine.hpp"
#include "simulated_backend.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::Engine;
using jbox::control::RouteManager;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeDevice(const char* uid, double rate,
                             std::uint32_t in_ch, std::uint32_t out_ch) {
    BackendDeviceInfo d{};
    d.uid  = uid;
    d.name = uid;
    d.direction =
        (in_ch > 0 ? jbox::control::kBackendDirectionInput  : 0u) |
        (out_ch > 0 ? jbox::control::kBackendDirectionOutput : 0u);
    d.input_channel_count  = in_ch;
    d.output_channel_count = out_ch;
    d.nominal_sample_rate  = rate;
    d.buffer_frame_size    = 256;
    return d;
}

}  // namespace

TEST_CASE("Route carries 48k -> 44.1k end-to-end through AudioConverter",
          "[route_rate_mismatch]") {
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend& backend = *backend_ptr;
    backend.addDevice(makeDevice("src48", 48000.0, 2, 0));
    backend.addDevice(makeDevice("dst44", 44100.0, 0, 2));

    Engine engine(std::move(backend_ptr), /*spawn_sampler_thread=*/false);
    engine.enumerateDevices();

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "src48";
    cfg.dest_uid   = "dst44";
    cfg.mapping    = {{0, 0}, {1, 1}};
    cfg.name       = "test-mismatch";

    jbox_error_t err{};
    const auto id = engine.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(engine.startRoute(id) == JBOX_OK);

    // Drive buffers. Deliver 48k input in 280-frame chunks; pull 44.1k
    // output in 256-frame chunks.
    // src:dst ratio = 48000:44100 ≈ 1.088. Over-feed the source slightly
    // so the converter never runs dry late in the run; the ring absorbs
    // the excess.
    constexpr std::uint32_t src_chunk = 280;
    constexpr std::uint32_t dst_chunk = 256;
    constexpr std::size_t   chunks    = 100;  // ~0.58 s at 44.1k

    std::vector<float> src_buf(src_chunk * 2, 0.0f);
    std::vector<float> dst_buf(dst_chunk * 2, 0.0f);

    // Full-scale sine on both channels.
    const double freq   = 440.0;
    const double phase_step = 2.0 * std::numbers::pi * freq / 48000.0;
    double phase = 0.0;

    double peak_abs = 0.0;
    std::size_t nonzero_frames = 0;

    for (std::size_t k = 0; k < chunks; ++k) {
        for (std::uint32_t f = 0; f < src_chunk; ++f) {
            const float v = static_cast<float>(std::sin(phase));
            src_buf[f * 2 + 0] = v;
            src_buf[f * 2 + 1] = v;
            phase += phase_step;
        }
        backend.deliverBuffer("src48", src_chunk, src_buf.data(), nullptr);
        std::memset(dst_buf.data(), 0, dst_buf.size() * sizeof(float));
        backend.deliverBuffer("dst44", dst_chunk, nullptr, dst_buf.data());
        for (std::uint32_t f = 0; f < dst_chunk; ++f) {
            const double s = std::max(std::fabs(dst_buf[f * 2 + 0]),
                                      std::fabs(dst_buf[f * 2 + 1]));
            if (s > 1e-4) ++nonzero_frames;
            peak_abs = std::max(peak_abs, s);
        }
    }

    // After priming, nearly every output frame carries signal. A small
    // allowance covers the AudioConverter priming transient (at the
    // Mastering SRC complexity, a few hundred frames at start).
    REQUIRE(nonzero_frames > (chunks * dst_chunk) * 9 / 10);
    REQUIRE(peak_abs <= 1.05);  // no clipping beyond headroom
    REQUIRE(peak_abs >= 0.5);   // meaningful amplitude
    REQUIRE(engine.stopRoute(id) == JBOX_OK);
}
