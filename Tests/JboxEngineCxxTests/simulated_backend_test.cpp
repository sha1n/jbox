// simulated_backend_test.cpp — tests for the SimulatedBackend.

#include <catch_amalgamated.hpp>

#include "simulated_backend.hpp"

#include <vector>

using jbox::control::BackendDeviceInfo;
using jbox::control::IOProcId;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionNone;
using jbox::control::kBackendDirectionOutput;
using jbox::control::kInvalidIOProcId;
using jbox::control::SimulatedBackend;

namespace {

BackendDeviceInfo makeDevice(const std::string& uid,
                             const std::string& name,
                             std::uint32_t direction,
                             std::uint32_t in_channels,
                             std::uint32_t out_channels,
                             double sr = 48000.0,
                             std::uint32_t buf = 64) {
    BackendDeviceInfo info;
    info.uid = uid;
    info.name = name;
    info.direction = direction;
    info.input_channel_count = in_channels;
    info.output_channel_count = out_channels;
    info.nominal_sample_rate = sr;
    info.buffer_frame_size = buf;
    return info;
}

// Test fixture: buffer received / produced by callbacks.
struct CallbackCapture {
    std::vector<float> last_input;
    std::uint32_t      input_frames = 0;
    std::uint32_t      input_channels = 0;
    std::uint32_t      input_invocations = 0;

    std::vector<float> output_fill;  // what we will write on each call
    std::uint32_t      output_invocations = 0;
};

void inputCallbackTrampoline(const float* samples,
                             std::uint32_t frames,
                             std::uint32_t channels,
                             void* user_data) {
    auto* cap = static_cast<CallbackCapture*>(user_data);
    cap->last_input.assign(samples, samples + frames * channels);
    cap->input_frames = frames;
    cap->input_channels = channels;
    cap->input_invocations++;
}

void outputCallbackTrampoline(float* samples,
                              std::uint32_t frames,
                              std::uint32_t channels,
                              void* user_data) {
    auto* cap = static_cast<CallbackCapture*>(user_data);
    const std::size_t n = static_cast<std::size_t>(frames) * channels;
    if (cap->output_fill.size() >= n) {
        std::copy_n(cap->output_fill.begin(), n, samples);
    }
    cap->output_invocations++;
}

}  // namespace

TEST_CASE("SimulatedBackend: empty backend enumerates to empty list", "[sim_backend]") {
    SimulatedBackend b;
    auto devices = b.enumerate();
    REQUIRE(devices.empty());
}

TEST_CASE("SimulatedBackend: addDevice then enumerate returns the device", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("uid1", "Device One", kBackendDirectionInput | kBackendDirectionOutput, 2, 2));
    auto devices = b.enumerate();
    REQUIRE(devices.size() == 1);
    REQUIRE(devices[0].uid == "uid1");
    REQUIRE(devices[0].name == "Device One");
    REQUIRE((devices[0].direction & kBackendDirectionInput)  != 0);
    REQUIRE((devices[0].direction & kBackendDirectionOutput) != 0);
}

TEST_CASE("SimulatedBackend: removeDevice drops the device from enumeration", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("uid1", "Device One", kBackendDirectionInput, 1, 0));
    b.addDevice(makeDevice("uid2", "Device Two", kBackendDirectionOutput, 0, 1));
    b.removeDevice("uid1");
    auto devices = b.enumerate();
    REQUIRE(devices.size() == 1);
    REQUIRE(devices[0].uid == "uid2");
}

TEST_CASE("SimulatedBackend: openInputCallback fails for unknown device", "[sim_backend]") {
    SimulatedBackend b;
    CallbackCapture cap;
    REQUIRE(b.openInputCallback("nope", inputCallbackTrampoline, &cap) == kInvalidIOProcId);
}

TEST_CASE("SimulatedBackend: openInputCallback fails if device has no input", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("out-only", "Out", kBackendDirectionOutput, 0, 2));
    CallbackCapture cap;
    REQUIRE(b.openInputCallback("out-only", inputCallbackTrampoline, &cap) == kInvalidIOProcId);
}

TEST_CASE("SimulatedBackend: openOutputCallback fails if device has no output", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("in-only", "In", kBackendDirectionInput, 2, 0));
    CallbackCapture cap;
    REQUIRE(b.openOutputCallback("in-only", outputCallbackTrampoline, &cap) == kInvalidIOProcId);
}

TEST_CASE("SimulatedBackend: double input registration fails", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput | kBackendDirectionOutput, 2, 2));
    CallbackCapture cap1, cap2;
    REQUIRE(b.openInputCallback("io", inputCallbackTrampoline, &cap1) != kInvalidIOProcId);
    REQUIRE(b.openInputCallback("io", inputCallbackTrampoline, &cap2) == kInvalidIOProcId);
}

TEST_CASE("SimulatedBackend: closeCallback releases the slot", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput, 2, 0));
    CallbackCapture cap;
    IOProcId id = b.openInputCallback("io", inputCallbackTrampoline, &cap);
    REQUIRE(id != kInvalidIOProcId);

    b.closeCallback(id);
    // Re-registration should succeed now.
    REQUIRE(b.openInputCallback("io", inputCallbackTrampoline, &cap) != kInvalidIOProcId);
}

TEST_CASE("SimulatedBackend: closeCallback is safe with kInvalidIOProcId", "[sim_backend]") {
    SimulatedBackend b;
    b.closeCallback(kInvalidIOProcId);  // must not crash
}

TEST_CASE("SimulatedBackend: deliverBuffer is a no-op before startDevice", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput, 2, 0));
    CallbackCapture cap;
    b.openInputCallback("io", inputCallbackTrampoline, &cap);

    float input[4] = {1, 2, 3, 4};
    b.deliverBuffer("io", 2, input, nullptr);
    REQUIRE(cap.input_invocations == 0);
}

TEST_CASE("SimulatedBackend: deliverBuffer invokes input callback after start", "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput, 2, 0));
    CallbackCapture cap;
    b.openInputCallback("io", inputCallbackTrampoline, &cap);
    REQUIRE(b.startDevice("io"));

    float input[4] = {10.0f, 11.0f, 20.0f, 21.0f};
    b.deliverBuffer("io", 2, input, nullptr);

    REQUIRE(cap.input_invocations == 1);
    REQUIRE(cap.input_frames == 2);
    REQUIRE(cap.input_channels == 2);
    REQUIRE(cap.last_input == std::vector<float>{10.0f, 11.0f, 20.0f, 21.0f});
}

TEST_CASE("SimulatedBackend: deliverBuffer invokes output callback and captures result",
          "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("out", "Out", kBackendDirectionOutput, 0, 2));
    CallbackCapture cap;
    cap.output_fill = {0.5f, -0.5f, 0.25f, -0.25f};
    b.openOutputCallback("out", outputCallbackTrampoline, &cap);
    REQUIRE(b.startDevice("out"));

    std::vector<float> captured(4, 9999.0f);
    b.deliverBuffer("out", 2, nullptr, captured.data());

    REQUIRE(cap.output_invocations == 1);
    REQUIRE(captured == std::vector<float>{0.5f, -0.5f, 0.25f, -0.25f});
}

TEST_CASE("SimulatedBackend: deliverBuffer fires both directions when both registered",
          "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("duplex", "D", kBackendDirectionInput | kBackendDirectionOutput, 2, 2));
    CallbackCapture in_cap, out_cap;
    out_cap.output_fill = {0.1f, 0.2f};
    b.openInputCallback("duplex",  inputCallbackTrampoline,  &in_cap);
    b.openOutputCallback("duplex", outputCallbackTrampoline, &out_cap);
    REQUIRE(b.startDevice("duplex"));

    float input[2] = {7.0f, 8.0f};
    std::vector<float> out(2);
    b.deliverBuffer("duplex", 1, input, out.data());

    REQUIRE(in_cap.input_invocations  == 1);
    REQUIRE(out_cap.output_invocations == 1);
    REQUIRE(in_cap.last_input == std::vector<float>{7.0f, 8.0f});
    REQUIRE(out == std::vector<float>{0.1f, 0.2f});
}

TEST_CASE("SimulatedBackend: stopDevice silences subsequent deliverBuffer calls",
          "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput, 2, 0));
    CallbackCapture cap;
    b.openInputCallback("io", inputCallbackTrampoline, &cap);
    REQUIRE(b.startDevice("io"));

    float input[2] = {1, 2};
    b.deliverBuffer("io", 1, input, nullptr);
    REQUIRE(cap.input_invocations == 1);

    b.stopDevice("io");
    b.deliverBuffer("io", 1, input, nullptr);
    REQUIRE(cap.input_invocations == 1);  // unchanged
}

TEST_CASE("SimulatedBackend: startDevice twice returns false the second time",
          "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput, 1, 0));
    REQUIRE(b.startDevice("io") == true);
    REQUIRE(b.startDevice("io") == false);
}

TEST_CASE("SimulatedBackend: unknown-device start and deliver are safe no-ops",
          "[sim_backend]") {
    SimulatedBackend b;
    REQUIRE(b.startDevice("nope") == false);
    b.stopDevice("nope");             // no crash
    b.deliverBuffer("nope", 1, nullptr, nullptr);  // no crash
}

TEST_CASE("SimulatedBackend: removeDevice during started state stops delivery",
          "[sim_backend]") {
    SimulatedBackend b;
    b.addDevice(makeDevice("io", "IO", kBackendDirectionInput, 1, 0));
    CallbackCapture cap;
    b.openInputCallback("io", inputCallbackTrampoline, &cap);
    b.startDevice("io");

    float input[1] = {0.5f};
    b.deliverBuffer("io", 1, input, nullptr);
    REQUIRE(cap.input_invocations == 1);

    b.removeDevice("io");
    b.deliverBuffer("io", 1, input, nullptr);
    REQUIRE(cap.input_invocations == 1);  // unchanged — device is gone
}
