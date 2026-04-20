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
#include <string>
#include <unordered_map>

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
    void closeCallback(IOProcId id) override;
    bool startDevice(const std::string& uid) override;
    void stopDevice(const std::string& uid) override;

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
};

}  // namespace jbox::control

#endif  // JBOX_CONTROL_CORE_AUDIO_BACKEND_HPP
