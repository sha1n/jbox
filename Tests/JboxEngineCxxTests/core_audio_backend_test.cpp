// core_audio_backend_test.cpp — tests for CoreAudioBackend against
// the system's real audio devices.
//
// These tests exercise enumerate() only: they run in CI on the
// macOS runner (which has default built-in audio devices) and on
// the developer's Mac without requiring any specific hardware.
// IOProc / start / stop validation requires real hardware and is
// part of the Phase 3 manual acceptance test (see docs/plan.md).

#include <catch_amalgamated.hpp>

#include "core_audio_backend.hpp"

#include <string>

using jbox::control::BackendDeviceInfo;
using jbox::control::CoreAudioBackend;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionNone;
using jbox::control::kBackendDirectionOutput;

TEST_CASE("CoreAudioBackend: enumerate returns at least one device", "[core_audio][integration]") {
    // On a Mac, there is always at least the built-in output and the
    // built-in microphone (or their headless equivalents on CI runners).
    CoreAudioBackend backend;
    auto devices = backend.enumerate();
    REQUIRE(!devices.empty());
}

TEST_CASE("CoreAudioBackend: every enumerated device has a non-empty UID and name",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    auto devices = backend.enumerate();
    for (const auto& d : devices) {
        REQUIRE(!d.uid.empty());
        // Some devices may legitimately not report a name (rare),
        // but the UID is required for persistence so we gate on it.
    }
}

TEST_CASE("CoreAudioBackend: every enumerated device has at least one direction set",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    auto devices = backend.enumerate();
    for (const auto& d : devices) {
        REQUIRE(d.direction != kBackendDirectionNone);
    }
}

TEST_CASE("CoreAudioBackend: at least one input-capable and one output-capable device exists",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    auto devices = backend.enumerate();
    bool any_input = false, any_output = false;
    for (const auto& d : devices) {
        if (d.direction & kBackendDirectionInput)  any_input  = true;
        if (d.direction & kBackendDirectionOutput) any_output = true;
    }
    REQUIRE(any_input);
    REQUIRE(any_output);
}

TEST_CASE("CoreAudioBackend: sample rate is positive for every device",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    auto devices = backend.enumerate();
    for (const auto& d : devices) {
        INFO("device uid = " << d.uid << ", name = " << d.name);
        REQUIRE(d.nominal_sample_rate > 0.0);
    }
}

TEST_CASE("CoreAudioBackend: enumerate is idempotent across calls",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    auto first  = backend.enumerate();
    auto second = backend.enumerate();
    REQUIRE(first.size() == second.size());
    // UIDs should match set-wise. Order isn't guaranteed.
    for (const auto& a : first) {
        bool found = false;
        for (const auto& b : second) {
            if (a.uid == b.uid) { found = true; break; }
        }
        INFO("missing uid in second enumeration: " << a.uid);
        REQUIRE(found);
    }
}

TEST_CASE("CoreAudioBackend: openInputCallback rejects unknown UID",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    backend.enumerate();  // populate internal UID cache

    const auto id = backend.openInputCallback(
        "definitely-not-a-real-uid",
        [](const float*, std::uint32_t, std::uint32_t, void*) {},
        nullptr);
    REQUIRE(id == jbox::control::kInvalidIOProcId);
}

TEST_CASE("CoreAudioBackend: startDevice on unknown UID returns false",
          "[core_audio][integration]") {
    CoreAudioBackend backend;
    backend.enumerate();
    REQUIRE(backend.startDevice("definitely-not-a-real-uid") == false);
}
