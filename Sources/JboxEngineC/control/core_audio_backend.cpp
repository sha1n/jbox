// core_audio_backend.cpp — Core Audio HAL implementation of IDeviceBackend.

#include "core_audio_backend.hpp"

#include <CoreAudio/CoreAudio.h>

#include <cstring>
#include <vector>

namespace jbox::control {

// -----------------------------------------------------------------------------
// Per-IOProc bookkeeping and the RT trampoline.
// -----------------------------------------------------------------------------

struct CoreAudioBackend::IOProcRecord {
    std::string         device_uid;
    AudioDeviceID       device_id      = kAudioObjectUnknown;
    AudioDeviceIOProcID ca_proc_id     = nullptr;

    bool is_input = false;
    std::uint32_t channels = 0;

    InputIOProcCallback  input_cb  = nullptr;
    OutputIOProcCallback output_cb = nullptr;
    void*                user_data = nullptr;

    // Pre-allocated interleave / de-interleave scratch, sized at open
    // time. Capacity covers any plausible buffer size the device can
    // be set to; no growth happens at RT.
    std::vector<float> scratch;
};

namespace {

// Conservative upper bound on the per-IOProc scratch. Worst-case
// frame_count for audio devices is ~8192 at low sample rates; 64
// channels covers V31 (32 in) and Apollo (18+ out, more with virtual).
constexpr std::size_t kScratchMaxFrames   = 8192;
constexpr std::size_t kScratchMaxChannels = 64;

// Helper: read a CFString property from any AudioObject.
std::string getStringProperty(AudioObjectID object,
                              AudioObjectPropertySelector selector,
                              AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress addr{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(object, &addr, 0, nullptr, &dataSize) != noErr) {
        return {};
    }
    if (dataSize != sizeof(CFStringRef)) {
        return {};
    }
    CFStringRef str = nullptr;
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &dataSize, &str) != noErr ||
        str == nullptr) {
        return {};
    }
    const CFIndex length = CFStringGetLength(str);
    const CFIndex maxBytes =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(maxBytes), '\0');
    const Boolean ok = CFStringGetCString(str, result.data(), maxBytes, kCFStringEncodingUTF8);
    CFRelease(str);
    if (!ok) return {};
    result.resize(std::strlen(result.c_str()));
    return result;
}

// Helper: total channel count across all streams in a given scope.
std::uint32_t getChannelCount(AudioObjectID device,
                              AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration,
                                    scope,
                                    kAudioObjectPropertyElementMain};
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &dataSize) != noErr) {
        return 0;
    }
    std::vector<std::uint8_t> buffer(dataSize, 0);
    auto* list = reinterpret_cast<AudioBufferList*>(buffer.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &dataSize, list) != noErr) {
        return 0;
    }
    std::uint32_t total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        total += list->mBuffers[i].mNumberChannels;
    }
    return total;
}

double getNominalSampleRate(AudioObjectID device) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &rate) != noErr) {
        return 0.0;
    }
    return static_cast<double>(rate);
}

std::uint32_t getBufferFrameSize(AudioObjectID device) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyBufferFrameSize,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 frames = 0;
    UInt32 size = sizeof(frames);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &frames) != noErr) {
        return 0;
    }
    return frames;
}

// RT thread trampoline. Core Audio invokes this from its audio
// callback; we must NOT allocate, lock, or syscall here.
OSStatus ioProcTrampoline(AudioObjectID /*inDevice*/,
                          const AudioTimeStamp* /*inNow*/,
                          const AudioBufferList* inInputData,
                          const AudioTimeStamp* /*inInputTime*/,
                          AudioBufferList* outOutputData,
                          const AudioTimeStamp* /*inOutputTime*/,
                          void* inClientData) {
    auto* rec = static_cast<CoreAudioBackend::IOProcRecord*>(inClientData);

    if (rec->is_input) {
        if (rec->input_cb == nullptr || inInputData == nullptr ||
            inInputData->mNumberBuffers == 0) {
            return noErr;
        }
        const std::uint32_t channels = rec->channels;

        if (inInputData->mNumberBuffers == 1 &&
            inInputData->mBuffers[0].mNumberChannels == channels) {
            // Already interleaved.
            const std::uint32_t frame_count =
                inInputData->mBuffers[0].mDataByteSize /
                (sizeof(float) * channels);
            rec->input_cb(
                static_cast<const float*>(inInputData->mBuffers[0].mData),
                frame_count, channels, rec->user_data);
        } else {
            // Non-interleaved: one buffer per channel. Interleave into
            // the pre-allocated scratch.
            const std::uint32_t per_ch_bytes = inInputData->mBuffers[0].mDataByteSize;
            const std::uint32_t frame_count = per_ch_bytes / sizeof(float);
            float* scratch = rec->scratch.data();
            for (std::uint32_t ch = 0; ch < channels && ch < inInputData->mNumberBuffers; ++ch) {
                const auto* src = static_cast<const float*>(inInputData->mBuffers[ch].mData);
                for (std::uint32_t f = 0; f < frame_count; ++f) {
                    scratch[f * channels + ch] = src[f];
                }
            }
            rec->input_cb(scratch, frame_count, channels, rec->user_data);
        }
    } else {
        if (rec->output_cb == nullptr || outOutputData == nullptr ||
            outOutputData->mNumberBuffers == 0) {
            return noErr;
        }
        const std::uint32_t channels = rec->channels;

        if (outOutputData->mNumberBuffers == 1 &&
            outOutputData->mBuffers[0].mNumberChannels == channels) {
            // Interleaved output. Zero the buffer, then let the
            // callback fill it directly.
            auto* dst = static_cast<float*>(outOutputData->mBuffers[0].mData);
            const std::uint32_t byteSize = outOutputData->mBuffers[0].mDataByteSize;
            std::memset(dst, 0, byteSize);
            const std::uint32_t frame_count = byteSize / (sizeof(float) * channels);
            rec->output_cb(dst, frame_count, channels, rec->user_data);
        } else {
            // Non-interleaved: callback writes into scratch (zeroed
            // first), then we de-interleave into outOutputData.
            const std::uint32_t per_ch_bytes = outOutputData->mBuffers[0].mDataByteSize;
            const std::uint32_t frame_count = per_ch_bytes / sizeof(float);
            float* scratch = rec->scratch.data();
            std::memset(scratch, 0,
                        sizeof(float) * frame_count * channels);
            rec->output_cb(scratch, frame_count, channels, rec->user_data);
            for (std::uint32_t ch = 0; ch < channels && ch < outOutputData->mNumberBuffers; ++ch) {
                auto* dst = static_cast<float*>(outOutputData->mBuffers[ch].mData);
                for (std::uint32_t f = 0; f < frame_count; ++f) {
                    dst[f] = scratch[f * channels + ch];
                }
            }
        }
    }

    return noErr;
}

}  // namespace

// -----------------------------------------------------------------------------
// CoreAudioBackend lifecycle
// -----------------------------------------------------------------------------

CoreAudioBackend::CoreAudioBackend() = default;

CoreAudioBackend::~CoreAudioBackend() {
    // Best-effort cleanup: stop any running devices and tear down
    // IOProcs. Any failures here are logged implicitly via macOS
    // kernel logs; nothing we can propagate from a destructor.
    for (auto& [uid, started] : started_) {
        if (!started) continue;
        auto idIt = device_ids_.find(uid);
        if (idIt == device_ids_.end()) continue;
        for (const auto& [proc_id, rec] : ioprocs_) {
            if (rec->device_uid == uid && rec->ca_proc_id != nullptr) {
                AudioDeviceStop(idIt->second, rec->ca_proc_id);
            }
        }
        started = false;
    }
    for (auto& [proc_id, rec] : ioprocs_) {
        if (rec->ca_proc_id != nullptr) {
            AudioDeviceDestroyIOProcID(rec->device_id, rec->ca_proc_id);
            rec->ca_proc_id = nullptr;
        }
    }
}

// -----------------------------------------------------------------------------
// Enumerate
// -----------------------------------------------------------------------------

std::vector<BackendDeviceInfo> CoreAudioBackend::enumerate() {
    std::vector<BackendDeviceInfo> result;

    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr,
                                       &dataSize) != noErr ||
        dataSize == 0) {
        return result;
    }

    std::vector<AudioDeviceID> ids(dataSize / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &dataSize, ids.data()) != noErr) {
        return result;
    }

    device_ids_.clear();

    for (AudioDeviceID id : ids) {
        BackendDeviceInfo info;
        info.uid = getStringProperty(id, kAudioDevicePropertyDeviceUID);
        if (info.uid.empty()) continue;
        info.name = getStringProperty(id, kAudioObjectPropertyName);

        info.input_channel_count  = getChannelCount(id, kAudioObjectPropertyScopeInput);
        info.output_channel_count = getChannelCount(id, kAudioObjectPropertyScopeOutput);
        info.direction            = kBackendDirectionNone;
        if (info.input_channel_count  > 0) info.direction |= kBackendDirectionInput;
        if (info.output_channel_count > 0) info.direction |= kBackendDirectionOutput;
        if (info.direction == kBackendDirectionNone) {
            // No audio either way — skip (e.g., some capture-only
            // pseudo devices may surface this way).
            continue;
        }
        info.nominal_sample_rate = getNominalSampleRate(id);
        info.buffer_frame_size   = getBufferFrameSize(id);

        device_ids_[info.uid] = id;
        result.push_back(std::move(info));
    }

    return result;
}

// -----------------------------------------------------------------------------
// open / close IOProcs
// -----------------------------------------------------------------------------

namespace {
// Register a single record with Core Audio. Returns true on success;
// leaves `rec->ca_proc_id` populated on success.
bool registerIOProc(CoreAudioBackend::IOProcRecord& rec) {
    AudioDeviceIOProcID ca_id = nullptr;
    const OSStatus err = AudioDeviceCreateIOProcID(
        rec.device_id, ioProcTrampoline, &rec, &ca_id);
    if (err != noErr || ca_id == nullptr) {
        return false;
    }
    rec.ca_proc_id = ca_id;
    return true;
}
}  // namespace

IOProcId CoreAudioBackend::openInputCallback(const std::string& uid,
                                             InputIOProcCallback callback,
                                             void* user_data) {
    if (callback == nullptr) return kInvalidIOProcId;
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return kInvalidIOProcId;
    const AudioDeviceID id = it->second;
    const std::uint32_t channels = getChannelCount(id, kAudioObjectPropertyScopeInput);
    if (channels == 0) return kInvalidIOProcId;

    // One input IOProc per device — matches the simulated backend
    // contract and the engine's multiplexing model.
    for (const auto& [proc_id, rec] : ioprocs_) {
        if (rec->device_uid == uid && rec->is_input) return kInvalidIOProcId;
    }

    auto rec = std::make_unique<IOProcRecord>();
    rec->device_uid = uid;
    rec->device_id  = id;
    rec->is_input   = true;
    rec->channels   = channels;
    rec->input_cb   = callback;
    rec->user_data  = user_data;
    rec->scratch.assign(kScratchMaxFrames * kScratchMaxChannels, 0.0f);

    if (!registerIOProc(*rec)) return kInvalidIOProcId;

    const IOProcId id_out = next_id_++;
    ioprocs_[id_out] = std::move(rec);
    return id_out;
}

IOProcId CoreAudioBackend::openOutputCallback(const std::string& uid,
                                              OutputIOProcCallback callback,
                                              void* user_data) {
    if (callback == nullptr) return kInvalidIOProcId;
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return kInvalidIOProcId;
    const AudioDeviceID id = it->second;
    const std::uint32_t channels = getChannelCount(id, kAudioObjectPropertyScopeOutput);
    if (channels == 0) return kInvalidIOProcId;

    for (const auto& [proc_id, rec] : ioprocs_) {
        if (rec->device_uid == uid && !rec->is_input) return kInvalidIOProcId;
    }

    auto rec = std::make_unique<IOProcRecord>();
    rec->device_uid = uid;
    rec->device_id  = id;
    rec->is_input   = false;
    rec->channels   = channels;
    rec->output_cb  = callback;
    rec->user_data  = user_data;
    rec->scratch.assign(kScratchMaxFrames * kScratchMaxChannels, 0.0f);

    if (!registerIOProc(*rec)) return kInvalidIOProcId;

    const IOProcId id_out = next_id_++;
    ioprocs_[id_out] = std::move(rec);
    return id_out;
}

void CoreAudioBackend::closeCallback(IOProcId id) {
    if (id == kInvalidIOProcId) return;
    auto it = ioprocs_.find(id);
    if (it == ioprocs_.end()) return;

    auto& rec = *it->second;
    if (rec.ca_proc_id != nullptr) {
        // If this device is currently started for this IOProc, stop
        // it first. AudioDeviceStop is idempotent; safe to call
        // unconditionally, but being explicit is clearer.
        AudioDeviceStop(rec.device_id, rec.ca_proc_id);
        AudioDeviceDestroyIOProcID(rec.device_id, rec.ca_proc_id);
        rec.ca_proc_id = nullptr;
    }
    ioprocs_.erase(it);
}

// -----------------------------------------------------------------------------
// start / stop device
// -----------------------------------------------------------------------------

bool CoreAudioBackend::startDevice(const std::string& uid) {
    auto idIt = device_ids_.find(uid);
    if (idIt == device_ids_.end()) return false;
    const AudioDeviceID id = idIt->second;

    if (started_[uid]) return false;  // already started

    bool any = false;
    for (auto& [proc_id, rec] : ioprocs_) {
        if (rec->device_uid != uid || rec->ca_proc_id == nullptr) continue;
        if (AudioDeviceStart(id, rec->ca_proc_id) == noErr) {
            any = true;
        }
    }
    // Mark started even if no IOProcs are registered yet; a later
    // open*Callback that wants to take effect would require a
    // stop+open+start cycle per the documented contract.
    started_[uid] = true;
    return any || ioprocs_.empty();
}

void CoreAudioBackend::stopDevice(const std::string& uid) {
    auto idIt = device_ids_.find(uid);
    if (idIt == device_ids_.end()) return;
    const AudioDeviceID id = idIt->second;

    auto it = started_.find(uid);
    if (it == started_.end() || !it->second) return;

    for (auto& [proc_id, rec] : ioprocs_) {
        if (rec->device_uid != uid || rec->ca_proc_id == nullptr) continue;
        AudioDeviceStop(id, rec->ca_proc_id);
    }
    it->second = false;
}

}  // namespace jbox::control
