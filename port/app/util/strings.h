// String helpers of the app: trimming and splitting, and UTF-8 / UTF-16 conversions (the
// launcher's Windows dialogs and the command line take UTF-16).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace App::Util {

/// The string without the spaces, tabs and line ends (" \t\r\n") at both ends.
std::string StripSpaces(const std::string& text);

/// The pieces between `delim`s. A piece after the last delimiter is dropped when it is empty,
/// so "a,b," is {"a", "b"}, "a,,b" is {"a", "", "b"} and "" is {}.
std::vector<std::string> SplitString(const std::string& text, char delim);

/// UTF-8 to UTF-16. Bytes that are not valid UTF-8 become U+FFFD, one per bad byte.
std::u16string Utf8ToUtf16(std::string_view text);

/// UTF-16 to UTF-8. A surrogate without its pair becomes U+FFFD.
std::string Utf16ToUtf8(std::u16string_view text);

/// The platform's wide string: UTF-16 where wchar_t is 16 bits wide (Windows), else UTF-32.
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

} // namespace App::Util
