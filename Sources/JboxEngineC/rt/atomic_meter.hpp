// atomic_meter.hpp — per-channel peak-tracking meter, RT-safe.
//
// The RT (audio) thread calls `updateMax(channel, abs_value)` every
// block to feed the absolute sample amplitude into the meter. The UI
// thread periodically calls `readAndReset(channel)` to read the peak
// accumulated since the last read and reset it to zero.
//
// Memory ordering is `relaxed` throughout: peak values are self-
// consistent and don't need to synchronise with other state.
//
// Concurrency behaviour
// ---------------------
// The pattern is single-producer (RT thread, `updateMax`) and
// single-consumer (UI thread, `readAndReset`). Updates use a
// compare-exchange loop so that interleaving with the consumer
// cannot lose a strictly-greater peak that arrived during the read.
// One write may be "lost" in the interleaving (the update-max sees
// the reset-to-zero and writes its value on top of 0), but that is
// the expected behaviour for peak metering.
//
// See docs/spec.md §§ 2.8, 2.10.

#ifndef JBOX_RT_ATOMIC_METER_HPP
#define JBOX_RT_ATOMIC_METER_HPP

#include <array>
#include <atomic>
#include <cstddef>

namespace jbox::rt {

// Maximum channel count supported by a single AtomicMeter instance.
// Sized generously above any realistic device channel count (V31: 32,
// Apollo: ~18 physical + 16 virtual = ~34). A single meter occupies
// `MaxChannels * sizeof(std::atomic<float>) = 256` bytes at this size.
inline constexpr std::size_t kAtomicMeterMaxChannels = 64;

// Value reported when a channel index is out of range. A sentinel
// rather than an assert so the RT path stays bounded even under bugs.
inline constexpr float kAtomicMeterInvalidChannel = -1.0f;

class AtomicMeter {
public:
    AtomicMeter() noexcept {
        for (auto& peak : peaks_) {
            peak.store(0.0f, std::memory_order_relaxed);
        }
    }

    // Non-copyable, non-movable: atomics cannot be safely relocated.
    AtomicMeter(const AtomicMeter&) = delete;
    AtomicMeter& operator=(const AtomicMeter&) = delete;
    AtomicMeter(AtomicMeter&&) = delete;
    AtomicMeter& operator=(AtomicMeter&&) = delete;

    // RT-safe. Atomic compare-exchange loop — bounded retries, no
    // allocation, no lock. Monotonic: only ever raises the stored value.
    // Caller is expected to pass a non-negative amplitude; negative
    // values are accepted but treated as "no update" since any stored
    // value will be >= 0.
    void updateMax(std::size_t channel, float abs_value) noexcept {
        if (channel >= kAtomicMeterMaxChannels) {
            return;
        }
        float current = peaks_[channel].load(std::memory_order_relaxed);
        while (abs_value > current) {
            if (peaks_[channel].compare_exchange_weak(
                    current, abs_value,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
            // compare_exchange_weak writes the observed value into
            // `current` on failure; loop re-checks against fresh value.
        }
    }

    // RT-safe-enough for UI polling; this is the read side, not called
    // from the audio thread. Returns the peak since the last read and
    // atomically resets the stored peak to zero.
    float readAndReset(std::size_t channel) noexcept {
        if (channel >= kAtomicMeterMaxChannels) {
            return kAtomicMeterInvalidChannel;
        }
        return peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
    }

    // Non-resetting peek. Useful for tests and non-destructive reads.
    float peek(std::size_t channel) const noexcept {
        if (channel >= kAtomicMeterMaxChannels) {
            return kAtomicMeterInvalidChannel;
        }
        return peaks_[channel].load(std::memory_order_relaxed);
    }

    static constexpr std::size_t capacity() noexcept {
        return kAtomicMeterMaxChannels;
    }

private:
    std::array<std::atomic<float>, kAtomicMeterMaxChannels> peaks_{};
};

}  // namespace jbox::rt

#endif  // JBOX_RT_ATOMIC_METER_HPP
