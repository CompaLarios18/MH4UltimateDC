// The two trace formats the replay reads (N1-11): the IPC trace of `--trace-ipc-full`
// (tools/native/tracefmt.py, `# ipctrace`) and the SVC trace of `--trace-svc` (`# svctrace`). Both are
// read as tracefmt.py reads them, with one difference: a line that cannot be understood is an error
// (with its line number), where tracefmt.py skips it, so a damaged trace never reads as a shorter one.
#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::ipc_replay {

// One translate descriptor of a command: `<kind>:<index>:<size>:<token>` (tracefmt.IpcDesc). Kinds are
// static, mapped-r, mapped-w, mapped-rw, pxi, copy, move, pid. A buffer's token is its CRC-32
// (`0x%08x`) or `-`; a handle descriptor's is the names of its handles joined by `+`.
struct IpcDesc {
    std::string kind;
    u32 index = 0;
    u32 size = 0;
    std::string token;
    bool IsHandles() const { return kind == "copy" || kind == "move"; }
    bool IsBuffer() const {
        return kind == "static" || kind == "mapped-r" || kind == "mapped-w" || kind == "mapped-rw";
    }
};

struct IpcRecord {
    std::string thread;
    u32 seq = 0;
    std::string session;
    std::optional<u32> cmd;                 // the request's header word; none: never seen
    std::optional<std::vector<u32>> in;     // every word of the request, as the client wrote it
    std::vector<IpcDesc> desc;
    std::optional<std::vector<u32>> out;    // every word of the reply; none: no reply
    std::vector<IpcDesc> outdesc;
    std::map<u32, std::vector<u8>> data;    // descriptor index -> the bytes of an input buffer
    u64 ticks = 0;
    std::size_t line = 0;
};

struct IpcTrace {
    std::vector<IpcRecord> records;  // in file order
};

// One call of an SVC trace (tracefmt.Call): the registers r0 to r3 as the call was made, and as it
// completed when `has_out`.
struct SvcCall {
    std::string thread;
    u32 svc = 0;
    std::array<u32, 4> args{};
    std::array<u32, 4> out{};
    bool has_out = false;
    std::size_t line = 0;
};

struct SvcTrace {
    std::vector<SvcCall> calls;  // in file order
};

// False, and a message with the line number in *error, when the text is not a trace of that kind:
// no header, a line that does not parse, a request line without `seq=`.
bool ParseIpcTrace(std::string_view text, IpcTrace* trace, std::string* error);
bool ParseSvcTrace(std::string_view text, SvcTrace* trace, std::string* error);
// The same for a file; also false when it cannot be read.
bool LoadIpcTrace(const std::string& path, IpcTrace* trace, std::string* error);
bool LoadSvcTrace(const std::string& path, SvcTrace* trace, std::string* error);

}  // namespace mh4n::ipc_replay
