// Derived from Azahar src/common/string_util.cpp and src/common/file_util.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; both say "GPLv2 or any later
// version"): the expected values for which characters StripSpaces strips, that SplitString drops
// the empty piece after the last delimiter, that Exists ignores a trailing '/', that
// CreateFullPath makes the folders up to the last '/', and that the file helpers return a byte
// count (0 for an empty write).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (string_util.cpp, file_util.cpp)
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project (string_util.cpp, second notice)
// Copyright Dolphin Emulator Project (file_util.cpp, second notice)
//
// The app's util/ (N0-20): logging, strings and files, with synthetic data only. Built with
// -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest and {fmt}.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "util/file.h"
#include "util/log.h"
#include "util/strings.h"

namespace fs = std::filesystem;
using namespace App::Util;

namespace {

/// A folder of its own under the system's temp folder, removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("mh4u_util_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string operator/(const std::string& name) const {
        return (path / name).generic_string();
    }
    std::string root() const {
        return path.generic_string();
    }
};

struct Logged {
    LogLevel level;
    std::string file;
    unsigned line;
    std::string function;
    std::string message;
};
std::vector<Logged> g_logged;

void Capture(LogLevel level, const char* file, unsigned line, const char* function, const std::string& message) {
    g_logged.push_back({level, file, line, function, message});
}

} // namespace

TEST_CASE("StripSpaces trims spaces, tabs and line ends at both ends only") {
    CHECK(StripSpaces("  a b \t\r\n") == "a b");
    CHECK(StripSpaces("a") == "a");
    CHECK(StripSpaces("") == "");
    CHECK(StripSpaces(" \t\r\n ") == "");
    CHECK(StripSpaces("\ta  b\n") == "a  b");
    // not a space: form feed, vertical tab, non-breaking space
    CHECK(StripSpaces("\fa\v") == "\fa\v");
    CHECK(StripSpaces("\xC2\xA0x") == "\xC2\xA0x");
}

TEST_CASE("SplitString drops the empty piece after the last delimiter only") {
    using V = std::vector<std::string>;
    CHECK(SplitString("a,b,c", ',') == V{"a", "b", "c"});
    CHECK(SplitString("a,b,", ',') == V{"a", "b"});
    CHECK(SplitString("a,,b", ',') == V{"a", "", "b"});
    CHECK(SplitString(",a", ',') == V{"", "a"});
    CHECK(SplitString("a,b,,", ',') == V{"a", "b", ""});
    CHECK(SplitString(",", ',') == V{""});
    CHECK(SplitString("", ',') == V{});
    CHECK(SplitString("abc", ',') == V{"abc"});
    CHECK(SplitString("W,S,A,D,", ',').size() == 4);
}

TEST_CASE("UTF-8 and UTF-16 convert both ways") {
    CHECK(Utf8ToUtf16("abc") == u"abc");
    CHECK(Utf8ToUtf16("\xC3\xA9") == u"é");                         // 2 bytes
    CHECK(Utf8ToUtf16("\xE2\x82\xAC") == u"€");                     // 3 bytes
    CHECK(Utf8ToUtf16("\xF0\x9F\x98\x80") == u"\xD83D\xDE00");           // 4 bytes: a surrogate pair
    CHECK(Utf16ToUtf8(u"\xD83D\xDE00") == "\xF0\x9F\x98\x80");
    CHECK(Utf16ToUtf8(u"aé€") == "a\xC3\xA9\xE2\x82\xAC");
    CHECK(Utf8ToUtf16("") == u"");
    CHECK(Utf16ToUtf8(u"") == "");
    const std::string mixed = "x\xC3\xA9y\xE2\x82\xACz\xF0\x9F\x98\x80";
    CHECK(Utf16ToUtf8(Utf8ToUtf16(mixed)) == mixed);
}

TEST_CASE("invalid input becomes U+FFFD and never reads past the end") {
    CHECK(Utf8ToUtf16("\xFF") == u"�");
    CHECK(Utf8ToUtf16("a\x80z") == u"a�z");              // a continuation byte alone
    CHECK(Utf8ToUtf16("\xE2\x82") == u"��");        // cut short: one per bad byte
    CHECK(Utf8ToUtf16("\xC0\x80") == u"��");        // overlong NUL
    CHECK(Utf8ToUtf16("\xED\xA0\x80") == u"���"); // an encoded surrogate
    CHECK(Utf8ToUtf16("\xF4\x90\x80\x80") == u"����"); // past U+10FFFF
    CHECK(Utf16ToUtf8(u"\xD800") == "\xEF\xBF\xBD");          // a high surrogate alone
    CHECK(Utf16ToUtf8(u"\xDC00x") == "\xEF\xBF\xBD" "x");     // a low surrogate alone
    CHECK(Utf16ToUtf8(u"\xD800x") == "\xEF\xBF\xBD" "x");     // a high one not followed by a low
}

TEST_CASE("wide strings round-trip") {
    const std::string text = "p\xC3\xA1th/\xE2\x82\xAC/\xF0\x9F\x98\x80";
    CHECK(WideToUtf8(Utf8ToWide(text)) == text);
    CHECK(Utf8ToWide("").empty());
    CHECK(WideToUtf8(std::wstring()).empty());
    const std::wstring abc = Utf8ToWide("abc");
    CHECK((abc.size() == 3 && abc[0] == L'a' && abc[1] == L'b' && abc[2] == L'c'));
    // one wchar_t per code point where wchar_t is 32 bits wide, a surrogate pair where it is 16
    CHECK(Utf8ToWide("\xF0\x9F\x98\x80").size() == (sizeof(wchar_t) == 2 ? 2u : 1u));
    // a wide character that is not a Unicode value
    CHECK(WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xD800))) == "\xEF\xBF\xBD");
}

TEST_CASE("Exists and IsDirectory") {
    TempDir dir;
    const std::string file = dir / "f.txt";
    REQUIRE(WriteStringToFile(false, file, "x") == 1);
    CHECK(Exists(file));
    CHECK_FALSE(IsDirectory(file));
    CHECK(Exists(dir.root()));
    CHECK(Exists(dir.root() + "/"));       // a trailing '/' is ignored
    CHECK(Exists(dir.root() + "///"));
    CHECK(IsDirectory(dir.root() + "/"));
    CHECK(Exists(file + "/"));             // as for a file: the '/' is stripped first
    CHECK_FALSE(Exists(dir / "missing"));
    CHECK_FALSE(IsDirectory(dir / "missing"));
    CHECK_FALSE(Exists(dir / "f.txt/below"));  // under a file
    CHECK_FALSE(Exists(""));
    CHECK(Exists("/"));
}

TEST_CASE("CreateFullPath makes the folders up to the last separator") {
    TempDir dir;
    CHECK(CreateFullPath(dir / "a/b/c.txt"));
    CHECK(IsDirectory(dir / "a/b"));
    CHECK_FALSE(Exists(dir / "a/b/c.txt"));        // the file itself is not made
    CHECK(CreateFullPath(dir / "x/y/"));
    CHECK(IsDirectory(dir / "x/y"));
    CHECK(CreateFullPath(dir / "x/y/"));           // again: it exists
    CHECK(CreateFullPath("no_separator.txt"));     // nothing to make
    CHECK_FALSE(Exists("no_separator.txt"));
}

TEST_CASE("CreateFullPath fails under a file and past 100 levels") {
    TempDir dir;
    REQUIRE(WriteStringToFile(false, dir / "file", "x") == 1);
    g_logged.clear();
    const LogSink before = SetLogSink(Capture);
    CHECK_FALSE(CreateFullPath(dir / "file/sub/x.txt"));
    std::string deep = dir.root() + "/";
    for (int i = 0; i < 101; ++i) {
        deep += "d/";
    }
    // the root's own separators count too: the check stops it before the end
    CHECK_FALSE(CreateFullPath(deep));
    SetLogSink(before);
    CHECK(g_logged.size() >= 2);
    bool too_deep = false;
    for (const Logged& l : g_logged) {
        CHECK(l.level == LogLevel::Error);
        too_deep = too_deep || l.message.find("too deep") != std::string::npos;
    }
    CHECK(too_deep);
}

TEST_CASE("WriteStringToFile and ReadFileToString count bytes") {
    TempDir dir;
    const std::string file = dir / "t.txt";
    const std::string content = "line one\nline two\n\xE2\x82\xAC";
    CHECK(WriteStringToFile(true, file, content) == content.size());
    std::string back = "stale";
    CHECK(ReadFileToString(true, file, back) == content.size());
    CHECK(back == content);
    // binary mode keeps every byte, NULs included
    const std::string raw("a\0b\r\n\xFF", 6);
    CHECK(WriteStringToFile(false, file, raw) == 6);
    CHECK(ReadFileToString(false, file, back) == 6);
    CHECK(back == raw);
    // a longer file replaced by a shorter one
    CHECK(WriteStringToFile(false, file, "ab") == 2);
    CHECK(ReadFileToString(false, file, back) == 2);
    CHECK(back == "ab");
}

TEST_CASE("an empty write makes an empty file and counts 0, like a failed one") {
    TempDir dir;
    const std::string file = dir / "e.txt";
    CHECK(WriteStringToFile(true, file, "") == 0);
    CHECK(Exists(file));
    std::string back = "stale";
    CHECK(ReadFileToString(true, file, back) == 0);
    CHECK(back.empty());
}

TEST_CASE("file errors: nothing there, no folder to write in, a folder instead of a file") {
    TempDir dir;
    std::string back = "stale";
    CHECK(ReadFileToString(true, dir / "missing.txt", back) == 0);
    CHECK(back == "stale");                              // untouched
    CHECK(WriteStringToFile(true, dir / "nodir/f.txt", "x") == 0);
    CHECK_FALSE(Exists(dir / "nodir"));
    CHECK(WriteStringToFile(true, dir.root(), "x") == 0);  // it is a folder
}

TEST_CASE("ListDirectory names the entries and fails on a missing folder or a file") {
    TempDir dir;
    REQUIRE(WriteStringToFile(false, dir / "one.txt", "1") == 1);
    REQUIRE(CreateFullPath(dir / "sub/"));
    std::vector<std::string> names{"stale"};
    REQUIRE(ListDirectory(dir.root(), names));
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"one.txt", "sub"});
    CHECK(ListDirectory(dir / "sub", names));
    CHECK(names.empty());
    names = {"kept"};
    CHECK_FALSE(ListDirectory(dir / "missing", names));
    CHECK_FALSE(ListDirectory(dir / "one.txt", names));
    CHECK(names == std::vector<std::string>{"kept"});    // untouched when it fails
}

TEST_CASE("paths and file names are UTF-8") {
    TempDir dir;
    const std::string file = dir / "caf\xC3\xA9 \xE2\x82\xAC.txt";
    CHECK(WriteStringToFile(false, file, "ok") == 2);
    CHECK(Exists(file));
    std::vector<std::string> names;
    REQUIRE(ListDirectory(dir.root(), names));
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "caf\xC3\xA9 \xE2\x82\xAC.txt");
}

TEST_CASE("logging goes to the sink with the call site and the formatted message") {
    g_logged.clear();
    const LogSink before = SetLogSink(Capture);
    const unsigned line = __LINE__ + 1;
    APP_LOG_INFO("{} and {:04x}", "text", 255);
    APP_LOG_WARNING("w");
    APP_LOG_ERROR("e {}", std::string("s"));
    APP_LOG_CRITICAL("c");
    APP_LOG_DEBUG("d");
    CHECK(SetLogSink(before) == Capture);
    REQUIRE(g_logged.size() == 5);
    CHECK(g_logged[0].level == LogLevel::Info);
    CHECK(g_logged[0].message == "text and 00ff");
    CHECK(g_logged[0].line == line);
    CHECK(g_logged[0].file == std::string(__FILE__));
    CHECK(g_logged[0].function.rfind("DOCTEST_ANON_FUNC_", 0) == 0);  // the enclosing function's __func__
    CHECK(g_logged[1].level == LogLevel::Warning);
    CHECK(g_logged[2].level == LogLevel::Error);
    CHECK(g_logged[2].message == "e s");
    CHECK(g_logged[3].level == LogLevel::Critical);
    CHECK(g_logged[4].level == LogLevel::Debug);
}

TEST_CASE("without a sink a message goes to stderr and does not crash") {
    const LogSink before = SetLogSink(nullptr);
    APP_LOG_INFO("to stderr {}", 1);
    SetLogSink(before);
    CHECK(true);
}
