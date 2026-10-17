#include "util/log.h"

#include <atomic>
#include <cstdio>

namespace App::Util {

namespace {

std::atomic<LogSink> g_sink{nullptr};

const char* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warning:
        return "Warning";
    case LogLevel::Error:
        return "Error";
    case LogLevel::Critical:
        return "Critical";
    }
    return "?";
}

} // namespace

LogSink SetLogSink(LogSink sink) {
    return g_sink.exchange(sink);
}

void WriteLog(LogLevel level, const char* file, unsigned line, const char* function,
              const std::string& message) {
    if (const LogSink sink = g_sink.load()) {
        sink(level, file, line, function, message);
        return;
    }
    std::fprintf(stderr, "[%s] %s:%u %s: %s\n", LevelName(level), file, line, function,
                 message.c_str());
}

} // namespace App::Util
