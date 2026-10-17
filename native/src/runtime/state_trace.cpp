// --state-trace FILE [--observable FILE]: the game's observable state at each game frame, in N0-07's
// format (tools/native/state_diff.py). A line is written whenever the frame counter changes (the
// runtime's frame watcher calls us):
//
//   # statetrace v1 <date> game=0x<title id> cpu=native new3ds=off
//   f=<frame> t=<ticks> <name>=0x<hex, two digits per byte of the value's size> | <name>=-
//
// The observable file (config/native/observable.txt, parsed by tools/native/state_diff.py too) has
// one value per line: `<expr> <size> <name> <eq|ignore>`, where <expr> is an address or a pointer
// chain: the outermost [..] reads <size> bytes at the address inside it, every inner [..] is a u32
// read, and a null or unmapped pointer gives `-`.
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "mh4n/log.h"
#include "mh4n/runtime.h"
#include "trace_util.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Runtime";
constexpr const char* kDefaultObservable = "config/native/observable.txt";

struct StateValue {
    std::string expr;  // without the outermost brackets
    u32 size;
    std::string name;
};

bool IsWrapped(const std::string& text) {
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return false;
    }
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        depth += text[i] == '[';
        depth -= text[i] == ']';
        if (depth == 0) {
            return i + 1 == text.size();
        }
    }
    return false;
}

// Splits at the '+' outside brackets; false if the brackets do not match.
bool SplitTerms(const std::string& text, std::vector<std::string>& terms) {
    int depth = 0;
    std::string current;
    terms.clear();
    for (const char c : text) {
        depth += c == '[';
        depth -= c == ']';
        if (depth < 0) {
            return false;
        }
        if (c == '+' && depth == 0) {
            terms.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    terms.push_back(current);
    return depth == 0;
}

bool IsHexTerm(const std::string& term) {
    return term.size() > 2 && term[0] == '0' && term[1] == 'x' &&
           term.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos;
}

bool CheckAddressExpr(const std::string& text) {
    std::vector<std::string> terms;
    if (!SplitTerms(text, terms)) {
        return false;
    }
    for (const std::string& term : terms) {
        if (IsWrapped(term) ? !CheckAddressExpr(term.substr(1, term.size() - 2)) : !IsHexTerm(term)) {
            return false;
        }
    }
    return true;
}

bool LoadObservable(const std::string& path, std::vector<StateValue>& values, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read the observable file " + path + " (--observable FILE)";
        return false;
    }
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        line = line.substr(0, line.find('#'));
        std::istringstream fields(line);
        std::string expr, size, name, mode, extra;
        if (!(fields >> expr)) {
            continue;  // blank
        }
        if (!(fields >> size >> name >> mode) || (fields >> extra)) {
            error = fmt::format("{}:{}: want '<expr> <size> <name> <eq|ignore>'", path, number);
            return false;
        }
        const std::string inner = IsWrapped(expr) ? expr.substr(1, expr.size() - 2) : expr;
        if (!CheckAddressExpr(inner) || (size != "1" && size != "2" && size != "4") ||
            (mode != "eq" && mode != "ignore")) {
            error = fmt::format("{}:{}: bad expression, size or mode", path, number);
            return false;
        }
        values.push_back({inner, static_cast<u32>(std::atoi(size.c_str())), name});
    }
    if (values.empty()) {
        error = path + " lists no values";
        return false;
    }
    return true;
}

// Reads `size` (1, 2 or 4) bytes, little-endian; false if any of them is not mapped.
bool ReadGuest(const GuestMemory& memory, u32 address, u32 size, u32& value) {
    if (address + size < address) {
        return false;
    }
    u8 bytes[4] = {};
    if (!memory.ReadBlock(bytes, address, size)) {
        return false;
    }
    value = 0;
    for (u32 i = 0; i < size; ++i) {
        value |= static_cast<u32>(bytes[i]) << (8 * i);
    }
    return true;
}

// The address an expression stands for; false if a pointer in a chain is null or unreadable.
bool EvalAddress(const GuestMemory& memory, const std::string& text, u32& address) {
    std::vector<std::string> terms;
    if (!SplitTerms(text, terms)) {
        return false;
    }
    address = 0;
    for (const std::string& term : terms) {
        u32 part = 0;
        if (IsWrapped(term)) {
            u32 inner = 0;
            if (!EvalAddress(memory, term.substr(1, term.size() - 2), inner) ||
                !ReadGuest(memory, inner, 4, part) || part == 0) {
                return false;
            }
        } else {
            part = static_cast<u32>(std::strtoul(term.c_str(), nullptr, 16));
        }
        address += part;
    }
    return true;
}

class StateTracer {
public:
    StateTracer(Runtime& rt, const std::string& path, std::vector<StateValue> values)
        : rt_(rt), file_(path), values_(std::move(values)) {}
    bool ok() const { return file_.ok(); }

    void WriteHeader() {
        const ProcessImage* image = rt_.Image();
        file_.Write(detail::TraceHeader("statetrace", image ? image->program_id : 0));
    }

    void OnFrame(u32 frame, u64 tick) {
        std::string line = fmt::format("f={} t={}", frame, tick);
        for (const StateValue& v : values_) {
            u32 address = 0, value = 0;
            if (EvalAddress(rt_.Memory(), v.expr, address) && ReadGuest(rt_.Memory(), address, v.size, value)) {
                line += fmt::format(" {}=0x{:0{}x}", v.name, value, v.size * 2);
            } else {
                line += fmt::format(" {}=-", v.name);
            }
        }
        line += '\n';
        file_.Write(line);
    }

    void Finish() { file_.Close(); }

private:
    Runtime& rt_;
    detail::TraceFile file_;
    std::vector<StateValue> values_;
};

void StartStateTrace(Runtime& rt) {
    const auto path = rt.GetOptions().Get("--state-trace");
    if (!path) {
        return;
    }
    std::vector<StateValue> values;
    std::string error;
    if (!LoadObservable(rt.GetOptions().GetString("--observable", kDefaultObservable), values, error)) {
        MH4N_LOG(Error, kLog, "{}", error);
        rt.RequestExit(2, error);
        return;
    }
    auto tracer = std::make_shared<StateTracer>(rt, *path, std::move(values));
    if (!tracer->ok()) {
        MH4N_LOG(Error, kLog, "cannot write {}", *path);
        rt.RequestExit(2, "cannot write " + *path);
        return;
    }
    tracer->WriteHeader();
    StateTracer* const raw = tracer.get();  // rt.Own keeps it alive as long as the callback
    rt.OnFrame([raw](u32 frame, u64 tick) { raw->OnFrame(frame, tick); });
    rt.OnFinish([tracer] { tracer->Finish(); });
    rt.Own(tracer);
}

MH4N_REGISTER(BootStep, "runtime.state-trace", BootStep{Phase::Tracing, StartStateTrace});

}  // namespace

}  // namespace mh4n
