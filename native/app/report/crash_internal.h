// Shared by the crash code of the two platforms (crash_posix.cpp, crash_win32.cpp). What is here
// and runs inside the handler is async-signal-safe: fixed buffers, no allocation, no fmt, no
// logging.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mh4n::app::report::detail {

/// Text appended to a caller's buffer, without allocation. What does not fit is dropped.
class Text {
public:
    Text(char* buffer, std::size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    void Put(char c) {
        if (size_ + 1 < capacity_) {
            buffer_[size_++] = c;
        }
    }
    void Str(const char* text) {
        for (; *text != '\0'; ++text) {
            Put(*text);
        }
    }
    /// Decimal.
    void Dec(std::uint64_t value) {
        char digits[20];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (n > 0) {
            Put(digits[--n]);
        }
    }
    /// "0x" and `digits` hexadecimal digits (at most 16), lower case.
    void Hex(std::uint64_t value, int digits) {
        Str("0x");
        for (int i = digits - 1; i >= 0; --i) {
            Put("0123456789abcdef"[(value >> (4 * i)) & 0xF]);
        }
    }
    /// "0x" and the digits of `value` without leading zeros.
    void HexMin(std::uint64_t value) {
        int digits = 1;
        while (digits < 16 && (value >> (4 * digits)) != 0) {
            ++digits;
        }
        Hex(value, digits);
    }
    const char* data() const { return buffer_; }
    std::size_t size() const { return size_; }
    void Clear() { size_ = 0; }

private:
    char* buffer_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

inline constexpr int kMaxFrames = 32;
/// SetGameState's "not published yet".
inline constexpr std::uint64_t kUnsetValue = ~std::uint64_t{0};

/// The process id, for the crash file's name.
unsigned long ProcessId();

/// "mh4u-native crash v1" and "commit: <the build's revision>".
void AppendPreamble(Text& text);
/// "game frame: <n or unknown>" and "scene: <n or unknown>", from SetGameState.
void AppendGameState(Text& text);

// ---- the platform side ----------------------------------------------------------------------------

/// Opens `crash_path` (UTF-8; the folder exists) for the handler, closing the file of an earlier
/// call, and installs the handlers the first time.
bool PlatformInstall(const std::string& crash_path);
/// True while the handler has written nothing to the crash file.
bool PlatformUnused();
/// Closes the crash file.
void PlatformClose();

}  // namespace mh4n::app::report::detail
