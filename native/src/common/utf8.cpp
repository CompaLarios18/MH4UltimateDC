#include "utf8.h"

#include <cstddef>
#include <cstdint>

namespace mh4n::utf8 {

namespace {
constexpr std::uint32_t kReplacement = 0xFFFD;
}

std::u16string ToUtf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint8_t lead = static_cast<std::uint8_t>(text[i]);
        std::uint32_t code = kReplacement;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2, code = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3, code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4, code = lead & 0x07u;
        }
        if (length > 1) {
            bool good = i + length <= text.size();
            for (std::size_t k = 1; good && k < length; ++k) {
                const std::uint8_t next = static_cast<std::uint8_t>(text[i + k]);
                good = (next & 0xC0) == 0x80;
                code = (code << 6) | (next & 0x3Fu);
            }
            if (!good || code > 0x10FFFF || (code >= 0xD800 && code < 0xE000)) {
                code = kReplacement;
                length = 1;
            }
        }
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

std::string FromUtf16(std::u16string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = text[i];
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00u);
            ++i;
        } else if (code >= 0xD800 && code < 0xE000) {
            code = kReplacement;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

#if defined(_WIN32)
static_assert(sizeof(wchar_t) == sizeof(char16_t), "wchar_t is UTF-16 on Windows");

std::wstring ToWide(std::string_view utf8) {
    const std::u16string units = ToUtf16(utf8);
    return std::wstring(units.begin(), units.end());
}

std::string FromWide(const wchar_t* text) {
    return text == nullptr ? std::string() : FromWide(std::wstring_view(text));
}

std::string FromWide(std::wstring_view text) {
    const std::u16string units(text.begin(), text.end());
    return FromUtf16(units);
}
#endif

}  // namespace mh4n::utf8
