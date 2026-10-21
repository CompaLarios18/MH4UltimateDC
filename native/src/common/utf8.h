// UTF-8 and UTF-16 text, as pure functions (N1-13d, D45). The host side of the file system speaks UTF-8
// everywhere (--user, the game folder, the names in an archive); Windows speaks UTF-16, and its "ANSI"
// functions lose every character outside the active code page. These conversions are what the runner uses
// to read the command line and what the file calls use to reach the wide-character API. They are plain
// functions of text, so a Linux build tests them.
//
// Invalid input never fails: a byte that does not start a valid UTF-8 sequence, a truncated sequence, an
// overlong or out-of-range value and a surrogate written in UTF-8 each become U+FFFD (one per bad byte),
// and a UTF-16 unit that is a surrogate without its partner becomes U+FFFD.
#pragma once

#include <string>
#include <string_view>

namespace mh4n::utf8 {

std::u16string ToUtf16(std::string_view utf8);
std::string FromUtf16(std::u16string_view utf16);

#if defined(_WIN32)
// The same for the wchar_t of Windows (UTF-16, two bytes). `nullptr` is the empty string.
std::wstring ToWide(std::string_view utf8);
std::string FromWide(const wchar_t* text);
std::string FromWide(std::wstring_view text);
#endif

}  // namespace mh4n::utf8
