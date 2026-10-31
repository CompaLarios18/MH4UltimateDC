#include "launcher/environment.h"

#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace Launcher::Environment {

namespace {

/// The AppImage runtime's own variables: its mount, the AppImage file, the name it was started
/// by and the folder it was started in.
const char* const RUNTIME_VARIABLES[] = {"APPDIR", "APPIMAGE", "ARGV0", "OWD"};

bool InsideAny(const std::string& path, const std::vector<std::string>& mounts) {
    return std::any_of(mounts.begin(), mounts.end(),
                       [&path](const std::string& mount) { return Inside(path, mount); });
}

std::string NameOf(const std::string& entry) {
    return entry.substr(0, entry.find('='));
}

} // namespace

bool Inside(const std::string& path, const std::string& dir) {
    if (dir.empty() || path.size() < dir.size() || path.compare(0, dir.size(), dir) != 0) {
        return false;
    }
    return path.size() == dir.size() || dir.back() == '/' || path[dir.size()] == '/';
}

std::vector<std::string> WithoutAppImage(const std::vector<std::string>& env,
                                         const std::vector<std::string>& mounts) {
    if (mounts.empty()) {
        return env;
    }
    std::vector<std::string> out;
    out.reserve(env.size());
    for (const auto& entry : env) {
        const auto equals = entry.find('=');
        if (equals == std::string::npos || equals == 0) {
            out.push_back(entry);
            continue;
        }
        const std::string name = entry.substr(0, equals);
        if (std::find(std::begin(RUNTIME_VARIABLES), std::end(RUNTIME_VARIABLES), name) !=
            std::end(RUNTIME_VARIABLES)) {
            continue;
        }
        // the value's parts: those in a mount go
        const bool preload = name == "LD_PRELOAD";
        const std::string value = entry.substr(equals + 1);
        std::vector<std::string> kept;
        bool dropped = false;
        std::size_t start = 0;
        while (start <= value.size()) {
            std::size_t end = preload ? value.find_first_of(": ", start) : value.find(':', start);
            if (end == std::string::npos) {
                end = value.size();
            }
            std::string part = value.substr(start, end - start);
            if (!part.empty() && InsideAny(part, mounts)) {
                dropped = true;
            } else if (!part.empty()) {
                kept.push_back(std::move(part));
            }
            start = end + 1;
        }
        if (!dropped) {
            out.push_back(entry);
            continue;
        }
        if (kept.empty()) {
            continue;  // (the AppImage set it, or only added to an empty one)
        }
        std::string joined = name + "=";
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (i > 0) {
                joined += preload ? ' ' : ':';
            }
            joined += kept[i];
        }
        out.push_back(std::move(joined));
    }
    return out;
}

std::vector<std::string> Changed(const std::vector<std::string>& before,
                                 const std::vector<std::string>& after) {
    const std::unordered_set<std::string> kept(after.begin(), after.end());
    std::vector<std::string> names;
    for (const auto& entry : before) {
        if (kept.count(entry) != 0) {
            continue;
        }
        std::string name = NameOf(entry);
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(std::move(name));
        }
    }
    return names;
}

} // namespace Launcher::Environment
