// Logging (N1-01): MH4N_LOG(level, "Category", "fmt {}", args...), formatted with fmt.
//
// Levels are Trace, Debug, Info, Warning, Error, Critical. The filter has the syntax of
// `mh4u_runner --log`: space-separated `Category:Level` rules, applied in order, where `*` sets
// every category (and drops the earlier per-category rules), e.g. "*:Info Kernel:Debug". Level
// names are matched ignoring case. Unlike Azahar, a rule for "Service" also covers "Service.FS"
// unless that has a rule of its own.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "mh4n/types.h"

namespace mh4n {

enum class LogLevel : u8 { Trace, Debug, Info, Warning, Error, Critical };

const char* LogLevelName(LogLevel level);
std::optional<LogLevel> ParseLogLevel(std::string_view name);

class LogFilter {
public:
    explicit LogFilter(LogLevel default_level = LogLevel::Info) : default_(default_level) {}

    // Applies the rules of `spec` on top of the current state. A malformed rule is skipped and
    // described in `*error` (one line per rule); returns false if there was one.
    bool Parse(std::string_view spec, std::string* error = nullptr);

    LogLevel LevelFor(std::string_view category) const;
    bool Enabled(LogLevel level, std::string_view category) const {
        return level >= LevelFor(category);
    }
    // The lowest level any category logs at.
    LogLevel Lowest() const;

private:
    LogLevel default_;
    std::map<std::string, LogLevel, std::less<>> overrides_;
};

struct LogRecord {
    LogLevel level;
    std::string_view category;
    std::string_view file;
    int line;
    std::string_view message;
};

// A sink is called with the sink mutex held, so it must not log. An empty sink restores the
// default one, which writes "[Level] Category: message" to stderr.
using LogSink = std::function<void(const LogRecord&)>;

void SetLogFilter(const LogFilter& filter); // the default is "*:Info"
LogFilter GetLogFilter();
void SetLogSink(LogSink sink);

// True if a message of `level` for `category` passes the filter (cheap when it doesn't).
bool LogEnabled(LogLevel level, std::string_view category);
void LogWrite(LogLevel level, std::string_view category, std::string_view file, int line,
              std::string_view message);

} // namespace mh4n

// `level` is one of the LogLevel names without the prefix: MH4N_LOG(Info, "Kernel", "x={}", x).
// The arguments are not evaluated when the message is filtered out.
#define MH4N_LOG(level, category, ...)                                                             \
    do {                                                                                           \
        if (::mh4n::LogEnabled(::mh4n::LogLevel::level, category)) {                               \
            ::mh4n::LogWrite(::mh4n::LogLevel::level, category, __FILE__, __LINE__,                \
                             ::fmt::format(__VA_ARGS__));                                          \
        }                                                                                          \
    } while (0)
