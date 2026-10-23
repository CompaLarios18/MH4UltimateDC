// Crash files and tester reports of mh4u-native (N3-13, D69): the crash handler in a child process (this
// binary started again with --crash-test <scenario> <user folder>), Redact on one synthetic line per kind
// of thing it hides, the settings allowlist, Pending, and Make on a synthetic user folder whose output
// goes through tools/native/report_check.py. Synthetic data only: documentation-range addresses
// (192.0.2.x, 2001:db8::), made-up MAC addresses and names, made-up folders.
#define MH4N_TEST_OWN_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "../../src/services/cfg/cfg_store.h"
#include "mh4n/memory.h"
#include "report/report.h"

namespace fs = std::filesystem;
namespace report = mh4n::app::report;

namespace {

std::string g_self_path;

// ---- helpers -----------------------------------------------------------------------------------------------

// A folder of its own under the temporary folder, removed with the object.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* what) {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("mh4n-report-") + what + "-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.generic_string(); }
};

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

struct ChildResult {
    int exit_code = -1;  // -1 if it did not exit normally
    int signal = 0;
    std::string output;  // stdout and stderr
};

ChildResult RunSelf(const std::string& arguments) {
    ChildResult result;
    const std::string command = "\"" + g_self_path + "\" " + arguments + " 2>&1";
#if defined(_WIN32)
    // _popen runs `cmd /c <command>`, and cmd strips the first and the last quote of a command that starts with a
    // quote and has more of them ("path" --crash-test null "dir" is that): wrapped in one more pair it runs as written
    const std::string shell_command = "\"" + command + "\"";
    FILE* pipe = _popen(shell_command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    REQUIRE(pipe != nullptr);
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
        result.output.append(buffer, n);
    }
#if defined(_WIN32)
    result.exit_code = _pclose(pipe);
#else
    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        if (result.exit_code > 128) {  // the shell reports a signal as 128 + n
            result.signal = result.exit_code - 128;
        }
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
#endif
    return result;
}

// The crash files of <user>/log/ (what the children left).
std::vector<fs::path> CrashFiles(const fs::path& user) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(user / "log", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("crash-", 0) == 0) {
            files.push_back(it->path());
        }
    }
    return files;
}

// tools/native/report_check.py: found from this source file's folder (__FILE__ is relative to the
// build folder in a build with relative paths) and from the working folder, upwards.
fs::path FindChecker() {
    std::vector<fs::path> starts;
    std::error_code ec;
    starts.push_back(fs::absolute(fs::path(__FILE__), ec).parent_path());
    starts.push_back(fs::current_path(ec));
    for (fs::path dir : starts) {
        for (int up = 0; up < 10 && !dir.empty(); ++up, dir = dir.parent_path()) {
            const fs::path candidate = dir / "tools" / "native" / "report_check.py";
            if (fs::exists(candidate, ec)) {
                return candidate.lexically_normal();
            }
            if (dir == dir.parent_path()) {
                break;
            }
        }
    }
    return {};
}

// The python that runs report_check.py: "python3", or "python" where that is the name (Windows).
// Without any, the test fails when MH4N_REQUIRE_TEST_DATA=1 (CI) and says so and passes otherwise.
bool RunChecker(const fs::path& report_file, const std::vector<std::string>& names, int* exit_code,
                std::string* output) {
    const fs::path script = FindChecker();
    REQUIRE_MESSAGE(!script.empty(), "tools/native/report_check.py was not found");
    std::string args;
    for (const std::string& name : names) {
        args += " --name \"" + name + "\"";
    }
    for (const char* python : {"python3", "python"}) {
        const std::string command = std::string(python) + " \"" + script.lexically_normal().string() + "\" \"" +
                                    report_file.string() + "\"" + args + " 2>&1";
#if defined(_WIN32)
        FILE* pipe = _popen(command.c_str(), "r");
#else
        FILE* pipe = popen(command.c_str(), "r");
#endif
        if (pipe == nullptr) {
            continue;
        }
        output->clear();
        char buffer[256];
        std::size_t n;
        while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
            output->append(buffer, n);
        }
#if defined(_WIN32)
        *exit_code = _pclose(pipe);
#else
        const int status = pclose(pipe);
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
        // (127: the shell; 9009 and the Microsoft Store's stub: Windows. The checker itself never prints these.)
        const bool missing = *exit_code == 127 || *exit_code == 9009 || Contains(*output, "not found") ||
                             Contains(*output, "is not recognized");
        if (!missing) {
            return true;
        }
    }
    const char* require = std::getenv("MH4N_REQUIRE_TEST_DATA");
    if (require != nullptr && *require != '\0' && *require != '0') {
        FAIL("python3 is missing, and MH4N_REQUIRE_TEST_DATA=1");
    }
    MESSAGE("python3 is missing: the report was not given to report_check.py");
    return false;
}

}  // namespace

// ---- the crash handler -------------------------------------------------------------------------------------

namespace {

// A crash file's lines that name a frame: "#<n> <module>+0x<hex>".
std::vector<std::string> FrameLines(const std::string& text) {
    static const std::regex frame(R"(^#\d+ \S+\+0x[0-9a-f]+$)");
    std::vector<std::string> frames;
    for (const std::string& line : Lines(text)) {
        if (std::regex_match(line, frame)) {
            frames.push_back(line);
        }
    }
    return frames;
}

// The signal a null access gives: SIGSEGV; on macOS the kernel may report SIGBUS instead.
#if !defined(_WIN32)
bool IsFaultSignal(int signal) {
#if defined(__APPLE__)
    return signal == SIGSEGV || signal == SIGBUS;
#else
    return signal == SIGSEGV;
#endif
}
#endif

bool IsFaultLine(const std::string& line) {
#if defined(_WIN32)
    return line.rfind("signal: EXCEPTION_ACCESS_VIOLATION (0xc0000005)", 0) == 0;
#elif defined(__APPLE__)
    return line == "signal: SIGSEGV (11)" || line == "signal: SIGBUS (10)";
#else
    return line == "signal: SIGSEGV (11)";
#endif
}

fs::path OneCrashFile(const TempDir& dir) {
    const std::vector<fs::path> files = CrashFiles(dir.path);
    REQUIRE(files.size() == 1);
    return files[0];
}

}  // namespace

TEST_CASE("a null dereference leaves a crash file with module+offset frames and the published frame") {
    TempDir dir("crash");
    const ChildResult child = RunSelf("--crash-test null \"" + dir.str() + "\"");
    CAPTURE(child.output);
    CHECK(child.exit_code != 0);
#if !defined(_WIN32)
    CHECK(IsFaultSignal(child.signal));  // the default action still ends the process
#endif
    const std::string text = ReadFile(OneCrashFile(dir));
    CAPTURE(text);
    const std::vector<std::string> lines = Lines(text);
    REQUIRE(lines.size() > 6);
    CHECK(lines[0] == "mh4u-native crash v1");
    CHECK(lines[1].rfind("commit: ", 0) == 0);
    CHECK(lines[1].size() > 8);
    CHECK(IsFaultLine(lines[2]));
    CHECK(lines[3] == "fault address: 0x0000000000000000");
    CHECK(lines[4] == "game frame: 1234");
    CHECK(lines[5] == "scene: 7");
    CHECK(lines[6] == "frames:");
    const std::vector<std::string> frames = FrameLines(text);
#if defined(_WIN32)
    CHECK(frames.size() >= 1);
#else
    CHECK(frames.size() >= 2);
#endif
    CHECK(frames.size() <= 32);
    // a frame is in this program, which shows by its file name only ("app_report_test", with ".exe" on Windows)
    const std::string module = fs::path(g_self_path).filename().stem().string();
    CHECK(std::any_of(frames.begin(), frames.end(),
                      [&](const std::string& frame) { return Contains(frame, module); }));
    // modules by name: no path of any kind
    for (const std::string& frame : frames) {
        CHECK(frame.find('/') == std::string::npos);
        CHECK(frame.find('\\') == std::string::npos);
    }
    // the file is a report's input: nothing in it is an address, a name or a path
    CHECK(report::Redact(text, {}, "", "") == text);
}

TEST_CASE("a crash without published state says unknown") {
    TempDir dir("unknown");
    const ChildResult child = RunSelf("--crash-test unpublished \"" + dir.str() + "\"");
    CAPTURE(child.output);
    CHECK(child.exit_code != 0);
    const std::string text = ReadFile(OneCrashFile(dir));
    CHECK(Contains(text, "game frame: unknown\nscene: unknown\n"));
}

#if !defined(_WIN32)
TEST_CASE("the handler installed before ours is still called, after the file is written") {
    TempDir dir("previous");
    const ChildResult child = RunSelf("--crash-test previous \"" + dir.str() + "\"");
    CAPTURE(child.output);
    CHECK(child.exit_code == 77);
    CHECK(child.output == "previous handler\n");
    const std::string text = ReadFile(OneCrashFile(dir));
    CHECK(IsFaultLine(Lines(text)[2]));
    CHECK(Contains(text, "game frame: 1234"));
    CHECK(FrameLines(text).size() >= 2);
}

TEST_CASE("the guest memory's handler, installed after ours, chains to it") {
    TempDir dir("chain");
    const ChildResult child = RunSelf("--crash-test memory \"" + dir.str() + "\"");
    CAPTURE(child.output);
    CHECK(IsFaultSignal(child.signal));
    CHECK(child.output.find("bad guest memory access") == std::string::npos);  // not the guest's fault
    const std::string text = ReadFile(OneCrashFile(dir));
    CHECK(IsFaultLine(Lines(text)[2]));
    CHECK(Contains(text, "game frame: 1234"));
}

TEST_CASE("an abort is written too, and still aborts") {
    TempDir dir("abort");
    const ChildResult child = RunSelf("--crash-test abort \"" + dir.str() + "\"");
    CAPTURE(child.output);
    CHECK(child.signal == SIGABRT);
    const std::string text = ReadFile(OneCrashFile(dir));
    CHECK(Contains(text, "signal: SIGABRT (6)"));
    CHECK(FrameLines(text).size() >= 2);
}
#endif

TEST_CASE("an orderly exit leaves no crash file") {
    TempDir dir("clean");
    const ChildResult child = RunSelf("--crash-test clean \"" + dir.str() + "\"");
    CAPTURE(child.output);
    CHECK(child.exit_code == 0);
    CHECK(CrashFiles(dir.path).empty());
    CHECK_FALSE(report::Pending(dir.str()));
}

TEST_CASE("Install makes the crash file in advance and moves it with a second call") {
    TempDir first("first");
    TempDir second("second");
    report::Install(first.str());
    CHECK(CrashFiles(first.path).size() == 1);
    report::Install(second.str());
    CHECK(CrashFiles(first.path).empty());  // the empty one was taken away
    CHECK(CrashFiles(second.path).size() == 1);
    CHECK_FALSE(report::Pending(second.str()));  // an empty crash file is not a crash
}

// ---- Redact ------------------------------------------------------------------------------------------------

namespace {

const std::vector<std::string> kNames = {"fakeuser", "Fake Console"};
const std::string kUserDir = "/synthetic/mh4u/user";
const std::string kHome = "/home/fakeuser";

std::string R(const std::string& text) {
    return report::Redact(text, kNames, kUserDir, kHome);
}

}  // namespace

TEST_CASE("Redact: absolute paths") {
    // Windows: drive paths with spaces in their folders, either separator
    CHECK(R("opened C:\\Users\\Fake Person\\Games\\MH4U\\game.cxi ok") == "opened <path> ok");
    CHECK(R("D:/fake/dir/file.bin, then") == "<path>, then");
    CHECK(R("(E:\\a\\b.txt)") == "(<path>)");
    CHECK(R("two C:\\a\\b and D:\\c\\d here") == "two <path> and <path> here");
    CHECK(R("C:\\Program Files (x86)\\Fake\\tool.exe") == "<path>");
    // UNC
    CHECK(R("share \\\\fake-server\\share\\folder\\file.txt end") == "share <path> end");
    CHECK(R("//fake-server/share/file.txt") == "<path>");
    // POSIX: two or more components
    CHECK(R("loaded /opt/fake/lib/libfake.so.1 now") == "loaded <path> now");
    CHECK(R("HOME=/var/fake/x") == "HOME=<path>");
    CHECK(R("\"/usr/lib/fake.so\"") == "\"<path>\"");
    CHECK(R("/Users/fake/Library/Application Support/Fake/native/game/x.cxi") == "<path>");
    // not paths
    CHECK(R("file:///opt/fake/x.cxi now") == "file:<path> now");  // a URL of a file is a path
    CHECK(R("frame 12:/opt/fake/x") == "frame 12:<path>");
    CHECK(R("/tmp//opt/fake/lib.so") == "<path>");  // a doubled slash
    CHECK(R("path:\\\\fake-server\\share\\f") == "path:<path>");
    // a path right after a quote ends at the closing quote, spaces in its last part included
    CHECK(R("'/var/lib/a b/c d.txt' done") == "'<path>' done");
    CHECK(R("open \"/var/lib/a b/c d.txt\" ok") == "open \"<path>\" ok");
    CHECK(R("open \"C:\\Users\\Alice Smith\\x\\a b.txt\" ok") == "open \"<path>\" ok");
    CHECK(R("open 'D:/Alice Smith/a b.txt' ok") == "open '<path>' ok");
    CHECK(R("open \"/var/lib/a b/c d.txt") == "open \"<path> d.txt");  // no closing quote: the usual rule
    // an unmatched closing bracket ends a path; one that closes a bracket of the path stays in it
    CHECK(R("see (/home/bob/x/y) and [/usr/lib/x.so]") == "see (<path>) and [<path>]");
    CHECK(R("see (C:\\a\\b) and [D:\\c\\d]") == "see (<path>) and [<path>]");
    CHECK(R("/a/b[1]/c") == "<path>");
    CHECK(R("/a/b[1] x") == "<path> x");
    CHECK(R("(see /a/b(1) now)") == "(see <path> now)");
    CHECK(R("/tmp alone") == "/tmp alone");
    CHECK(R("a/b/c and 1/2/3 and n/a") == "a/b/c and 1/2/3 and n/a");
    CHECK(R("https://example.invalid/x/y") == "https://example.invalid/x/y");
    CHECK(R("mh4n::app::Main dsp::DSP 12:34:56 16:9") == "mh4n::app::Main dsp::DSP 12:34:56 16:9");
}

TEST_CASE("Redact: the user folder and the home folder stand alone, anything longer is a path") {
    CHECK(R("user folder /synthetic/mh4u/user") == "user folder <user>");
    CHECK(R("user folder /synthetic/mh4u/user/") == "user folder <user>");
    CHECK(R("(/synthetic/mh4u/user)") == "(<user>)");
    CHECK(R("home /home/fakeuser.") == "home <home>.");
    CHECK(R("wrote /synthetic/mh4u/user/log/x.txt") == "wrote <path>");
    CHECK(R("under /home/fakeuser/Documents/game") == "under <path>");
    CHECK(R("/synthetic/mh4u/user2") == "<path>");  // a folder that only starts like the user folder
    CHECK(R("/home/fakeuser2") == "<path>");
    // Windows folders compare without case and with either separator
    CHECK(report::Redact("at c:/users/fake/mh4u/user now", {}, "C:\\Users\\Fake\\mh4u\\user\\", "") ==
          "at <user> now");
}

TEST_CASE("Redact: names, console ids, MAC addresses, invites") {
    CHECK(R("hello fakeuser and Fake Console!") == "hello <os-user> and <console-user>!");
    CHECK(R("FAKEUSER wrote as fakeuser123") == "<os-user> wrote as <os-user>123");
    CHECK(report::Redact("al and Alice", {"al"}, "", "") == "<os-user> and Alice");  // a short name: whole words
    CHECK(report::Redact("<path> and path", {"path"}, "", "") == "<path> and <os-user>");  // not inside a token
    CHECK(report::Redact("x Bob y", {"", "Bob"}, "", "") == "x <console-user> y");
    CHECK(report::Redact("x Carol y", {"", "", "Carol"}, "", "") == "x <name> y");

    CHECK(R("new console id 0123456789abcdef.") == "new console id <console-id>.");
    CHECK(R("Console ID: 0x0123456789ABCDEF") == "Console ID: <console-id>");
    CHECK(R("console id 0123456789abcde") == "console id 0123456789abcde");  // 15 digits
    CHECK(R("console id 0123456789abcdef0") == "console id 0123456789abcdef0");  // 17

    CHECK(R("mac 18:2A:7B:00:00:01 and aa-bb-cc-dd-ee-ff.") == "mac <mac> and <mac>.");
    CHECK(R("not 18:2A:7B:00:00:01:02 nor 18:2A:7B:00:00") == "not 18:2A:7B:00:00:01:02 nor 18:2A:7B:00:00");

    CHECK(R("join mh4u://192.0.2.7:24872?password=fakepass now") == "join <invite> now");
    CHECK(R("(MH4U://example.invalid:1) after") == "(<invite> after");  // it ends at a space: nothing of it is left
}

TEST_CASE("Redact: IPv4 and IPv6 addresses, with ports") {
    CHECK(R("peer 192.0.2.7:24872 ok") == "peer <address> ok");
    CHECK(R("peer 192.0.2.1.") == "peer <address>.");
    CHECK(R("a=198.51.100.255,b=203.0.113.0") == "a=<address>,b=<address>");
    CHECK(R("version 1.2.3 and 300.1.1.1 and 1.2.3.4.5 and 1.2.3.1234") ==
          "version 1.2.3 and 300.1.1.1 and 1.2.3.4.5 and 1.2.3.1234");

    CHECK(R("peer 2001:db8::1 ok") == "peer <address> ok");
    CHECK(R("peer [2001:db8::1]:8080 ok") == "peer <address> ok");
    CHECK(R("peer [2001:db8::1] ok") == "peer <address> ok");
    CHECK(R("full 2001:0db8:0000:0000:0000:0000:0000:0001.") == "full <address>.");
    CHECK(R("loopback ::1, link fe80::1ff:fe23:4567:890a;") == "loopback <address>, link <address>;");
    CHECK(R("addr:2001:db8::7") == "addr:<address>");
    CHECK(R("trailing 2001:db8:: and 2001:db8::1:") == "trailing <address> and <address>:");
    CHECK(R("ends 2001:db8::") == "ends <address>");
    // not addresses
    CHECK(R("at 12:34:56 and 16:9 and 3440:1440 and 1:2:3:4:5:6:7") == "at 12:34:56 and 16:9 and 3440:1440 and 1:2:3:4:5:6:7");
    CHECK(R("mh4n::app::Main dsp::DSP std::vector :: a::b 1:2:3:4:5:6:7:8:9") ==
          "mh4n::app::Main dsp::DSP std::vector :: a::b 1:2:3:4:5:6:7:8:9");
    CHECK(R("2001:db8:::1 and 12345::1 and ::::") == "2001:db8:::1 and 12345::1 and ::::");
    CHECK(R("frame: 12 scene: ee") == "frame: 12 scene: ee");
}

TEST_CASE("Redact is idempotent and keeps what it does not hide") {
    const std::string text =
        "[Info] Loader: opened C:\\Users\\Fake Person\\g.cxi from 192.0.2.7:1 as fakeuser\n"
        "[Info] fs: sync at frame 120, mac 18:2A:7B:00:00:01, console id 0123456789abcdef\n"
        "[Warning] net: [2001:db8::5]:9 invite mh4u://192.0.2.7:1?password=x /opt/fake/lib/x.so\n";
    const std::string once = R(text);
    CHECK(R(once) == once);
    CHECK(Contains(once, "[Info] Loader: opened <path> from <address> as <os-user>\n"));
    CHECK(Contains(once, "fs: sync at frame 120, mac <mac>, console id <console-id>\n"));
    CHECK(Contains(once, "[Warning] net: <address> invite <invite> <path>\n"));
    CHECK(report::Redact("", {}, "", "") == "");
}

TEST_CASE("Redact and the checker agree: what Redact returns passes report_check.py") {
    // Lines made of fake fragments, some of what is hidden and some that is not, joined by punctuation.
    const std::vector<std::string> hidden = {
        "C:\\Users\\Fake Person\\Games\\x.cxi", "D:/fake/dir/file.bin", "\\\\fake-server\\share\\f.txt",
        "/opt/fake/lib/libfake.so.1", "/Users/fake/Library/Application Support/Fake/x", "/synthetic/mh4u/user/log/a.txt",
        "192.0.2.7", "192.0.2.7:24872", "198.51.100.255", "2001:db8::1", "[2001:db8::1]:8080", "fe80::1ff:fe23:4567:890a",
        "::1", "2001:0db8:0000:0000:0000:0000:0000:0001", "18:2A:7B:00:00:01", "aa-bb-cc-dd-ee-ff",
        "console id 0123456789abcdef", "Console ID: 0x0123456789ABCDEF", "mh4u://192.0.2.7:24872?password=fakepass",
        "fakeuser", "Fake Console", "/synthetic/mh4u/user", "/home/fakeuser"};
    const std::vector<std::string> plain = {
        "mh4n::app::Main", "dsp::DSP", "12:34:56", "16:9", "1.2.3", "300.1.1.1", "frame 120", "scene 7", "[Info]",
        "Kernel:", "a/b/c", "3440x1440@120", "0x1234abcd", "dead::beef", "e:", "ok", "/tmp", "n/a", "1:2:3:4:5:6:7",
        "<path>", "std::vector", "id", "a1", "face", "::", ":::", "1..2", "x.y.z", "http://example.invalid/p/q"};
    const std::vector<std::string> joins = {" ", ", ", "=", " (", ") ", " ; ", ": ", ":", ".", "/", "-", "["};
    std::uint32_t state = 0;
    auto next = [&](std::size_t n) {
        state = state * 1664525u + 1013904223u;
        return static_cast<std::size_t>((state >> 8) % n);
    };
    std::string text;
    std::size_t changed = 0;
    const std::uint32_t kSeeds[] = {12345u, 777u, 4242u, 99991u};
    const int count = 600;
    for (int i = 0; i < count; ++i) {
        if (i % 150 == 0) {
            state = kSeeds[i / 150];
        }
        std::string line;
        bool has_hidden = false;
        bool glued = false;  // a join that makes the fragments run into each other
        const int parts = 2 + static_cast<int>(next(4));
        for (int k = 0; k < parts; ++k) {
            const bool pick_hidden = next(3) == 0;
            line += pick_hidden ? hidden[next(hidden.size())] : plain[next(plain.size())];
            has_hidden = has_hidden || pick_hidden;
            if (k + 1 < parts) {
                const std::string& join = joins[next(joins.size())];
                glued = glued || join == ":" || join == "." || join == "/" || join == "-" || join == "[" || join == "=";
                line += join;
            }
        }
        const std::string redacted = R(line);
        CHECK_MESSAGE(R(redacted) == redacted, line);  // idempotent
        changed += redacted != line ? 1 : 0;
        if (has_hidden && !glued) {
            CHECK_MESSAGE(redacted != line, line);
        }
        text += redacted + "\n";
    }
    CHECK(changed > 150);
    TempDir dir("agree");
    const fs::path file = dir.path / "report.txt";
    WriteFile(file, std::string(report::kMarker) + "\n" + text);
    int exit_code = -1;
    std::string checker_output;
    if (RunChecker(file, {"fakeuser", "Fake Console"}, &exit_code, &checker_output)) {
        CHECK_MESSAGE(exit_code == 0, checker_output);
    }
}

// ---- Pending -----------------------------------------------------------------------------------------------

TEST_CASE("Pending: a crash file newer than the newest report, both ways") {
    TempDir dir("pending");
    const fs::path log = dir.path / "log";
    const std::string user = dir.str();
    CHECK_FALSE(report::Pending(user));  // no log folder
    fs::create_directories(log);
    CHECK_FALSE(report::Pending(user));  // nothing in it

    const auto now = fs::file_time_type::clock::now();
    WriteFile(log / "crash-100.txt", "mh4u-native crash v1\n");
    fs::last_write_time(log / "crash-100.txt", now - std::chrono::hours(3));
    CHECK(report::Pending(user));  // a crash and no report

    WriteFile(log / "report-1.txt", "x\n");
    fs::last_write_time(log / "report-1.txt", now - std::chrono::hours(2));
    CHECK_FALSE(report::Pending(user));  // the report is newer

    WriteFile(log / "crash-200.txt", "mh4u-native crash v1\n");
    fs::last_write_time(log / "crash-200.txt", now - std::chrono::hours(1));
    CHECK(report::Pending(user));  // a newer crash

    WriteFile(log / "report-2.txt", "x\n");
    fs::last_write_time(log / "report-2.txt", now);
    CHECK_FALSE(report::Pending(user));

    // an empty crash file (opened in advance, never written) is not a crash, and neither are other names
    WriteFile(log / "crash-300.txt", "");
    fs::last_write_time(log / "crash-300.txt", now + std::chrono::hours(1));
    WriteFile(log / "crash-400.log", "mh4u-native crash v1\n");
    fs::last_write_time(log / "crash-400.log", now + std::chrono::hours(1));
    WriteFile(log / "mh4u_native_log.txt", "[Info] x\n");
    CHECK_FALSE(report::Pending(user));
}

// ---- Make --------------------------------------------------------------------------------------------------

namespace {

// A synthetic user folder: settings with every [Multiplayer] key set, a build record, a log full of the
// things a report must not keep, a crash file, and a config savegame whose console user name is made up.
void MakeUserFolder(const fs::path& user) {
    WriteFile(user / "mh4u-app.ini",
              "# synthetic\n"
              "[Game]\n"
              "game = /synthetic/dir/MH4U Fake.cxi\n"
              "launcher = false\n"
              "runtime = azahar\n"
              "[Display]\n"
              "renderer = vulkan\n"
              "fullscreen = true\n"
              "shader = fake-shader.glsl\n"
              "screens = overlay\n"
              "[Audio]\n"
              "volume = 40\n"
              "output = auto\n"
              "[Controls]\n"
              "controller = auto\n"
              "mouse_sensitivity = 50\n"
              "stylus_key = Left Alt\n"
              "param_button_a = engine:sdl,port:0,button:0 ; a comment\n"
              "[Multiplayer]\n"
              "nickname = Fake Hunter\n"
              "join = 192.0.2.7:24872\n"
              "password = hunter2fake\n"
              "host = 24872\n"
              "room_port = 24873\n"
              "room_name = Fake Room\n"
              "room_members = 4\n"
              "lobby = lobby.example.invalid\n"
              "token = faketoken123\n"
              "room_public = true\n"
              "last_room = mh4u://192.0.2.7:24872?password=hunter2fake\n"
              "[Mods]\n"
              "catalog_url = https://example.invalid/catalog.json\n"
              "[Debug]\n"
              "perf_log = /synthetic/dir/perf.csv\n");
    WriteFile(user / "mh4u.ini",
              "# MH4U PC port options.\n"
              "fps = 120\n"
              "widescreen = 16:9\n"
              "touch_overlay = top-right 30 85\n"
              "skip_end = ZL+ZR\n");
    WriteFile(user / "recomp" / "0004000000126300_native.build.json",
              "{\n \"format\": 1,\n \"program_id\": \"0004000000126300\",\n"
              " \"library\": \"0004000000126300_native.so\",\n \"library_size\": 12345,\n"
              " \"library_mtime_ns\": 99,\n \"text_size\": 4096,\n \"code_size\": 8192,\n"
              " \"code_mods\": [\n  {\n   \"kind\": \"ips\",\n   \"sha256\": \"" + std::string(64, 'a') + "\"\n  }\n ],\n"
              " \"fingerprint\": \"" + std::string(64, 'b') + "\",\n \"changes_code\": false,\n"
              " \"built\": \"2026-10-22T12:34:56Z\"\n}\n");

    std::string log;
    log += "[Debug] Kernel: a debug line that stays out\n";
    log += "[Info] Loader: opened C:\\Users\\Fake Person\\Games\\MH4U\\game.cxi\n";
    log += "[Info] Net: peer 192.0.2.7:24872 and [2001:db8::1]:8080, mac 18:2A:7B:00:00:01\n";
    log += "[Info] Cfg: console id 0123456789abcdef for ConsoleFoo\n";
    log += "[Info] Net: invite mh4u://192.0.2.7:24872?password=hunter2fake\n";
    log += "[Info] Kernel: thread started on /opt/fake/lib/libfake.so.1 as fakeuser\n";
    log += "00000010  41 42 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  |ABCDEFGHIJKLMNOP|\n";
    log += "[Info] dense " + std::string(1100, 'A') + "\n";
    log += "[Info] hash " + std::string(64, 'c') + "\n";
    for (int i = 0; i < 80; ++i) {
        log += "[Warning] ipc: cmd=0x" + std::to_string(10000 + i) + " not implemented\n";
    }
    log += "[Error] Kernel: bad access at frame 77\n";
    log += "Service.FS <Info> sync at frame 78\n";
    WriteFile(user / "log" / "mh4u_native_log.txt", log);
    WriteFile(user / "log" / "crash-4242.txt",
              "mh4u-native crash v1\ncommit: abcdef0\nsignal: SIGSEGV (11)\nfault address: 0x0000000000000010\n"
              "game frame: 321\nscene: 5\nframes:\n#0 mh4u-native+0x1234\n#1 libc.so.6+0x2a1ca\n");

    // the console's user name, in the config savegame
    mh4n::cfg::ConfigStore store(user.generic_string());
    unsigned char block[0x1C] = {};
    REQUIRE(!mh4n::IsError(store.Get(mh4n::cfg::kBlockUsername, sizeof block, mh4n::cfg::kGlobal, block)));
    const char16_t name[] = u"ConsoleFoo";
    for (int i = 0; i < 10; ++i) {
        block[i * 2] = static_cast<unsigned char>(name[i] & 0xFF);
        block[i * 2 + 1] = static_cast<unsigned char>(name[i] >> 8);
    }
    REQUIRE(!mh4n::IsError(store.Set(mh4n::cfg::kBlockUsername, sizeof block, mh4n::cfg::kGlobal, block)));
    REQUIRE(store.Save());
}

}  // namespace

TEST_CASE("CollectNames reads the console's user name only from a config savegame that exists") {
    TempDir bare("names-bare");
    const std::vector<std::string> none = report::CollectNames(bare.str());
    REQUIRE(none.size() == 2);
    CHECK(none[1].empty());
    CHECK_FALSE(fs::exists(bare.path / "nand"));  // never made
    CHECK_FALSE(fs::exists(bare.path / "sysdata"));

    TempDir full("names-full");
    MakeUserFolder(full.path);
    const std::vector<std::string> names = report::CollectNames(full.str());
    REQUIRE(names.size() == 2);
    CHECK(names[1] == "ConsoleFoo");
}

TEST_CASE("Make: a report that passes the checker, with the settings allowlist and the log rules") {
    TempDir dir("make");
    MakeUserFolder(dir.path);
    report::MakeInput input;
    input.user_dir = dir.str();
    input.app_commit = "0123456789";
    input.system.os = "Linux";
    input.system.os_version = "6.8.0";
    input.system.cpu_model = "Fake CPU 9000";
    input.system.logical_cores = 12;
    input.system.ram_bytes = 16ull * 1024 * 1024 * 1024;
    input.system.gpu_name = "Fake GPU 1";
    input.system.driver_version = "560.94";
    input.system.vulkan_version = "1.3.280";
    input.system.display_modes = {"3440x1440@120", "1920x1080@60"};
    input.names = {"fakeuser", ""};  // the console's name is filled in from the savegame
    input.last_status = "exit 3 after 4521 frames";
    const fs::path out = dir.path / "log" / "report-1.txt";
    REQUIRE(report::Make(input, out.generic_string()));

    const std::string text = ReadFile(out);
    CAPTURE(text);
    const std::vector<std::string> lines = Lines(text);
    REQUIRE(lines.size() > 20);
    CHECK(lines[0] == "mh4u-native alpha report v1");
    CHECK(lines.size() <= 650);

    // versions and system
    CHECK(Contains(text, "app commit: 0123456789\n"));
    CHECK(Contains(text, "code library 0004000000126300_native.so, format 1, program 0004000000126300, size 12345, "
                         "text 4096, code 8192, changes code false, built 2026-10-22T12:34:56Z, code mods 1 (ips), "
                         "fingerprint bbbbbbbbbbbbbbbb\n"));
    CHECK(Contains(text, "os: Linux 6.8.0\n"));
    CHECK(Contains(text, "ram: 16384 MiB\n"));
    CHECK(Contains(text, "display mode: 3440x1440@120\n"));
    CHECK(Contains(text, "exit 3 after 4521 frames\n"));

    // settings by allowlist: the four sections and mh4u.ini; paths as <path>; never [Multiplayer], [Mods], [Debug]
    CHECK(Contains(text, "[Game]\ngame = <path>\nlauncher = false\nruntime = azahar\n"));
    CHECK(Contains(text, "[Display]\nrenderer = vulkan\nfullscreen = true\nshader = <path>\nscreens = overlay\n"));
    CHECK(Contains(text, "[Audio]\nvolume = 40\noutput = auto\n"));
    CHECK(Contains(text, "[Controls]\ncontroller = auto\nmouse_sensitivity = 50\nstylus_key = Left Alt\n"
                         "param_button_a = engine:sdl,port:0,button:0\n"));
    CHECK(Contains(text, "mh4u.ini\nfps = 120\nwidescreen = 16:9\ntouch_overlay = top-right 30 85\nskip_end = ZL+ZR\n"));
    for (const char* never : {"[Multiplayer]", "[Mods]", "[Debug]", "nickname", "Fake Hunter", "hunter2fake", "24872",
                              "24873", "Fake Room", "lobby", "faketoken123", "catalog", "last_room", "perf",
                              "192.0.2.7", "2001:db8", "18:2A:7B", "0123456789abcdef", "ConsoleFoo", "fakeuser",
                              "Fake Person", "/opt/fake", "MH4U Fake", "mh4u://"}) {
        CHECK_MESSAGE(!Contains(text, never), never);
    }

    // the crash file, redacted
    CHECK(Contains(text, "== newest crash file ==\nmh4u-native crash v1\ncommit: abcdef0\nsignal: SIGSEGV (11)\n"));
    CHECK(Contains(text, "#1 libc.so.6+0x2a1ca\n"));

    // the log: Debug out, the lines with things redacted, listing and dense lines out, the traces limited
    CHECK_FALSE(Contains(text, "a debug line"));
    CHECK(Contains(text, "[Info] Loader: opened <path>\n"));
    CHECK(Contains(text, "[Info] Net: peer <address> and <address>, mac <mac>\n"));
    CHECK(Contains(text, "[Info] Cfg: console id <console-id> for <console-user>\n"));
    CHECK(Contains(text, "[Info] Net: invite <invite>\n"));
    CHECK(Contains(text, "[Info] Kernel: thread started on <path> as <os-user>\n"));
    CHECK_FALSE(Contains(text, "ABCDEFGHIJKLMNOP"));
    CHECK_FALSE(Contains(text, "AAAAAAAAAA"));
    CHECK_FALSE(Contains(text, "cccccccccccccccc"));
    CHECK(Contains(text, "30 trace-like lines left out\n"));
    int traces = 0;
    for (const std::string& line : lines) {
        traces += Contains(line, "cmd=0x") ? 1 : 0;
    }
    CHECK(traces == 50);
    CHECK(Contains(text, "[Warning] ipc: cmd=0x10079 not implemented\n"));  // the newest stay
    CHECK_FALSE(Contains(text, "cmd=0x10029 "));                            // the 30 oldest went
    CHECK(Contains(text, "[Error] Kernel: bad access at frame 77\n"));
    CHECK(Contains(text, "Service.FS <Info> sync at frame 78\n"));

    // the checker says ok
    int exit_code = -1;
    std::string checker_output;
    if (RunChecker(out, {"fakeuser", "ConsoleFoo", "Fake Person", "Fake Hunter"}, &exit_code, &checker_output)) {
        CAPTURE(checker_output);
        CHECK(exit_code == 0);
        CHECK(checker_output == "ok\n");
    }
}

TEST_CASE("Make on a bare user folder still writes a report the checker accepts") {
    TempDir dir("make-bare");
    report::MakeInput input;
    input.user_dir = dir.str();
    const fs::path out = dir.path / "report-bare.txt";
    REQUIRE(report::Make(input, out.generic_string()));
    const std::string text = ReadFile(out);
    CAPTURE(text);
    CHECK(Lines(text)[0] == "mh4u-native alpha report v1");
    CHECK(Contains(text, "mh4u-app.ini: none\n"));
    CHECK(Contains(text, "== newest crash file ==\nnone\n"));
    CHECK(Contains(text, "== log: none =="));
    CHECK_FALSE(fs::exists(dir.path / "nand"));  // Make never creates the console's config
    int exit_code = -1;
    std::string checker_output;
    if (RunChecker(out, {}, &exit_code, &checker_output)) {
        CAPTURE(checker_output);
        CHECK(exit_code == 0);
    }
}

TEST_CASE("Make keeps the report under the checker's line limit with the most log it can") {
    TempDir dir("make-long");
    std::string log;
    for (int i = 0; i < 2000; ++i) {
        log += "[Info] Kernel: line " + std::to_string(i) + "\n";
    }
    WriteFile(dir.path / "log" / "mh4u_native_log.txt", log);
    report::MakeInput input;
    input.user_dir = dir.str();
    const fs::path out = dir.path / "report-long.txt";
    REQUIRE(report::Make(input, out.generic_string()));
    const std::string text = ReadFile(out);
    const std::vector<std::string> lines = Lines(text);
    CHECK(lines.size() <= 650);
    CHECK(lines.size() > 500);
    CHECK(Contains(text, "[Info] Kernel: line 1999\n"));    // the newest line is there
    CHECK_FALSE(Contains(text, "[Info] Kernel: line 1400\n"));  // 500 lines at most
}

// ---- the child side of the crash tests -------------------------------------------------------------------

namespace {

volatile int g_sink;

int RunCrashScenario(const std::string& name, const std::string& user_dir) {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // no dialog for a crash on purpose
#else
    rlimit no_core = {0, 0};
    setrlimit(RLIMIT_CORE, &no_core);  // and no core file
#endif
    if (name == "previous") {
#if !defined(_WIN32)
        struct sigaction action;
        std::memset(&action, 0, sizeof action);
        action.sa_handler = [](int) {
            const char text[] = "previous handler\n";
            (void)!write(1, text, sizeof text - 1);
            _exit(77);
        };
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGBUS, &action, nullptr);  // (a null access is a SIGBUS on some platforms)
#endif
    }
    report::Install(user_dir);
    if (name == "clean") {
        return 0;
    }
    if (name != "unpublished") {
        report::SetGameState(1234, 7);
    }
    if (name == "memory") {
#if !defined(_WIN32)
        auto memory = mh4n::GuestMemory::Create();  // installs its fault handler after ours
        if (memory == nullptr) {
            return 1;
        }
#endif
    }
    if (name == "abort") {
        std::abort();
    }
    g_sink = *static_cast<volatile int*>(nullptr);  // a fault of the process
    return 0;                                         // not reached
}

}  // namespace

int main(int argc, char** argv) {
    g_self_path = argv[0];
#if defined(__linux__)
    {
        char path[4096];
        const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        if (n > 0) {
            path[n] = '\0';
            g_self_path = path;
        }
    }
#endif
    if (argc >= 4 && std::strcmp(argv[1], "--crash-test") == 0) {
        return RunCrashScenario(argv[2], argv[3]);
    }
    return doctest::Context(argc, argv).run();
}
