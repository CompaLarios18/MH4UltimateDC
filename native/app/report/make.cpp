// report::Make, CollectNames and Pending (N3-13, D69). The report is a local text file; this code
// reads the player's own files (settings, log, crash file, build record) and writes one more. Every
// line goes through Redact and then the rules of tools/native/report_check.py.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "../../src/common/utf8.h"
#include "../../src/services/cfg/cfg_store.h"
#include "internal.h"
#include "patterns.h"
#include "report.h"

namespace mh4n::app::report {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kLogTailBytes = 2 * 1024 * 1024;  // the end of the log file that is looked at
constexpr int kLogLines = 500;
constexpr std::size_t kCrashFileBytes = 16 * 1024;
constexpr int kCrashFileLines = 60;
constexpr std::size_t kSettingsLines = 250;
constexpr int kMaxReportBudget = kMaxReportLines - 10;  // room for what the checks count differently

std::string JoinPath(const std::string& folder, const std::string& name) {
    if (folder.empty() || folder.back() == '/' || folder.back() == '\\') {
        return folder + name;
    }
    return folder + "/" + name;
}

fs::path HostPath(const std::string& utf8_path) {
    return utf8::PathFromUtf8(utf8_path);
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerCase(std::string s) {
    for (char& c : s) {
        c = Lower(c);
    }
    return s;
}

bool IsSpace(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

std::string Trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && IsSpace(s[b])) {
        ++b;
    }
    while (e > b && IsSpace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// The last `max_bytes` of a file (all of it when it is shorter), and whether it was cut at the front.
bool ReadTail(const std::string& path, std::size_t max_bytes, std::string* text, bool* cut = nullptr) {
    std::ifstream in(HostPath(path), std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    std::streamoff start = 0;
    if (size > static_cast<std::streamoff>(max_bytes)) {
        start = size - static_cast<std::streamoff>(max_bytes);
    }
    in.seekg(start, std::ios::beg);
    text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (cut != nullptr) {
        *cut = start > 0;
    }
    return true;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::size_t stop = end;
        if (stop > start && text[stop - 1] == '\r') {
            --stop;
        }
        lines.push_back(text.substr(start, stop - start));
        start = end + 1;
    }
    return lines;
}

// ---- the rules of the gate ------------------------------------------------------------------------------

std::vector<std::regex> Compile(const char* const* patterns, std::size_t count) {
    std::vector<std::regex> compiled;
    for (std::size_t i = 0; i < count; ++i) {
        compiled.emplace_back(patterns[i], std::regex::ECMAScript | std::regex::optimize);
    }
    return compiled;
}

bool MatchesAny(const std::vector<std::regex>& patterns, const std::string& line) {
    for (const std::regex& pattern : patterns) {
        if (std::regex_search(line, pattern)) {
            return true;
        }
    }
    return false;
}

bool IsTraceLike(const std::string& line) {
    static const std::vector<std::regex> patterns =
        Compile(kTracePatterns, sizeof kTracePatterns / sizeof kTracePatterns[0]);
    return MatchesAny(patterns, line);
}

bool IsListing(const std::string& line) {
    static const std::vector<std::regex> patterns =
        Compile(kListingPatterns, sizeof kListingPatterns / sizeof kListingPatterns[0]);
    return MatchesAny(patterns, line);
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool HasLongHexRun(const std::string& line) {
    int run = 0;
    for (char c : line) {
        run = IsHex(c) ? run + 1 : 0;
        if (run >= kHexRunLimit) {
            return true;
        }
    }
    return false;
}

// DENSE_LINE of the guard: a line of 1024 or more characters holding a run of that many base64 or hex
// characters.
bool IsDense(const std::string& line) {
    if (line.size() < static_cast<std::size_t>(kDenseLineMin)) {
        return false;
    }
    int run = 0;
    for (char c : line) {
        const bool blob = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '+' || c == '/' || c == '=';
        run = blob ? run + 1 : 0;
        if (run >= kDenseLineMin) {
            return true;
        }
    }
    return false;
}

// `s` with every byte that is not part of well-formed UTF-8 replaced by '?', so that a report is
// always text (a POSIX file name is a byte string, and the log may hold one).
std::string ValidUtf8(const std::string& s) {
    std::string out;
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t length = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3
                           : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = length != 0 && i + length <= n;
        for (std::size_t k = 1; ok && k < length; ++k) {
            ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        }
        if (ok && length >= 3) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) ||
                (c == 0xF4 && c1 >= 0x90)) {
                ok = false;  // overlong, a surrogate, past U+10FFFF
            }
        }
        if (ok) {
            out.append(s, i, length);
            i += length;
        } else {
            out += '?';
            ++i;
        }
    }
    return out;
}

// ---- the report being built ---------------------------------------------------------------------------

struct Builder {
    std::vector<std::string> names;
    std::string user_dir;
    std::vector<std::string> lines;

    // One line (or more, when `text` has newlines): redacted, cut, and dropped when the gate would
    // refuse it. Returns whether something was kept.
    bool Add(const std::string& text) {
        bool kept = false;
        for (const std::string& raw : SplitLines(text.empty() ? std::string("\n") : text)) {
            std::string line;
            if (!Filter(raw, &line)) {
                continue;
            }
            lines.push_back(std::move(line));
            kept = true;
        }
        return kept;
    }

    // The line the report keeps for `raw`, if it keeps one.
    bool Filter(const std::string& raw, std::string* out) const {
        const std::string text = ValidUtf8(raw);
        if (IsDense(text)) {
            return false;
        }
        std::string line = Redact(text, names, user_dir);
        if (line.size() > static_cast<std::size_t>(kMaxLineLength)) {
            std::size_t cut = kMaxLineLength;
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
                --cut;  // not in the middle of a character
            }
            line.resize(cut);
            line += "...";
        }
        for (char& c : line) {
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
                c = ' ';  // a control character is not text
            }
        }
        if (HasLongHexRun(line) || IsListing(line)) {
            return false;
        }
        *out = std::move(line);
        return true;
    }
};

std::string EnvironmentValue(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
            return value;
        }
    }
    return {};
}

// ---- settings --------------------------------------------------------------------------------------------

struct IniEntry {
    std::string section;
    std::string key;
    std::string value;
};

std::vector<IniEntry> ParseIni(const std::string& text) {
    std::vector<IniEntry> entries;
    std::string section;
    for (const std::string& raw : SplitLines(text)) {
        const std::string line = Trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            const std::size_t close = line.find(']');
            section = close == std::string::npos ? line.substr(1) : line.substr(1, close - 1);
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string value = line.substr(equals + 1);
        for (std::size_t i = 0; i < value.size(); ++i) {  // an inline comment: " ;" (as inih reads it)
            if (value[i] == ';' && (i == 0 || IsSpace(value[i - 1]))) {
                value.resize(i);
                break;
            }
        }
        entries.push_back({section, Trim(line.substr(0, equals)), Trim(value)});
    }
    return entries;
}

// A key that holds a personal or network setting, wherever it is found.
bool IsPersonalKey(const std::string& key) {
    const std::string k = LowerCase(key);
    for (const char* name : {"password", "token", "join", "lobby", "last_room", "nickname", "host",
                             "room_name", "catalog_url"}) {
        if (k == name) {
            return true;
        }
    }
    return false;
}

// "key = value", the value as `<path>` when it names a file or folder.
std::string SettingLine(const IniEntry& entry) {
    std::string value = entry.value;
    const std::string key = LowerCase(entry.key);
    if (!value.empty() && (key == "game" || key == "shader" || ContainsAbsolutePath(value))) {
        value = "<path>";
    }
    return entry.key + " = " + value;
}

void AddSettings(Builder& b, const std::string& user_dir) {
    std::string text;
    if (ReadTail(JoinPath(user_dir, "mh4u-app.ini"), 256 * 1024, &text)) {
        b.Add("mh4u-app.ini");
        std::string section;
        std::size_t count = 0;
        for (const IniEntry& entry : ParseIni(text)) {
            const std::string name = LowerCase(entry.section);
            if (name != "game" && name != "display" && name != "audio" && name != "controls") {
                continue;
            }
            if (IsPersonalKey(entry.key) || count >= kSettingsLines) {
                continue;
            }
            if (entry.section != section) {
                section = entry.section;
                b.Add("[" + section + "]");
            }
            b.Add(SettingLine(entry));
            ++count;
        }
    } else {
        b.Add("mh4u-app.ini: none");
    }
    if (ReadTail(JoinPath(user_dir, "mh4u.ini"), 256 * 1024, &text)) {
        b.Add("mh4u.ini");
        std::size_t count = 0;
        for (const IniEntry& entry : ParseIni(text)) {
            if (IsPersonalKey(entry.key) || count >= kSettingsLines) {
                continue;
            }
            b.Add(SettingLine(entry));
            ++count;
        }
    } else {
        b.Add("mh4u.ini: none");
    }
}

// ---- the code library's build record --------------------------------------------------------------------

// The text of a JSON string value `"key": "..."` (the first one), without escapes handled beyond \".
std::string JsonString(const std::string& json, const std::string& key, std::size_t from = 0,
                       std::size_t* after = nullptr) {
    const std::string needle = "\"" + key + "\"";
    std::size_t at = json.find(needle, from);
    while (at != std::string::npos) {
        std::size_t i = at + needle.size();
        while (i < json.size() && IsSpace(json[i])) {
            ++i;
        }
        if (i < json.size() && json[i] == ':') {
            ++i;
            while (i < json.size() && IsSpace(json[i])) {
                ++i;
            }
            if (i < json.size() && json[i] == '"') {
                std::string value;
                for (++i; i < json.size() && json[i] != '"'; ++i) {
                    if (json[i] == '\\' && i + 1 < json.size()) {
                        ++i;
                    }
                    value += json[i];
                }
                if (after != nullptr) {
                    *after = i;
                }
                return value;
            }
            return {};
        }
        at = json.find(needle, at + 1);
    }
    if (after != nullptr) {
        *after = std::string::npos;
    }
    return {};
}

// A number or true/false after `"key":`, as text ("" when absent or something else).
std::string JsonScalar(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t at = json.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    std::size_t i = at + needle.size();
    while (i < json.size() && IsSpace(json[i])) {
        ++i;
    }
    if (i >= json.size() || json[i] != ':') {
        return {};
    }
    ++i;
    while (i < json.size() && IsSpace(json[i])) {
        ++i;
    }
    std::string value;
    while (i < json.size() && (std::isalnum(static_cast<unsigned char>(json[i])) != 0 || json[i] == '-' || json[i] == '.')) {
        value += json[i++];
    }
    return value;
}

void AddBuildRecords(Builder& b, const std::string& user_dir) {
    std::vector<std::string> records;
    std::error_code ec;
    for (fs::directory_iterator it(HostPath(JoinPath(user_dir, "recomp")), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (EndsWith(name, ".build.json") && name.find("_native") != std::string::npos) {
            records.push_back(name);
        }
    }
    std::sort(records.begin(), records.end());
    if (records.empty()) {
        b.Add("code library: no build record");
        return;
    }
    for (const std::string& name : records) {
        std::string json;
        if (!ReadTail(JoinPath(JoinPath(user_dir, "recomp"), name), 64 * 1024, &json)) {
            continue;
        }
        std::string line = "code library " + (JsonString(json, "library").empty() ? name : JsonString(json, "library"));
        const std::pair<const char*, const char*> fields[] = {
            {"format", "format"},     {"program_id", "program"}, {"library_size", "size"},
            {"text_size", "text"},    {"code_size", "code"},     {"changes_code", "changes code"},
        };
        for (const auto& [key, label] : fields) {
            std::string value = JsonScalar(json, key);
            if (value.empty()) {
                value = JsonString(json, key);
            }
            if (!value.empty()) {
                line += std::string(", ") + label + " " + value;
            }
        }
        const std::string built = JsonString(json, "built");
        if (!built.empty()) {
            line += ", built " + built;
        }
        // the code mods: their kinds and how many, never their hashes
        std::string kinds;
        int mods = 0;
        for (std::size_t at = 0;;) {
            std::size_t after = std::string::npos;
            const std::string kind = JsonString(json, "kind", at, &after);
            if (after == std::string::npos) {
                break;
            }
            ++mods;
            kinds += (kinds.empty() ? "" : " ") + kind;
            at = after + 1;
        }
        line += ", code mods " + std::to_string(mods) + (kinds.empty() ? "" : " (" + kinds + ")");
        const std::string fingerprint = JsonString(json, "fingerprint");
        if (!fingerprint.empty()) {
            line += ", fingerprint " + fingerprint.substr(0, 16);
        }
        b.Add(line);
    }
}

// ---- the log and the crash file -------------------------------------------------------------------------

// The level a log line says: the first "[Name]" or "<Name>" (a level's name) in its first 100
// characters. 2 (Info) for a line that says none.
int LevelOf(const std::string& line) {
    const std::size_t limit = std::min<std::size_t>(line.size(), 100);
    for (std::size_t i = 0; i < limit; ++i) {
        if (line[i] != '[' && line[i] != '<') {
            continue;
        }
        const char close = line[i] == '[' ? ']' : '>';
        const std::size_t end = line.find(close, i + 1);
        if (end == std::string::npos || end - i > 12) {
            continue;
        }
        const std::string name = LowerCase(line.substr(i + 1, end - i - 1));
        if (name == "trace") return 0;
        if (name == "debug") return 1;
        if (name == "info") return 2;
        if (name == "warning" || name == "warn") return 3;
        if (name == "error") return 4;
        if (name == "critical" || name == "fatal") return 5;
    }
    return 2;
}

// The last `limit` log lines at Info or above, redacted; of those only the newest kTraceLineLimit that
// look like a trace stay, the older ones becoming one line.
std::vector<std::string> LogLines(const Builder& b, const std::string& user_dir, int limit, bool* found) {
    std::string text;
    bool cut = false;
    *found = ReadTail(JoinPath(JoinPath(user_dir, "log"), "mh4u_native_log.txt"), kLogTailBytes, &text, &cut);
    std::vector<std::string> selected;
    if (!*found) {
        return selected;
    }
    std::vector<std::string> lines = SplitLines(text);
    if (cut && !lines.empty()) {
        lines.erase(lines.begin());  // the front of the file's tail is half a line
    }
    std::vector<std::string> info_or_above;
    for (const std::string& line : lines) {
        if (LevelOf(line) >= 2) {
            info_or_above.push_back(line);
        }
    }
    const std::size_t first = info_or_above.size() > static_cast<std::size_t>(kLogLines)
                                  ? info_or_above.size() - kLogLines
                                  : 0;
    std::vector<std::string> kept;
    for (std::size_t i = first; i < info_or_above.size(); ++i) {
        std::string line;
        if (b.Filter(info_or_above[i], &line)) {
            kept.push_back(std::move(line));
        }
    }
    // trace-like lines: the newest kTraceLineLimit stay
    std::vector<std::size_t> trace;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (IsTraceLike(kept[i])) {
            trace.push_back(i);
        }
    }
    std::vector<bool> drop(kept.size(), false);
    std::size_t left_out = 0;
    std::size_t first_dropped = kept.size();
    if (trace.size() > static_cast<std::size_t>(kTraceLineLimit)) {
        left_out = trace.size() - kTraceLineLimit;
        for (std::size_t k = 0; k < left_out; ++k) {
            drop[trace[k]] = true;
        }
        first_dropped = trace[0];
    }
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i == first_dropped) {
            selected.push_back(std::to_string(left_out) + " trace-like lines left out");
        }
        if (!drop[i]) {
            selected.push_back(std::move(kept[i]));
        }
    }
    // the report's size: the newest lines that fit
    if (selected.size() > static_cast<std::size_t>(std::max(limit, 0))) {
        selected.erase(selected.begin(), selected.end() - std::max(limit, 0));
    }
    return selected;
}

// The newest crash-*.txt of <user>/log/ that is not empty; its path, or "".
std::string NewestFile(const std::string& user_dir, const char* prefix, const char* suffix,
                       bool non_empty, fs::file_time_type* time) {
    std::string best;
    fs::file_time_type best_time{};
    std::error_code ec;
    for (fs::directory_iterator it(HostPath(JoinPath(user_dir, "log")), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!StartsWith(name, prefix) || !EndsWith(name, suffix)) {
            continue;
        }
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) {
            continue;
        }
        if (non_empty) {
            const std::uintmax_t size = it->file_size(e2);
            if (e2 || size == 0) {
                continue;
            }
        }
        const fs::file_time_type t = it->last_write_time(e2);
        if (e2) {
            continue;
        }
        if (best.empty() || t > best_time || (t == best_time && name > best)) {
            best = name;
            best_time = t;
        }
    }
    if (best.empty()) {
        return {};
    }
    *time = best_time;
    return JoinPath(JoinPath(user_dir, "log"), best);
}

// ---- the system ------------------------------------------------------------------------------------------

std::string OrUnknown(const std::string& s) {
    return s.empty() ? std::string("unknown") : s;
}

void AddSystem(Builder& b, const SystemInfo& system) {
    b.Add("os: " + OrUnknown(system.os) + (system.os_version.empty() ? "" : " " + system.os_version));
    b.Add("cpu: " + OrUnknown(system.cpu_model));
    b.Add("logical cores: " + (system.logical_cores == 0 ? std::string("unknown") : std::to_string(system.logical_cores)));
    b.Add("ram: " + (system.ram_bytes == 0 ? std::string("unknown")
                                           : std::to_string(system.ram_bytes / (1024 * 1024)) + " MiB"));
    b.Add("gpu: " + OrUnknown(system.gpu_name));
    b.Add("gpu driver: " + OrUnknown(system.driver_version));
    b.Add("vulkan: " + OrUnknown(system.vulkan_version));
    if (system.display_modes.empty()) {
        b.Add("display modes: unknown");
    }
    std::size_t shown = 0;
    for (const std::string& mode : system.display_modes) {
        if (shown++ >= 16) {
            break;
        }
        b.Add("display mode: " + mode);
    }
}

}  // namespace

std::vector<std::string> CollectNames(const std::string& user_dir) {
    std::vector<std::string> names(2);
    names[0] = EnvironmentValue({"USER", "USERNAME", "LOGNAME"});
    std::error_code ec;
    if (!user_dir.empty() && fs::is_regular_file(HostPath(cfg::ConfigPath(user_dir)), ec) && !ec) {
        cfg::ConfigStore store(user_dir);  // reads the file; nothing is created or written
        names[1] = utf8::FromUtf16(store.GetUsername());
    }
    return names;
}

bool Pending(const std::string& user_dir) {
    fs::file_time_type crash_time{};
    fs::file_time_type report_time{};
    const std::string crash = NewestFile(user_dir, "crash-", ".txt", true, &crash_time);
    if (crash.empty()) {
        return false;
    }
    const std::string report = NewestFile(user_dir, "report-", ".txt", false, &report_time);
    return report.empty() || crash_time > report_time;
}

bool Make(const MakeInput& input, const std::string& out_path) {
    Builder b;
    b.user_dir = input.user_dir;
    b.names = input.names;
    const std::vector<std::string> collected = CollectNames(input.user_dir);
    for (std::size_t i = 0; i < collected.size(); ++i) {
        if (b.names.size() <= i) {
            b.names.resize(i + 1);
        }
        if (b.names[i].empty()) {
            b.names[i] = collected[i];
        }
    }

    b.lines.push_back(kMarker);
    b.Add("== versions ==");
    b.Add("app commit: " + OrUnknown(input.app_commit));
    AddBuildRecords(b, input.user_dir);
    b.Add("");
    b.Add("== system ==");
    AddSystem(b, input.system);
    b.Add("");
    b.Add("== settings ==");
    AddSettings(b, input.user_dir);
    b.Add("");
    b.Add("== last run ==");
    b.Add(input.last_status.empty() ? std::string("no status") : input.last_status);
    b.Add("");
    b.Add("== newest crash file ==");
    fs::file_time_type crash_time{};
    const std::string crash_path = NewestFile(input.user_dir, "crash-", ".txt", true, &crash_time);
    std::string crash_text;
    if (!crash_path.empty() && ReadTail(crash_path, kCrashFileBytes, &crash_text)) {
        int count = 0;
        for (const std::string& line : SplitLines(crash_text)) {
            if (count++ >= kCrashFileLines) {
                break;
            }
            b.Add(line);
        }
    } else {
        b.Add("none");
    }
    b.Add("");

    bool log_found = false;
    const int budget = std::min(kLogLines, kMaxReportBudget - static_cast<int>(b.lines.size()) - 2);
    const std::vector<std::string> log = LogLines(b, input.user_dir, budget, &log_found);
    if (!log_found) {
        b.Add("== log: none ==");
    } else {
        b.Add("== log: the last " + std::to_string(log.size()) + " lines at Info or above ==");
        for (const std::string& line : log) {
            b.lines.push_back(line);  // already redacted and filtered
        }
    }

    std::string text;
    for (const std::string& line : b.lines) {
        text += line;
        text += '\n';
    }
    const fs::path out = HostPath(out_path);
    std::error_code ec;
    if (out.has_parent_path()) {
        fs::create_directories(out.parent_path(), ec);
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return static_cast<bool>(file);
}

}  // namespace mh4n::app::report
