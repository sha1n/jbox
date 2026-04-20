// log_drainer.cpp — background thread that drains the RT log queue
// into a user-provided sink (os_log by default).

#include "log_drainer.hpp"

#include "rt_log_codes.hpp"

#include <os/log.h>

#include <utility>

namespace jbox::control {

namespace {

// One shared log object for the engine. `os_log_create` is safe to
// call repeatedly with the same arguments (the OS dedupes), but a
// single static avoids the syscall on every event.
os_log_t engineLog() {
    static os_log_t log = os_log_create("com.jbox.app", "engine");
    return log;
}

// Human-readable name for a log code. Falls through to "unknown" so a
// forward-looking producer (newer code, older drainer) still produces
// readable output.
const char* codeName(jbox::rt::RtLogCode code) {
    switch (code) {
        case jbox::rt::kLogNone:             return "none";
        case jbox::rt::kLogUnderrun:         return "underrun";
        case jbox::rt::kLogOverrun:          return "overrun";
        case jbox::rt::kLogChannelMismatch:  return "channel_mismatch";
        case jbox::rt::kLogConverterShort:   return "converter_short";
        case jbox::rt::kLogRouteStarted:     return "route_started";
        case jbox::rt::kLogRouteStopped:     return "route_stopped";
        case jbox::rt::kLogRouteWaiting:     return "route_waiting";
        case jbox::rt::kLogRouteError:       return "route_error";
    }
    return "unknown";
}

}  // namespace

// -----------------------------------------------------------------------------
// Default sink
// -----------------------------------------------------------------------------

void LogDrainer::defaultOsLogSink(const jbox::rt::RtLogEvent& event) {
    os_log(engineLog(),
           "jbox evt=%{public}s route=%u a=%llu b=%llu ts=%llu",
           codeName(event.code),
           event.route_id,
           static_cast<unsigned long long>(event.value_a),
           static_cast<unsigned long long>(event.value_b),
           static_cast<unsigned long long>(event.timestamp));
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

LogDrainer::LogDrainer()
    : LogDrainer(&LogDrainer::defaultOsLogSink, std::chrono::milliseconds(100)) {}

LogDrainer::LogDrainer(Sink sink, std::chrono::milliseconds poll_interval)
    : poll_interval_(poll_interval),
      sink_(std::move(sink)) {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { runLoop(); });
}

LogDrainer::~LogDrainer() { stop(); }

void LogDrainer::stop() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running && thread_.joinable()) {
        thread_.join();
    }
}

void LogDrainer::setSink(Sink sink) {
    std::lock_guard<std::mutex> g(sink_mu_);
    sink_ = std::move(sink);
}

bool LogDrainer::waitForEmpty(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (queue_.approxSize() > 0) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// -----------------------------------------------------------------------------
// Consumer loop
// -----------------------------------------------------------------------------

void LogDrainer::runLoop() {
    jbox::rt::RtLogEvent event{};
    while (running_.load(std::memory_order_acquire)) {
        bool drained_any = false;
        while (queue_.tryPop(event)) {
            Sink sink_copy;
            {
                std::lock_guard<std::mutex> g(sink_mu_);
                sink_copy = sink_;
            }
            if (sink_copy) {
                sink_copy(event);
                delivered_.fetch_add(1, std::memory_order_relaxed);
            }
            drained_any = true;
        }
        if (!drained_any) {
            std::this_thread::sleep_for(poll_interval_);
        }
    }

    // Final drain on shutdown so callers that pushed shortly before
    // stop() still see their events delivered (useful for tests and
    // for operator-visible shutdown notes).
    while (queue_.tryPop(event)) {
        Sink sink_copy;
        {
            std::lock_guard<std::mutex> g(sink_mu_);
            sink_copy = sink_;
        }
        if (sink_copy) {
            sink_copy(event);
            delivered_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace jbox::control
