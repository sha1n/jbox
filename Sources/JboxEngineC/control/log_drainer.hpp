// log_drainer.hpp — background drainer for the RT log queue.
//
// Owns a `DefaultRtLogQueue` and a single consumer thread. Producers
// (RT and control code) push `RtLogEvent` records into the queue; the
// drainer pops them at a low cadence (default ~100 ms) and hands each
// event to a Sink callback. In production the sink writes to `os_log`;
// tests install a capture sink to assert on drained events.
//
// Lifecycle: construction starts the thread; `stop()` (or destruction)
// joins it. Safe to `stop()` multiple times.
//
// Thread model:
//   - RT producer (audio thread):   tryPush via `queue()`, no locks.
//   - Control producer (any non-RT thread): same queue(), lock-free.
//   - Consumer (this class' internal thread): tryPop + sink call.
//   - Exactly one consumer. Do not call `queue().tryPop` elsewhere.

#ifndef JBOX_CONTROL_LOG_DRAINER_HPP
#define JBOX_CONTROL_LOG_DRAINER_HPP

#include "rt_log_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>

namespace jbox::control {

class LogDrainer {
public:
    // Called once per drained event on the drainer thread.
    // Implementations must not re-enter the queue.
    using Sink = std::function<void(const jbox::rt::RtLogEvent&)>;

    // Construct a drainer with the default os_log sink and the default
    // poll interval (100 ms). The thread starts immediately.
    LogDrainer();

    // Test / advanced construction: custom sink and poll interval.
    LogDrainer(Sink sink, std::chrono::milliseconds poll_interval);

    ~LogDrainer();

    LogDrainer(const LogDrainer&) = delete;
    LogDrainer& operator=(const LogDrainer&) = delete;

    // Access to the underlying queue for producers. The pointer stays
    // valid for the drainer's lifetime.
    jbox::rt::DefaultRtLogQueue* queue() noexcept { return &queue_; }

    // Idempotent. After stop(), the drainer thread is joined and no
    // further events will be sunk, but the queue itself remains usable
    // (additional pushes will simply accumulate until destruction).
    void stop();

    // Test helper: block until the queue is empty OR `timeout` elapses.
    // Returns true if empty. Not part of the production contract.
    bool waitForEmpty(std::chrono::milliseconds timeout);

    // Test helper: replace the sink. Only callable before the first
    // pop (or after stop()) to keep the sink stable while the thread
    // is running; internally serialised with a mutex just to be safe.
    void setSink(Sink sink);

    // Count of events the drainer has delivered since start. For tests.
    std::size_t deliveredCount() const noexcept {
        return delivered_.load(std::memory_order_relaxed);
    }

    // Default os_log-based sink. Exposed so other callers (e.g. the
    // bridge) can log using the same formatting for one-off messages.
    static void defaultOsLogSink(const jbox::rt::RtLogEvent& event);

private:
    void runLoop();

    jbox::rt::DefaultRtLogQueue queue_;
    std::chrono::milliseconds   poll_interval_;
    Sink                        sink_;
    std::mutex                  sink_mu_;

    std::atomic<bool>           running_{false};
    std::atomic<std::size_t>    delivered_{0};
    std::thread                 thread_;
};

// Stringify an RT log code for human-readable output. Used by both
// `defaultOsLogSink` (os_log line) and `RotatingFileSink` (file line)
// so the two destinations show identical event names. Returns
// "unknown" for codes a future producer adds before the drainer is
// updated to know about them.
const char* logCodeName(jbox::rt::RtLogCode code) noexcept;

}  // namespace jbox::control

#endif  // JBOX_CONTROL_LOG_DRAINER_HPP
