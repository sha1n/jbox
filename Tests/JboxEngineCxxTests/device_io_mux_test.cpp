// device_io_mux_test.cpp — unit tests for DeviceIOMux.
//
// Exercises the RCU-style attach / detach behaviour against
// SimulatedBackend: multiple routes attached to the same device all
// observe each input buffer, detach removes a route cleanly from the
// dispatch list, and the destructor closes the backend handles.

#include <catch_amalgamated.hpp>

#include "device_backend.hpp"
#include "device_io_mux.hpp"
#include "simulated_backend.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::DeviceIOMux;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionOutput;
using jbox::control::SimulatedBackend;

namespace {

struct InputSink {
    std::atomic<std::uint32_t> calls{0};
    std::atomic<std::uint32_t> frames{0};
    float last_sample = 0.0f;
};

void inputSinkCallback(const float* samples,
                       std::uint32_t frame_count,
                       std::uint32_t /*channel_count*/,
                       void* user) {
    auto* s = static_cast<InputSink*>(user);
    s->calls.fetch_add(1, std::memory_order_relaxed);
    s->frames.fetch_add(frame_count, std::memory_order_relaxed);
    if (frame_count > 0) s->last_sample = samples[0];
}

struct OutputSource {
    std::atomic<std::uint32_t> calls{0};
    float value = 0.0f;
};

void outputSourceCallback(float* samples,
                          std::uint32_t frame_count,
                          std::uint32_t channel_count,
                          void* user) {
    auto* s = static_cast<OutputSource*>(user);
    s->calls.fetch_add(1, std::memory_order_relaxed);
    const std::size_t total =
        static_cast<std::size_t>(frame_count) * channel_count;
    for (std::size_t i = 0; i < total; ++i) samples[i] += s->value;
}

BackendDeviceInfo makeInput(const std::string& uid, std::uint32_t ch) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionInput;
    info.input_channel_count = ch;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    return info;
}

BackendDeviceInfo makeOutput(const std::string& uid, std::uint32_t ch) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = uid;
    info.direction = kBackendDirectionOutput;
    info.output_channel_count = ch;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    return info;
}

}  // namespace

TEST_CASE("DeviceIOMux: single input attach / detach", "[device_io_mux]") {
    SimulatedBackend backend;
    backend.addDevice(makeInput("src", 2));

    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);

    InputSink sink;
    REQUIRE(mux.attachInput(&sink, &inputSinkCallback, &sink));
    REQUIRE(mux.hasAnyInput());

    std::vector<float> buf(32 * 2);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = 0.5f;
    backend.deliverBuffer("src", 32, buf.data(), nullptr);

    REQUIRE(sink.calls.load() == 1);
    REQUIRE(sink.frames.load() == 32);
    REQUIRE(sink.last_sample == 0.5f);

    mux.detachInput(&sink);
    REQUIRE_FALSE(mux.hasAnyInput());

    // Further deliveries should not invoke the callback.
    backend.deliverBuffer("src", 32, buf.data(), nullptr);
    REQUIRE(sink.calls.load() == 1);
}

TEST_CASE("DeviceIOMux: two input callbacks share one IOProc",
          "[device_io_mux]") {
    SimulatedBackend backend;
    backend.addDevice(makeInput("src", 2));

    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);

    InputSink a, b;
    REQUIRE(mux.attachInput(&a, &inputSinkCallback, &a));
    REQUIRE(mux.attachInput(&b, &inputSinkCallback, &b));

    std::vector<float> buf(16 * 2, 1.0f);
    backend.deliverBuffer("src", 16, buf.data(), nullptr);

    REQUIRE(a.calls.load() == 1);
    REQUIRE(b.calls.load() == 1);
    REQUIRE(a.frames.load() == 16);
    REQUIRE(b.frames.load() == 16);

    // Detach only `a`; `b` should keep receiving.
    mux.detachInput(&a);
    backend.deliverBuffer("src", 16, buf.data(), nullptr);
    REQUIRE(a.calls.load() == 1);
    REQUIRE(b.calls.load() == 2);

    mux.detachInput(&b);
    REQUIRE_FALSE(mux.hasAnyInput());
}

TEST_CASE("DeviceIOMux: duplicate key is rejected", "[device_io_mux]") {
    SimulatedBackend backend;
    backend.addDevice(makeInput("src", 2));

    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);
    InputSink s;
    REQUIRE(mux.attachInput(&s, &inputSinkCallback, &s));
    REQUIRE_FALSE(mux.attachInput(&s, &inputSinkCallback, &s));
}

TEST_CASE("DeviceIOMux: attach is refused when the device lacks the direction",
          "[device_io_mux]") {
    SimulatedBackend backend;
    backend.addDevice(makeInput("src", 2));
    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);

    OutputSource src;
    REQUIRE_FALSE(mux.attachOutput(&src, &outputSourceCallback, &src));
    REQUIRE_FALSE(mux.hasAnyOutput());
}

TEST_CASE("DeviceIOMux: multiple output callbacks compose additively",
          "[device_io_mux]") {
    SimulatedBackend backend;
    backend.addDevice(makeOutput("dst", 2));
    DeviceIOMux mux(backend, "dst", /*in*/ 0, /*out*/ 2);

    // v1 channel-mapping rules disallow two routes targeting the same
    // destination channel, but the trampoline itself does not enforce
    // that — verify the callbacks compose in the order they were
    // attached.
    OutputSource a{.calls = {}, .value = 0.25f};
    OutputSource b{.calls = {}, .value = 0.75f};

    REQUIRE(mux.attachOutput(&a, &outputSourceCallback, &a));
    REQUIRE(mux.attachOutput(&b, &outputSourceCallback, &b));

    std::vector<float> out(8 * 2, 0.0f);
    backend.deliverBuffer("dst", 8, nullptr, out.data());
    for (float v : out) REQUIRE(v == Catch::Approx(1.0f));
}

TEST_CASE("DeviceIOMux: destructor releases IOProcs and stops the device",
          "[device_io_mux]") {
    SimulatedBackend backend;
    backend.addDevice(makeInput("src", 2));

    InputSink sink;
    {
        DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);
        REQUIRE(mux.attachInput(&sink, &inputSinkCallback, &sink));

        std::vector<float> buf(8 * 2, 0.125f);
        backend.deliverBuffer("src", 8, buf.data(), nullptr);
        REQUIRE(sink.calls.load() == 1);
    }

    // After the mux is destroyed, delivering a buffer should be a
    // no-op (simulated backend requires a live callback registration).
    std::vector<float> buf(8 * 2, 0.0f);
    backend.deliverBuffer("src", 8, buf.data(), nullptr);
    REQUIRE(sink.calls.load() == 1);
}

TEST_CASE("DeviceIOMux: input and output coexist on the same device",
          "[device_io_mux]") {
    SimulatedBackend backend;
    BackendDeviceInfo info;
    info.uid = "duplex";
    info.name = "duplex";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 64;
    backend.addDevice(info);

    DeviceIOMux mux(backend, "duplex", /*in*/ 2, /*out*/ 2);

    InputSink in;
    OutputSource out{.calls = {}, .value = 0.5f};
    REQUIRE(mux.attachInput(&in, &inputSinkCallback, &in));
    REQUIRE(mux.attachOutput(&out, &outputSourceCallback, &out));

    std::vector<float> in_buf(4 * 2, 0.25f);
    std::vector<float> out_buf(4 * 2, 0.0f);
    backend.deliverBuffer("duplex", 4, in_buf.data(), out_buf.data());

    REQUIRE(in.calls.load() == 1);
    REQUIRE(out.calls.load() == 1);
    for (float v : out_buf) REQUIRE(v == Catch::Approx(0.5f));
}

// Phase 6 refinement #6: DeviceIOMux asks the backend to lower the
// device's buffer frame size when any attached route supplies a
// non-zero `requested_buffer_frames`, and restores the original when
// the last requester detaches. Routes that pass 0 don't participate,
// so a mixed scenario (one requester + one no-opinion) keeps the
// buffer small for as long as the requester is attached. The
// restore happens inside `releaseExclusive` from the backend's
// snapshot, not via a second `requestBufferFrameSize` call.
TEST_CASE("DeviceIOMux: buffer request shrinks + refcount restores",
          "[device_io_mux][low_latency]") {
    SimulatedBackend backend;
    BackendDeviceInfo info = makeInput("src", 2);
    info.buffer_frame_size = 1024;
    backend.addDevice(info);

    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);

    InputSink safe_sink;
    InputSink low_sink;

    // Attach a no-opinion route first: no buffer-size request, no
    // exclusive claim.
    REQUIRE(mux.attachInput(&safe_sink, &inputSinkCallback, &safe_sink,
                            /*requested_buffer_frames*/ 0));
    REQUIRE(backend.bufferSizeRequests().empty());
    REQUIRE_FALSE(backend.isExclusive("src"));
    REQUIRE(backend.currentBufferFrameSize("src") == 1024);

    // Attach a requester: 0→1 transition claims exclusive + shrinks.
    REQUIRE(mux.attachInput(&low_sink, &inputSinkCallback, &low_sink,
                            /*requested_buffer_frames*/ 64));
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE(backend.bufferSizeRequests().back().uid       == "src");
    REQUIRE(backend.bufferSizeRequests().back().requested == 64);
    REQUIRE(backend.isExclusive("src"));
    REQUIRE(backend.currentBufferFrameSize("src") == 64);

    // Detach the no-opinion route: nothing changes.
    mux.detachInput(&safe_sink);
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE(backend.isExclusive("src"));
    REQUIRE(backend.currentBufferFrameSize("src") == 64);

    // Detach the requester: 1→0 transition releases exclusive, which
    // internally restores the original buffer size from the
    // snapshot. The restore does NOT flow through requestBufferFrame-
    // Size, so the request log still has just the one shrink entry.
    mux.detachInput(&low_sink);
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE_FALSE(backend.isExclusive("src"));
    REQUIRE(backend.currentBufferFrameSize("src") == 1024);
}

TEST_CASE("DeviceIOMux: two requesters take the smallest frame count",
          "[device_io_mux][low_latency]") {
    SimulatedBackend backend;
    BackendDeviceInfo info = makeInput("src", 2);
    info.buffer_frame_size = 512;
    backend.addDevice(info);

    DeviceIOMux mux(backend, "src", /*in*/ 2, /*out*/ 0);

    InputSink a;
    InputSink b;
    // First requester asks for 128.
    REQUIRE(mux.attachInput(&a, &inputSinkCallback, &a,
                            /*requested_buffer_frames*/ 128));
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE(backend.bufferSizeRequests().back().requested == 128);
    REQUIRE(backend.currentBufferFrameSize("src") == 128);

    // Second requester asks for 64 — smaller wins, triggers a new
    // request.
    REQUIRE(mux.attachInput(&b, &inputSinkCallback, &b,
                            /*requested_buffer_frames*/ 64));
    REQUIRE(backend.bufferSizeRequests().size() == 2);
    REQUIRE(backend.bufferSizeRequests().back().requested == 64);
    REQUIRE(backend.currentBufferFrameSize("src") == 64);

    // Detaching the 64-frame requester moves the min back to 128.
    mux.detachInput(&b);
    REQUIRE(backend.bufferSizeRequests().size() == 3);
    REQUIRE(backend.bufferSizeRequests().back().requested == 128);
    REQUIRE(backend.currentBufferFrameSize("src") == 128);

    // Last requester detaches — restore via releaseExclusive, no new
    // request entry.
    mux.detachInput(&a);
    REQUIRE(backend.bufferSizeRequests().size() == 3);
    REQUIRE(backend.currentBufferFrameSize("src") == 512);
}

TEST_CASE("DeviceIOMux: request on input + output of same device shares one shrink",
          "[device_io_mux][low_latency]") {
    // Same device acts as both source and destination (duplex
    // loopback). The route_manager attaches requests on both
    // directions of the same mux; the refcount must span both sides.
    SimulatedBackend backend;
    BackendDeviceInfo info;
    info.uid = "duplex";
    info.name = "duplex";
    info.direction = kBackendDirectionInput | kBackendDirectionOutput;
    info.input_channel_count = 2;
    info.output_channel_count = 2;
    info.nominal_sample_rate = 48000.0;
    info.buffer_frame_size = 1024;
    backend.addDevice(info);

    DeviceIOMux mux(backend, "duplex", /*in*/ 2, /*out*/ 2);

    InputSink in;
    OutputSource out{.calls = {}, .value = 0.25f};

    REQUIRE(mux.attachInput(&in, &inputSinkCallback, &in,
                            /*requested_buffer_frames*/ 64));
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE(backend.currentBufferFrameSize("duplex") == 64);

    // Same device, same target: the min hasn't changed, no second
    // request fires.
    REQUIRE(mux.attachOutput(&out, &outputSourceCallback, &out,
                             /*requested_buffer_frames*/ 64));
    REQUIRE(backend.bufferSizeRequests().size() == 1);

    mux.detachInput(&in);
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE(backend.currentBufferFrameSize("duplex") == 64);
    mux.detachOutput(&out);
    // Last requester detaches — restore via releaseExclusive.
    REQUIRE(backend.bufferSizeRequests().size() == 1);
    REQUIRE(backend.currentBufferFrameSize("duplex") == 1024);
}
