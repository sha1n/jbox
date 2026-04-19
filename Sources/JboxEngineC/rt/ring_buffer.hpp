// ring_buffer.hpp — lock-free SPSC ring buffer for interleaved audio samples.
//
// One instance bridges a source IOProc (producer, writes input samples)
// and a destination IOProc (consumer, reads samples to send to output).
// Both IOProc callbacks are RT-safe, so this class is RT-safe end-to-end:
// no allocation, no locks, no syscalls, no exceptions.
//
// The caller owns the backing storage. The class is allocation-free by
// design — callers in control/ provide a pre-allocated float buffer
// (typically via std::unique_ptr<float[]> or a stack array for tests).
//
// Layout
// ------
// Samples are stored interleaved: frame[i][ch] at
// `storage[i * channels_ + ch]`. The buffer capacity is expressed in
// FRAMES; total sample storage = capacity_frames * channels. One frame
// slot is reserved to distinguish full from empty, so the usable
// capacity is (capacity_frames - 1) frames.
//
// Memory ordering
// ---------------
// Classical SPSC pattern (same as RtLogQueue):
//   Producer: relaxed load own (head), acquire load peer (tail),
//             write payload, release store own.
//   Consumer: relaxed load own (tail), acquire load peer (head),
//             read payload, release store own.
//
// Overrun / underrun
// ------------------
// If a writer sees no space (buffer full), writeFrames returns less
// than the requested frame count (possibly zero). If a reader sees no
// data, readFrames returns a short count similarly. Higher layers
// translate these conditions into log events and drift-tracker input.
// The RingBuffer itself never asserts or terminates.
//
// See docs/spec.md §§ 2.4, 2.10.

#ifndef JBOX_RT_RING_BUFFER_HPP
#define JBOX_RT_RING_BUFFER_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace jbox::rt {

class RingBuffer {
public:
    // Construct over externally-owned storage.
    //   storage          : pointer to at least (capacity_frames * channels) floats.
    //   capacity_frames  : total frame slots including the one reserved for
    //                      the full/empty distinction. Must be >= 2.
    //   channels         : samples per frame. Must be >= 1.
    //
    // The buffer starts empty. Construction does not touch the storage.
    RingBuffer(float* storage,
               std::size_t capacity_frames,
               std::size_t channels) noexcept
        : storage_(storage),
          capacity_frames_(capacity_frames),
          channels_(channels) {}

    // Non-copyable, non-movable: atomic indices cannot be relocated,
    // and the storage ownership is external.
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // Producer side. RT-safe. Copies up to `frames` frames from `src`
    // (laid out as channels_-interleaved samples) into the buffer.
    // Returns the number of frames actually written; may be less than
    // requested if the buffer fills. On full return 0.
    std::size_t writeFrames(const float* src, std::size_t frames) noexcept {
        if (frames == 0) return 0;
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        const std::size_t space =
            (tail + capacity_frames_ - head - 1) % capacity_frames_;
        const std::size_t to_write = std::min(frames, space);
        if (to_write == 0) return 0;

        // Up to two contiguous chunks (with possible wrap-around).
        const std::size_t first = std::min(to_write, capacity_frames_ - head);
        std::memcpy(storage_ + head * channels_,
                    src,
                    first * channels_ * sizeof(float));
        if (to_write > first) {
            const std::size_t second = to_write - first;
            std::memcpy(storage_,
                        src + first * channels_,
                        second * channels_ * sizeof(float));
        }

        const std::size_t new_head = (head + to_write) % capacity_frames_;
        head_.store(new_head, std::memory_order_release);
        return to_write;
    }

    // Consumer side. RT-safe. Copies up to `frames` frames into `dst`.
    // Returns the number of frames actually read; may be less than
    // requested if the buffer drains. On empty return 0.
    std::size_t readFrames(float* dst, std::size_t frames) noexcept {
        if (frames == 0) return 0;
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        const std::size_t available =
            (head + capacity_frames_ - tail) % capacity_frames_;
        const std::size_t to_read = std::min(frames, available);
        if (to_read == 0) return 0;

        const std::size_t first = std::min(to_read, capacity_frames_ - tail);
        std::memcpy(dst,
                    storage_ + tail * channels_,
                    first * channels_ * sizeof(float));
        if (to_read > first) {
            const std::size_t second = to_read - first;
            std::memcpy(dst + first * channels_,
                        storage_,
                        second * channels_ * sizeof(float));
        }

        const std::size_t new_tail = (tail + to_read) % capacity_frames_;
        tail_.store(new_tail, std::memory_order_release);
        return to_read;
    }

    // Approximate observers. Safe to call from any thread; values may
    // be stale by the time the caller acts on them. Intended for
    // metrics / drift tracking, not correctness gating.
    std::size_t framesAvailableForRead() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head + capacity_frames_ - tail) % capacity_frames_;
    }

    std::size_t framesAvailableForWrite() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (tail + capacity_frames_ - head - 1) % capacity_frames_;
    }

    std::size_t channels() const noexcept { return channels_; }
    std::size_t capacityFrames() const noexcept { return capacity_frames_; }
    std::size_t usableCapacityFrames() const noexcept { return capacity_frames_ - 1; }

private:
    float* storage_;
    std::size_t capacity_frames_;
    std::size_t channels_;
    std::atomic<std::size_t> head_{0};  // next frame to write (producer index)
    std::atomic<std::size_t> tail_{0};  // next frame to read  (consumer index)
};

}  // namespace jbox::rt

#endif  // JBOX_RT_RING_BUFFER_HPP
