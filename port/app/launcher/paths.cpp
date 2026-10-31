// (Windows Vista's known folders: SHGetKnownFolderPath)
#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include "launcher/paths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace Launcher::Paths {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t MOST = 20000;  // entries a listing holds

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// (UTF-8 both ways: a std::string path is read in the ANSI code page on Windows)
fs::path ToPath(const std::string& s) {
    return fs::path(Wide(s));
}

std::string FromPath(const fs::path& p) {
    return Narrow(p.wstring());
}

bool IsSeparator(char c) {
    return c == '/' || c == '\\';
}

// FOLDERID_Desktop and FOLDERID_Downloads (knownfolders.h), here so that no library has to
// define them
const GUID DESKTOP_FOLDER = {0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};
const GUID DOWNLOADS_FOLDER = {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};

std::string KnownFolder(const GUID& id) {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) {
        out = Narrow(path);
    }
    CoTaskMemFree(path);
    return out;
}
#else
fs::path ToPath(const std::string& s) {
    return fs::path(s);
}

std::string FromPath(const fs::path& p) {
    return p.string();
}

bool IsSeparator(char c) {
    return c == '/';
}
#endif

char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

std::string Trim(const std::string& s) {
    const char* space = " \t\r\n";
    const auto first = s.find_first_not_of(space);
    if (first == std::string::npos) {
        return "";
    }
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

bool StartsWithAnyCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (Lower(s[i]) != Lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

int Hex(char c) {
    if (IsDigit(c)) {
        return c - '0';
    }
    const char l = Lower(c);
    return l >= 'a' && l <= 'f' ? l - 'a' + 10 : -1;
}

/// %20 and the like as their bytes (a file:// address).
std::string PercentDecoded(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && Hex(s[i + 1]) >= 0 && Hex(s[i + 2]) >= 0) {
            out += static_cast<char>(Hex(s[i + 1]) * 16 + Hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

/// <0, 0 or >0: `a` before, beside or after `b` (NameLess).
int CompareNames(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (IsDigit(a[i]) && IsDigit(b[j])) {
            // two numbers: by their value (the longer one, leading zeros aside, is larger)
            std::size_t a_end = i, b_end = j;
            while (a_end < a.size() && IsDigit(a[a_end])) {
                ++a_end;
            }
            while (b_end < b.size() && IsDigit(b[b_end])) {
                ++b_end;
            }
            while (i + 1 < a_end && a[i] == '0') {
                ++i;
            }
            while (j + 1 < b_end && b[j] == '0') {
                ++j;
            }
            if (a_end - i != b_end - j) {
                return a_end - i < b_end - j ? -1 : 1;
            }
            for (; i < a_end; ++i, ++j) {
                if (a[i] != b[j]) {
                    return a[i] < b[j] ? -1 : 1;
                }
            }
            continue;
        }
        const unsigned char x = static_cast<unsigned char>(Lower(a[i]));
        const unsigned char y = static_cast<unsigned char>(Lower(b[j]));
        if (x != y) {
            return x < y ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size()) {
        return 1;
    }
    return j < b.size() ? -1 : 0;
}

bool Hidden(const fs::path& path, const std::string& name) {
    if (!name.empty() && name[0] == '.') {
        return true;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
    (void)path;
    return false;
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
#endif

/// /proc/mounts writes a space, a tab, a newline and a backslash in a path as \040 and so on.
std::string MountUnescaped(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            const char a = s[i + 1], b = s[i + 2], c = s[i + 3];
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                out += static_cast<char>((a - '0') * 64 + (b - '0') * 8 + (c - '0'));
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

} // namespace

bool HasExtension(const std::string& name, const std::vector<std::string>& extensions) {
    if (extensions.empty()) {
        return true;
    }
    for (const auto& ext : extensions) {
        // (a name that is only the extension doesn't count)
        if (ext.empty() || name.size() <= ext.size() + 1) {
            continue;
        }
        const std::size_t dot = name.size() - ext.size() - 1;
        if (name[dot] == '.' && StartsWithAnyCase(name.substr(dot + 1), ext)) {
            return true;
        }
    }
    return false;
}

bool NameLess(const std::string& a, const std::string& b) {
    const int order = CompareNames(a, b);
    // the same but for case or leading zeros: still one order, the bytes'
    return order != 0 ? order < 0 : a < b;
}

std::string Clean(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    fs::path p = ToPath(path);
#ifdef _WIN32
    // "C:" alone is that drive's root here, not the folder this program is at on it
    if (p.has_root_name() && !p.has_root_directory() && p.relative_path().empty()) {
        p = ToPath(path + "\\");
    }
#endif
    std::error_code ec;
    fs::path absolute = fs::absolute(p, ec);
    if (ec) {
        absolute = p;
    }
    absolute = absolute.lexically_normal();
    std::string s = FromPath(absolute);
    const std::size_t root = FromPath(absolute.root_path()).size();
    while (s.size() > root && s.size() > 1 && IsSeparator(s.back())) {
        s.pop_back();
    }
    return s;
}

std::string Parent(const std::string& path) {
    const std::string clean = Clean(path);
    if (clean.empty()) {
        return "";
    }
    const fs::path p = ToPath(clean);
    if (!p.has_relative_path()) {
        return "";  // a root
    }
    return Clean(FromPath(p.parent_path()));
}

std::string Name(const std::string& path) {
    const std::string clean = Clean(path);
    const fs::path p = ToPath(clean);
    if (!p.has_relative_path()) {
        return clean;
    }
    return FromPath(p.filename());
}

Kind What(const std::string& path) {
    if (path.empty()) {
        return Kind::Missing;
    }
    std::error_code ec;
    const fs::file_status status = fs::status(ToPath(path), ec);
    if (ec || !fs::exists(status)) {
        return Kind::Missing;
    }
    return fs::is_directory(status) ? Kind::Folder : Kind::File;
}

Listing List(const std::string& folder, const std::vector<std::string>& extensions, bool files) {
    Listing listing;
    listing.folder = Clean(folder);
    std::error_code ec;
    fs::directory_iterator it(ToPath(listing.folder), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        listing.error = ec.message();
        return listing;
    }
    std::vector<Entry> folders, found;
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& path = it->path();
        Entry e;
        e.name = FromPath(path.filename());
        if (e.name.empty() || Hidden(path, e.name)) {
            continue;
        }
        e.path = FromPath(path);
        // (a link counts as what it leads to)
        std::error_code type;
        if (it->is_directory(type)) {
            e.folder = true;
            folders.push_back(std::move(e));
        } else if (files && it->is_regular_file(type) && HasExtension(e.name, extensions)) {
            std::error_code size_ec;
            const auto size = it->file_size(size_ec);
            e.size = size_ec ? 0 : static_cast<std::uint64_t>(size);
            found.push_back(std::move(e));
        }
    }
    const auto by_name = [](const Entry& a, const Entry& b) { return NameLess(a.name, b.name); };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(found.begin(), found.end(), by_name);
    listing.entries = std::move(folders);
    listing.entries.insert(listing.entries.end(), std::make_move_iterator(found.begin()),
                           std::make_move_iterator(found.end()));
    if (listing.entries.size() > MOST) {
        listing.left_out = listing.entries.size() - MOST;
        listing.entries.resize(MOST);
    }
    return listing;
}

std::string FromTyped(const std::string& text, const std::string& base, const std::string& home) {
    std::string s = Trim(text);
    // "Copy as path" on Windows, a shell's quotes
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        s = Trim(s.substr(1, s.size() - 2));
    }
    // a file manager's copy: file:///home/deck/My%20Games/...
    if (StartsWithAnyCase(s, "file://")) {
        s = s.substr(7);
        if (StartsWithAnyCase(s, "localhost/")) {
            s = s.substr(9);
        }
        s = PercentDecoded(s);
#ifdef _WIN32
        // file:///C:/Games: the drive comes after the slash
        if (s.size() >= 3 && IsSeparator(s[0]) && std::isalpha(static_cast<unsigned char>(s[1])) && s[2] == ':') {
            s = s.substr(1);
        }
#endif
    }
    if (s.empty()) {
        return "";
    }
    if (s[0] == '~' && (s.size() == 1 || IsSeparator(s[1])) && !home.empty()) {
        s = home + s.substr(1);
    }
    const auto resolve = [&base](const std::string& typed) {
        fs::path p = ToPath(typed);
        if (p.is_relative() && !base.empty()) {
            p = ToPath(base) / p;
        }
        return Clean(FromPath(p));
    };
    std::string path = resolve(s);
#ifndef _WIN32
    // a path dragged into a terminal: My\ Games
    if (s.find('\\') != std::string::npos && What(path) == Kind::Missing) {
        std::string plain;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
            }
            plain += s[i];
        }
        const std::string unescaped = resolve(plain);
        if (What(unescaped) != Kind::Missing) {
            path = unescaped;
        }
    }
#endif
    return path;
}

std::string Home() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n > 0 && n < std::size(buffer)) {
        return Clean(Narrow(std::wstring(buffer, n)));
    }
    return "";
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return Clean(home);
    }
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir && *user->pw_dir) {
        return Clean(user->pw_dir);
    }
    return "";
#endif
}

std::string UserDir(const std::string& text, const std::string& key, const std::string& home) {
    const std::string name = "XDG_" + key + "_DIR";
    std::istringstream lines(text);
    std::string line, found;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind(name, 0) != 0) {
            continue;
        }
        std::string value = Trim(line.substr(name.size()));
        if (value.empty() || value[0] != '=') {
            continue;  // another key that starts the same
        }
        value = Trim(value.substr(1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        // \" and \\ are the file's only escapes
        std::string plain;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                ++i;
            }
            plain += value[i];
        }
        // "$HOME/..." or an absolute path, the only two forms it takes
        if (plain.rfind("$HOME", 0) == 0) {
            plain = home + plain.substr(5);
        } else if (plain.empty() || plain[0] != '/') {
            continue;
        }
        found = plain;  // (the last one counts)
    }
    return found;
}

std::vector<Place> Mounted(const std::string& mounts) {
    std::vector<Place> places;
    std::istringstream lines(mounts);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string device, point;
        if (!(fields >> device >> point)) {
            continue;
        }
        point = MountUnescaped(point);
        bool removable = false;
        for (const char* root : {"/run/media/", "/media/", "/mnt/"}) {
            const std::size_t length = std::strlen(root);
            removable = removable || (point.size() > length && point.compare(0, length, root) == 0);
        }
        while (point.size() > 1 && point.back() == '/') {
            point.pop_back();
        }
        if (!removable) {
            continue;
        }
        const std::string name = point.substr(point.find_last_of('/') + 1);
        std::string label = device.rfind("/dev/mmcblk", 0) == 0 ? "SD card" : name;
        if (std::any_of(places.begin(), places.end(), [&label](const Place& p) { return p.label == label; })) {
            label += " (" + name + ")";
        }
        if (std::none_of(places.begin(), places.end(), [&point](const Place& p) { return p.path == point; })) {
            places.push_back({label, point, true});
        }
    }
    return places;
}

std::vector<Place> Places() {
    std::vector<Place> places;
    const auto add = [&places](const std::string& label, const std::string& path, bool drive, bool check) {
        if (path.empty() || (check && What(path) != Kind::Folder)) {
            return;
        }
        const std::string clean = Clean(path);
        if (std::none_of(places.begin(), places.end(), [&clean](const Place& p) { return p.path == clean; })) {
            places.push_back({label, clean, drive});
        }
    };
    const std::string home = Home();
    add("Home", home, false, true);
#if defined(_WIN32)
    add("Desktop", KnownFolder(DESKTOP_FOLDER), false, true);
    add("Downloads", KnownFolder(DOWNLOADS_FOLDER), false, true);
    // the drive letters (not looked into: an empty card reader or disc drive answers slowly)
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((drives & (1u << i)) == 0) {
            continue;
        }
        const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
        const UINT type = GetDriveTypeW(Wide(root).c_str());
        if (type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN) {
            add(root.substr(0, 2), root, true, false);
        }
    }
#else
    std::string desktop, downloads;
#ifndef __APPLE__
    // the desktop's own names for them (Escritorio, Descargas, ...)
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const std::string dirs = ReadFile((config && *config ? std::string(config) : home + "/.config") + "/user-dirs.dirs");
    desktop = UserDir(dirs, "DESKTOP", home);
    downloads = UserDir(dirs, "DOWNLOAD", home);
#endif
    if (!home.empty()) {
        add("Desktop", desktop.empty() ? home + "/Desktop" : desktop, false, true);
        add("Downloads", downloads.empty() ? home + "/Downloads" : downloads, false, true);
    }
#ifdef __APPLE__
    // the start-up disk is a link to / there
    std::error_code ec;
    for (fs::directory_iterator it("/Volumes", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code type;
        if (!it->is_symlink(type) && it->is_directory(type)) {
            add(FromPath(it->path().filename()), FromPath(it->path()), true, false);
        }
    }
#else
    for (const auto& place : Mounted(ReadFile("/proc/mounts"))) {
        add(place.label, place.path, true, true);
    }
#endif
    add("Root (/)", "/", true, true);
#endif
    return places;
}

} // namespace Launcher::Paths
