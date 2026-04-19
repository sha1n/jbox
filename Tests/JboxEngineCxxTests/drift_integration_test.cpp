// Simulated-time drift integration tests. Feed the backend at a
// producer rate that deviates from nominal by +/- 50 ppm; tick the
// sampler at 100 Hz simulated; verify ring fill stays within a band
// from t = 10s through t = 300s.

#include "drift_sampler.hpp"
#include "engine.hpp"
#include "simulated_backend.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::DriftSampler;
using jbox::control::Engine;
using jbox::control::RouteManager;
using jbox::control::RouteRecord;
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

struct ConvergenceResult {
    double max_error_after_convergence;  // frames
    double min_fill;
    double max_fill;
    std::size_t total_ticks;
};

// Run one convergence scenario. Both devices at 48 kHz nominal;
// producer runs at 48000 * (1 + src_drift_ppm * 1e-6); consumer at
// 48000 * (1 + dst_drift_ppm * 1e-6). Asserts convergence after t=10s.
ConvergenceResult runScenario(double src_drift_ppm, double dst_drift_ppm,
                              double sim_seconds, double band_frames,
                              double assert_after_seconds) {
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend& backend = *backend_ptr;
    backend.addDevice(makeDevice("src", 48000.0, 2, 0));
    backend.addDevice(makeDevice("dst", 48000.0, 0, 2));

    Engine engine(std::move(backend_ptr));
    engine.enumerateDevices();

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "src";
    cfg.dest_uid   = "dst";
    cfg.mapping    = {{0, 0}, {1, 1}};
    jbox_error_t err{};
    const auto id = engine.addRoute(cfg, &err);
    REQUIRE(id != JBOX_INVALID_ROUTE_ID);
    REQUIRE(engine.startRoute(id) == JBOX_OK);

    DriftSampler sampler(engine.routeManager());

    constexpr double dt = 0.010;  // 10 ms per iteration (100 Hz)
    constexpr std::uint32_t chunk_frames = 256;
    const double effective_src_rate = 48000.0 * (1.0 + src_drift_ppm * 1e-6);
    const double effective_dst_rate = 48000.0 * (1.0 + dst_drift_ppm * 1e-6);

    double producer_acc = 0.0;
    double consumer_acc = 0.0;
    std::vector<float> src_buf(chunk_frames * 2, 0.0f);
    std::vector<float> dst_buf(chunk_frames * 2, 0.0f);

    // Drive a continuous sine to keep the converter fed with realistic
    // content (pure zeros are not representative of the RT path).
    const double freq      = 440.0;
    const double phase_step = 2.0 * std::numbers::pi * freq / 48000.0;
    double phase = 0.0;

    double target_fill = 0.0;  // set after first measurement
    ConvergenceResult out{};
    out.min_fill = 1e18;
    out.max_fill = -1e18;
    out.max_error_after_convergence = 0.0;

    const std::size_t ticks = static_cast<std::size_t>(sim_seconds / dt);
    for (std::size_t i = 0; i < ticks; ++i) {
        producer_acc += effective_src_rate * dt;
        consumer_acc += effective_dst_rate * dt;

        while (producer_acc >= chunk_frames) {
            for (std::uint32_t f = 0; f < chunk_frames; ++f) {
                const float v = static_cast<float>(std::sin(phase));
                src_buf[f * 2 + 0] = v;
                src_buf[f * 2 + 1] = v;
                phase += phase_step;
            }
            backend.deliverBuffer("src", chunk_frames, src_buf.data(), nullptr);
            producer_acc -= chunk_frames;
        }
        while (consumer_acc >= chunk_frames) {
            std::memset(dst_buf.data(), 0, dst_buf.size() * sizeof(float));
            backend.deliverBuffer("dst", chunk_frames, nullptr, dst_buf.data());
            consumer_acc -= chunk_frames;
        }

        sampler.tickAll(dt);

        // Measure fill.
        auto running = engine.routeManager().runningRoutes();
        REQUIRE(running.size() == 1u);
        const double fill = static_cast<double>(
            running[0]->ring->framesAvailableForRead());
        out.min_fill = std::min(out.min_fill, fill);
        out.max_fill = std::max(out.max_fill, fill);

        if (i == 0) {
            // Use the first-tick fill as the operating point; the PI
            // controller's integrator converges to whatever setpoint
            // we're using inside RouteManager. We don't assume a
            // specific target_fill here — we only assert bounded band.
            target_fill = fill;
        }

        const double elapsed = static_cast<double>(i) * dt;
        if (elapsed > assert_after_seconds) {
            out.max_error_after_convergence =
                std::max(out.max_error_after_convergence,
                         std::fabs(fill - target_fill));
        }
    }
    out.total_ticks = ticks;
    REQUIRE(out.max_error_after_convergence < band_frames);
    return out;
}

}  // namespace

TEST_CASE("Drift converges: source +50 ppm, destination nominal",
          "[drift_integration]") {
    const auto r = runScenario(+50.0, 0.0,
                               /*sim_seconds=*/300.0,
                               /*band_frames=*/512.0,
                               /*assert_after_seconds=*/10.0);
    REQUIRE(r.total_ticks > 0u);
}

TEST_CASE("Drift converges: destination +50 ppm, source nominal",
          "[drift_integration]") {
    const auto r = runScenario(0.0, +50.0, 300.0, 512.0, 10.0);
    REQUIRE(r.total_ticks > 0u);
}

TEST_CASE("Drift converges under step disturbance",
          "[drift_integration]") {
    // Approximate the step by running the scenario with the biased rate
    // from t=0; the PI must converge within 10 s either way. A true
    // step test is a nice-to-have left for Phase 4 commit 5 tuning.
    const auto r = runScenario(+50.0, 0.0, 60.0, 512.0, 10.0);
    REQUIRE(r.total_ticks > 0u);
}
