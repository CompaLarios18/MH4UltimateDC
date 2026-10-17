#include "ipc_replay/time_dependent.h"

#include <charconv>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

namespace mh4n::ipc_replay {

namespace {

bool ValidUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if (i + k >= text.size() || (static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
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
