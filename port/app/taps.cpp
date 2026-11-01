#include "taps.h"

#include <algorithm>
#include <cctype>

namespace App::Taps {

namespace {

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string Strip(const std::string& s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && IsSpace(s[begin])) {
        ++begin;
    }
    while (end > begin && IsSpace(s[end - 1])) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/// The parts of `s` between the `by`s, each stripped (an empty `s` is one empty part).
std::vector<std::string> Split(const std::string& s, char by) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = s.find(by, start);
        if (at == std::string::npos) {
            parts.push_back(Strip(s.substr(start)));
            return parts;
        }
        parts.push_back(Strip(s.substr(start, at - start)));
        start = at + 1;
    }
}

/// A number at `i`, after spaces (`i` ends past its digits); false when there are no digits.
bool Number(const std::string& s, std::size_t& i, int& value) {
    while (i < s.size() && IsSpace(s[i])) {
        ++i;
    }
    const std::size_t start = i;
    long long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = std::min(v * 10 + (s[i] - '0'), 1000000000LL);  // far off the screen is enough
        ++i;
    }
    value = static_cast<int>(v);
    return i > start;
}

/// Takes `key` out of a binding's keys ("F | Mouse Right"); true if it was there.
bool RemoveFrom(std::string& keys, const std::string& key) {
    std::vector<std::string> names = Names(keys);
    const auto gone =
        std::remove_if(names.begin(), names.end(), [&key](const std::string& name) { return Same(name, key); });
    if (gone == names.end()) {
        return false;
    }
    names.erase(gone, names.end());
    keys = Join(names);
    return true;
}

/// `text` without a ; that mh4u-app.ini's reader (inih) would take for the start of a comment, one
/// after a space or first: the spaces before a ; go, and so does a ; with only spaces before it
/// ("Potion ; big" -> "Potion; big").
std::string IniSafe(const std::string& text) {
    std::string safe;
    for (const char c : text) {
        if (c == ';') {
            // (inih's own test for a space)
            while (!safe.empty() && std::isspace(static_cast<unsigned char>(safe.back()))) {
                safe.pop_back();
            }
            if (safe.empty()) {
                continue;
            }
        }
        safe += c;
    }
    return safe;
}

/// Takes `key` out of a stick's keys, its direction left empty: "W,S,A,D" -> "W,S,,D".
bool RemoveFromStick(std::string& keys, const std::string& key) {
    std::vector<std::string> parts = StickParts(keys);
    bool removed = false;
    for (std::string& part : parts) {
        if (Same(part, key)) {
            part.clear();
            removed = true;
        }
    }
    if (!removed) {
        return false;
    }
    // the fifth is the half-way key: without one there is no fifth part (as the menu writes it)
    if (parts.size() == 5 && parts[4].empty()) {
        parts.pop_back();
    }
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        joined += (i == 0 ? "" : ",") + parts[i];
    }
    keys = joined;
    return true;
}

bool Has(const std::string& keys, const std::string& key) {
    const std::vector<std::string> names = Names(keys);
    return std::any_of(names.begin(), names.end(), [&key](const std::string& name) { return Same(name, key); });
}

std::string TapName(int index) {
    return "touch_tap_" + std::to_string(index + 1);
}

} // namespace

bool Parse(const std::string& line, Tap& tap) {
    // the first @ with a point after it: a key's name can have one ("Keypad @"), and so can the name
    for (std::size_t at = line.find('@'); at != std::string::npos; at = line.find('@', at + 1)) {
        std::size_t i = at + 1;
        int x = 0, y = 0;
        if (!Number(line, i, x)) {
            continue;
        }
        std::size_t comma = i;
        while (comma < line.size() && IsSpace(line[comma])) {
            ++comma;
        }
        if (comma < line.size() && line[comma] == ',') {
            i = comma + 1;
        }
        if (!Number(line, i, y) || (i < line.size() && !IsSpace(line[i]))) {
            continue;  // (not "52 220px")
        }
        tap.keys = Join(Names(line.substr(0, at)));
        tap.x = std::clamp(x, 0, WIDTH - 1);
        tap.y = std::clamp(y, 0, HEIGHT - 1);
        tap.name = Strip(line.substr(i));
        return true;
    }
    return false;
}

std::string Format(const Tap& tap) {
    // the ; key right after a |, which Names reads past: "Z | ;" -> "Z |;", ";" -> "|;"
    std::string keys = Join(Names(tap.keys));
    if (!keys.empty() && keys.front() == ';') {
        keys.insert(0, "|");
    }
    std::string line = keys.empty() ? "@ " : IniSafe(keys) + " @ ";
    line += std::to_string(std::clamp(tap.x, 0, WIDTH - 1)) + " " + std::to_string(std::clamp(tap.y, 0, HEIGHT - 1));
    if (const std::string name = Strip(IniSafe(tap.name)); !name.empty()) {
        line += " " + name;
    }
    return line;
}

std::vector<std::string> Names(const std::string& keys) {
    std::vector<std::string> names;
    for (std::string& name : Split(keys, '|')) {
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

std::string Join(const std::vector<std::string>& names) {
    std::string keys;
    for (const std::string& name : names) {
        if (!name.empty()) {
            keys += (keys.empty() ? "" : " | ") + name;
        }
    }
    return keys;
}

bool Same(const std::string& a, const std::string& b) {
    return !a.empty() && a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string WithKey(const std::string& keys, int slot, const std::string& key) {
    std::vector<std::string> names = Names(keys);
    if (slot >= 0 && slot < static_cast<int>(names.size())) {
        names[slot] = key;
    } else {
        names.push_back(key);
    }
    return Join(names);
}

std::vector<std::string> StickParts(const std::string& keys) {
    if (Strip(keys).empty()) {
        return {};
    }
    return Split(keys, ',');
}

bool IsOwn(const Others& others, const std::string& key) {
    return std::any_of(others.own.begin(), others.own.end(), [&key](const std::string& keys) { return Has(keys, key); });
}

std::vector<std::string> Settle(std::vector<Tap>& taps, Others& others) {
    std::vector<std::string> log;
    for (int i = 0; i < static_cast<int>(taps.size()); ++i) {
        std::vector<std::string> kept;
        for (const std::string& key : Names(taps[i].keys)) {
            if (IsOwn(others, key)) {
                log.push_back(TapName(i) + ": " + key + " is the app's own key, not a tap's");
                continue;
            }
            if (std::any_of(kept.begin(), kept.end(), [&key](const std::string& k) { return Same(k, key); })) {
                continue;
            }
            int earlier = -1;
            for (int j = 0; j < i && earlier < 0; ++j) {
                earlier = Has(taps[j].keys, key) ? j : -1;
            }
            if (earlier >= 0) {
                log.push_back(TapName(i) + ": " + key + " is " + TapName(earlier) + "'s already");
                continue;
            }
            kept.push_back(key);
        }
        taps[i].keys = Join(kept);
        for (const std::string& key : kept) {
            for (auto& binding : others.bindings) {
                if (binding.second && RemoveFrom(*binding.second, key)) {
                    log.push_back(TapName(i) + " takes " + key + " from " + binding.first);
                }
            }
            for (auto& stick : others.sticks) {
                if (stick.second && RemoveFromStick(*stick.second, key)) {
                    log.push_back(TapName(i) + " takes " + key + " from " + stick.first);
                }
            }
        }
    }
    return log;
}

void Give(std::vector<Tap>& taps, int tap, int slot, const std::string& key, Others& others) {
    if (tap < 0 || tap >= static_cast<int>(taps.size()) || key.empty()) {
        return;
    }
    for (auto& binding : others.bindings) {
        if (binding.second) {
            RemoveFrom(*binding.second, key);
        }
    }
    for (auto& stick : others.sticks) {
        if (stick.second) {
            RemoveFromStick(*stick.second, key);
        }
    }
    for (int i = 0; i < static_cast<int>(taps.size()); ++i) {
        if (i != tap) {
            RemoveFrom(taps[i].keys, key);
        }
    }
    // and from the tap's other column: the key goes where it was asked for
    std::vector<std::string> names = Names(taps[tap].keys);
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        if (i != slot && Same(names[i], key)) {
            names[i].clear();
        }
    }
    if (slot >= 0 && slot < static_cast<int>(names.size())) {
        names[slot] = key;
    } else {
        names.push_back(key);
    }
    taps[tap].keys = Join(names);
}

bool Release(std::vector<Tap>& taps, const std::string& key) {
    bool had = false;
    for (Tap& tap : taps) {
        had = RemoveFrom(tap.keys, key) || had;
    }
    return had;
}

std::string WithoutTapKeys(const std::string& keys, const std::vector<Tap>& taps) {
    std::vector<std::string> kept;
    for (const std::string& key : Names(keys)) {
        if (std::none_of(taps.begin(), taps.end(), [&key](const Tap& tap) { return Has(tap.keys, key); })) {
            kept.push_back(key);
        }
    }
    return Join(kept);
}

} // namespace App::Taps
