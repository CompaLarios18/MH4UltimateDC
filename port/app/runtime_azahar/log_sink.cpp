#include "runtime_azahar/log_sink.h"

#include <string>

#include "common/logging/log.h"
#include "util/log.h"

namespace App::Azahar {

namespace {

Common::Log::Level LevelFor(Util::LogLevel level) {
    switch (level) {
    case Util::LogLevel::Debug:
        return Common::Log::Level::Debug;
    case Util::LogLevel::Info:
        return Common::Log::Level::Info;
    case Util::LogLevel::Warning:
        return Common::Log::Level::Warning;
    case Util::LogLevel::Error:
        return Common::Log::Level::Error;
    case Util::LogLevel::Critical:
        return Common::Log::Level::Critical;
    }
    return Common::Log::Level::Info;
}

void Forward(Util::LogLevel level, const char* file, unsigned line, const char* function,
             const std::string& message) {
    Common::Log::FmtLogMessage(Common::Log::Class::Frontend, LevelFor(level),
                               Common::Log::TrimSourcePath(file), line, function, "{}", message);
}

} // namespace

void InstallLogSink() {
    Util::SetLogSink(&Forward);
}

} // namespace App::Azahar
