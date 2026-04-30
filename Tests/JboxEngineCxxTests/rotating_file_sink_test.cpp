// rotating_file_sink_test.cpp — unit tests for the rotating-file log
// sink, the compositeSink helper, and the defaultJboxLogPath resolver.
//
// File-system tests use a per-test temporary directory under
// `std::filesystem::temp_directory_path()` and clean it up at the end
// of each case. No real `~/Library/Logs/Jbox/` access.

#include <catch_amalgamated.hpp>

#include "log_drainer.hpp"
#include "rotating_file_sink.hpp"
#include "rt_log_codes.hpp"
#include "rt_log_queue.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using jbox::control::compositeSink;
using jbox::control::defaultJboxLogPath;
using jbox::control::logCodeName;
using jbox::control::LogDrainer;
using jbox::control::RotatingFileSink;
using jbox::rt::RtLogEvent;

namespace {

// One-shot temp dir scoped to a single test case. Removes the tree on
// destruction so a failing assertion still cleans up.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937_64    gen(rd());
        std::ostringstream ss;
        ss << "jbox-rotating-sink-test-" << std::hex << gen();
        path_ = fs::temp_directory_path() / ss.str();
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

RtLogEvent makeEvent(std::uint32_t code, std::uint32_t route_id,
                     std::uint64_t a = 0, std::uint64_t b = 0,
                     std::uint64_t ts = 0) {
    return RtLogEvent{ts, code, route_id, a, b};
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f.good());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::size_t countLines(const std::string& content) {
    if (content.empty()) return 0;
    std::size_t n = 0;
    for (char c : content) {
        if (c == '\n') ++n;
    }
    return n;
}

}  // namespace

// -----------------------------------------------------------------------------
// logCodeName
// -----------------------------------------------------------------------------

TEST_CASE("logCodeName: known codes produce stable names",
          "[rotating_file_sink][log_code_name]") {
    REQUIRE(std::string(logCodeName(jbox::rt::kLogNone))            == "none");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogUnderrun))        == "underrun");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogOverrun))         == "overrun");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogChannelMismatch)) == "channel_mismatch");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogConverterShort))  == "converter_short");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogRouteStarted))    == "route_started");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogRouteStopped))    == "route_stopped");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogRouteWaiting))    == "route_waiting");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogRouteError))      == "route_error");
    REQUIRE(std::string(logCodeName(jbox::rt::kLogTeardownFailure)) == "teardown_failure");
}

TEST_CASE("logCodeName: unknown code falls through to 'unknown'",
          "[rotating_file_sink][log_code_name]") {
    REQUIRE(std::string(logCodeName(99999)) == "unknown");
}

// -----------------------------------------------------------------------------
// RotatingFileSink — basic write + format
// -----------------------------------------------------------------------------

TEST_CASE("RotatingFileSink: writes a single event with the expected line shape",
          "[rotating_file_sink][format]") {
    TempDir td;
    const fs::path log_path = td.path() / "jbox.log";

    RotatingFileSink sink(RotatingFileSink::Config{
        log_path, /*max_bytes_per_file=*/1024 * 1024, /*keep_count=*/3});
    sink(makeEvent(jbox::rt::kLogRouteStarted, /*route=*/42,
                   /*a=*/2, /*b=*/4, /*ts=*/12345));

    REQUIRE(sink.isHealthy());
    REQUIRE(fs::exists(log_path));

    const std::string content = readFile(log_path);
    REQUIRE(countLines(content) == 1);
    // ISO-8601 UTC prefix: YYYY-MM-DDTHH:MM:SS<frac>Z then two spaces.
    static const std::regex iso_prefix(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z  )");
    REQUIRE(std::regex_search(content, iso_prefix));
    // Body identical to what defaultOsLogSink would have produced.
    REQUIRE(content.find("jbox evt=route_started route=42 a=2 b=4 ts=12345")
            != std::string::npos);
    REQUIRE(content.back() == '\n');
    REQUIRE(sink.liveBytes() == content.size());
}

TEST_CASE("RotatingFileSink: appends multiple events as separate lines",
          "[rotating_file_sink][format]") {
    TempDir td;
    const fs::path log_path = td.path() / "jbox.log";

    RotatingFileSink sink(RotatingFileSink::Config{
        log_path, 1024 * 1024, 3});
    sink(makeEvent(jbox::rt::kLogUnderrun,     1, 10));
    sink(makeEvent(jbox::rt::kLogOverrun,      1, 20));
    sink(makeEvent(jbox::rt::kLogRouteStarted, 1));

    const std::string content = readFile(log_path);
    REQUIRE(countLines(content) == 3);
    REQUIRE(content.find("evt=underrun")      != std::string::npos);
    REQUIRE(content.find("evt=overrun")       != std::string::npos);
    REQUIRE(content.find("evt=route_started") != std::string::npos);
    REQUIRE(sink.liveBytes() == content.size());
}

TEST_CASE("RotatingFileSink: creates the parent directory on first write",
          "[rotating_file_sink][format]") {
    TempDir td;
    const fs::path log_path = td.path() / "nested" / "deeper" / "jbox.log";
    REQUIRE_FALSE(fs::exists(log_path.parent_path()));

    RotatingFileSink sink(RotatingFileSink::Config{log_path, 1024, 3});
    sink(makeEvent(jbox::rt::kLogRouteStarted, 1));

    REQUIRE(fs::exists(log_path.parent_path()));
    REQUIRE(fs::exists(log_path));
    REQUIRE(sink.isHealthy());
}

// -----------------------------------------------------------------------------
// RotatingFileSink — rotation
// -----------------------------------------------------------------------------

TEST_CASE("RotatingFileSink: rotates after exceeding max_bytes_per_file",
          "[rotating_file_sink][rotation]") {
    TempDir td;
    const fs::path log_path = td.path() / "jbox.log";
    // Pick a cap small enough that ~3-4 events trigger a rotation.
    RotatingFileSink sink(RotatingFileSink::Config{log_path, /*max=*/200, /*keep=*/3});

    for (std::uint32_t i = 0; i < 20; ++i) {
        sink(makeEvent(jbox::rt::kLogRouteStarted, /*route=*/i));
    }

    REQUIRE(sink.isHealthy());
    REQUIRE(fs::exists(log_path));
    REQUIRE(fs::exists(sink.rotatedFilePath(1)));
    REQUIRE(fs::exists(sink.rotatedFilePath(2)));

    // Live file is bounded by the cap.
    REQUIRE(sink.liveBytes() <= 200);

    // The most recent event landed in the live file; the oldest events
    // were rotated out and dropped (keep_count=3 retains only the
    // newest 3 files' worth of lines, not the full history).
    const std::string live = readFile(log_path);
    REQUIRE(live.find("route=19") != std::string::npos);
    REQUIRE(live.find("route=0 ") == std::string::npos);

    // Total surviving lines fit within the 3-file cap. With ~80-byte
    // lines and a 200-byte per-file cap, ~2 events fit per file → ≤ 9
    // surviving lines is a comfortable upper bound.
    std::size_t total_lines = countLines(live);
    total_lines += countLines(readFile(sink.rotatedFilePath(1)));
    total_lines += countLines(readFile(sink.rotatedFilePath(2)));
    REQUIRE(total_lines >= 1);
    REQUIRE(total_lines <= 9);
}

TEST_CASE("RotatingFileSink: rotation drops the oldest file when keep_count is exceeded",
          "[rotating_file_sink][rotation]") {
    TempDir td;
    const fs::path log_path = td.path() / "jbox.log";
    RotatingFileSink sink(RotatingFileSink::Config{log_path, /*max=*/200, /*keep=*/3});

    // Enough events to force at least 4 rotations — the oldest .1.log
    // shifted into .2.log shifted into would-be .3.log and dropped.
    for (std::uint32_t i = 0; i < 30; ++i) {
        sink(makeEvent(jbox::rt::kLogRouteStarted, /*route=*/i));
    }

    REQUIRE(fs::exists(sink.rotatedFilePath(1)));
    REQUIRE(fs::exists(sink.rotatedFilePath(2)));
    REQUIRE_FALSE(fs::exists(sink.rotatedFilePath(3)));

    // Successive files are progressively older — newest in live,
    // oldest in `.2.log`.
    auto firstRouteIn = [&](const fs::path& p) -> int {
        const std::string s = readFile(p);
        const auto pos = s.find("route=");
        REQUIRE(pos != std::string::npos);
        return std::atoi(s.c_str() + pos + 6);
    };
    const int newest_in_live = firstRouteIn(log_path);
    const int newest_in_1    = firstRouteIn(sink.rotatedFilePath(1));
    const int newest_in_2    = firstRouteIn(sink.rotatedFilePath(2));
    REQUIRE(newest_in_live > newest_in_1);
    REQUIRE(newest_in_1    > newest_in_2);
}

TEST_CASE("RotatingFileSink: keep_count caps the number of retained files",
          "[rotating_file_sink][rotation]") {
    TempDir td;
    const fs::path log_path = td.path() / "jbox.log";
    // keep_count=2 means at most live + 1 rotated.
    RotatingFileSink sink(RotatingFileSink::Config{log_path, /*max=*/200, /*keep=*/2});

    for (int i = 0; i < 50; ++i) {
        sink(makeEvent(jbox::rt::kLogRouteStarted, static_cast<std::uint32_t>(i)));
    }

    REQUIRE(fs::exists(log_path));
    REQUIRE(fs::exists(sink.rotatedFilePath(1)));
    // No `<base>.2.log` ever — keep_count=2 forbids it.
    REQUIRE_FALSE(fs::exists(sink.rotatedFilePath(2)));
}

TEST_CASE("RotatingFileSink: keep_count=1 retains only the live file",
          "[rotating_file_sink][rotation]") {
    TempDir td;
    const fs::path log_path = td.path() / "jbox.log";
    RotatingFileSink sink(RotatingFileSink::Config{log_path, /*max=*/200, /*keep=*/1});

    for (int i = 0; i < 30; ++i) {
        sink(makeEvent(jbox::rt::kLogRouteStarted, static_cast<std::uint32_t>(i)));
    }

    REQUIRE(fs::exists(log_path));
    REQUIRE_FALSE(fs::exists(sink.rotatedFilePath(1)));
    REQUIRE_FALSE(fs::exists(sink.rotatedFilePath(2)));
    REQUIRE(sink.liveBytes() <= 200);
}

// -----------------------------------------------------------------------------
// RotatingFileSink — failure handling
// -----------------------------------------------------------------------------

TEST_CASE("RotatingFileSink: unwritable parent directory flips to fail-silent",
          "[rotating_file_sink][failure]") {
    TempDir td;
    const fs::path locked_dir = td.path() / "locked";
    fs::create_directories(locked_dir);
    // Strip write permission so neither the file nor any nested dir
    // can be created. macOS honours these bits for the running user.
    fs::permissions(locked_dir,
                    fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);

    const fs::path log_path = locked_dir / "jbox.log";
    RotatingFileSink sink(RotatingFileSink::Config{log_path, 1024, 3});
    sink(makeEvent(jbox::rt::kLogRouteStarted, 1));

    REQUIRE_FALSE(sink.isHealthy());
    REQUIRE_FALSE(fs::exists(log_path));

    // Restore perms so TempDir cleanup can run.
    fs::permissions(locked_dir,
                    fs::perms::owner_all,
                    fs::perm_options::replace);
}

TEST_CASE("RotatingFileSink: subsequent writes after failure remain no-op",
          "[rotating_file_sink][failure]") {
    TempDir td;
    const fs::path locked_dir = td.path() / "locked";
    fs::create_directories(locked_dir);
    fs::permissions(locked_dir,
                    fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);

    const fs::path log_path = locked_dir / "jbox.log";
    RotatingFileSink sink(RotatingFileSink::Config{log_path, 1024, 3});

    for (int i = 0; i < 5; ++i) {
        sink(makeEvent(jbox::rt::kLogRouteStarted, static_cast<std::uint32_t>(i)));
    }
    REQUIRE_FALSE(sink.isHealthy());

    fs::permissions(locked_dir,
                    fs::perms::owner_all,
                    fs::perm_options::replace);
}

// -----------------------------------------------------------------------------
// compositeSink
// -----------------------------------------------------------------------------

TEST_CASE("compositeSink: dispatches every event to every sub-sink in order",
          "[composite_sink]") {
    std::vector<int> order;
    auto a = [&order](const RtLogEvent&) { order.push_back(1); };
    auto b = [&order](const RtLogEvent&) { order.push_back(2); };
    auto c = [&order](const RtLogEvent&) { order.push_back(3); };

    auto sink = compositeSink({a, b, c});
    sink(makeEvent(jbox::rt::kLogRouteStarted, 1));
    sink(makeEvent(jbox::rt::kLogRouteStopped, 2));

    REQUIRE(order == std::vector<int>{1, 2, 3, 1, 2, 3});
}

TEST_CASE("compositeSink: empty input vector yields a usable no-op sink",
          "[composite_sink]") {
    auto sink = compositeSink({});
    // Must be safely callable.
    sink(makeEvent(jbox::rt::kLogRouteStarted, 1));
    SUCCEED();
}

TEST_CASE("compositeSink: integrates cleanly into a LogDrainer",
          "[composite_sink][log_drainer]") {
    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};
    auto a = [&](const RtLogEvent&) { count_a.fetch_add(1); };
    auto b = [&](const RtLogEvent&) { count_b.fetch_add(1); };

    LogDrainer drainer(compositeSink({a, b}), std::chrono::milliseconds(2));
    for (std::uint32_t i = 1; i <= 25; ++i) {
        REQUIRE(drainer.queue()->tryPush(
            makeEvent(jbox::rt::kLogUnderrun, i)));
    }
    drainer.stop();

    REQUIRE(count_a.load() == 25);
    REQUIRE(count_b.load() == 25);
}

// -----------------------------------------------------------------------------
// defaultJboxLogPath
// -----------------------------------------------------------------------------

namespace {

class HomeEnvGuard {
public:
    HomeEnvGuard() {
        const char* h = std::getenv("HOME");
        had_home_ = (h != nullptr);
        if (had_home_) saved_home_ = h;
    }
    ~HomeEnvGuard() {
        if (had_home_) {
            ::setenv("HOME", saved_home_.c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
    }
    HomeEnvGuard(const HomeEnvGuard&) = delete;
    HomeEnvGuard& operator=(const HomeEnvGuard&) = delete;

private:
    bool        had_home_;
    std::string saved_home_;
};

}  // namespace

TEST_CASE("defaultJboxLogPath: with $HOME set returns ~/Library/Logs/Jbox/<base>.log",
          "[default_jbox_log_path]") {
    HomeEnvGuard guard;
    ::setenv("HOME", "/tmp/jbox-fake-home", 1);

    const fs::path p = defaultJboxLogPath("Jbox");
    REQUIRE(p == fs::path("/tmp/jbox-fake-home/Library/Logs/Jbox/Jbox.log"));
}

TEST_CASE("defaultJboxLogPath: differentiates by process basename",
          "[default_jbox_log_path]") {
    HomeEnvGuard guard;
    ::setenv("HOME", "/tmp/jbox-fake-home", 1);

    REQUIRE(defaultJboxLogPath("Jbox").filename()           == "Jbox.log");
    REQUIRE(defaultJboxLogPath("JboxEngineCLI").filename()  == "JboxEngineCLI.log");
    REQUIRE(defaultJboxLogPath("Jbox").parent_path()
            == defaultJboxLogPath("JboxEngineCLI").parent_path());
}

TEST_CASE("defaultJboxLogPath: empty $HOME yields an empty path",
          "[default_jbox_log_path]") {
    HomeEnvGuard guard;
    ::unsetenv("HOME");

    REQUIRE(defaultJboxLogPath("Jbox").empty());
}
