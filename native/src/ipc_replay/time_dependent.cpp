#include "ipc_replay/time_dependent.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

namespace mh4n::ipc_replay {

namespace {

// Strict UTF-8, as Python's decoder is (Unicode, table 3-7): no overlong forms, no surrogates, nothing
// above U+10FFFF.
bool ValidUtf8(const std::string& text) {
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const auto continuation = [&](std::size_t i, unsigned char low, unsigned char high) {
        return i < text.size() && at(i) >= low && at(i) <= high;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = at(i);
        if (c < 0x80) {
            i += 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (!continuation(i + 1, 0x80, 0xBF)) {
                return false;
            }
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            // E0 takes A0..BF (no overlong form), ED takes 80..9F (no surrogate)
            const unsigned char low = c == 0xE0 ? 0xA0 : 0x80;
            const unsigned char high = c == 0xED ? 0x9F : 0xBF;
            if (!continuation(i + 1, low, high) || !continuation(i + 2, 0x80, 0xBF)) {
                return false;
            }
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            // F0 takes 90..BF (no overlong form), F4 takes 80..8F (nothing above U+10FFFF)
            const unsigned char low = c == 0xF0 ? 0x90 : 0x80;
            const unsigned char high = c == 0xF4 ? 0x8F : 0xBF;
            if (!continuation(i + 1, low, high) || !continuation(i + 2, 0x80, 0xBF) ||
                !continuation(i + 3, 0x80, 0xBF)) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

bool TimeDependent::LoadText(const std::string& text, const std::string& name, std::string* error) {
    if (!ValidUtf8(text)) {
        *error = fmt::format("{}: not a time-dependent list (not UTF-8 text)", name);
        return false;
    }
    std::istringstream in(text);
    std::string line;
    std::size_t number = 0;
    std::set<std::pair<std::string, u32>> found;
    while (std::getline(in, line)) {
        ++number;
        line = line.substr(0, line.find('#'));
        std::istringstream words(line);
        std::vector<std::string> parts;
        for (std::string word; words >> word;) {
            parts.push_back(word);
        }
        if (parts.empty()) {
            continue;
        }
        if (parts.size() != 2) {
            *error = fmt::format("{}:{}: expected `<session> <header>`", name, number);
            return false;
        }
        std::string_view digits = parts[1];
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            digits.remove_prefix(2);
        }
        u64 header = 0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), header, 16);
        if (digits.empty() || ec != std::errc() || end != digits.data() + digits.size() ||
            header > 0xFFFFFFFFull) {
            *error = fmt::format("{}:{}: the header is not hex", name, number);
            return false;
        }
        found.insert({parts[0], static_cast<u32>(header)});
    }
    entries_.insert(found.begin(), found.end());  // all or nothing
    return true;
}

bool TimeDependent::LoadFile(const std::string& path, std::string* error) {
    // A directory opens as a stream and reads as an empty file: refuse it.
    std::error_code status_error;
    if (std::filesystem::is_directory(path, status_error) || status_error) {
        *error = fmt::format("cannot read {}", path);
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = fmt::format("cannot read {}", path);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return LoadText(buffer.str(), path, error);
}

bool TimeDependent::Matches(const std::string& session, u32 header) const {
    if (entries_.count({session, header}) != 0) {
        return true;
    }
    // `fs:USER/file#3` -> `fs:USER/file`
    std::size_t end = session.size();
    while (end > 0 && session[end - 1] >= '0' && session[end - 1] <= '9') {
        --end;
    }
    if (end > 0 && end < session.size() && session[end - 1] == '#') {
        return entries_.count({session.substr(0, end - 1), header}) != 0;
    }
    return false;
}

}  // namespace mh4n::ipc_replay
