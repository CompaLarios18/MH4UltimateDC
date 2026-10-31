// (Windows Vista's file dialogs: IFileOpenDialog picks folders)
#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "launcher/pick.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <utility>

#include "launcher/paths.h"

#ifdef _WIN32
#include <SDL.h>
#include <SDL_syswm.h>
#include <fmt/format.h>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include "common/string_util.h"
#else
// (the standard library and the system's own calls only: tests/test_app_pick.py builds this
// part on its own, with dialogs of its own making)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Launcher::Pick {

namespace {

SDL_Window* owner = nullptr;

#ifdef _WIN32
HWND OwnerWindow() {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (owner && SDL_GetWindowWMInfo(owner, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        return info.info.win.window;
    }
    return nullptr;
}

/// A path with Windows' own separators, for its dialogs.
std::wstring WindowsPath(const std::string& path) {
    std::wstring w = Common::UTF8ToUTF16W(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

Result WindowsFile(const Request& request) {
    Result result;
    result.program = "Windows' file dialog";
    std::wstring patterns;
    for (const auto& ext : request.extensions) {
        patterns += (patterns.empty() ? L"" : L";") + std::wstring(L"*.") + Common::UTF8ToUTF16W(ext);
    }
    std::wstring filter;
    if (!patterns.empty()) {
        filter = Common::UTF8ToUTF16W(request.kind);
        filter.push_back(L'\0');
        filter += patterns;
        filter.push_back(L'\0');
    }
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    wchar_t file[MAX_PATH * 4] = {};
    // the file chosen last time, filled in
    if (!request.select.empty() && !request.extensions.empty() &&
        Paths::HasExtension(request.select, request.extensions)) {
        const std::wstring select = Common::UTF8ToUTF16W(request.select);
        if (select.size() < std::size(file)) {
            std::copy(select.begin(), select.end(), file);
        }
    }
    const std::wstring start = WindowsPath(request.start);
    const std::wstring title = Common::UTF8ToUTF16W(request.title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = OwnerWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        result.path = Common::UTF16ToUTF8(file);
    } else if (const DWORD error = CommDlgExtendedError(); error != 0) {
        result.error = fmt::format("error 0x{:04X}", static_cast<unsigned>(error));
    }
    return result;
}

Result WindowsFolder(const Request& request) {
    Result result;
    result.program = "Windows' folder dialog";
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(created)) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(Common::UTF8ToUTF16W(request.title).c_str());
        if (!request.start.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(WindowsPath(request.start).c_str(), nullptr,
                                                      IID_PPV_ARGS(&folder)))) {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        const HRESULT shown = dialog->Show(OwnerWindow());
        if (SUCCEEDED(shown)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result.path = Common::UTF16ToUTF8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        } else if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            result.error = fmt::format("error 0x{:08X}", static_cast<unsigned>(shown));
        }
        dialog->Release();
    } else {
        result.error = fmt::format("it didn't open (error 0x{:08X})", static_cast<unsigned>(created));
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return result;
}
#else
using Clock = std::chrono::steady_clock;
constexpr auto KILL_AFTER = std::chrono::seconds(2);  // after Cancel(): then forcibly
constexpr std::size_t KEPT = 64 * 1024;               // of what a dialog writes, for the path and the log

#ifdef __APPLE__
/// Double quotes for AppleScript.
std::string AppleString(const std::string& s) {
    std::string q = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            q += '\\';
        }
        q += c;
    }
    return q + "\"";
}
#else
/// `program` is in a folder of the PATH.
bool OnPath(const char* program) {
    const char* path = std::getenv("PATH");
    const std::string folders = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = folders.find(':', start);
        std::string folder = folders.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (folder.empty()) {
            folder = ".";
        }
        if (access((folder + "/" + program).c_str(), X_OK) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            return false;
        }
        start = end + 1;
    }
}
#endif

/// This process's environment for a program of the system's (a dialog): an AppImage's own
/// libraries left out of LD_LIBRARY_PATH and LD_PRELOAD. The dialog is built for the system's,
/// and the AppImage's own exec hook, which would leave them out, doesn't see posix_spawn.
std::vector<std::string> SystemEnvironment() {
    const char* appdir = std::getenv("APPDIR");
    const std::string inside = appdir && *appdir ? appdir : "";
    std::vector<std::string> environment;
    for (char** e = environ; *e; ++e) {
        std::string entry = *e;
        for (const auto& [name, separators] : {std::pair<const char*, const char*>{"LD_LIBRARY_PATH=", ":;"},
                                               std::pair<const char*, const char*>{"LD_PRELOAD=", ": "}}) {
            if (inside.empty() || entry.rfind(name, 0) != 0) {
                continue;
            }
            const std::string value = entry.substr(std::strlen(name));
            std::string kept;
            std::size_t start = 0;
            while (start <= value.size()) {
                std::size_t end = value.find_first_of(separators, start);
                if (end == std::string::npos) {
                    end = value.size();
                }
                const std::string part = value.substr(start, end - start);
                if (!part.empty() && part.rfind(inside, 0) != 0) {
                    kept += (kept.empty() ? "" : ":") + part;
                }
                start = end + 1;
            }
            entry = kept.empty() ? std::string() : name + kept;
        }
        if (!entry.empty()) {
            environment.push_back(std::move(entry));
        }
    }
    return environment;
}

/// The last lines a dialog wrote on its error output, on one line for the log.
std::string Tail(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        start = end + 1;
    }
    std::string out;
    for (std::size_t i = lines.size() > 6 ? lines.size() - 6 : 0; i < lines.size(); ++i) {
        out += (out.empty() ? "" : " | ") + lines[i];
    }
    if (out.size() > 800) {
        out = "..." + out.substr(out.size() - 800);
    }
    return out;
}

#ifdef __APPLE__
/// Status 1: osascript's cancel is error -128 ("User canceled"), anything else went wrong.
bool Cancelled(double, const std::string& err) {
    return err.find("-128") != std::string::npos;
}
#else
constexpr double AT_ONCE = 0.5;  // seconds: a dialog closed this soon never showed

/// What a toolkit says when its dialog can't show at all (no display to open, no platform).
bool CantShow(const std::string& err) {
    std::string lower = err;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* says : {"cannot open display", "could not connect to display", "failed to open display",
                             "dispatching to wayland display", "no qt platform plugin"}) {
        if (lower.find(says) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Status 1 is zenity's and kdialog's cancel, unless the dialog closed before it could have
/// shown, or said it couldn't show.
bool Cancelled(double took, const std::string& err) {
    return took >= AT_ONCE && !CantShow(err);
}
#endif
#endif

} // namespace

struct Dialog::Impl {
    Result result;
    bool done = true;
#ifndef _WIN32
    pid_t pid = -1;
    int out = -1;  ///< the read end of its output (the path)
    int err = -1;  ///< and of its error output (for the log)
    std::string out_text, err_text;
    Clock::time_point started{}, cancelled{};
    bool cancel = false;
    bool killed = false;

    bool Spawn(const std::vector<std::string>& args, std::string& error);
    void Read(int& fd, std::string& text);
    void Close();
    /// Its end: the path it printed, or why it didn't work (`known` false: no status to read).
    void Finish(bool known, int status);
#endif
};

#ifndef _WIN32
bool Dialog::Impl::Spawn(const std::vector<std::string>& args, std::string& error) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        error = std::strerror(errno);
        for (const int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
        return false;
    }
    // no end leaks into other children started meanwhile (dup2 gives the dialog its own), and
    // the launcher reads its ends without waiting
    for (const int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    for (const int fd : {out_pipe[0], err_pipe[0]}) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // a process group of its own (Cancel() reaches what it started too), default signal
    // handling and no blocked signals whatever this thread has
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                              POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attributes, &none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigdefault(&attributes, &defaults);

    std::vector<char*> argv;
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    std::vector<std::string> environment = SystemEnvironment();
    std::vector<char*> envp;
    for (auto& entry : environment) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    const int spawned = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawned != 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        pid = -1;
        error = std::strerror(spawned);
        return false;
    }
    out = out_pipe[0];
    err = err_pipe[0];
    started = Clock::now();
    return true;
}

void Dialog::Impl::Read(int& fd, std::string& text) {
    if (fd < 0) {
        return;
    }
    char buffer[4096];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            if (text.size() < KEPT) {
                text.append(buffer, static_cast<std::size_t>(n));
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(fd);  // the end of it (or a pipe that can't be read)
            fd = -1;
        }
        return;
    }
}

void Dialog::Impl::Close() {
    for (int* fd : {&out, &err}) {
        if (*fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}

void Dialog::Impl::Finish(bool known, int status) {
    Read(out, out_text);
    Read(err, err_text);
    // (something it started may still hold its own ends: what it printed is in already)
    Close();
    pid = -1;
    done = true;
    if (cancel) {
        return;  // given up on: nothing comes of it
    }
    const double took = std::chrono::duration<double>(Clock::now() - started).count();
    std::string path = out_text.substr(0, out_text.find('\n'));
    while (!path.empty() && path.back() == '\r') {
        path.pop_back();
    }
    if (known && WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        const char* name = strsignal(number);
        result.error = "it stopped on signal " + std::to_string(number) + " (" + (name ? name : "unknown") + ")";
    } else {
        const int code = known && WIFEXITED(status) ? WEXITSTATUS(status) : 0;
        if (code == 0) {
            result.path = path;  // (empty: nothing was chosen)
        } else if (code != 1 || !Cancelled(took, err_text)) {
            char seconds[32];
            std::snprintf(seconds, sizeof(seconds), "%.1f", took);
            result.error = "it closed after " + std::string(seconds) + " s with status " + std::to_string(code);
        }
    }
    if (!result.error.empty()) {
        if (const std::string said = Tail(err_text); !said.empty()) {
            result.error += ", saying: " + said;
        }
    }
}
#endif

void SetOwner(SDL_Window* window) {
    owner = window;
}

bool Available() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return OnPath("zenity") || OnPath("kdialog");
#endif
}

bool GameMode() {
#if defined(_WIN32) || defined(__APPLE__)
    return false;
#else
    const auto value = [](const char* name) {
        const char* v = std::getenv(name);
        return std::string(v ? v : "");
    };
    std::string desktop = value("XDG_CURRENT_DESKTOP");
    std::transform(desktop.begin(), desktop.end(), desktop.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !value("GAMESCOPE_WAYLAND_DISPLAY").empty() || desktop.find("gamescope") != std::string::npos ||
           value("SteamGamepadUI") == "1";
#endif
}

Dialog::Dialog() : impl(std::make_unique<Impl>()) {}

Dialog::~Dialog() {
#ifndef _WIN32
    Impl& d = *impl;
    if (!d.done && d.pid > 0) {
        kill(-d.pid, SIGTERM);
        // a moment to go, then forcibly
        int status = 0;
        bool gone = false;
        for (int i = 0; i < 50 && !gone; ++i) {
            const pid_t ended = waitpid(d.pid, &status, WNOHANG);
            gone = ended == d.pid || (ended < 0 && errno != EINTR);
            if (!gone) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!gone) {
            kill(-d.pid, SIGKILL);
            while (waitpid(d.pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    d.Close();
#endif
}

void Dialog::Start(const Request& request) {
    Impl& d = *impl;
    d.result = Result{};
    d.done = true;
#if defined(_WIN32)
    d.result = request.folder ? WindowsFolder(request) : WindowsFile(request);
#else
    // where it opens: the folder, with the last choice in it shown
    std::string where;
    if (!request.start.empty()) {
        where = request.start + "/" + request.select;
    }
    std::vector<std::string> args;
#ifdef __APPLE__
    d.result.program = "osascript";
    const std::string location =
        request.start.empty() ? "" : " default location (POSIX file " + AppleString(request.start) + ")";
    std::string script;
    if (request.folder) {
        script = "POSIX path of (choose folder with prompt " + AppleString(request.title) + location + ")";
    } else {
        std::string types;
        for (const auto& ext : request.extensions) {
            types += (types.empty() ? "" : ", ") + AppleString(ext);
        }
        script = "POSIX path of (choose file with prompt " + AppleString(request.title) +
                 (types.empty() ? "" : " of type {" + types + "}") + location + ")";
    }
    args = {"osascript", "-e", script};
#else
    // each extension in both cases (a GTK filter's patterns heed it)
    std::string patterns;
    for (const auto& ext : request.extensions) {
        std::string upper = ext;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        patterns += (patterns.empty() ? "*." : " *.") + ext + (upper != ext ? " *." + upper : "");
    }
    if (OnPath("zenity")) {
        d.result.program = "zenity";
        args = {"zenity", "--file-selection", "--title=" + request.title};
        if (request.folder) {
            args.push_back("--directory");
        } else if (!patterns.empty()) {
            args.push_back("--file-filter=" + request.kind + " | " + patterns);
            args.push_back("--file-filter=All files | *");
        }
        if (!where.empty()) {
            args.push_back("--filename=" + where);
        }
    } else if (OnPath("kdialog")) {
        d.result.program = "kdialog";
        args = {"kdialog", "--title", request.title,
                request.folder ? "--getexistingdirectory" : "--getopenfilename", where.empty() ? "." : where};
        if (!request.folder && !patterns.empty()) {
            args.push_back(patterns + "|" + request.kind);
        }
    } else {
        d.result.available = false;
        return;
    }
#endif
    std::string error;
    if (!d.Spawn(args, error)) {
        d.result.error = "it didn't start: " + error;
        return;
    }
    d.done = false;
#endif
}

bool Dialog::Poll() {
#ifndef _WIN32
    Impl& d = *impl;
    if (d.done) {
        return true;
    }
    d.Read(d.out, d.out_text);
    d.Read(d.err, d.err_text);
    int status = 0;
    const pid_t ended = waitpid(d.pid, &status, WNOHANG);
    if (ended == d.pid) {
        d.Finish(true, status);
        return true;
    }
    if (ended < 0 && errno != EINTR) {
        d.Finish(false, 0);  // (no status to wait for: what it printed is all there is)
        return true;
    }
    if (d.cancel && !d.killed && Clock::now() - d.cancelled > KILL_AFTER) {
        kill(-d.pid, SIGKILL);
        d.killed = true;
    }
    return false;
#else
    return true;
#endif
}

void Dialog::Cancel() {
#ifndef _WIN32
    Impl& d = *impl;
    if (d.done || d.cancel) {
        return;
    }
    d.cancel = true;
    d.cancelled = Clock::now();
    kill(-d.pid, SIGTERM);
#endif
}

const Result& Dialog::Get() const {
    return impl->result;
}

} // namespace Launcher::Pick
