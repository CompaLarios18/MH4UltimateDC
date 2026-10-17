// Small helpers the runtime's traces share. Private to mh4n_runtime.
#pragma once

#include <cstdio>
#include <ctime>
#include <string>

#include "mh4n/loader.h"
#include "mh4n/types.h"

namespace mh4n::detail {

// `# <tool> v1 <ISO date> game=0x<title id> cpu=native new3ds=off`: the header of N0-04's and
// N0-07's formats (tools/native/tracefmt.py). The date is today's, UTC.
inline std::string TraceHeader(const char* tool, u64 program_id) {
    const std::time_t now = std::time(nullptr);
    char date[16] = "0000-00-00";
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
    }
    char line[160];
    std::snprintf(line, sizeof(line), "# %s v1 %s game=0x%016llx cpu=native new3ds=off\n", tool, date,
                  static_cast<unsigned long long>(program_id));
    return line;
}

// A file that is closed when the object goes.
class TraceFile {
public:
    explicit TraceFile(const std::string& path) : file_(std::fopen(path.c_str(), "w")) {}
    ~TraceFile() { Close(); }
    TraceFile(const TraceFile&) = delete;
    TraceFile& operator=(const TraceFile&) = delete;
    bool ok() const { return file_ != nullptr; }
    void Write(const std::string& text) {
        if (file_ != nullptr) {
            std::fputs(text.c_str(), file_);
        }
    }
    void Close() {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    std::FILE* file_;
};

}  // namespace mh4n::detail
