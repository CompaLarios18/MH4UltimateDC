// Derived from Azahar src/common/string_util.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): StripSpaces (which characters count as spaces) and SplitString (the empty
// piece after the last delimiter is dropped).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
#include "util/strings.h"

#include <cstdint>
#include <utility>

namespace App::Util {

namespace {

constexpr char32_t REPLACEMENT = 0xFFFD;

/// The code point at `text[at]` and how many bytes it takes; U+FFFD and 1 for a bad byte.
std::pair<char32_t, std::size_t> DecodeUtf8(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    if (lead < 0x80) {
        return {lead, 1};
    }
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return {REPLACEMENT, 1};
    }
    if (at + length > text.size()) {
        return {REPLACEMENT, 1};
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = byte(at + i);
        if ((next & 0xC0) != 0x80) {
            return {REPLACEMENT, 1};
        }
        value = (value << 6) | (next & 0x3F);
    }
    // overlong forms, surrogates and values past U+10FFFF are not UTF-8
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return {REPLACEMENT, 1};
    }
    return {value, length};
}

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::u32string Utf8ToCodePoints(std::string_view text) {
    std::u32string out;
    for (std::size_t at = 0; at < text.size();) {
        const auto [cp, length] = DecodeUtf8(text, at);
        out += cp;
        at += length;
    }
    return out;
}

} // namespace

std::string StripSpaces(const std::string& text) {
    constexpr const char* SPACES = " \t\r\n";
    const std::size_t first = text.find_first_not_of(SPACES);
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(SPACES) - first + 1);
}

std::vector<std::string> SplitString(const std::string& text, char delim) {
    std::vector<std::string> pieces;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(delim, start);
        if (end == std::string::npos) {
            pieces.push_back(text.substr(start));
            break;
        }
        pieces.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return pieces;
}

std::u16string Utf8ToUtf16(std::string_view text) {
    std::u16string out;
    for (const char32_t cp : Utf8ToCodePoints(text)) {
        if (cp >= 0x10000) {
            const char32_t v = cp - 0x10000;
            out += static_cast<char16_t>(0xD800 + (v >> 10));
            out += static_cast<char16_t>(0xDC00 + (v & 0x3FF));
        } else {
            out += static_cast<char16_t>(cp);
        }
    }
    return out;
}

std::string Utf16ToUtf8(std::u16string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char16_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
                const char32_t cp =
                    0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) + (text[i + 1] - 0xDC00);
                AppendUtf8(out, cp);
                ++i;
            } else {
                AppendUtf8(out, REPLACEMENT);
            }
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            AppendUtf8(out, REPLACEMENT);
        } else {
            AppendUtf8(out, unit);
        }
    }
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if constexpr (sizeof(wchar_t) == 2) {
        const std::u16string wide = Utf8ToUtf16(text);
        return std::wstring(wide.begin(), wide.end());
    } else {
        const std::u32string points = Utf8ToCodePoints(text);
        return std::wstring(points.begin(), points.end());
    }
}

std::string WideToUtf8(const std::wstring& text) {
    if constexpr (sizeof(wchar_t) == 2) {
        return Utf16ToUtf8(std::u16string(text.begin(), text.end()));
    } else {
        std::string out;
        for (const wchar_t c : text) {
            const auto cp = static_cast<char32_t>(c);
            const bool bad = cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF);
            AppendUtf8(out, bad ? REPLACEMENT : cp);
        }
        return out;
    }
}

} // namespace App::Util
