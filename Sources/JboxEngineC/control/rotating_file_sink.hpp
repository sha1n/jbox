// rotating_file_sink.hpp — size-rotating plain-text file sink for the
// engine log pipeline.
//
// Sits behind a `LogDrainer::Sink` callable so it can be composed with
// the existing `defaultOsLogSink` via `compositeSink(...)`. Both
// destinations receive every drained event; failure on the file side
// (permission denied, disk full, parent dir unwritable) downgrades the
// file sink to fail-silent — `isHealthy()` flips to false and further
// invocations no-op — while the os_log side keeps running. This is the
// "fall back to os_log-only rather than dropping the whole pipeline"
// contract from `docs/plan.md` Phase 8.
//
// Threading: not internally synchronized. Designed for the single
// `LogDrainer` consumer thread. If a future caller wires the sink
// elsewhere, wrap externally.
//
// File format (one event per line):
//
//   <ISO-8601 UTC>  jbox evt=<name> route=<u32> a=<u64> b=<u64> ts=<u64>
//
// where the body after the timestamp matches the existing
// `defaultOsLogSink` line body so file and Console output align.
//
// Rotation strategy (size-based):
//   - Tracks bytes written to the live file in-process.
//   - On a write that would push the file over `max_bytes_per_file`,
//     rotates first then writes the event into a fresh live file.
//   - Rotation shifts: live `<base>.log` → `<base>.1.log`,
//     `<base>.1.log` → `<base>.2.log`, …, oldest is deleted.
//   - `keep_count` includes the live file. keep_count = 3 caps the
//     directory at `<base>.log` + `<base>.1.log` + `<base>.2.log`.
//   - Minimum keep_count is 1 (no rotated files retained).
//
// See `docs/spec.md § 2.9` for the broader logging-pipeline contract.

#ifndef JBOX_CONTROL_ROTATING_FILE_SINK_HPP
#define JBOX_CONTROL_ROTATING_FILE_SINK_HPP

#include "log_drainer.hpp"
#include "rt_log_codes.hpp"
#include "rt_log_queue.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jbox::control {

class RotatingFileSink {
public:
    struct Config {
        // Absolute path of the live log file.  Parent directory will
        // be created on first write if missing.
        std::filesystem::path log_path;
        // Cap before rotation. Soft — a single line may push the file
        // marginally past the cap, then rotation runs on the next
        // write. Pick something comfortably larger than one log line.
        std::size_t max_bytes_per_file;
        // Total files retained, live + rotated. Must be >= 1.
        std::size_t keep_count;
    };

    explicit RotatingFileSink(Config cfg);
    ~RotatingFileSink();

    RotatingFileSink(const RotatingFileSink&) = delete;
    RotatingFileSink& operator=(const RotatingFileSink&) = delete;

    // Sink invocation. Lazily creates the parent directory and opens
    // the live file on first call. Failures flip `healthy_` to false
    // and the call returns silently.
    void operator()(const jbox::rt::RtLogEvent& event);

    // True once the sink has been opened and no I/O failure has been
    // observed. Starts true; only flips false on failure. Inspectable
    // by callers that want to know whether file logging is live.
    bool isHealthy() const noexcept { return healthy_; }

    // Bytes written to the live file since the last rotation (or
    // since open). Test introspection.
    std::size_t liveBytes() const noexcept { return current_size_; }

    // Path of rotated file at index `idx` (1-based — `<base>.1.log`,
    // `<base>.2.log`, …). Returned even if the file does not exist
    // on disk; callers test existence themselves.
    std::filesystem::path rotatedFilePath(std::size_t idx) const;

private:
    void ensureOpen();
    void rotate();
    static void formatLine(const jbox::rt::RtLogEvent& event,
                           std::string& out);

    Config        cfg_;
    std::ofstream stream_;
    std::size_t   current_size_ = 0;
    bool          opened_       = false;
    bool          healthy_      = true;
};

// Compose multiple drainer sinks into one. Each sub-sink is invoked
// in iteration order on every event. Sub-sinks are captured by value;
// the resulting Sink is safe to hand to a `LogDrainer` for the
// drainer's full lifetime. Empty input vector yields a no-op sink.
LogDrainer::Sink compositeSink(std::vector<LogDrainer::Sink> sinks);

// Resolve the canonical per-process log file path:
//
//   $HOME/Library/Logs/Jbox/<process_basename>.log
//
// `process_basename` is what differentiates app vs. CLI logs ("Jbox"
// for the .app, "JboxEngineCLI" for the headless CLI). Returns an
// empty path if `$HOME` is unset, signalling the caller to skip the
// file sink and let the os_log path run alone.
std::filesystem::path defaultJboxLogPath(const std::string& process_basename);

}  // namespace jbox::control

#endif  // JBOX_CONTROL_ROTATING_FILE_SINK_HPP
