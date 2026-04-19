// drift_tuning_harness.cpp — gated out of normal test runs via the
// [.tuning] tag. Run explicitly with:
//   swift run JboxEngineCxxTests [tuning]
//
// Sweeps (Kp, Ki) across a grid, runs each convergence scenario, and
// prints a table of convergence time + steady-state error. Not an
// automated gate — the final gains chosen are committed by hand into
// control/drift_tracker.cpp (see Task 8 steps 2-4).
//
// CAVEAT: under the current test shape (SimulatedBackend + real Apple
// AudioConverter), the ring fill is dominated by the converter's
// internal buffer flush that fires on every setInputRate() call, not
// by the PI controller's corrective math. Harness output will be
// largely flat across the grid until tests are rewritten to either
// stub the converter or run against real hardware (Phase 4 soak).

#include "drift_sampler.hpp"
#include "drift_tracker.hpp"
#include "engine.hpp"
#include "simulated_backend.hpp"

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

using namespace jbox::control;

namespace {

struct GridPoint {
    double kp;
    double ki;
    double convergence_time_s;  // first t with |fill-target| < 256 frames (-1 if never)
    double steady_state_max_err;
};

GridPoint run(double kp, double ki, double drift_ppm) {
    auto backend_ptr = std::make_unique<SimulatedBackend>();
    SimulatedBackend& backend = *backend_ptr;

    BackendDeviceInfo src{};
    src.uid = "src"; src.name = "src";
    src.direction = kBackendDirectionInput;
    src.input_channel_count = 2; src.output_channel_count = 0;
    src.nominal_sample_rate = 48000.0; src.buffer_frame_size = 256;
    backend.addDevice(src);
    BackendDeviceInfo dst{};
    dst.uid = "dst"; dst.name = "dst";
    dst.direction = kBackendDirectionOutput;
    dst.input_channel_count = 0; dst.output_channel_count = 2;
    dst.nominal_sample_rate = 48000.0; dst.buffer_frame_size = 256;
    backend.addDevice(dst);

    Engine engine(std::move(backend_ptr), /*spawn_sampler_thread=*/false);
    engine.enumerateDevices();

    RouteManager::RouteConfig cfg;
    cfg.source_uid = "src"; cfg.dest_uid = "dst";
    cfg.mapping = {{0, 0}, {1, 1}};
    jbox_error_t err{};
    const auto id = engine.addRoute(cfg, &err);
    engine.startRoute(id);

    // Replace the tracker's gains on this route with the grid point.
    auto running = engine.routeManager().runningRoutes();
    REQUIRE(running.size() == 1u);
    running[0]->tracker = DriftTracker(kp, ki, 100.0);

    constexpr double dt = 0.010;
    constexpr std::uint32_t chunk = 256;
    const double eff_src = 48000.0 * (1.0 + drift_ppm * 1e-6);
    double p_acc = 0.0, c_acc = 0.0;
    std::vector<float> sbuf(chunk * 2, 0.0f);
    std::vector<float> dbuf(chunk * 2, 0.0f);
    const double target_fill =
        static_cast<double>(running[0]->ring->usableCapacityFrames()) * 0.5;

    GridPoint out{kp, ki, -1.0, 0.0};
    constexpr std::size_t ticks = 30000;  // 300 s
    for (std::size_t i = 0; i < ticks; ++i) {
        p_acc += eff_src * dt;
        c_acc += 48000.0 * dt;
        while (p_acc >= chunk) {
            backend.deliverBuffer("src", chunk, sbuf.data(), nullptr); p_acc -= chunk;
        }
        while (c_acc >= chunk) {
            std::memset(dbuf.data(), 0, dbuf.size() * sizeof(float));
            backend.deliverBuffer("dst", chunk, nullptr, dbuf.data()); c_acc -= chunk;
        }
        engine.driftSampler().tickAll(dt);
        const double fill = static_cast<double>(running[0]->ring->framesAvailableForRead());
        const double abs_err = std::fabs(fill - target_fill);
        const double elapsed = static_cast<double>(i) * dt;
        if (out.convergence_time_s < 0.0 && abs_err < 256.0) out.convergence_time_s = elapsed;
        if (elapsed > 30.0) out.steady_state_max_err = std::max(out.steady_state_max_err, abs_err);
    }
    return out;
}

}  // namespace

TEST_CASE("Drift tuning harness sweeps (Kp, Ki)",
          "[.tuning]") {  // leading dot = skipped by default
    const std::vector<double> kps = {1e-7, 3e-7, 1e-6, 3e-6, 1e-5};
    const std::vector<double> kis = {1e-10, 1e-9, 1e-8, 1e-7};
    std::puts("\nKp          Ki          scenario     converge_s  ss_max_err");
    for (double kp : kps) {
        for (double ki : kis) {
            const auto r_up = run(kp, ki, +50.0);
            const auto r_dn = run(kp, ki, -50.0);
            std::printf("%-11.2e %-11.2e +50ppm       %10.3f  %8.1f\n",
                        kp, ki, r_up.convergence_time_s, r_up.steady_state_max_err);
            std::printf("%-11.2e %-11.2e -50ppm       %10.3f  %8.1f\n",
                        kp, ki, r_dn.convergence_time_s, r_dn.steady_state_max_err);
        }
    }
}
