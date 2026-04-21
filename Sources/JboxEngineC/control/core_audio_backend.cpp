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

    enum class Mode { kInput, kOutput, kDuplex };
    Mode mode = Mode::kInput;

    // For kInput  : channels = input channel count.
    // For kOutput : channels = output channel count.
    // For kDuplex : unused; input_channels + output_channels hold both.
    std::uint32_t channels        = 0;
    std::uint32_t input_channels  = 0;
    std::uint32_t output_channels = 0;

    InputIOProcCallback  input_cb  = nullptr;
    OutputIOProcCallback output_cb = nullptr;
    DuplexIOProcCallback duplex_cb = nullptr;
    void*                user_data = nullptr;

    // Pre-allocated interleave / de-interleave scratch, sized at open
    // time. Capacity covers any plausible buffer size the device can
    // be set to; no growth happens at RT.
    std::vector<float> scratch;         // input (kInput / kDuplex) or output (kOutput)
    std::vector<float> output_scratch;  // kDuplex only: interleaved output staging
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

// Per-scope HAL latency. kAudioDevicePropertyLatency is the engine-side
// buffer depth the device reports; most drivers under-report, but the
// number we do get is a defensible lower bound for the pill in
// docs/spec.md § 2.12. Returns 0 on query failure or when the device
// has no channels in this scope.
std::uint32_t getDeviceLatencyFrames(AudioObjectID device,
                                     AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyLatency,
                                    scope,
                                    kAudioObjectPropertyElementMain};
    UInt32 frames = 0;
    UInt32 size = sizeof(frames);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &frames) != noErr) {
        return 0;
    }
    return frames;
}

std::uint32_t getSafetyOffsetFrames(AudioObjectID device,
                                    AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertySafetyOffset,
                                    scope,
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
// Read `frame_count` input frames into `scratch` (interleaved) from
// `inInputData`, interleaving if the device presents non-interleaved
// per-channel buffers. Returns the frame count actually available.
std::uint32_t readInputInterleaved(const AudioBufferList* inInputData,
                                   std::uint32_t channels,
                                   float* scratch) {
    if (inInputData == nullptr || inInputData->mNumberBuffers == 0) return 0;
    if (inInputData->mNumberBuffers == 1 &&
        inInputData->mBuffers[0].mNumberChannels == channels) {
        const std::uint32_t frame_count =
            inInputData->mBuffers[0].mDataByteSize /
            (sizeof(float) * channels);
        std::memcpy(scratch, inInputData->mBuffers[0].mData,
                    frame_count * channels * sizeof(float));
        return frame_count;
    }
    const std::uint32_t per_ch_bytes = inInputData->mBuffers[0].mDataByteSize;
    const std::uint32_t frame_count = per_ch_bytes / sizeof(float);
    for (std::uint32_t ch = 0; ch < channels && ch < inInputData->mNumberBuffers; ++ch) {
        const auto* src = static_cast<const float*>(inInputData->mBuffers[ch].mData);
        for (std::uint32_t f = 0; f < frame_count; ++f) {
            scratch[f * channels + ch] = src[f];
        }
    }
    return frame_count;
}

// De-interleave `frame_count` interleaved frames in `scratch` into
// `outOutputData`'s per-channel buffers, or memcpy if the device uses
// interleaved output.
void writeOutputFromInterleaved(AudioBufferList* outOutputData,
                                std::uint32_t channels,
                                std::uint32_t frame_count,
                                const float* scratch) {
    if (outOutputData == nullptr || outOutputData->mNumberBuffers == 0) return;
    if (outOutputData->mNumberBuffers == 1 &&
        outOutputData->mBuffers[0].mNumberChannels == channels) {
        std::memcpy(outOutputData->mBuffers[0].mData, scratch,
                    frame_count * channels * sizeof(float));
        return;
    }
    for (std::uint32_t ch = 0; ch < channels && ch < outOutputData->mNumberBuffers; ++ch) {
        auto* dst = static_cast<float*>(outOutputData->mBuffers[ch].mData);
        for (std::uint32_t f = 0; f < frame_count; ++f) {
            dst[f] = scratch[f * channels + ch];
        }
    }
}

OSStatus ioProcTrampoline(AudioObjectID /*inDevice*/,
                          const AudioTimeStamp* /*inNow*/,
                          const AudioBufferList* inInputData,
                          const AudioTimeStamp* /*inInputTime*/,
                          AudioBufferList* outOutputData,
                          const AudioTimeStamp* /*inOutputTime*/,
                          void* inClientData) {
    using Mode = CoreAudioBackend::IOProcRecord::Mode;
    auto* rec = static_cast<CoreAudioBackend::IOProcRecord*>(inClientData);

    if (rec->mode == Mode::kDuplex) {
        if (rec->duplex_cb == nullptr) return noErr;
        if (inInputData == nullptr || outOutputData == nullptr) return noErr;
        if (inInputData->mNumberBuffers == 0 ||
            outOutputData->mNumberBuffers == 0) return noErr;

        float* in_scratch  = rec->scratch.data();
        float* out_scratch = rec->output_scratch.data();
        const std::uint32_t in_ch  = rec->input_channels;
        const std::uint32_t out_ch = rec->output_channels;

        const std::uint32_t in_frames =
            readInputInterleaved(inInputData, in_ch, in_scratch);

        // Compute output frame count from the device's output buffer.
        const std::uint32_t out_per_ch_bytes =
            outOutputData->mBuffers[0].mDataByteSize;
        const std::uint32_t out_frames_linear =
            out_per_ch_bytes / (sizeof(float) * out_ch);
        const std::uint32_t out_frames_planar =
            out_per_ch_bytes / sizeof(float);
        const bool interleaved_out =
            outOutputData->mNumberBuffers == 1 &&
            outOutputData->mBuffers[0].mNumberChannels == out_ch;
        const std::uint32_t out_frames =
            interleaved_out ? out_frames_linear : out_frames_planar;

        // Zero the output scratch before handing it to the callback.
        std::memset(out_scratch, 0,
                    sizeof(float) * out_frames * out_ch);

        rec->duplex_cb(in_scratch, in_frames, in_ch,
                       out_scratch, out_frames, out_ch,
                       rec->user_data);

        writeOutputFromInterleaved(outOutputData, out_ch, out_frames, out_scratch);
        return noErr;
    }

    if (rec->mode == Mode::kInput) {
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
    } else {  // Mode::kOutput
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

        if (info.input_channel_count > 0) {
            info.input_device_latency_frames =
                getDeviceLatencyFrames(id, kAudioObjectPropertyScopeInput);
            info.input_safety_offset_frames =
                getSafetyOffsetFrames(id, kAudioObjectPropertyScopeInput);
        }
        if (info.output_channel_count > 0) {
            info.output_device_latency_frames =
                getDeviceLatencyFrames(id, kAudioObjectPropertyScopeOutput);
            info.output_safety_offset_frames =
                getSafetyOffsetFrames(id, kAudioObjectPropertyScopeOutput);
        }

        device_ids_[info.uid] = id;
        result.push_back(std::move(info));
    }

    return result;
}

// -----------------------------------------------------------------------------
// Per-channel names
// -----------------------------------------------------------------------------

std::vector<std::string> CoreAudioBackend::channelNames(
    const std::string& uid,
    std::uint32_t direction) {
    std::vector<std::string> names;

    // Exactly one direction flag must be set; otherwise we don't know
    // which scope to query and an empty result is the cleanest answer.
    const bool is_input  = (direction == kBackendDirectionInput);
    const bool is_output = (direction == kBackendDirectionOutput);
    if (!is_input && !is_output) return names;

    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return names;
    const AudioDeviceID device_id = it->second;

    const AudioObjectPropertyScope scope = is_input
        ? kAudioObjectPropertyScopeInput
        : kAudioObjectPropertyScopeOutput;

    const std::uint32_t channels = getChannelCount(device_id, scope);
    names.reserve(channels);

    // Channels are 1-indexed in Core Audio's element addressing.
    for (std::uint32_t ch = 1; ch <= channels; ++ch) {
        AudioObjectPropertyAddress addr{
            kAudioObjectPropertyElementName, scope, ch};
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize(device_id, &addr, 0, nullptr,
                                           &dataSize) != noErr ||
            dataSize != sizeof(CFStringRef)) {
            names.emplace_back();
            continue;
        }
        CFStringRef str = nullptr;
        if (AudioObjectGetPropertyData(device_id, &addr, 0, nullptr,
                                       &dataSize, &str) != noErr ||
            str == nullptr) {
            names.emplace_back();
            continue;
        }
        const CFIndex length = CFStringGetLength(str);
        const CFIndex maxBytes =
            CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
        std::string out(static_cast<std::size_t>(maxBytes), '\0');
        const Boolean ok = CFStringGetCString(
            str, out.data(), maxBytes, kCFStringEncodingUTF8);
        CFRelease(str);
        if (ok) {
            out.resize(std::strlen(out.c_str()));
        } else {
            out.clear();
        }
        names.push_back(std::move(out));
    }
    return names;
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
        if (rec->device_uid != uid) continue;
        if (rec->mode == IOProcRecord::Mode::kInput ||
            rec->mode == IOProcRecord::Mode::kDuplex) {
            return kInvalidIOProcId;
        }
    }

    auto rec = std::make_unique<IOProcRecord>();
    rec->device_uid = uid;
    rec->device_id  = id;
    rec->mode       = IOProcRecord::Mode::kInput;
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
        if (rec->device_uid != uid) continue;
        if (rec->mode == IOProcRecord::Mode::kOutput ||
            rec->mode == IOProcRecord::Mode::kDuplex) {
            return kInvalidIOProcId;
        }
    }

    auto rec = std::make_unique<IOProcRecord>();
    rec->device_uid = uid;
    rec->device_id  = id;
    rec->mode       = IOProcRecord::Mode::kOutput;
    rec->channels   = channels;
    rec->output_cb  = callback;
    rec->user_data  = user_data;
    rec->scratch.assign(kScratchMaxFrames * kScratchMaxChannels, 0.0f);

    if (!registerIOProc(*rec)) return kInvalidIOProcId;

    const IOProcId id_out = next_id_++;
    ioprocs_[id_out] = std::move(rec);
    return id_out;
}

IOProcId CoreAudioBackend::openDuplexCallback(const std::string& uid,
                                              DuplexIOProcCallback callback,
                                              void* user_data) {
    if (callback == nullptr) return kInvalidIOProcId;
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return kInvalidIOProcId;
    const AudioDeviceID id = it->second;
    const std::uint32_t in_channels  = getChannelCount(id, kAudioObjectPropertyScopeInput);
    const std::uint32_t out_channels = getChannelCount(id, kAudioObjectPropertyScopeOutput);
    if (in_channels == 0 || out_channels == 0) return kInvalidIOProcId;

    // Exclusive: refuse if any IOProc (input, output, or another duplex)
    // already targets this device. Duplex is the direct-monitor fast
    // path and takes over the device while running.
    for (const auto& [proc_id, rec] : ioprocs_) {
        if (rec->device_uid == uid) return kInvalidIOProcId;
    }

    auto rec = std::make_unique<IOProcRecord>();
    rec->device_uid      = uid;
    rec->device_id       = id;
    rec->mode            = IOProcRecord::Mode::kDuplex;
    rec->input_channels  = in_channels;
    rec->output_channels = out_channels;
    rec->duplex_cb       = callback;
    rec->user_data       = user_data;
    rec->scratch.assign(kScratchMaxFrames * kScratchMaxChannels, 0.0f);
    rec->output_scratch.assign(kScratchMaxFrames * kScratchMaxChannels, 0.0f);

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

std::uint32_t CoreAudioBackend::currentBufferFrameSize(const std::string& uid) {
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return 0;
    return getBufferFrameSize(it->second);
}

bool CoreAudioBackend::claimExclusive(const std::string& uid) {
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return false;
    const AudioDeviceID id = it->second;

    AudioObjectPropertyAddress addr{kAudioDevicePropertyHogMode,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    // Set to our pid; Core Audio atomically swings hog ownership to us
    // if no other process currently holds it, or fails.
    pid_t desired = getpid();
    if (AudioObjectSetPropertyData(id, &addr, 0, nullptr,
                                   sizeof(desired), &desired) != noErr) {
        return false;
    }
    // Re-read to confirm — the HAL may have atomically written back
    // a different pid if another claim happened concurrently.
    pid_t current = -1;
    UInt32 size = sizeof(current);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr,
                                   &size, &current) != noErr) {
        return false;
    }
    return current == getpid();
}

void CoreAudioBackend::releaseExclusive(const std::string& uid) {
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return;
    const AudioDeviceID id = it->second;

    AudioObjectPropertyAddress addr{kAudioDevicePropertyHogMode,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    pid_t unowned = -1;
    (void)AudioObjectSetPropertyData(id, &addr, 0, nullptr,
                                     sizeof(unowned), &unowned);
}

std::uint32_t CoreAudioBackend::requestBufferFrameSize(const std::string& uid,
                                                      std::uint32_t frames) {
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end() || frames == 0) return 0;
    const AudioDeviceID id = it->second;

    // Clamp the request into the device's supported range. Devices
    // that do not expose the range property get the request through
    // unclamped; most then enforce their own clamp internally.
    AudioObjectPropertyAddress rangeAddr{
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    AudioValueRange range{0.0, 0.0};
    UInt32 rangeSize = sizeof(range);
    std::uint32_t target = frames;
    if (AudioObjectGetPropertyData(id, &rangeAddr, 0, nullptr,
                                   &rangeSize, &range) == noErr) {
        if (range.mMinimum > 0 && target < static_cast<std::uint32_t>(range.mMinimum)) {
            target = static_cast<std::uint32_t>(range.mMinimum);
        }
        if (range.mMaximum > 0 && target > static_cast<std::uint32_t>(range.mMaximum)) {
            target = static_cast<std::uint32_t>(range.mMaximum);
        }
    }

    AudioObjectPropertyAddress setAddr{
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 value = target;
    (void)AudioObjectSetPropertyData(id, &setAddr, 0, nullptr,
                                     sizeof(value), &value);
    // Re-read: the HAL may or may not honour the exact request.
    return getBufferFrameSize(id);
}

}  // namespace jbox::control
