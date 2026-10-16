#include "mh4n/log.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace mh4n {
namespace {

constexpr const char* kLevelNames[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct State {
    std::shared_mutex filter_mutex;
    LogFilter filter{LogLevel::Info};
    std::atomic<u8> lowest{static_cast<u8>(LogLevel::Info)};

    std::mutex sink_mutex;
    LogSink sink;
};

State& GetState() {
    static State state;
    return state;
}

void DefaultSink(const LogRecord& record) {
    std::string line;
    line.reserve(record.message.size() + 48);
    line += '[';
    line += LogLevelName(record.level);
    line += "] ";
    line += record.category;
    line += ": ";
    line += record.message;
    if (record.level >= LogLevel::Error && !record.file.empty()) {
        line += " (";
        line += record.file;
        line += ':';
        line += std::to_string(record.line);
        line += ')';
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
}

} // namespace

const char* LogLevelName(LogLevel level) {
    const auto index = static_cast<std::size_t>(level);
    return index < std::size(kLevelNames) ? kLevelNames[index] : "?";
}

std::optional<LogLevel> ParseLogLevel(std::string_view name) {
    for (std::size_t i = 0; i < std::size(kLevelNames); ++i) {
        if (EqualsIgnoreCase(name, kLevelNames[i])) {
            return static_cast<LogLevel>(i);
        }
    }
    return std::nullopt;
}

bool LogFilter::Parse(std::string_view spec, std::string* error) {
    bool all_ok = true;
    auto fail = [&](std::string_view rule, std::string_view why) {
        all_ok = false;
        if (error) {
            if (!error->empty()) {
                *error += '\n';
            }
            *error += "log filter rule '";
            *error += rule;
            *error += "': ";
            *error += why;
        }
    };

    std::size_t pos = 0;
    while (pos < spec.size()) {
        while (pos < spec.size() && std::isspace(static_cast<unsigned char>(spec[pos]))) {
            ++pos;
        }
        const std::size_t begin = pos;
        while (pos < spec.size() && !std::isspace(static_cast<unsigned char>(spec[pos]))) {
            ++pos;
        }
        if (begin == pos) {
            break;
        }
        const std::string_view rule = spec.substr(begin, pos - begin);
        const std::size_t colon = rule.find(':');
        if (colon == std::string_view::npos) {
            fail(rule, "expected Category:Level");
            continue;
        }
        const std::string_view category = rule.substr(0, colon);
        const std::optional<LogLevel> level = ParseLogLevel(rule.substr(colon + 1));
        if (category.empty()) {
            fail(rule, "empty category");
        } else if (!level) {
            fail(rule, "unknown level");
        } else if (category == "*") {
            default_ = *level;
            overrides_.clear();
        } else {
            overrides_[std::string(category)] = *level;
        }
    }
    return all_ok;
}

LogLevel LogFilter::LevelFor(std::string_view category) const {
    // The category itself, then its parents: "Service.FS.Read" -> "Service.FS" -> "Service".
    while (true) {
        auto it = overrides_.find(category);
        if (it != overrides_.end()) {
            return it->second;
        }
        const std::size_t dot = category.rfind('.');
        if (dot == std::string_view::npos) {
            return default_;
        }
        category = category.substr(0, dot);
    }
}

LogLevel LogFilter::Lowest() const {
    LogLevel lowest = default_;
    for (const auto& entry : overrides_) {
        if (entry.second < lowest) {
            lowest = entry.second;
        }
    }
    return lowest;
}

void SetLogFilter(const LogFilter& filter) {
    State& state = GetState();
    std::unique_lock lock(state.filter_mutex);
    state.filter = filter;
    state.lowest.store(static_cast<u8>(filter.Lowest()), std::memory_order_relaxed);
}

LogFilter GetLogFilter() {
    State& state = GetState();
    std::shared_lock lock(state.filter_mutex);
    return state.filter;
}

void SetLogSink(LogSink sink) {
    State& state = GetState();
    std::lock_guard lock(state.sink_mutex);
    state.sink = std::move(sink);
}

bool LogEnabled(LogLevel level, std::string_view category) {
    State& state = GetState();
    if (static_cast<u8>(level) < state.lowest.load(std::memory_order_relaxed)) {
        return false;
    }
    std::shared_lock lock(state.filter_mutex);
    return state.filter.Enabled(level, category);
}

void LogWrite(LogLevel level, std::string_view category, std::string_view file, int line,
              std::string_view message) {
    State& state = GetState();
    const LogRecord record{level, category, file, line, message};
    std::lock_guard lock(state.sink_mutex);
    if (state.sink) {
        state.sink(record);
    } else {
        DefaultSink(record);
    }
}

} // namespace mh4n
