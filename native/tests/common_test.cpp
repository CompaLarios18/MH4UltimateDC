// mh4n_common: result codes, the log filter and sink, assertions, CRC-32.
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#define MH4N_HAVE_FORK 1
#endif

#include "mh4n/assert.h"
#include "mh4n/crc32.h"
#include "mh4n/log.h"
#include "mh4n/types.h"

using namespace mh4n;

TEST_CASE("MakeResult uses the 3DS bit layout") {
    // description 0x3FF, module 0xFF, summary 0x3F, level 0x1F: every field full
    CHECK(MakeResult(0x3FF, 0xFF, 0x3F, 0x1F) == 0xFFE3FFFFu);
    CHECK(MakeResult(1, 0, 0, 0) == 1u);
    CHECK(MakeResult(0, 1, 0, 0) == (1u << 10));
    CHECK(MakeResult(0, 0, 1, 0) == (1u << 21));
    CHECK(MakeResult(0, 0, 0, 1) == (1u << 27));
    // 0xD8E007F7 is "invalid handle"-like: level 0x1B (permanent), summary 0x07, module 0x01
    constexpr Result r = MakeResult(0x3F7, 1, 7, 0x1B);
    CHECK(r == 0xD8E007F7u);
    CHECK(ResultDescription(r) == 0x3F7u);
    CHECK(ResultModule(r) == 1u);
    CHECK(ResultSummary(r) == 7u);
    CHECK(ResultLevel(r) == 0x1Bu);
    // a field wider than its bits does not leak into the next one
    CHECK(MakeResult(0x400, 0, 0, 0) == 0u);
    CHECK(MakeResult(0, 0x100, 0, 0) == 0u);
    CHECK(MakeResult(0, 0, 0x40, 0) == 0u);
    CHECK(MakeResult(0, 0, 0, 0x20) == 0u);
}

TEST_CASE("IsError looks at the top bit of the level") {
    CHECK_FALSE(IsError(0));
    CHECK_FALSE(IsError(MakeResult(5, 5, 5, 1)));   // info
    CHECK_FALSE(IsError(MakeResult(5, 5, 5, 0xF))); // top bit clear
    CHECK(IsError(MakeResult(5, 5, 5, 0x10)));
    CHECK(IsError(MakeResult(5, 5, 5, 0x1F)));
    CHECK(IsError(0xD8E007F7u));
    CHECK(IsSuccess(0));
}

TEST_CASE("log filter parsing") {
    LogFilter filter;
    CHECK(filter.Parse("*:Info Kernel:Debug"));
    CHECK(filter.LevelFor("Kernel") == LogLevel::Debug);
    CHECK(filter.LevelFor("Service") == LogLevel::Info);
    CHECK(filter.Enabled(LogLevel::Debug, "Kernel"));
    CHECK_FALSE(filter.Enabled(LogLevel::Debug, "Service"));
    CHECK(filter.Enabled(LogLevel::Info, "Service"));
    CHECK(filter.Enabled(LogLevel::Critical, "Anything"));

    SUBCASE("a later * resets the earlier rules") {
        CHECK(filter.Parse("*:Error"));
        CHECK(filter.LevelFor("Kernel") == LogLevel::Error);
    }
    SUBCASE("rules apply in order") {
        CHECK(filter.Parse("Kernel:Warning Kernel:Trace"));
        CHECK(filter.LevelFor("Kernel") == LogLevel::Trace);
    }
    SUBCASE("a rule covers the sub-categories without a rule of their own") {
        CHECK(filter.Parse("Service:Debug Service.FS:Error"));
        CHECK(filter.LevelFor("Service") == LogLevel::Debug);
        CHECK(filter.LevelFor("Service.HID") == LogLevel::Debug);
        CHECK(filter.LevelFor("Service.FS") == LogLevel::Error);
        CHECK(filter.LevelFor("Service.FS.Read") == LogLevel::Error);
        CHECK(filter.LevelFor("ServiceX") == LogLevel::Info);
    }
    SUBCASE("level names ignore case, white space is free") {
        CHECK(filter.Parse("  *:warning \t Kernel:TRACE\n"));
        CHECK(filter.LevelFor("Foo") == LogLevel::Warning);
        CHECK(filter.LevelFor("Kernel") == LogLevel::Trace);
    }
    SUBCASE("every level name") {
        const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
        for (int i = 0; i < 6; ++i) {
            CHECK(ParseLogLevel(names[i]) == static_cast<LogLevel>(i));
            CHECK(std::string(LogLevelName(static_cast<LogLevel>(i))) == names[i]);
        }
        CHECK_FALSE(ParseLogLevel("Verbose").has_value());
    }
    SUBCASE("Lowest") {
        CHECK(filter.Lowest() == LogLevel::Debug);
        LogFilter quiet(LogLevel::Error);
        CHECK(quiet.Lowest() == LogLevel::Error);
    }
}

TEST_CASE("malformed log rules are reported and skipped") {
    LogFilter filter;
    std::string error;
    CHECK_FALSE(filter.Parse("Kernel Foo:Loud :Info Good:Debug", &error));
    CHECK(error.find("'Kernel'") != std::string::npos);
    CHECK(error.find("'Foo:Loud'") != std::string::npos);
    CHECK(error.find("':Info'") != std::string::npos);
    CHECK(error.find("Good") == std::string::npos);
    CHECK(filter.LevelFor("Good") == LogLevel::Debug);
    CHECK(filter.LevelFor("Foo") == LogLevel::Info);
    CHECK(filter.Parse("")); // nothing to parse is fine
}

namespace {
struct CapturedLog {
    LogLevel level;
    std::string category;
    std::string message;
};

// Installs a sink and a filter for one test and puts the defaults back.
struct LogCapture {
    std::vector<CapturedLog> records;
    LogCapture(const char* filter) {
        LogFilter parsed;
        parsed.Parse(filter);
        SetLogFilter(parsed);
        SetLogSink([this](const LogRecord& r) {
            records.push_back({r.level, std::string(r.category), std::string(r.message)});
        });
    }
    ~LogCapture() {
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
};
} // namespace

TEST_CASE("MH4N_LOG formats with fmt and honours the filter") {
    LogCapture capture("*:Warning Kernel:Debug");
    MH4N_LOG(Info, "Service", "dropped {}", 1);
    MH4N_LOG(Warning, "Service", "kept {} and {:#x}", "a", 255);
    MH4N_LOG(Debug, "Kernel", "no arguments");
    MH4N_LOG(Trace, "Kernel", "dropped");
    REQUIRE(capture.records.size() == 2);
    CHECK(capture.records[0].level == LogLevel::Warning);
    CHECK(capture.records[0].category == "Service");
    CHECK(capture.records[0].message == "kept a and 0xff");
    CHECK(capture.records[1].category == "Kernel");
    CHECK(capture.records[1].message == "no arguments");
}

TEST_CASE("MH4N_LOG does not evaluate its arguments when filtered out") {
    LogCapture capture("*:Error");
    int evaluated = 0;
    MH4N_LOG(Info, "X", "{}", ++evaluated);
    CHECK(evaluated == 0);
    MH4N_LOG(Error, "X", "{}", ++evaluated);
    CHECK(evaluated == 1);
}

TEST_CASE("the log filter can be read back") {
    LogCapture capture("*:Error Kernel:Trace");
    CHECK(GetLogFilter().LevelFor("Kernel") == LogLevel::Trace);
    CHECK(GetLogFilter().LevelFor("Other") == LogLevel::Error);
}

TEST_CASE("MH4N_ASSERT passes silently when the condition holds") {
    LogCapture capture("*:Trace");
    int evaluated = 0;
    MH4N_ASSERT(++evaluated == 1);
    MH4N_ASSERT(evaluated == 1, "evaluated={}", evaluated);
    CHECK(evaluated == 1);
    CHECK(capture.records.empty());
}

#ifdef MH4N_HAVE_FORK
namespace {
// Runs `body` in a child process; returns its wait status, and the child's log line.
template <class F>
int RunInChild(F body, std::string& logged) {
    int pipe_fds[2];
    REQUIRE(pipe(pipe_fds) == 0);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(pipe_fds[0]);
        SetLogSink([&](const LogRecord& r) {
            const std::string line = std::string(LogLevelName(r.level)) + "|" +
                                     std::string(r.category) + "|" + std::string(r.message);
            const ssize_t written = write(pipe_fds[1], line.data(), line.size());
            (void)written;
        });
        body();
        _exit(0);
    }
    close(pipe_fds[1]);
    char buffer[512];
    const ssize_t n = read(pipe_fds[0], buffer, sizeof buffer);
    logged.assign(buffer, n > 0 ? static_cast<std::size_t>(n) : 0);
    close(pipe_fds[0]);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    return status;
}
} // namespace

TEST_CASE("a failed MH4N_ASSERT logs at Critical and aborts") {
    std::string logged;
    const int status = RunInChild([] { MH4N_ASSERT(1 + 1 == 3, "math is {}", "broken"); }, logged);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
    CHECK(logged.rfind("Critical|Assert|", 0) == 0);
    CHECK(logged.find("1 + 1 == 3") != std::string::npos);
    CHECK(logged.find("math is broken") != std::string::npos);
}

TEST_CASE("MH4N_UNREACHABLE logs and aborts") {
    std::string logged;
    const int status = RunInChild([] { MH4N_UNREACHABLE(); }, logged);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
    CHECK(logged.find("unreachable") != std::string::npos);
}
#endif

TEST_CASE("CRC-32 test vectors") {
    auto crc = [](const char* text) { return Crc32(text, std::strlen(text)); };
    CHECK(crc("") == 0x00000000u);
    CHECK(crc("a") == 0xE8B7BE43u);
    CHECK(crc("abc") == 0x352441C2u);
    CHECK(crc("123456789") == 0xCBF43926u);
    CHECK(crc("The quick brown fox jumps over the lazy dog") == 0x414FA339u);
    const unsigned char zeros[32] = {};
    CHECK(Crc32(zeros, sizeof zeros) == 0x190A55ADu);
    const unsigned char ones[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(Crc32(ones, sizeof ones) == 0xFF6CAB0Bu);
}

TEST_CASE("CRC-32 continues over a second block") {
    const char* text = "123456789";
    const u32 whole = Crc32(text, 9);
    for (std::size_t split = 0; split <= 9; ++split) {
        CHECK(Crc32(text + split, 9 - split, Crc32(text, split)) == whole);
    }
}
