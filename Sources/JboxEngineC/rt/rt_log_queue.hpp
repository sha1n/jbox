// rt_log_queue.hpp — lock-free SPSC queue of fixed-size log records.
//
// The RT (audio) thread cannot call `os_log_*`, `printf`, or any other
// logging routine that may allocate or syscall. Instead it enqueues
// opaque RtLogEvent records into this queue via `tryPush(...)`, which
// is lock-free, allocation-free, and syscall-free. A background
// drainer thread calls `tryPop(...)` at a slow cadence (~100 Hz) and
// translates the records into real log output.
//
// Concurrency model: single-producer (RT thread) / single-consumer
// (drainer thread). Multi-producer or multi-consumer use is not
// supported and will corrupt the queue.
//
// Memory ordering follows the classical SPSC pattern:
//   * Producer does a relaxed load of its own index (head), acquire
//     load of the consumer's index (tail) to establish that freed
//     slots are visible, writes the payload, then release-stores the
//     new head.
//   * Consumer mirrors: relaxed load of tail, acquire load of head,
//     reads payload, release-stores the new tail.
//
// See docs/spec.md §§ 2.9, 2.10.

#ifndef JBOX_RT_RT_LOG_QUEUE_HPP
#define JBOX_RT_RT_LOG_QUEUE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace jbox::rt {

// Event code for log records. Plain uint32_t rather than enum to
// decouple the queue type from any particular event taxonomy; callers
// keep their own enum of codes. Zero is reserved as "no event".
using RtLogCode = std::uint32_t;

// A single record. Fixed-size, POD-like; trivially copyable so that
// aggregate assignment inside tryPush / tryPop is a bit-for-bit copy.
//
// Fields are numeric only — no pointers, no variable-length data.
// If richer diagnostic context is needed, encode it in `value_a` /
// `value_b` (e.g., two 32-bit fields packed into a uint64_t) and
// interpret in the drainer.
struct RtLogEvent {
    std::uint64_t timestamp;   // opaque monotonic counter; producer chooses the clock.
    RtLogCode     code;        // numeric event code; 0 means "none".
    std::uint32_t route_id;    // associated route id (0 if unrelated to any route).
    std::uint64_t value_a;     // event-specific payload.
    std::uint64_t value_b;     // event-specific payload.
};

static_assert(sizeof(RtLogEvent) == 32, "RtLogEvent must stay compact (32 bytes)");

// Fixed-capacity lock-free SPSC queue.
//
// `Capacity` must be a power of two. One slot is reserved to
// distinguish full from empty, so the usable capacity is
// `Capacity - 1` records.
template <std::size_t Capacity>
class RtLogQueue {
    static_assert(Capacity >= 2, "RtLogQueue capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "RtLogQueue capacity must be a power of two");

public:
    RtLogQueue() noexcept = default;

    // Non-copyable, non-movable.
    RtLogQueue(const RtLogQueue&) = delete;
    RtLogQueue& operator=(const RtLogQueue&) = delete;
    RtLogQueue(RtLogQueue&&) = delete;
    RtLogQueue& operator=(RtLogQueue&&) = delete;

    // Producer side. RT-safe: no allocation, no syscalls, no locks.
    // Returns false if the queue is full (caller should drop the
    // event; future extensions may increment a drop counter).
    bool tryPush(const RtLogEvent& event) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kIndexMask;
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (next == tail) {
            return false;  // full
        }
        buffer_[head] = event;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Called from the drainer thread, not from RT.
    // Returns false if the queue is empty.
    bool tryPop(RtLogEvent& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;  // empty
        }
        out = buffer_[tail];
        tail_.store((tail + 1) & kIndexMask, std::memory_order_release);
        return true;
    }

    // Approximate observer. Safe to call from any thread but may
    // return a stale value; intended for metrics, not correctness.
    std::size_t approxSize() const noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        return (head + Capacity - tail) & kIndexMask;
    }

    // Maximum simultaneously-live events. Accounts for the reserved slot.
    static constexpr std::size_t usableCapacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr std::size_t kIndexMask = Capacity - 1;

    std::array<RtLogEvent, Capacity> buffer_{};
    std::atomic<std::size_t> head_{0};  // next slot to write (producer)
    std::atomic<std::size_t> tail_{0};  // next slot to read (consumer)
};

// Default-sized queue used throughout the engine. 1023 usable records
// at 32 bytes each = 32 KiB backing storage. At ~100 Hz drain rate,
// this gives roughly 10 seconds of buffering at the drainer's
// sustainable rate.
using DefaultRtLogQueue = RtLogQueue<1024>;

}  // namespace jbox::rt

#endif  // JBOX_RT_RT_LOG_QUEUE_HPP
