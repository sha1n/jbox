// core_audio_hal_translation.cpp — see header for the contract.

#include "core_audio_hal_translation.hpp"

namespace jbox::control {

std::optional<DeviceChangeEvent> translateHalPropertyChange(
    AudioObjectPropertySelector selector,
    const std::string& device_uid,
    UInt32 is_alive_readback) {
    switch (selector) {
        case kAudioHardwarePropertyDevices:
            // System-scope event with no specific subject. The
            // listener was bound to the system object; the device_uid
            // argument is ignored.
            return DeviceChangeEvent{DeviceChangeEvent::kDeviceListChanged, ""};

        case kAudioDevicePropertyDeviceIsAlive:
            // Only the alive→not-alive edge is interesting here. The
            // reverse edge is reported via kAudioHardwarePropertyDevices
            // (the listener fires when the device becomes enumerable
            // again).
            if (is_alive_readback == 0) {
                return DeviceChangeEvent{
                    DeviceChangeEvent::kDeviceIsNotAlive, device_uid};
            }
            return std::nullopt;

        case kAudioAggregateDevicePropertyActiveSubDeviceList:
            return DeviceChangeEvent{
                DeviceChangeEvent::kAggregateMembersChanged, device_uid};

        default:
            return std::nullopt;
    }
}

}  // namespace jbox::control
