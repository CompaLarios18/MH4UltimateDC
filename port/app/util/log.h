// The app's own logging: APP_LOG_INFO("started {}", name) and its siblings. A message goes to
// the sink the runtime installed (the Azahar runtime forwards to Azahar's logger, class
// Frontend, so the log file and its filter stay as they were); without a sink it goes to stderr.
//
// The message is formatted before the sink sees it, whatever the sink's filter says. A sink may
// be called from any thread. Install it before other threads log.
#pragma once

#include <string>
#include <utility>
#include <fmt/format.h>

namespace App::Util {

enum class LogLevel { Debug, Info, Warning, Error, Critical };

/// `file` is the source file's __FILE__ as the compiler gave it (the sink trims it as it likes).
using LogSink = void (*)(LogLevel level, const char* file, unsigned line, const char* function,
                         const std::string& message);

/// Installs the sink (null: stderr again) and returns the one that was installed.
LogSink SetLogSink(LogSink sink);

/// Writes an already formatted message to the sink.
void WriteLog(LogLevel level, const char* file, unsigned line, const char* function,
              const std::string& message);

template <typename... Args>
void Log(LogLevel level, const char* file, unsigned line, const char* function,
         fmt::format_string<Args...> format, Args&&... args) {
    WriteLog(level, file, line, function, fmt::format(format, std::forward<Args>(args)...));
}

} // namespace App::Util

#define APP_LOG_DEBUG(...) \
    ::App::Util::Log(::App::Util::LogLevel::Debug, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define APP_LOG_INFO(...) \
    ::App::Util::Log(::App::Util::LogLevel::Info, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define APP_LOG_WARNING(...) \
    ::App::Util::Log(::App::Util::LogLevel::Warning, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define APP_LOG_ERROR(...) \
    ::App::Util::Log(::App::Util::LogLevel::Error, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define APP_LOG_CRITICAL(...) \
    ::App::Util::Log(::App::Util::LogLevel::Critical, __FILE__, __LINE__, __func__, __VA_ARGS__)
