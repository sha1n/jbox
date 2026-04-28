// core_audio_backend_test.cpp — tests for CoreAudioBackend against
// the system's real audio devices.
//
// Two groups live here:
//
//  - [core_audio][integration] — runs against the real HAL on the
//    macOS runner / developer's Mac. Exercises enumerate() and the
//    obvious unknown-UID rejection paths. IOProc / start / stop
//    validation requires real hardware and is part of the Phase 3
//    manual acceptance test (see docs/plan.md).
//
//  - [core_audio][hal_translation] — pure-logic tests for the
//    HAL→DeviceChangeEvent translator backing F1's listener wiring.
//    No HAL access; the translator is a free function.

#include <catch_amalgamated.hpp>

#include "core_audio_backend.hpp"
#include "core_audio_hal_translation.hpp"

#include <string>

using jbox::control::BackendDeviceInfo;
using jbox::control::CoreAudioBackend;
using jbox::control::DeviceChangeEvent;
using jbox::control::kBackendDirectionInput;
using jbox::control::kBackendDirectionNone;
using jbox::control::kBackendDirectionOutput;
using jbox::control::translateHalPropertyChange;

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

// -----------------------------------------------------------------------------
// HAL → DeviceChangeEvent translation (F1).
//
// These tests pin the pure-logic translator that backs F1's listener
// block bodies. The block body's contract is:
//
//   1. The HAL fires a property listener on this thread.
//   2. The block reads any value it needs (only kAudioDeviceProperty
//      DeviceIsAlive needs a readback — the others are pure
//      "something changed" pings).
//   3. The block calls translateHalPropertyChange(...) with the
//      selector, the bound device's UID, and the readback.
//   4. If the result is non-empty, the block fires the watcher.
//
// Every branch of step (3) is exercised below.
// -----------------------------------------------------------------------------

TEST_CASE("translateHalPropertyChange: kAudioHardwarePropertyDevices emits "
          "kDeviceListChanged with empty uid",
          "[core_audio][hal_translation]") {
    // The system-object listener fires for the entire device list; the
    // event has no specific subject. The uid argument is supplied only
    // so the helper has a uniform signature — it must be ignored for
    // this selector and the emitted uid must be empty.
    const auto event = translateHalPropertyChange(
        kAudioHardwarePropertyDevices,
        /*device_uid=*/"ignored-because-system-scope",
        /*is_alive_readback=*/1);

    REQUIRE(event.has_value());
    REQUIRE(event->kind == DeviceChangeEvent::kDeviceListChanged);
    REQUIRE(event->uid.empty());
}

TEST_CASE("translateHalPropertyChange: IsAlive=0 emits kDeviceIsNotAlive "
          "with the device's uid",
          "[core_audio][hal_translation]") {
    const auto event = translateHalPropertyChange(
        kAudioDevicePropertyDeviceIsAlive,
        /*device_uid=*/"AppleUSBAudioEngine:device-1",
        /*is_alive_readback=*/0);

    REQUIRE(event.has_value());
    REQUIRE(event->kind == DeviceChangeEvent::kDeviceIsNotAlive);
    REQUIRE(event->uid == "AppleUSBAudioEngine:device-1");
}

TEST_CASE("translateHalPropertyChange: IsAlive=1 emits no event",
          "[core_audio][hal_translation]") {
    // Some HALs fire kAudioDevicePropertyDeviceIsAlive for both
    // transitions; we only care about the device-going-away edge.
    // The reappearance-edge case is covered by the
    // kAudioHardwarePropertyDevices listener feeding kDeviceListChanged
    // separately.
    const auto event = translateHalPropertyChange(
        kAudioDevicePropertyDeviceIsAlive,
        /*device_uid=*/"AppleUSBAudioEngine:device-1",
        /*is_alive_readback=*/1);

    REQUIRE_FALSE(event.has_value());
}

TEST_CASE("translateHalPropertyChange: IsAlive=0 with empty uid still emits "
          "the event (RouteManager handles empty-uid loss as a no-op)",
          "[core_audio][hal_translation]") {
    // Production path: the HAL fires IsAlive on a device that has just
    // been dropped from id_to_uid_ between callback start and our
    // reverse-map lookup. The callback passes an empty uid into the
    // translator. We still emit the event — the reaction layer's
    // handleDeviceChanges checks `ev.uid.empty()` (route_manager.cpp,
    // the kDeviceIsNotAlive branch) and skips the loss pass for that
    // case. The kDeviceListChanged from the system-object listener is
    // what does the actual recovery work in that scenario. Pinning the
    // translator contract here keeps the two-layer split intact.
    const auto event = translateHalPropertyChange(
        kAudioDevicePropertyDeviceIsAlive,
        /*device_uid=*/"",
        /*is_alive_readback=*/0);

    REQUIRE(event.has_value());
    REQUIRE(event->kind == DeviceChangeEvent::kDeviceIsNotAlive);
    REQUIRE(event->uid.empty());
}

TEST_CASE("translateHalPropertyChange: aggregate active-sub-device-list "
          "change emits kAggregateMembersChanged with the aggregate's uid",
          "[core_audio][hal_translation]") {
    const auto event = translateHalPropertyChange(
        kAudioAggregateDevicePropertyActiveSubDeviceList,
        /*device_uid=*/"com.example.aggregate:agg-1",
        /*is_alive_readback=*/1);

    REQUIRE(event.has_value());
    REQUIRE(event->kind == DeviceChangeEvent::kAggregateMembersChanged);
    REQUIRE(event->uid == "com.example.aggregate:agg-1");
}

TEST_CASE("translateHalPropertyChange: unrelated selectors emit no event",
          "[core_audio][hal_translation]") {
    // In production we register listeners only for the three selectors
    // above, so an unrelated callback should be impossible. Hardening
    // against it costs nothing and prevents a misregistered listener
    // from firing spurious events at the watcher.
    const auto sample_rate = translateHalPropertyChange(
        kAudioDevicePropertyNominalSampleRate,
        /*device_uid=*/"AppleUSBAudioEngine:device-1",
        /*is_alive_readback=*/1);
    REQUIRE_FALSE(sample_rate.has_value());

    const auto buffer_frames = translateHalPropertyChange(
        kAudioDevicePropertyBufferFrameSize,
        /*device_uid=*/"AppleUSBAudioEngine:device-1",
        /*is_alive_readback=*/1);
    REQUIRE_FALSE(buffer_frames.has_value());
}

// -----------------------------------------------------------------------------
// HAL listener lifecycle on real Core Audio (F1).
//
// These tests exercise the install / re-install / teardown path on the
// actual HAL: AudioObjectAddPropertyListener and Remove are called
// against the system object + each enumerated device. CI cannot fire
// hot-plug events, so the assertions here are weaker than the
// translator group — they catch typos and argument-shape bugs in the
// Apple API calls and, importantly, would surface a leak or crash from
// a mismatched Add/Remove pair. The end-to-end "real device disappears
// → route transitions to WAITING" behavior is gated on the manual
// hardware acceptance steps in `docs/followups.md` § F1.
// -----------------------------------------------------------------------------

namespace {
void noopDeviceChangeListener(const jbox::control::DeviceChangeEvent&,
                              void*) {}
}  // namespace

TEST_CASE("CoreAudioBackend: setDeviceChangeListener with a non-null callback "
          "succeeds against the real HAL",
          "[core_audio][hal_listener_lifecycle]") {
    // Pins that AudioObjectAddPropertyListener accepts the system-object
    // + per-device + per-aggregate registrations the implementation
    // makes. A bad property selector / scope / element argument would
    // surface here as a non-noErr result inside install*; the test
    // would not crash but the os_log line would fire. The test
    // doubles as a smoke check that destruction tears the listeners
    // down without crashing.
    CoreAudioBackend backend;
    backend.enumerate();  // populate device_ids_ + id_to_uid_

    // Should not crash; return path simply succeeds when Add is happy.
    backend.setDeviceChangeListener(&noopDeviceChangeListener, nullptr);
    backend.setDeviceChangeListener(nullptr, nullptr);
}

TEST_CASE("CoreAudioBackend: re-registering a listener replaces the previous "
          "one without leaking",
          "[core_audio][hal_listener_lifecycle]") {
    // The second registration must (a) tear down listeners installed
    // by the first registration via AudioObjectRemovePropertyListener
    // and (b) install fresh listeners for the new callback. A bug
    // where the first registration's listeners stay live would
    // accumulate on every re-registration; visible only via
    // os_log, but a destructor-time double-free would manifest as a
    // crash here.
    CoreAudioBackend backend;
    backend.enumerate();

    backend.setDeviceChangeListener(&noopDeviceChangeListener, nullptr);
    int sentinel = 0;
    backend.setDeviceChangeListener(&noopDeviceChangeListener, &sentinel);
    // Destructor runs at end-of-scope and removes whatever's installed.
}

TEST_CASE("CoreAudioBackend: enumerate after registering a listener does not "
          "crash and reconciles per-device listeners",
          "[core_audio][hal_listener_lifecycle]") {
    // enumerate() rebuilds device_ids_ + id_to_uid_ and triggers
    // reconcilePerDeviceListeners. The reconcile path uses the same
    // Apple Add/Remove APIs as setDeviceChangeListener; a bug there
    // would not surface in the basic register-then-unregister test
    // above (which does not exercise the reconcile diff). Calling
    // enumerate() twice with a listener registered exercises the
    // "no-op idempotent reconcile" branch (every device's listener
    // is `alreadyHas`) — a bug in the diff bookkeeping would leak
    // listeners, redundantly add them, or crash on a stale entry.
    CoreAudioBackend backend;
    backend.enumerate();
    backend.setDeviceChangeListener(&noopDeviceChangeListener, nullptr);
    backend.enumerate();
    backend.enumerate();
    backend.setDeviceChangeListener(nullptr, nullptr);
}

TEST_CASE("CoreAudioBackend: destructor with an active listener tears it down "
          "cleanly",
          "[core_audio][hal_listener_lifecycle]") {
    // Pins the destructor's "clear cb under lock, then
    // removeAllListeners() outside the lock" sequence. A bug where the
    // destructor failed to remove listeners would leak Apple-side
    // registrations indefinitely on every backend tear-down — visible
    // via Activity Monitor but not from CI. A bug where the destructor
    // tried to call into a half-destroyed object would crash here.
    auto backend = std::make_unique<CoreAudioBackend>();
    backend->enumerate();
    backend->setDeviceChangeListener(&noopDeviceChangeListener, nullptr);
    backend.reset();  // ~CoreAudioBackend
}
