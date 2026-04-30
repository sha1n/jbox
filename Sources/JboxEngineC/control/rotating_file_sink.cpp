// rotating_file_sink.cpp — implementation of the size-rotating
// plain-text log file sink, the compositeSink helper, and the
// defaultJboxLogPath resolver. See the header for the contract.

#include "rotating_file_sink.hpp"

#include "log_drainer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace jbox::control {

namespace fs = std::filesystem;

namespace {

// ISO-8601 UTC with millisecond precision. e.g. 2026-05-01T12:34:56.789Z
// Returned as a string for splice into the file line.
std::string formatNowIso8601Utc() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t   = clock::to_time_t(now);
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()) %
                     1000;
    std::tm tm_utc{};
    ::gmtime_r(&t, &tm_utc);
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  tm_utc.tm_hour,
                  tm_utc.tm_min,
                  tm_utc.tm_sec,
                  static_cast<long long>(ms.count()));
    return std::string(buf);
}

}  // namespace

// -----------------------------------------------------------------------------
// RotatingFileSink
// -----------------------------------------------------------------------------

RotatingFileSink::RotatingFileSink(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.keep_count < 1) cfg_.keep_count = 1;
}

RotatingFileSink::~RotatingFileSink() = default;

fs::path RotatingFileSink::rotatedFilePath(std::size_t idx) const {
    // Insert ".<idx>" before the original suffix, e.g.
    //   /path/to/jbox.log -> /path/to/jbox.<idx>.log
    fs::path p          = cfg_.log_path;
    const auto stem     = p.stem().string();      // "jbox"
    const auto ext      = p.extension().string(); // ".log"
    const auto parent   = p.parent_path();
    std::ostringstream ss;
    ss << stem << '.' << idx << ext;
    return parent / ss.str();
}

void RotatingFileSink::ensureOpen() {
    if (opened_ || !healthy_) return;
    std::error_code ec;
    const fs::path parent = cfg_.log_path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            healthy_ = false;
            return;
        }
    }
    // Pick up any pre-existing size so an interrupted run doesn't
    // keep growing the live file forever past the cap.
    if (fs::exists(cfg_.log_path, ec)) {
        const auto sz = fs::file_size(cfg_.log_path, ec);
        if (!ec) current_size_ = static_cast<std::size_t>(sz);
    }
    stream_.open(cfg_.log_path, std::ios::out | std::ios::app | std::ios::binary);
    if (!stream_.is_open() || !stream_.good()) {
        healthy_ = false;
        return;
    }
    opened_ = true;
}

void RotatingFileSink::rotate() {
    // Close before renaming. macOS allows rename on an open file but
    // closing first keeps the post-rotate stream pointed at a fresh
    // inode (otherwise the descriptor would still target the rotated
    // file).
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    std::error_code ec;

    if (cfg_.keep_count == 1) {
        // Live-only: truncate by removing and re-opening fresh.
        fs::remove(cfg_.log_path, ec);
    } else {
        // Drop the oldest survivor (becomes redundant after the shift).
        fs::remove(rotatedFilePath(cfg_.keep_count - 1), ec);

        // Shift `<base>.<i>.log` -> `<base>.<i+1>.log`, oldest first.
        for (std::size_t i = cfg_.keep_count - 2; i >= 1; --i) {
            const auto src = rotatedFilePath(i);
            const auto dst = rotatedFilePath(i + 1);
            if (fs::exists(src, ec)) {
                fs::rename(src, dst, ec);
            }
        }
        // Live file becomes `.1.log`.
        if (fs::exists(cfg_.log_path, ec)) {
            fs::rename(cfg_.log_path, rotatedFilePath(1), ec);
            if (ec) {
                healthy_ = false;
                return;
            }
        }
    }

    current_size_ = 0;
    opened_       = false;
    ensureOpen();
}

void RotatingFileSink::formatLine(const jbox::rt::RtLogEvent& event,
                                  std::string& out) {
    out.clear();
    out.append(formatNowIso8601Utc());
    out.append("  jbox evt=");
    out.append(logCodeName(event.code));
    char tail[160];
    std::snprintf(tail, sizeof(tail),
                  " route=%u a=%llu b=%llu ts=%llu\n",
                  static_cast<unsigned>(event.route_id),
                  static_cast<unsigned long long>(event.value_a),
                  static_cast<unsigned long long>(event.value_b),
                  static_cast<unsigned long long>(event.timestamp));
    out.append(tail);
}

void RotatingFileSink::operator()(const jbox::rt::RtLogEvent& event) {
    if (!healthy_) return;
    ensureOpen();
    if (!healthy_) return;

    std::string line;
    formatLine(event, line);

    // Pre-write rotation: if appending would push the live file past
    // the cap, rotate first so the cap is honoured. Skip when the
    // live file is empty — a single line larger than the cap is
    // accepted into a fresh file rather than recursively rotating.
    if (current_size_ > 0 && current_size_ + line.size() > cfg_.max_bytes_per_file) {
        rotate();
        if (!healthy_) return;
    }

    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream_.flush();
    if (!stream_.good()) {
        healthy_ = false;
        return;
    }
    current_size_ += line.size();
}

// -----------------------------------------------------------------------------
// compositeSink
// -----------------------------------------------------------------------------

LogDrainer::Sink compositeSink(std::vector<LogDrainer::Sink> sinks) {
    return [sinks = std::move(sinks)](const jbox::rt::RtLogEvent& event) {
        for (const auto& s : sinks) {
            if (s) s(event);
        }
    };
}

// -----------------------------------------------------------------------------
// defaultJboxLogPath
// -----------------------------------------------------------------------------

fs::path defaultJboxLogPath(const std::string& process_basename) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return fs::path(home) / "Library" / "Logs" / "Jbox" /
           (process_basename + ".log");
}

}  // namespace jbox::control
