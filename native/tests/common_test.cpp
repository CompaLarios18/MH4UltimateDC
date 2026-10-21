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

#include "../src/common/utf8.h"

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

// ---- UTF-8 and UTF-16 (D45) -------------------------------------------------------------------------------------------

namespace {
// The text of the paths that carry the Windows fix: "José-ñ-日本" as UTF-8 bytes and as UTF-16 units. Written
// as escapes so that no source or compiler setting changes it.
const std::string kAccented = std::string("Jos") + "\xC3\xA9" + "-" + "\xC3\xB1" + "-" + "\xE6\x97\xA5" + "\xE6\x9C\xAC";
const std::u16string kAccentedUnits = std::u16string(u"Jos") + char16_t(0x00E9) + u'-' + char16_t(0x00F1) + u'-' +
                                      char16_t(0x65E5) + char16_t(0x672C);
}  // namespace

TEST_CASE("UTF-8 to UTF-16: ASCII, two, three and four byte characters, and the empty string") {
    CHECK(utf8::ToUtf16("") == std::u16string());
    CHECK(utf8::ToUtf16("abc/def.txt") == std::u16string(u"abc/def.txt"));
    CHECK(utf8::ToUtf16(kAccented) == kAccentedUnits);
    CHECK(utf8::ToUtf16("\xC3\xA9") == std::u16string(1, char16_t(0x00E9)));
    CHECK(utf8::ToUtf16("\xE2\x82\xAC") == std::u16string(1, char16_t(0x20AC)));
    // U+1F600 is a surrogate pair in UTF-16
    CHECK(utf8::ToUtf16("\xF0\x9F\x98\x80") == std::u16string({char16_t(0xD83D), char16_t(0xDE00)}));
    // the largest code point, and the first one of each length
    CHECK(utf8::ToUtf16("\xF4\x8F\xBF\xBF") == std::u16string({char16_t(0xDBFF), char16_t(0xDFFF)}));
    CHECK(utf8::ToUtf16("\x7F") == std::u16string(1, char16_t(0x7F)));
    CHECK(utf8::ToUtf16("\xC2\x80") == std::u16string(1, char16_t(0x80)));
    CHECK(utf8::ToUtf16("\xE0\xA0\x80") == std::u16string(1, char16_t(0x800)));
    CHECK(utf8::ToUtf16("\xF0\x90\x80\x80") == std::u16string({char16_t(0xD800), char16_t(0xDC00)}));
    // a null inside the text is kept (the size says where the text ends)
    CHECK(utf8::ToUtf16(std::string_view("a\0b", 3)) == std::u16string({u'a', char16_t(0), u'b'}));
}

TEST_CASE("UTF-8 to UTF-16: bad input becomes U+FFFD, one per bad byte, and never stops the text") {
    const char16_t bad = 0xFFFD;
    CHECK(utf8::ToUtf16("\xFF") == std::u16string(1, bad));
    CHECK(utf8::ToUtf16("\x80") == std::u16string(1, bad));          // a continuation byte alone
    CHECK(utf8::ToUtf16("a\xC3") == std::u16string({u'a', bad}));    // cut short at the end
    CHECK(utf8::ToUtf16("a\xE2\x82") == std::u16string({u'a', bad, bad}));
    CHECK(utf8::ToUtf16("\xC3z") == std::u16string({bad, u'z'}));    // not followed by a continuation
    CHECK(utf8::ToUtf16("\xF8\x88\x80\x80\x80") == std::u16string(5, bad));  // a five byte lead
    CHECK(utf8::ToUtf16("\xF4\x90\x80\x80") == std::u16string(4, bad));       // beyond U+10FFFF
    // a surrogate written in UTF-8 (CESU-8) is not text
    CHECK(utf8::ToUtf16("\xED\xA0\x80") == std::u16string(3, bad));
    CHECK(utf8::ToUtf16("\xED\xB0\x80") == std::u16string(3, bad));
    // what follows a bad byte is read on
    CHECK(utf8::ToUtf16(std::string("\xFF") + kAccented) == std::u16string(1, bad) + kAccentedUnits);
}

TEST_CASE("UTF-16 to UTF-8: ASCII, accents, surrogate pairs, lone surrogates and the empty string") {
    CHECK(utf8::FromUtf16(u"") == "");
    CHECK(utf8::FromUtf16(u"abc/def.txt") == "abc/def.txt");
    CHECK(utf8::FromUtf16(kAccentedUnits) == kAccented);
    CHECK(utf8::FromUtf16(std::u16string({char16_t(0xD83D), char16_t(0xDE00)})) == "\xF0\x9F\x98\x80");
    CHECK(utf8::FromUtf16(std::u16string({char16_t(0xDBFF), char16_t(0xDFFF)})) == "\xF4\x8F\xBF\xBF");
    CHECK(utf8::FromUtf16(std::u16string(1, char16_t(0x20AC))) == "\xE2\x82\xAC");
    CHECK(utf8::FromUtf16(std::u16string({u'a', char16_t(0), u'b'})) == std::string("a\0b", 3));
    // a lone surrogate, high or low, becomes U+FFFD (EF BF BD) and the text goes on
    const std::string replacement = "\xEF\xBF\xBD";
    CHECK(utf8::FromUtf16(std::u16string(1, char16_t(0xD800))) == replacement);
    CHECK(utf8::FromUtf16(std::u16string(1, char16_t(0xDC00))) == replacement);
    CHECK(utf8::FromUtf16(std::u16string({u'a', char16_t(0xD83D)})) == "a" + replacement);          // at the end
    CHECK(utf8::FromUtf16(std::u16string({char16_t(0xD83D), u'a'})) == replacement + "a");          // not followed by its partner
    CHECK(utf8::FromUtf16(std::u16string({char16_t(0xDE00), char16_t(0xD83D)})) == replacement + replacement);  // reversed
    CHECK(utf8::FromUtf16(std::u16string({char16_t(0xD83D), char16_t(0xD83D), char16_t(0xDE00)})) ==
          replacement + "\xF0\x9F\x98\x80");  // the second high surrogate takes the low one
}

TEST_CASE("UTF-8 and UTF-16 round-trip, for text that is valid") {
    for (const std::string& text : {std::string(), std::string("plain"), kAccented, std::string("\xF0\x9F\x98\x80x\xF0\x9F\x8E\xAE"),
                                   std::string("/user/") + kAccented + "/sdmc/Nintendo 3DS/"}) {
        CHECK(utf8::FromUtf16(utf8::ToUtf16(text)) == text);
    }
    // every code point of the BMP except the surrogates, and a few beyond it
    std::u16string all;
    for (u32 c = 1; c < 0x10000; ++c) {
        if (c < 0xD800 || c >= 0xE000) {
            all.push_back(static_cast<char16_t>(c));
        }
    }
    CHECK(utf8::ToUtf16(utf8::FromUtf16(all)) == all);
    for (const u32 c : {0x10000u, 0x1F600u, 0x10FFFFu, 0x20000u}) {
        const u32 v = c - 0x10000;
        const std::u16string pair({static_cast<char16_t>(0xD800 + (v >> 10)), static_cast<char16_t>(0xDC00 + (v & 0x3FF))});
        CHECK(utf8::ToUtf16(utf8::FromUtf16(pair)) == pair);
    }
}

#if defined(_WIN32)
TEST_CASE("UTF-8 and the wide characters of Windows") {
    CHECK(utf8::ToWide(kAccented) == std::wstring(L"Jos\u00E9-\u00F1-\u65E5\u672C"));
    CHECK(utf8::FromWide(L"Jos\u00E9-\u00F1-\u65E5\u672C") == kAccented);
    CHECK(utf8::FromWide(static_cast<const wchar_t*>(nullptr)) == "");
    CHECK(utf8::FromWide(std::wstring_view()) == "");
    CHECK(utf8::ToWide("") == std::wstring());
    CHECK(utf8::FromWide(L"\xD83D") == "\xEF\xBF\xBD");
}
#endif
