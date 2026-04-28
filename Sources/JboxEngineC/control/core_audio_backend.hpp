// core_audio_backend.hpp — production IDeviceBackend backed by Core Audio HAL.
//
// Uses AudioObjectGetPropertyData / AudioDeviceCreateIOProcID for
// device enumeration and IOProc registration respectively. IOProc
// trampolines on the Core Audio RT thread invoke the user-supplied
// callback with interleaved float samples; non-interleaved device
// formats are interleaved/de-interleaved inside the trampoline via
// a per-IOProc pre-allocated scratch buffer (no RT allocation).
//
// Hardware validation: this code lives in production but is exercised
// only by enumerate() in CI (CI runners have no real audio hardware).
// Full path — IOProc register, AudioDeviceStart, samples flowing —
// requires real hardware. Manual acceptance per docs/plan.md § Phase 3.
//
// Assumptions about device formats
// --------------------------------
// * Sample format is 32-bit float. Most macOS audio devices expose
//   Float32 by default; if a device doesn't, open*Callback returns
//   kInvalidIOProcId.
// * Interleaved OR non-interleaved layouts are both handled. The
//   trampoline detects via AudioBufferList::mNumberBuffers and
//   interleaves or de-interleaves as needed.

#ifndef JBOX_CONTROL_CORE_AUDIO_BACKEND_HPP
#define JBOX_CONTROL_CORE_AUDIO_BACKEND_HPP

#include "device_backend.hpp"

#include <CoreAudio/CoreAudio.h>

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jbox::control {

class CoreAudioBackend final : public IDeviceBackend {
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    CoreAudioBackend(const CoreAudioBackend&) = delete;
    CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;

    // IDeviceBackend.
    std::vector<BackendDeviceInfo> enumerate() override;
    std::vector<std::string> channelNames(const std::string& uid,
                                          std::uint32_t direction) override;
    IOProcId openInputCallback(const std::string& uid,
                               InputIOProcCallback callback,
                               void* user_data) override;
    IOProcId openOutputCallback(const std::string& uid,
                                OutputIOProcCallback callback,
                                void* user_data) override;
    IOProcId openDuplexCallback(const std::string& uid,
                                DuplexIOProcCallback callback,
                                void* user_data) override;
    bool closeCallback(IOProcId id) override;
    bool startDevice(const std::string& uid) override;
    void stopDevice(const std::string& uid) override;
    std::uint32_t currentBufferFrameSize(const std::string& uid) override;
    void setBufferFrameSize(const std::string& uid,
                            std::uint32_t frames) override;
    void setDeviceChangeListener(DeviceChangeListener cb,
                                 void* user_data) override;

    // Forward declaration kept public so the .cpp-local trampoline
    // and helper functions can reference it without becoming friends.
    // The full definition remains a .cpp detail.
    struct IOProcRecord;

private:

    // Cache of UID -> AudioDeviceID. Refreshed on each enumerate().
    // AudioDeviceIDs are NOT stable across reboots or hardware
    // topology changes; the UID is what we store long-term.
    std::unordered_map<std::string, AudioDeviceID> device_ids_;

    // Registered IOProcs, keyed by our public IOProcId.
    std::unordered_map<IOProcId, std::unique_ptr<IOProcRecord>> ioprocs_;

    // Per-device "started" state, paired with AudioDeviceStart/Stop
    // calls on the underlying IOProcs.
    std::unordered_map<std::string, bool> started_;

    IOProcId next_id_ = 1;

    // Phase 7.6.4 / F1: HAL property-listener wiring.
    //
    // Two state buckets, with different ownership rules:
    //
    //   1. Cross-thread state (callback_state_mutex_):
    //        device_change_cb_, device_change_user_, id_to_uid_
    //      Read by the HAL callback under shared lock; written by the
    //      control thread under exclusive lock. The control thread
    //      acquires the lock only to read or publish state — never
    //      while calling into Apple.
    //
    //   2. Single-control-thread state (no lock):
    //        system_listener_installed_, per_device_listeners_
    //      Mutated by `setDeviceChangeListener`, `enumerate`, and
    //      `~CoreAudioBackend`. The HAL callback never touches these,
    //      so the only safety obligation is the engine's documented
    //      single-control-thread model (`engine.hpp:8`). That model
    //      already covers `device_ids_`, `started_`, `ioprocs_` —
    //      F1's two new fields ride on the same assumption.
    //
    // The deadlock to avoid: Apple's AudioObjectRemovePropertyListener
    // blocks until in-flight callbacks for the listener being removed
    // complete. The HAL callback acquires the shared lock. If we held
    // the exclusive lock during Remove, a callback firing on another
    // object would block on the shared lock; Remove would block on the
    // first callback; the first callback would block on the shared
    // lock — hung. The rule: NEVER hold callback_state_mutex_ during
    // any Apple AudioObject Add/Remove call.
    struct HalListenerEntry {
        AudioObjectID              object;
        AudioObjectPropertyAddress address;
    };

    mutable std::shared_mutex callback_state_mutex_;
    DeviceChangeListener device_change_cb_   = nullptr;
    void*                device_change_user_ = nullptr;
    std::unordered_map<AudioObjectID, std::string> id_to_uid_;

    bool                          system_listener_installed_ = false;
    std::vector<HalListenerEntry> per_device_listeners_;

    // Static thunk for AudioObjectAddPropertyListener. Apple invokes
    // it with `this` as `inClientData`. Forwards to onHalPropertyEvent.
    static OSStatus halPropertyListenerCallback(
        AudioObjectID inObjectID,
        UInt32 inNumberAddresses,
        const AudioObjectPropertyAddress* inAddresses,
        void* inClientData);

    void onHalPropertyEvent(
        AudioObjectID object,
        UInt32 num_addresses,
        const AudioObjectPropertyAddress* addresses);

    // Control-thread-only helpers. None of these hold
    // callback_state_mutex_ while calling into Apple.
    void installSystemListener();
    void removeAllListeners();
    void reconcilePerDeviceListeners();

    // True if the device with the given AudioDeviceID is an aggregate
    // (has a non-empty active sub-device list).
    static bool isAggregateDevice(AudioDeviceID id);
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_CORE_AUDIO_BACKEND_HPP
