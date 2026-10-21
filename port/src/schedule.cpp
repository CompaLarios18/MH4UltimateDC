// Parsing of the runner's scripts (N0-08). Standard library only; see schedule.h.
#include "schedule.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace Schedule {

namespace {

constexpr const char* kButtonNames[kButtonCount] = {"A",  "B",  "X",     "Y",      "UP",
                                                    "DOWN", "LEFT", "RIGHT", "L",    "R",
                                                    "START", "SELECT", "ZL",  "ZR",   "HOME"};

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

std::string Trim(const std::string& text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool AllDigits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

/// A whole number of frames, at most 2^32 - 1.
bool ParseFrameCount(const std::string& digits, std::uint32_t& out) {
    if (!AllDigits(digits) || digits.size() > 10) {
        return false;
    }
    const unsigned long long value = std::strtoull(digits.c_str(), nullptr, 10);
    if (value > 0xffffffffULL) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

/// A finite, non-negative decimal number and nothing else.
bool ParseMs(const std::string& text, double& out) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0])) || text[0] == '+') {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value) || value < 0.0) {
        return false;
    }
    out = value;
    return true;
}

bool ParseFloat(const std::string& text, float& out) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value)) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

bool ParseHex32(const std::string& text, std::uint32_t& out) {
    if (text.empty() || !std::isxdigit(static_cast<unsigned char>(text[text.size() > 2 && text[1] == 'x' ? 2 : 0]))) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 16);
    if (end != text.c_str() + text.size() || value > 0xffffffffULL) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool ParseHexBytes(const std::string& hex, std::vector<std::uint8_t>& out) {
    if (hex.empty() || hex.size() % 2) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        if (!std::isxdigit(static_cast<unsigned char>(hex[i])) ||
            !std::isxdigit(static_cast<unsigned char>(hex[i + 1]))) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return true;
}

std::string Number(double value) {
    std::ostringstream out;
    out << std::setprecision(15) << value; // "3000", "1500.5", "1234567.5"
    return out.str();
}

std::string HexBytes(const std::vector<std::uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string Hex8(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%08x", value);
    return buffer;
}

bool Fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

/// One event: WHEN:BUTTON[:HOLD], WHEN:TOUCH:x:y[:HOLD], WHEN:CIRCLE:x:y[:HOLD].
bool ParseEvent(const std::string& item, Press& press, std::string& error) {
    const auto f = Split(item, ':');
    if (f.size() < 2) {
        return Fail(error, "event \"" + item + "\" is not WHEN:BUTTON");
    }
    Press p;
    if (!When::Parse(f[0], p.start)) {
        return Fail(error, "bad time \"" + f[0] + "\" (milliseconds, or f<frame>)");
    }
    std::size_t hold_at = 2;
    if (f[1] == "TOUCH" || f[1] == "CIRCLE") {
        p.kind = f[1] == "TOUCH" ? PressKind::Touch : PressKind::Circle;
        if (f.size() < 4 || !ParseFloat(f[2], p.x) || !ParseFloat(f[3], p.y)) {
            return Fail(error, "\"" + item + "\" needs numeric x and y");
        }
        hold_at = 4;
    } else {
        int index = -1;
        for (int i = 0; i < kButtonCount; ++i) {
            if (f[1] == kButtonNames[i]) {
                index = i;
            }
        }
        if (index < 0) {
            return Fail(error, "unknown button \"" + f[1] + "\"");
        }
        p.button = index;
    }
    if (f.size() > hold_at + 1) {
        return Fail(error, "too many fields in \"" + item + "\"");
    }
    if (f.size() == hold_at + 1 && !Hold::Parse(f[hold_at], p.hold)) {
        return Fail(error, "bad hold \"" + f[hold_at] + "\" (milliseconds, or f<frames>)");
    }
    press = p;
    return true;
}

} // namespace

const char* ButtonName(int index) {
    return index >= 0 && index < kButtonCount ? kButtonNames[index] : "?";
}

bool When::Parse(const std::string& text, When& out) {
    When w;
    if (!text.empty() && text[0] == 'f') {
        w.by_frame = true;
        if (!ParseFrameCount(text.substr(1), w.frame)) {
            return false;
        }
    } else if (!ParseMs(text, w.ms)) {
        return false;
    }
    out = w;
    return true;
}

std::string When::ToString() const {
    return by_frame ? "f" + std::to_string(frame) : Number(ms) + "ms";
}

bool Hold::Parse(const std::string& text, Hold& out) {
    Hold h;
    if (!text.empty() && text[0] == 'f') {
        h.by_frame = true;
        if (!ParseFrameCount(text.substr(1), h.frames)) {
            return false;
        }
    } else if (!ParseMs(text, h.ms)) {
        return false;
    }
    out = h;
    return true;
}

std::string Hold::ToString() const {
    return by_frame ? "f" + std::to_string(frames) : Number(ms) + "ms";
}

bool Press::Active(double now_ms, std::uint32_t now_frame) {
    if (!start.Due(now_ms, now_frame)) {
        return false;
    }
    if (!started_) {
        started_ = true;
        started_ms_ = now_ms;
        started_frame_ = now_frame;
    }
    if (hold.by_frame) {
        const std::uint64_t from = start.by_frame ? start.frame : started_frame_;
        return now_frame < from + hold.frames;
    }
    const double from = start.by_frame ? started_ms_ : start.ms;
    return now_ms < from + hold.ms;
}

bool ParseInput(const std::string& script, Plan& plan, std::string& error) {
    std::vector<Press> parsed;
    for (const std::string& item : Split(script, ',')) {
        Press p;
        if (!ParseEvent(Trim(item), p, error)) {
            return false;
        }
        parsed.push_back(p);
    }
    plan.presses.insert(plan.presses.end(), parsed.begin(), parsed.end());
    return true;
}

bool ParseInputText(const std::string& text, Plan& plan, std::string& error) {
    std::vector<Press> parsed;
    int number = 0;
    for (std::string line : Split(text, '\n')) {
        ++number;
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        for (const std::string& item : Split(line, ',')) {
            Press p;
            if (!ParseEvent(Trim(item), p, error)) {
                error = "line " + std::to_string(number) + ": " + error;
                return false;
            }
            parsed.push_back(p);
        }
    }
    plan.presses.insert(plan.presses.end(), parsed.begin(), parsed.end());
    return true;
}

bool ParseInputFile(const std::string& path, Plan& plan, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Fail(error, "cannot read " + path);
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ParseInputText(text, plan, error);
}

bool ParseDump(const std::string& arg, Plan& plan, std::string& error) {
    const auto f = Split(arg, ':');
    Dump d;
    if (f.size() != 3 || !When::Parse(f[0], d.when) || !ParseHex32(f[1], d.addr) || !ParseHex32(f[2], d.size) ||
        d.size == 0) {
        return Fail(error, "want WHEN:ADDR:SIZE, hex address and size");
    }
    plan.dumps.push_back(d);
    return true;
}

bool ParseReplace(const std::string& arg, Plan& plan, std::string& error) {
    const auto f = Split(arg, ':');
    Replace r;
    if (f.size() != 3 || !When::Parse(f[0], r.when) || !ParseHexBytes(f[1], r.from) || !ParseHexBytes(f[2], r.to) ||
        r.from.size() != r.to.size()) {
        return Fail(error, "want WHEN:FROM:TO, hex bytes of equal length");
    }
    plan.replaces.push_back(std::move(r));
    return true;
}

bool ParseOptionChange(const std::string& arg, Plan& plan, std::string& error) {
    const auto colon = arg.find(':');
    const auto eq = colon == std::string::npos ? colon : arg.find('=', colon);
    OptionChange c;
    if (colon == std::string::npos || eq == std::string::npos || eq == colon + 1 ||
        !When::Parse(arg.substr(0, colon), c.when)) {
        return Fail(error, "want WHEN:KEY=VALUE");
    }
    c.key = arg.substr(colon + 1, eq - colon - 1);
    c.value = arg.substr(eq + 1);
    plan.option_changes.push_back(std::move(c));
    return true;
}

bool ParseFreeze(const std::string& arg, Plan& plan, std::string& error) {
    const auto f = Split(arg, ':');
    Freeze z;
    if (f.size() != 2 || !When::Parse(f[0], z.when) || !ParseMs(f[1], z.host_ms) || z.host_ms <= 0.0) {
        return Fail(error, "want WHEN:HOST_MS");
    }
    plan.freezes.push_back(z);
    return true;
}

bool ParseCallCounts(const std::string& arg, Plan& plan, std::string& error) {
    CallCounts c;
    if (!When::Parse(arg, c.when)) {
        return Fail(error, "want a time in milliseconds, or f<frame>");
    }
    plan.call_counts.push_back(c);
    return true;
}

bool ParseShotsFrom(const std::string& arg, Plan& plan, std::string& error) {
    When w;
    if (!When::Parse(arg, w)) {
        return Fail(error, "want a time in milliseconds, or f<frame>");
    }
    plan.has_shots_from = true;
    plan.shots_from = w;
    return true;
}

bool ParseShotEvery(const std::string& arg, Plan& plan, std::string& error) {
    When w;
    if (!When::Parse(arg, w) || (w.by_frame && w.frame == 0)) {
        return Fail(error, "want a number of milliseconds, or f<N> with N at least 1");
    }
    if (!w.by_frame && w.ms < 1.0) {
        w.ms = 1.0;
    }
    plan.has_shot_every = true;
    plan.shot_every = w;
    return true;
}

bool ParseStop(const std::string& arg, Plan& plan, std::string& error) {
    When w;
    if (!When::Parse(arg, w)) {
        return Fail(error, "want milliseconds, or f<frame>");
    }
    if (w.by_frame) {
        plan.has_stop_frame = true;
        plan.stop_frame = w.frame;
    } else {
        plan.has_stop_ms = true;
        plan.stop_ms = w.ms;
    }
    return true;
}

bool ParseFrames(const std::string& arg, Plan& plan, std::string& error) {
    std::uint32_t frames = 0;
    if (!ParseFrameCount(arg, frames)) {
        return Fail(error, "want a number of frames");
    }
    plan.has_stop_frame = true;
    plan.stop_frame = frames;
    return true;
}

std::string Describe(const Plan& plan) {
    std::ostringstream out;
    out << "# schedule v1\n";
    if (plan.has_stop_ms || !plan.has_stop_frame) {
        out << "stop ms=" << Number(plan.stop_ms) << "\n";
    }
    if (plan.has_stop_frame) {
        out << "stop frames=" << plan.stop_frame << "\n";
    }
    for (const Press& p : plan.presses) {
        out << "press at=" << p.start.ToString() << " hold=" << p.hold.ToString();
        switch (p.kind) {
        case PressKind::Button:
            out << " kind=button button=" << ButtonName(p.button);
            break;
        case PressKind::Touch:
            out << " kind=touch x=" << Number(p.x) << " y=" << Number(p.y);
            break;
        case PressKind::Circle:
            out << " kind=circle x=" << Number(p.x) << " y=" << Number(p.y);
            break;
        }
        out << "\n";
    }
    for (const Dump& d : plan.dumps) {
        out << "dump at=" << d.when.ToString() << " addr=" << Hex8(d.addr) << " size=" << Hex8(d.size) << "\n";
    }
    for (const Replace& r : plan.replaces) {
        out << "replace at=" << r.when.ToString() << " from=" << HexBytes(r.from) << " to=" << HexBytes(r.to) << "\n";
    }
    for (const OptionChange& c : plan.option_changes) {
        out << "set at=" << c.when.ToString() << " key=" << c.key << " value=" << c.value << "\n";
    }
    for (const Freeze& z : plan.freezes) {
        out << "freeze at=" << z.when.ToString() << " host=" << Number(z.host_ms) << "ms\n";
    }
    for (const CallCounts& c : plan.call_counts) {
        out << "call-counts at=" << c.when.ToString() << "\n";
    }
    if (plan.has_shots_from) {
        out << "shots-from at=" << plan.shots_from.ToString() << "\n";
    }
    if (plan.has_shot_every && plan.shot_every.by_frame) {
        out << "shot-every every=" << plan.shot_every.ToString()
            << " first=" << (plan.has_shots_from ? plan.shots_from.ToString() : plan.shot_every.ToString())
            << "\n";
    }
    return out.str();
}

namespace {

std::string MsStem(double ms) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%07.0f", ms);
    return buffer;
}

std::string FrameStem(std::uint64_t frame) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "f%07llu", static_cast<unsigned long long>(frame));
    return buffer;
}

} // namespace

ShotSchedule::ShotSchedule(const Plan& plan) {
    if (plan.has_shot_every && plan.shot_every.by_frame) {
        frame_interval_ = true;
        every_frames_ = plan.shot_every.frame ? plan.shot_every.frame : 1;
        if (plan.has_shots_from && !plan.shots_from.by_frame) {
            first_by_ms_ = true;
            first_ms_ = plan.shots_from.ms;
        } else {
            next_frame_ = plan.has_shots_from ? plan.shots_from.frame : every_frames_;
        }
    } else {
        every_ms_ = plan.has_shot_every ? plan.shot_every.ms : 1000.0;
        if (plan.has_shots_from && plan.shots_from.by_frame) {
            first_by_frame_ = true;
            first_frame_ = plan.shots_from.frame;
        }
        next_ms_ = plan.has_shots_from && !plan.shots_from.by_frame ? plan.shots_from.ms : every_ms_;
    }
}

bool ShotSchedule::Due(double now_ms, std::uint32_t now_frame, std::string& stem) {
    if (frame_interval_) {
        if (first_by_ms_) {
            if (now_ms < first_ms_) {
                return false;
            }
            first_by_ms_ = false;
            stem = FrameStem(now_frame);
            next_frame_ = static_cast<std::uint64_t>(now_frame) + every_frames_;
            return true;
        }
        if (now_frame < next_frame_) {
            return false;
        }
        stem = FrameStem(next_frame_);
        next_frame_ += every_frames_;
        while (next_frame_ <= now_frame) { // the counter skipped over it: no shot for that frame
            next_frame_ += every_frames_;
        }
        return true;
    }
    if (first_by_frame_) {
        if (now_frame < first_frame_) {
            return false;
        }
        first_by_frame_ = false;
        stem = FrameStem(first_frame_);
        next_ms_ = std::floor(now_ms) + every_ms_;
        return true;
    }
    if (now_ms < next_ms_) {
        return false;
    }
    stem = MsStem(next_ms_);
    next_ms_ += every_ms_;
    return true;
}

bool ShotSchedule::DrawingNeeded(double now_ms, double draw_window_ms) const {
    return frame_interval_ || first_by_frame_ || now_ms >= next_ms_ - draw_window_ms;
}

} // namespace Schedule
