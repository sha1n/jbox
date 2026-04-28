// core_audio_hal_translation.hpp — pure HAL→DeviceChangeEvent translator.
//
// CoreAudioBackend installs HAL property listeners via
// AudioObjectAddPropertyListener (function-pointer variant) on the
// system object + each enumerated device + each aggregate. Apple
// invokes the static thunk on a HAL-internal thread; the thunk
// forwards to a member that, after reading any per-event state under
// a shared lock, ends up here. This translator owns the pure-logic
// step: given (selector, device_uid, IsAlive readback), what — if any
// — DeviceChangeEvent should fire?
//
// Splitting the translation out keeps the testable kernel separable
// from the genuinely hardware-bound listener plumbing (Apple Add /
// Remove calls, the readback round-trip, the cross-thread state).
//
// See docs/followups.md § F1.

#ifndef JBOX_CONTROL_CORE_AUDIO_HAL_TRANSLATION_HPP
#define JBOX_CONTROL_CORE_AUDIO_HAL_TRANSLATION_HPP

#include "device_backend.hpp"

#include <CoreAudio/CoreAudio.h>

#include <optional>
#include <string>

namespace jbox::control {

// Translate one HAL property-change observation into a
// DeviceChangeEvent, or std::nullopt if the property is unrelated or
// signals no event.
//
// `selector`           — the HAL property that fired.
// `device_uid`         — UID of the device the listener was bound to.
//                         Ignored for selectors scoped to the system
//                         object (kAudioHardwarePropertyDevices).
// `is_alive_readback`  — current value of kAudioDevicePropertyDeviceIs
//                         Alive, queried by the caller when the
//                         selector is kAudioDevicePropertyDeviceIsAlive.
//                         Ignored otherwise; pass 1 (or any value) when
//                         the selector is something else.
std::optional<DeviceChangeEvent> translateHalPropertyChange(
    AudioObjectPropertySelector selector,
    const std::string& device_uid,
    UInt32 is_alive_readback);

}  // namespace jbox::control

#endif  // JBOX_CONTROL_CORE_AUDIO_HAL_TRANSLATION_HPP
