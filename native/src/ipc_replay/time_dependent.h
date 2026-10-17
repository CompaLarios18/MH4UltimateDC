// The commands whose replies depend on time (N1-11): a tick count, a timer state. The replay compares
// them by result only. The file has the shape of the one `tools/compare_ipc_traces.py --ignore` reads:
//
//     <session> <header>   # reason
//
// `<session>` is the trace's session name (`fs:USER`; `fs:USER/file` for every file session of
// fs:USER), `<header>` the request's header word in hex. `#` starts a comment; empty lines are skipped.
// The reader is not the same code: this one demands strict UTF-8 as Python's decoder does, splits fields
// at ASCII white space only (Python's split also takes Unicode white space) and takes a header of hex
// digits with an optional 0x, at most 32 bits (Python's int(x, 16) also takes a sign, underscores and
// any size). A file this reads and Python does not is one with Unicode white space inside what this
// takes for one field; a file Python reads and this does not is one with a header int(x, 16) takes and
// this does not, or with a lone CR (Python ends a line there).
#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::ipc_replay {

class TimeDependent {
public:
    // Adds the entries of a file. False, and a message with the file and line in *error, for a file that
    // cannot be read (a directory included), is not UTF-8 text, or has a line that is not
    // `<session> <hex header>`.
    bool LoadFile(const std::string& path, std::string* error);
    // The same for text already read (`name` is what messages call it).
    bool LoadText(const std::string& text, const std::string& name, std::string* error);
    void Add(std::string session, u32 header) { entries_.insert({std::move(session), header}); }

    // Whether `header` sent through `session` (`fs:USER/file#3`) is listed, by its name or by its name
    // without the instance number.
    bool Matches(const std::string& session, u32 header) const;
    std::size_t size() const { return entries_.size(); }

private:
    std::set<std::pair<std::string, u32>> entries_;
};

}  // namespace mh4n::ipc_replay
