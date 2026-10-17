#include "ipc_replay/trace.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include "mh4n/ipc.h"

namespace mh4n::ipc_replay {

namespace {

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(separator, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

std::vector<std::string_view> Fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// A number with an optional 0x prefix; `hex_default` reads digits without a prefix as hex.
bool ParseNumber(std::string_view text, bool hex_default, u64* out) {
    int base = hex_default ? 16 : 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return false;
    }
    u64 value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (ec != std::errc() || end != text.data() + text.size()) {
        return false;
    }
    *out = value;
    return true;
}

bool ParseWord(std::string_view text, u32* out) {
    u64 value = 0;
    if (!ParseNumber(text, /*hex_default=*/true, &value) || value > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<u32>(value);
    return true;
}

std::string Where(std::size_t line, const std::string& what) {
    return fmt::format("line {}: {}", line, what);
}

// `-` and the empty field are "none".
bool ParseWords(std::string_view text, std::optional<std::vector<u32>>* out, std::string* why) {
    out->reset();
    if (text.empty() || text == "-") {
        return true;
    }
    std::vector<u32> words;
    for (const std::string_view part : Split(text, ',')) {
        u32 word = 0;
        if (!ParseWord(part, &word)) {
            *why = "a word that is not a number";
            return false;
        }
        words.push_back(word);
    }
    *out = std::move(words);
    return true;
}

bool ParseDescs(std::string_view text, std::vector<IpcDesc>* out, std::string* why) {
    out->clear();
    if (text.empty() || text == "-") {
        return true;
    }
    for (const std::string_view item : Split(text, ',')) {
        // The token of a handle descriptor holds service names, which have ':' in them: split at the
        // first three colons only.
        std::array<std::string_view, 4> parts;
        std::string_view rest = item;
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t at = rest.find(':');
            if (at == std::string_view::npos) {
                *why = "a descriptor without index and size";
                return false;
            }
            parts[i] = rest.substr(0, at);
            rest.remove_prefix(at + 1);
        }
        parts[3] = rest;
        IpcDesc desc;
        desc.kind = std::string(parts[0]);
        u64 index = 0, size = 0;
        if (!ParseNumber(parts[1], false, &index) || !ParseNumber(parts[2], false, &size)) {
            *why = "a descriptor whose index or size is not a number";
            return false;
        }
        // An index is a word of the 64-word command buffer, and a handle descriptor holds at most that
        // many handles: anything else is a damaged line.
        if (index >= ipc::kCommandBufferLength ||
            ((desc.kind == "copy" || desc.kind == "move") && size > ipc::kCommandBufferLength)) {
            *why = "a descriptor outside the command buffer";
            return false;
        }
        desc.index = static_cast<u32>(index);
        desc.size = static_cast<u32>(size);
        desc.token = std::string(parts[3]);
        out->push_back(std::move(desc));
    }
    return true;
}

bool ParseHexBytes(std::string_view text, std::vector<u8>* out) {
    if (text.size() % 2 != 0) {
        return false;
    }
    out->clear();
    out->reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
        if (ec != std::errc() || end != text.data() + i + 2) {
            return false;
        }
        out->push_back(static_cast<u8>(value));
    }
    return true;
}

// The header line: `# <tool> v<n> ...`. Returns the tool name, or empty when the line is not one.
std::string HeaderTool(std::string_view line) {
    const auto fields = Fields(line);
    if (fields.size() >= 3 && fields[0] == "#" && fields[2].size() > 1 && fields[2][0] == 'v') {
        return std::string(fields[1]);
    }
    return {};
}

template <class Handler>
bool ForEachLine(std::string_view text, const char* tool, Handler handler, std::string* error) {
    bool have_header = false;
    std::size_t number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const auto fields = Fields(line);
        if (fields.empty()) {
            continue;
        }
        if (!have_header) {
            if (HeaderTool(line) != tool) {
                *error = Where(number, fmt::format("not a {} (the first line is not `# {} v1 ...`)",
                                                   tool, tool));
                return false;
            }
            have_header = true;
            continue;
        }
        if (fields[0].starts_with('#')) {
            continue;
        }
        if (!handler(fields, number, error)) {
            return false;
        }
    }
    if (!have_header) {
        *error = fmt::format("empty: no `# {} v1 ...` header", tool);
        return false;
    }
    return true;
}

}  // namespace

bool ParseIpcTrace(std::string_view text, IpcTrace* trace, std::string* error) {
    *trace = {};
    const auto handler = [&](const std::vector<std::string_view>& fields, std::size_t line,
                             std::string* err) {
        IpcRecord record;
        record.thread = std::string(fields[0]);
        record.line = line;
        bool have_seq = false;
        std::string why;
        for (std::size_t i = 1; i < fields.size(); ++i) {
            const std::size_t eq = fields[i].find('=');
            if (eq == std::string_view::npos) {
                *err = Where(line, fmt::format("`{}` is not key=value", fields[i]));
                return false;
            }
            const std::string_view key = fields[i].substr(0, eq);
            const std::string_view value = fields[i].substr(eq + 1);
            bool ok = true;
            if (key == "seq") {
                u64 seq = 0;
                ok = ParseNumber(value, false, &seq) && seq <= 0xFFFFFFFFull;
                record.seq = static_cast<u32>(seq);
                have_seq = ok;
                why = "seq is not a number";
            } else if (key == "session") {
                record.session = std::string(value);
            } else if (key == "cmd") {
                if (value != "-") {
                    u32 word = 0;
                    ok = ParseWord(value, &word);
                    record.cmd = word;
                    why = "cmd is not a number";
                }
            } else if (key == "in") {
                ok = ParseWords(value, &record.in, &why);
            } else if (key == "out") {
                ok = ParseWords(value, &record.out, &why);
            } else if (key == "desc") {
                ok = ParseDescs(value, &record.desc, &why);
            } else if (key == "outdesc") {
                ok = ParseDescs(value, &record.outdesc, &why);
            } else if (key == "t") {
                ok = ParseNumber(value, false, &record.ticks);
                why = "t is not a number";
            } else if (key == "data") {
                const std::size_t colon = value.find(':');
                u64 index = 0;
                std::vector<u8> bytes;
                ok = colon != std::string_view::npos && ParseNumber(value.substr(0, colon), false, &index) &&
                     index < ipc::kCommandBufferLength && ParseHexBytes(value.substr(colon + 1), &bytes);
                record.data[static_cast<u32>(index)] = std::move(bytes);
                why = "data is not <index>:<hex bytes>";
            }  // other keys are ignored, as tracefmt.py does
            if (!ok) {
                *err = Where(line, why);
                return false;
            }
        }
        if (!have_seq) {
            *err = Where(line, "a request line without seq=");
            return false;
        }
        trace->records.push_back(std::move(record));
        return true;
    };
    return ForEachLine(text, "ipctrace", handler, error);
}

bool ParseSvcTrace(std::string_view text, SvcTrace* trace, std::string* error) {
    *trace = {};
    const auto handler = [&](const std::vector<std::string_view>& fields, std::size_t line,
                             std::string* err) {
        // <thread> <svc> pc=<hex> lr=<hex> <r0> <r1> <r2> <r3> t=<ticks> [in4=..] [out=<r0>,<r1>,<r2>,<r3>]
        if (fields.size() < 9) {
            *err = Where(line, "an SVC call needs <thread> <svc> pc= lr= r0 r1 r2 r3 t=");
            return false;
        }
        SvcCall call;
        call.thread = std::string(fields[0]);
        call.line = line;
        u32 svc = 0;
        if (!ParseWord(fields[1], &svc)) {
            *err = Where(line, "the SVC number is not a number");
            return false;
        }
        call.svc = svc;
        for (std::size_t i = 0; i < 4; ++i) {
            if (!ParseWord(fields[4 + i], &call.args[i])) {
                *err = Where(line, "a register is not a number");
                return false;
            }
        }
        for (std::size_t i = 9; i < fields.size(); ++i) {
            if (!fields[i].starts_with("out=")) {
                continue;
            }
            const std::string_view value = fields[i].substr(4);
            if (value == "-") {
                continue;
            }
            const auto parts = Split(value, ',');
            if (parts.size() != 4) {
                *err = Where(line, "out= needs four registers");
                return false;
            }
            for (std::size_t k = 0; k < 4; ++k) {
                if (!ParseWord(parts[k], &call.out[k])) {
                    *err = Where(line, "an out register is not a number");
                    return false;
                }
            }
            call.has_out = true;
        }
        trace->calls.push_back(call);
        return true;
    };
    // `# thread` lines are metadata; the SVC numbers and registers are hex digits without a prefix.
    return ForEachLine(text, "svctrace", handler, error);
}

namespace {

bool ReadFile(const std::string& path, std::string* text, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = fmt::format("cannot read {}", path);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    *text = buffer.str();
    return true;
}

}  // namespace

bool LoadIpcTrace(const std::string& path, IpcTrace* trace, std::string* error) {
    std::string text;
    if (!ReadFile(path, &text, error)) {
        return false;
    }
    if (!ParseIpcTrace(text, trace, error)) {
        *error = path + ": " + *error;
        return false;
    }
    return true;
}

bool LoadSvcTrace(const std::string& path, SvcTrace* trace, std::string* error) {
    std::string text;
    if (!ReadFile(path, &text, error)) {
        return false;
    }
    if (!ParseSvcTrace(text, trace, error)) {
        *error = path + ": " + *error;
        return false;
    }
    return true;
}

}  // namespace mh4n::ipc_replay
