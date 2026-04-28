// core_audio_backend.cpp — Core Audio HAL implementation of IDeviceBackend.

#include "core_audio_backend.hpp"
#include "audio_buffer_interleave.hpp"
#include "core_audio_hal_translation.hpp"

#include <CoreAudio/CoreAudio.h>
#include <os/log.h>

#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>

namespace jbox::control {

namespace {
os_log_t backendLog() {
    static os_log_t log = os_log_create("com.jbox.app", "engine");
    return log;
}
}  // namespace

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
// callback; we must NOT allocate, lock, or syscall here. Interleave
// / de-interleave helpers live in audio_buffer_interleave.hpp so
// they can be unit-tested against synthetic AudioBufferLists.
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
    // F1: tear down HAL property listeners FIRST. Apple's
    // AudioObjectRemovePropertyListener blocks until any in-flight
    // callback completes, so by the time these calls return, no
    // HAL thread can be inside onHalPropertyEvent — safe to destroy
    // the rest of the backend afterward. Clear the cross-thread
    // state under lock so any callback in flight on its way to
    // taking the shared lock observes a null cb and bails.
    {
        std::unique_lock<std::shared_mutex> lock(callback_state_mutex_);
        device_change_cb_   = nullptr;
        device_change_user_ = nullptr;
    }
    removeAllListeners();

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

    // F1: publish the AudioObjectID → UID reverse map under the
    // exclusive lock (so HAL callbacks see consistent state), then
    // reconcile per-device listeners outside the lock. reconcile
    // bails as a no-op when no listener is registered.
    {
        std::unique_lock<std::shared_mutex> lock(callback_state_mutex_);
        id_to_uid_.clear();
        id_to_uid_.reserve(device_ids_.size());
        for (const auto& [uid, id] : device_ids_) {
            id_to_uid_[id] = uid;
        }
    }
    reconcilePerDeviceListeners();

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

void CoreAudioBackend::setDeviceChangeListener(DeviceChangeListener cb,
                                               void* user_data) {
    // F1: install (or tear down) HAL property listeners.
    //
    //   1. Tear down whatever's currently installed (control-thread-
    //      only state — no lock needed for the bookkeeping; the Apple
    //      Remove calls themselves can run unlocked).
    //   2. Publish the new cb/ud under exclusive lock briefly.
    //   3. If cb is non-null, install fresh listeners. Apple Add
    //      calls run unlocked; the per-device set is rebuilt from
    //      whatever id_to_uid_ currently holds (typically empty on
    //      first registration, populated after the first
    //      enumerate()).
    removeAllListeners();
    {
        std::unique_lock<std::shared_mutex> lock(callback_state_mutex_);
        device_change_cb_   = cb;
        device_change_user_ = user_data;
    }
    if (cb != nullptr) {
        installSystemListener();
        reconcilePerDeviceListeners();
    }
}

OSStatus CoreAudioBackend::halPropertyListenerCallback(
    AudioObjectID inObjectID,
    UInt32 inNumberAddresses,
    const AudioObjectPropertyAddress* inAddresses,
    void* inClientData) {
    auto* self = static_cast<CoreAudioBackend*>(inClientData);
    self->onHalPropertyEvent(inObjectID, inNumberAddresses, inAddresses);
    return noErr;
}

void CoreAudioBackend::onHalPropertyEvent(
    AudioObjectID object,
    UInt32 num_addresses,
    const AudioObjectPropertyAddress* addresses) {
    // Snapshot the listener + reverse map under shared lock so the
    // control thread can mutate state without contending. Then
    // dispatch translation outside the lock — including the
    // AudioObjectGetPropertyData read for IsAlive, which can take
    // arbitrary time and must not block the control thread.
    DeviceChangeListener cb;
    void*                ud;
    std::string          uid;
    {
        std::shared_lock<std::shared_mutex> lock(callback_state_mutex_);
        cb = device_change_cb_;
        ud = device_change_user_;
        if (cb == nullptr) return;
        if (object != kAudioObjectSystemObject) {
            auto it = id_to_uid_.find(object);
            if (it != id_to_uid_.end()) uid = it->second;
        }
    }

    for (UInt32 i = 0; i < num_addresses; ++i) {
        const AudioObjectPropertyAddress& addr = addresses[i];
        // For IsAlive, read the current value to distinguish the
        // alive→not-alive edge from the reverse.
        UInt32 is_alive_readback = 1;
        if (addr.mSelector == kAudioDevicePropertyDeviceIsAlive) {
            UInt32 size = sizeof(is_alive_readback);
            AudioObjectPropertyAddress query_addr = addr;
            if (AudioObjectGetPropertyData(object, &query_addr, 0, nullptr,
                                           &size, &is_alive_readback) != noErr) {
                // Read failure → treat as "not alive" so we surface the
                // event. The reaction layer is idempotent on the loss
                // path, so a spurious kDeviceIsNotAlive on a still-alive
                // device is corrected by the next list-changed pass.
                is_alive_readback = 0;
            }
        }
        const auto event = translateHalPropertyChange(
            addr.mSelector, uid, is_alive_readback);
        if (event.has_value()) {
            cb(*event, ud);
        }
    }
}

void CoreAudioBackend::installSystemListener() {
    // Control-thread only; no lock held.
    if (system_listener_installed_) return;
    AudioObjectPropertyAddress addr{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    const OSStatus err = AudioObjectAddPropertyListener(
        kAudioObjectSystemObject, &addr,
        &CoreAudioBackend::halPropertyListenerCallback, this);
    if (err == noErr) {
        system_listener_installed_ = true;
    } else {
        os_log_error(backendLog(),
                     "AddPropertyListener(system,Devices) failed: %d",
                     static_cast<int>(err));
    }
}

void CoreAudioBackend::removeAllListeners() {
    // Control-thread only; no lock held. Apple Remove blocks on
    // in-flight callbacks; not holding the mutex means the callback
    // can release the shared lock before Remove returns.
    for (const auto& entry : per_device_listeners_) {
        (void)AudioObjectRemovePropertyListener(
            entry.object, &entry.address,
            &CoreAudioBackend::halPropertyListenerCallback, this);
    }
    per_device_listeners_.clear();
    if (system_listener_installed_) {
        AudioObjectPropertyAddress addr{
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain};
        (void)AudioObjectRemovePropertyListener(
            kAudioObjectSystemObject, &addr,
            &CoreAudioBackend::halPropertyListenerCallback, this);
        system_listener_installed_ = false;
    }
}

void CoreAudioBackend::reconcilePerDeviceListeners() {
    // Control-thread only; no lock held during Apple calls.
    //
    // 1. Snapshot the current device set under the shared lock.
    // 2. Drop the lock.
    // 3. Diff against per_device_listeners_ (control-thread-only state).
    // 4. Call Apple Add/Remove with no lock held.
    std::unordered_map<AudioObjectID, std::string> snapshot;
    bool listener_active = false;
    {
        std::shared_lock<std::shared_mutex> lock(callback_state_mutex_);
        listener_active = (device_change_cb_ != nullptr);
        if (!listener_active) return;
        snapshot = id_to_uid_;
    }

    // Pass 1: remove entries whose AudioObjectID is no longer
    // enumerated.
    std::vector<HalListenerEntry> kept;
    kept.reserve(per_device_listeners_.size());
    for (const auto& entry : per_device_listeners_) {
        if (snapshot.find(entry.object) == snapshot.end()) {
            (void)AudioObjectRemovePropertyListener(
                entry.object, &entry.address,
                &CoreAudioBackend::halPropertyListenerCallback, this);
        } else {
            kept.push_back(entry);
        }
    }
    per_device_listeners_ = std::move(kept);

    auto alreadyHas = [&](AudioObjectID obj,
                          AudioObjectPropertySelector selector) {
        for (const auto& e : per_device_listeners_) {
            if (e.object == obj && e.address.mSelector == selector) return true;
        }
        return false;
    };

    // Pass 2: install IsAlive + (per-aggregate) ActiveSubDeviceList
    // listeners on any device that does not yet carry them.
    for (const auto& [id, uid] : snapshot) {
        if (!alreadyHas(id, kAudioDevicePropertyDeviceIsAlive)) {
            HalListenerEntry entry;
            entry.object  = id;
            entry.address = AudioObjectPropertyAddress{
                kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain};
            const OSStatus err = AudioObjectAddPropertyListener(
                id, &entry.address,
                &CoreAudioBackend::halPropertyListenerCallback, this);
            if (err == noErr) {
                per_device_listeners_.push_back(entry);
            } else {
                os_log_error(backendLog(),
                             "AddPropertyListener(IsAlive, %{public}s) failed: %d",
                             uid.c_str(), static_cast<int>(err));
            }
        }
        if (isAggregateDevice(id) &&
            !alreadyHas(id,
                        kAudioAggregateDevicePropertyActiveSubDeviceList)) {
            HalListenerEntry entry;
            entry.object  = id;
            entry.address = AudioObjectPropertyAddress{
                kAudioAggregateDevicePropertyActiveSubDeviceList,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain};
            const OSStatus err = AudioObjectAddPropertyListener(
                id, &entry.address,
                &CoreAudioBackend::halPropertyListenerCallback, this);
            if (err == noErr) {
                per_device_listeners_.push_back(entry);
            } else {
                os_log_error(backendLog(),
                             "AddPropertyListener(SubDeviceList, %{public}s) failed: %d",
                             uid.c_str(), static_cast<int>(err));
            }
        }
    }
}

bool CoreAudioBackend::isAggregateDevice(AudioDeviceID id) {
    AudioObjectPropertyAddress addr{
        kAudioAggregateDevicePropertyActiveSubDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr) {
        return false;
    }
    // Non-aggregate devices either return an error above or report a
    // zero size. Aggregate devices report > 0 even when empty.
    return size > 0;
}

bool CoreAudioBackend::closeCallback(IOProcId id) {
    if (id == kInvalidIOProcId) return true;
    auto it = ioprocs_.find(id);
    if (it == ioprocs_.end()) return true;

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
    return true;
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

namespace {

// Active sub-devices of an aggregate device, queried via the
// HAL property. Returns an empty list for non-aggregate devices.
std::vector<AudioObjectID> getActiveSubDevices(AudioObjectID aggregate) {
    AudioObjectPropertyAddress addr{
        kAudioAggregateDevicePropertyActiveSubDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(aggregate, &addr, 0, nullptr,
                                       &size) != noErr || size == 0) {
        return {};
    }
    const std::size_t count = size / sizeof(AudioObjectID);
    std::vector<AudioObjectID> ids(count);
    if (AudioObjectGetPropertyData(aggregate, &addr, 0, nullptr,
                                   &size, ids.data()) != noErr) {
        return {};
    }
    return ids;
}

// Single Core Audio property write. Returns the post-write
// readback so callers can log discrepancies (the HAL may clamp
// into the device's range; max-across-clients may force a
// larger value). RT-unsafe; control thread only.
std::uint32_t writeBufferFrameSize(AudioObjectID id,
                                   std::uint32_t frames) {
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    UInt32 value = frames;
    (void)AudioObjectSetPropertyData(id, &addr, 0, nullptr,
                                     sizeof(value), &value);
    UInt32 after = 0;
    UInt32 afterSize = sizeof(after);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr,
                                   &afterSize, &after) != noErr) {
        return 0;
    }
    return after;
}

}  // namespace

void CoreAudioBackend::setBufferFrameSize(const std::string& uid,
                                          std::uint32_t frames) {
    if (frames == 0) return;
    auto it = device_ids_.find(uid);
    if (it == device_ids_.end()) return;
    const AudioDeviceID id = it->second;

    // For aggregate devices the effective buffer is `max(member
    // buffer_frame_size)`. To get the aggregate to actually run at
    // `frames` the request has to land on each member; writing the
    // aggregate's own property is a no-op when a member is held
    // larger by another client. We enumerate the active sub-device
    // list and write to each member directly — exactly the way
    // Superior Drummer / any vanilla Core Audio client writes
    // `kAudioDevicePropertyBufferFrameSize`. No hog mode is
    // claimed here; macOS resolves with `max-across-clients` so
    // co-resident apps stay alive.
    for (AudioObjectID sub : getActiveSubDevices(id)) {
        const std::uint32_t applied = writeBufferFrameSize(sub, frames);
        os_log_info(backendLog(),
                    "buffer-frames request %u → 0x%x applied=%u (sub of %{public}s)",
                    frames, sub, applied, uid.c_str());
    }
    const std::uint32_t applied = writeBufferFrameSize(id, frames);
    os_log_info(backendLog(),
                "buffer-frames request %u → %{public}s applied=%u",
                frames, uid.c_str(), applied);
}

}  // namespace jbox::control
