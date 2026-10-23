// report::Redact (N3-13, D69): what identifies the player or the game's files is replaced before
// anything goes into a report. Hand-written scanners, one pass each, in this order: invites,
// the user and home folders, absolute paths, console ids, MAC addresses, IPv4 addresses, IPv6
// addresses, the names. tools/native/report_check.py implements the same rules in Python as the
// gate; tests/test_native_report_check.py and app_report_test.cpp hold the same lines.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "internal.h"
#include "report.h"

namespace mh4n::app::report {
namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsAlnum(char c) { return IsDigit(c) || IsAlpha(c); }
bool IsWord(char c) { return IsAlnum(c) || c == '_'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsSep(char c) { return c == '/' || c == '\\'; }
bool IsSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool MatchLower(const std::string& s, std::size_t i, const char* lower_text) {
    for (std::size_t k = 0; lower_text[k] != '\0'; ++k) {
        if (i + k >= s.size() || Lower(s[i + k]) != lower_text[k]) {
            return false;
        }
    }
    return true;
}

// ---- mh4u:// invites ----------------------------------------------------------------------------------

std::string Invites(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (MatchLower(s, i, "mh4u://")) {
            std::size_t j = i + 7;
            while (j < s.size() && !IsSpace(s[j]) && s[j] != '"' && s[j] != '\'' && s[j] != '<' &&
                   s[j] != '>') {
                ++j;
            }
            out += "<invite>";
            i = j;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// ---- the user folder and the home folder --------------------------------------------------------------

// An absolute folder: a drive path, a UNC path or one that starts with a separator.
bool IsAbsolute(const std::string& p) {
    if (p.size() >= 3 && IsAlpha(p[0]) && p[1] == ':' && IsSep(p[2])) {
        return true;
    }
    return p.size() >= 2 && IsSep(p[0]);
}

std::string TrimSeparators(std::string p) {
    while (p.size() > 1 && IsSep(p.back()) && !(p.size() == 3 && p[1] == ':')) {
        p.pop_back();
    }
    return p;
}

// The length of `folder` at s[i...] when it stands there as a whole path (not the start of a longer
// one), and the separator that ends it; 0 when it does not.
std::size_t FolderAt(const std::string& s, std::size_t i, const std::string& folder) {
    if (i > 0) {
        const char p = s[i - 1];
        if (IsWord(p) || p == '.' || p == '-' || p == '~' || IsSep(p)) {
            return 0;
        }
    }
    const bool windows = folder.size() >= 2 && (folder[1] == ':' || IsSep(folder[1]));
    if (i + folder.size() > s.size()) {
        return 0;
    }
    for (std::size_t k = 0; k < folder.size(); ++k) {
        const char a = s[i + k];
        const char b = folder[k];
        if (a == b) {
            continue;
        }
        if (windows && ((IsSep(a) && IsSep(b)) || Lower(a) == Lower(b))) {
            continue;
        }
        return 0;
    }
    std::size_t j = i + folder.size();
    if (j == s.size()) {
        return j - i;
    }
    const char c = s[j];
    auto ends = [&](std::size_t k) {  // nothing that continues a path at k
        return k >= s.size() || IsSpace(s[k]) || s[k] == '"' || s[k] == '\'' || s[k] == ')' ||
               s[k] == ']' || s[k] == ',' || s[k] == ';';
    };
    if (IsSep(c)) {
        return ends(j + 1) ? j + 1 - i : 0;
    }
    if (c == '.') {
        return ends(j + 1) ? j - i : 0;
    }
    if (IsWord(c) || c == '-' || c == '~') {
        return 0;
    }
    return j - i;
}

std::string Folders(const std::string& s, const std::vector<std::pair<std::string, const char*>>& folders) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t length = 0;
        const char* token = nullptr;
        for (const auto& [folder, name] : folders) {
            length = FolderAt(s, i, folder);
            if (length != 0) {
                token = name;
                break;
            }
        }
        if (length != 0) {
            out += token;
            i += length;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// ---- absolute paths -------------------------------------------------------------------------------------

// A character of a path segment that is not a separator (what the second component of a POSIX path
// must start with).
bool IsComponentChar(char c) {
    return !IsSpace(c) && !IsSep(c) && c != '"' && c != '\'' && c != '<' && c != '>' && c != '|' &&
           c != '*' && c != '?' && c != ',' && c != ';' && c != ')' && c != ']' && c != '}';
}

// How far the path that began with its first separator (or drive prefix) goes: `pos` is the start of
// its next segment. A segment followed by a separator may hold up to two spaces ("Application
// Support", "Program Files (x86)", a user's name), the last one ends at the first space.
// `colon_ends`: a colon ends a segment (drive and UNC paths: "C:\a and C:\b" are two paths).
std::size_t ConsumeSegments(const std::string& s, std::size_t pos, bool both_separators, bool colon_ends) {
    const std::size_t n = s.size();
    auto separator = [&](char c) { return both_separators ? IsSep(c) : c == '/'; };
    std::size_t end = pos;
    // brackets opened inside the path and not closed yet: a ')' or ']' with none open ends the path
    // ("see (/a/b) and [/c/d]" is two paths), one that closes a bracket of the path stays in it
    int parens = 0;
    int brackets = 0;
    for (;;) {
        std::size_t j = pos;
        int spaces = 0;
        int segment_parens = parens;
        int segment_brackets = brackets;
        bool too_many = false;
        while (j < n && !separator(s[j])) {
            const char c = s[j];
            if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' ||
                static_cast<unsigned char>(c) < 0x20 || (colon_ends && c == ':')) {
                break;
            }
            if (c == '(') {
                ++segment_parens;
            } else if (c == '[') {
                ++segment_brackets;
            } else if (c == ')' || c == ']') {
                int& open = c == ')' ? segment_parens : segment_brackets;
                if (open == 0) {
                    break;  // unmatched: the path ended before it
                }
                --open;
            }
            if (c == ' ' && ++spaces > 2) {
                too_many = true;
                break;
            }
            ++j;
        }
        if (!too_many && j < n && separator(s[j])) {
            pos = j + 1;  // a middle segment
            end = pos;
            parens = segment_parens;
            brackets = segment_brackets;
            continue;
        }
        std::size_t f = pos;  // the last segment
        while (f < j && !IsSpace(s[f]) && s[f] != ',' && s[f] != ';' && s[f] != '}' &&
               !(colon_ends && s[f] == ':')) {
            const char c = s[f];
            if (c == '(') {
                ++parens;
            } else if (c == '[') {
                ++brackets;
            } else if (c == ')' || c == ']') {
                int& open = c == ')' ? parens : brackets;
                if (open == 0) {
                    break;
                }
                --open;
            }
            ++f;
        }
        return f > pos ? f : end;
    }
}

// The length of the absolute path that starts at s[i], 0 when none does.
std::size_t PathAtUnquoted(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    const char c = s[i];
    const char p = i > 0 ? s[i - 1] : '\0';
    // a drive path: C:\ or C:/
    if (IsAlpha(c) && i + 2 < n && s[i + 1] == ':' && IsSep(s[i + 2]) && (i == 0 || !IsWord(p))) {
        return ConsumeSegments(s, i + 3, true, true) - i;
    }
    // a UNC path: \\host\share (not the "//host" of a URL: "https://host/x")
    if (IsSep(c) && i + 1 < n && IsSep(s[i + 1]) && (i == 0 || !(IsWord(p) || IsSep(p))) &&
        !(p == ':' && c == '/' && s[i + 1] == '/')) {
        std::size_t j = i + 2;
        while (j < n && !IsSpace(s[j]) && !IsSep(s[j])) {
            ++j;
        }
        if (j > i + 2 && j < n && IsSep(s[j])) {
            return ConsumeSegments(s, j + 1, true, true) - i;
        }
        return 0;
    }
    // a POSIX path of two or more components (a doubled slash counts as one); after a colon too
    // ("frame 12:/a/b"), except for the "//host" of a URL
    if (c == '/') {
        bool start;
        if (i == 0) {
            start = true;
        } else if (p == ':') {
            start = !(i + 1 < n && s[i + 1] == '/' && !(i + 2 < n && s[i + 2] == '/'));  // "file:///a/b" is a path
        } else {
            start = !(IsWord(p) || p == '.' || p == '~' || IsSep(p));
        }
        if (start) {
            std::size_t j = i + 1;
            while (j < n && s[j] == '/') {
                ++j;
            }
            const std::size_t first = j;
            while (j < n && IsComponentChar(s[j])) {
                ++j;
            }
            if (j > first) {
                std::size_t k = j;
                while (k < n && s[k] == '/') {
                    ++k;
                }
                if (k > j && k < n && IsComponentChar(s[k])) {
                    return ConsumeSegments(s, k, false, false) - i;
                }
            }
        }
    }
    return 0;
}

// The same, but a path that starts right after a quote ends at the matching quote on its line, so a
// space in its last segment ('/var/lib/a b/c d.txt') does not leave the rest of the name behind.
std::size_t PathAt(const std::string& s, std::size_t i) {
    const std::size_t length = PathAtUnquoted(s, i);
    if (length == 0 || i == 0 || (s[i - 1] != '"' && s[i - 1] != '\'')) {
        return length;
    }
    for (std::size_t k = i; k < s.size() && s[k] != '\n'; ++k) {
        if (s[k] == s[i - 1]) {
            return k - i >= length ? k - i : length;
        }
    }
    return length;
}

std::string Paths(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t length = PathAt(s, i);
        if (length != 0) {
            out += "<path>";
            i += length;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// ---- console ids --------------------------------------------------------------------------------------

std::string ConsoleIds(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if ((s[i] == 'c' || s[i] == 'C') && (i == 0 || !IsWord(s[i - 1])) &&
            MatchLower(s, i, "console id")) {
            std::size_t j = i + 10;
            while (j < s.size() && IsSpace(s[j])) {
                ++j;
            }
            if (j < s.size() && (s[j] == ':' || s[j] == '=')) {
                ++j;
            }
            while (j < s.size() && IsSpace(s[j])) {
                ++j;
            }
            std::size_t digits = j;
            if (MatchLower(s, digits, "0x")) {
                digits += 2;
            }
            std::size_t k = digits;
            while (k < s.size() && k - digits < 16 && IsHex(s[k])) {
                ++k;
            }
            if (k - digits == 16 && (k == s.size() || !IsHex(s[k]))) {
                out.append(s, i, j - i);
                out += "<console-id>";
                i = k;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// ---- MAC addresses --------------------------------------------------------------------------------------

bool MacSeparator(char c) { return c == ':' || c == '-'; }

std::size_t MacAt(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i + 17 > n || !IsHex(s[i])) {
        return 0;
    }
    if (i > 0) {
        const char p = s[i - 1];
        if (IsAlnum(p) || (MacSeparator(p) && i >= 2 && IsHex(s[i - 2]))) {
            return 0;
        }
    }
    const char separator = s[i + 2];
    if (!MacSeparator(separator)) {
        return 0;
    }
    for (std::size_t k = 0; k < 17; ++k) {
        const char c = s[i + k];
        if (k % 3 == 2 ? c != separator : !IsHex(c)) {
            return 0;
        }
    }
    const std::size_t end = i + 17;
    if (end < n && (IsHex(s[end]) || (MacSeparator(s[end]) && end + 1 < n && IsHex(s[end + 1])))) {
        return 0;
    }
    return 17;
}

std::string Macs(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t length = MacAt(s, i);
        if (length != 0) {
            out += "<mac>";
            i += length;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// ---- IPv4 -------------------------------------------------------------------------------------------------

// The end of the dotted-quad run that starts at s[i] when it is an IPv4 address, 0 otherwise.
std::size_t Ipv4At(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (!IsDigit(s[i])) {
        return 0;
    }
    if (i > 0) {
        const char p = s[i - 1];
        if (IsDigit(p) || (p == '.' && i >= 2 && IsDigit(s[i - 2]))) {
            return 0;
        }
    }
    std::size_t j = i;
    while (j < n && (IsDigit(s[j]) || s[j] == '.')) {
        ++j;
    }
    while (j > i && s[j - 1] == '.') {
        --j;
    }
    int parts = 0;
    std::size_t k = i;
    while (k < j) {
        std::size_t start = k;
        int value = 0;
        while (k < j && IsDigit(s[k])) {
            value = value * 10 + (s[k] - '0');
            ++k;
            if (k - start > 3) {
                return 0;
            }
        }
        if (k == start || value > 255) {
            return 0;
        }
        ++parts;
        if (k < j) {
            if (s[k] != '.') {
                return 0;
            }
            ++k;
        }
    }
    if (parts != 4) {
        return 0;
    }
    // an optional :port
    if (j + 1 < n && s[j] == ':' && IsDigit(s[j + 1])) {
        std::size_t q = j + 1;
        while (q < n && IsDigit(s[q])) {
            ++q;
        }
        if (q - (j + 1) <= 5) {
            j = q;
        }
    }
    return j;
}

std::string Ipv4(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t end = Ipv4At(s, i);
        if (end != 0) {
            out += "<address>";
            i = end;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// ---- IPv6 -------------------------------------------------------------------------------------------------

// Is s[begin, end) an IPv6 address: groups of 1 to 4 hex digits separated by ':', either eight of them
// or fewer around one '::'; at least one group has a digit.
bool IsIpv6(const std::string& s, std::size_t begin, std::size_t end) {
    int groups = 0;
    int compressions = 0;
    bool digit = false;
    std::size_t i = begin;
    if (end - begin < 2) {
        return false;
    }
    if (s[i] == ':') {
        if (s[i + 1] != ':') {
            return false;
        }
        ++compressions;
        i += 2;
    }
    while (i < end) {
        std::size_t start = i;
        while (i < end && IsHex(s[i])) {
            digit = digit || IsDigit(s[i]);
            ++i;
        }
        const std::size_t length = i - start;
        if (length == 0 || length > 4) {
            return false;
        }
        ++groups;
        if (i == end) {
            break;
        }
        // s[i] is ':'
        if (i + 1 < end && s[i + 1] == ':') {
            ++compressions;
            i += 2;
            if (i == end) {
                break;  // "1::" ends in the compression
            }
        } else {
            ++i;
            if (i == end) {
                return false;  // a lone colon at the end
            }
        }
    }
    if (compressions > 1 || !digit) {
        return false;
    }
    return compressions == 1 ? groups <= 7 : groups == 8;
}

std::string Ipv6(const std::string& s) {
    const std::size_t n = s.size();
    std::string out;
    std::size_t i = 0;
    while (i < n) {
        if (!(IsHex(s[i]) || s[i] == ':')) {
            out += s[i++];
            continue;
        }
        std::size_t run_end = i;
        while (run_end < n && (IsHex(s[run_end]) || s[run_end] == ':')) {
            ++run_end;
        }
        std::size_t begin = i;
        std::size_t end = run_end;
        // a single colon at either edge is punctuation ("addr:2001:db8::1", "2001:db8::1:")
        if (end - begin >= 2 && s[begin] == ':' && s[begin + 1] != ':') {
            ++begin;
        }
        if (end - begin >= 2 && s[end - 1] == ':' && s[end - 2] != ':') {
            --end;
        }
        // (the run ends at a character that is no hex digit: a word character there is a letter
        // after g, or an underscore, and the "address" is part of a longer word)
        const bool word_before = begin == i && i > 0 && IsWord(s[i - 1]);
        const bool word_after = end == run_end && run_end < n && IsWord(s[run_end]);
        if (word_before || word_after || !IsIpv6(s, begin, end)) {
            out.append(s, i, run_end - i);
            i = run_end;
            continue;
        }
        out.append(s, i, begin - i);
        std::size_t next = run_end;
        if (begin == i && i > 0 && s[i - 1] == '[' && end == run_end && run_end < n && s[run_end] == ']') {
            out.pop_back();  // the '[' before the address
            next = run_end + 1;
            if (next + 1 < n && s[next] == ':' && IsDigit(s[next + 1])) {  // [address]:port
                std::size_t q = next + 1;
                while (q < n && IsDigit(s[q])) {
                    ++q;
                }
                if (q - (next + 1) <= 5) {
                    next = q;
                }
            }
            out += "<address>";
        } else {
            out += "<address>";
            out.append(s, end, run_end - end);
        }
        i = next;
    }
    return out;
}

// ---- names ------------------------------------------------------------------------------------------------

// Marks the "<token>" spans (letters and hyphens between the brackets) a replacement made.
std::vector<bool> TokenSpans(const std::string& s) {
    std::vector<bool> inside(s.size(), false);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '<') {
            continue;
        }
        std::size_t j = i + 1;
        while (j < s.size() && (IsAlpha(s[j]) || s[j] == '-') && j - i <= 16) {
            ++j;
        }
        if (j < s.size() && s[j] == '>' && j > i + 1) {
            for (std::size_t k = i; k <= j; ++k) {
                inside[k] = true;
            }
            i = j;
        }
    }
    return inside;
}

std::string Names(const std::string& s, const std::vector<std::pair<std::string, std::string>>& names) {
    std::string text = s;
    for (const auto& [name, token] : names) {
        if (name.empty()) {
            continue;
        }
        std::string lower_name(name);
        for (char& c : lower_name) {
            c = Lower(c);
        }
        const std::vector<bool> inside = TokenSpans(text);
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            bool match = i + name.size() <= text.size();
            for (std::size_t k = 0; match && k < name.size(); ++k) {
                match = Lower(text[i + k]) == lower_name[k];
            }
            if (match && name.size() < 3) {  // a short name only as a word of its own
                const std::size_t end = i + name.size();
                match = (i == 0 || !IsWord(text[i - 1])) && (end == text.size() || !IsWord(text[end]));
            }
            if (match && !inside[i]) {
                out += token;
                i += name.size();
            } else {
                out += text[i++];
            }
        }
        text = std::move(out);
    }
    return text;
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

std::string EnvironmentHome() {
    for (const char* name : {"HOME", "USERPROFILE"}) {
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
            return value;
        }
    }
    return {};
}

}  // namespace

std::string Redact(const std::string& text, const std::vector<std::string>& names,
                   const std::string& user_dir, const std::string& home_dir) {
    std::vector<std::pair<std::string, const char*>> folders;
    const std::string user = TrimSeparators(Trim(user_dir));
    const std::string home = TrimSeparators(Trim(home_dir.empty() ? EnvironmentHome() : home_dir));
    if (IsAbsolute(user)) {
        folders.emplace_back(user, "<user>");
    }
    if (IsAbsolute(home) && home.size() > 1) {
        folders.emplace_back(home, "<home>");
    }
    // the longer folder first: the user folder usually lies inside the home folder
    std::stable_sort(folders.begin(), folders.end(),
                     [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    std::vector<std::pair<std::string, std::string>> hidden;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string name = Trim(names[i]);
        if (!name.empty()) {
            hidden.emplace_back(name, i == 0 ? "<os-user>" : i == 1 ? "<console-user>" : "<name>");
        }
    }
    std::stable_sort(hidden.begin(), hidden.end(),
                     [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    // One round can uncover what the next replaces ("<console-id>//opt/x": a path that followed a hex
    // digit before): rounds repeat until the text stops changing.
    std::string s = text;
    for (int round = 0; round < 6; ++round) {
        const std::string before = s;
        s = Invites(s);
        s = Folders(s, folders);
        s = Paths(s);
        s = ConsoleIds(s);
        s = Macs(s);
        s = Ipv4(s);
        s = Ipv6(s);
        s = Names(s, hidden);
        if (s == before) {
            break;
        }
    }
    return s;
}

bool ContainsAbsolutePath(const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (PathAt(text, i) != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace mh4n::app::report
