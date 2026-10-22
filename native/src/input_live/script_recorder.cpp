#include "input_live/script_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mh4n::input_live {

namespace {

// The button names of port/src/schedule.cpp and the pad bit each is, in the order of the
// script_input.cpp table. (input_live_test parses every name back with the real parser.)
struct ButtonName {
    u32 bit;
    const char* name;
};
constexpr ButtonName kButtonNames[12] = {
    {kPadA, "A"},       {kPadB, "B"},       {kPadX, "X"},         {kPadY, "Y"},
    {kPadUp, "UP"},     {kPadDown, "DOWN"}, {kPadLeft, "LEFT"},  {kPadRight, "RIGHT"},
    {kPadL, "L"},       {kPadR, "R"},       {kPadStart, "START"}, {kPadSelect, "SELECT"},
};

// script_input.cpp's constants: the circle pad's limit in raw units and the bottom screen's size.
constexpr float kCirclePadLimit = 154.0f;  // 0x9A

// The shortest plain decimal (no exponent) that reads back as the same float.
std::string FormatFloat(float value) {
    char text[64];
    for (int decimals = 0; decimals <= 9; ++decimals) {
        std::snprintf(text, sizeof text, "%.*f", decimals, static_cast<double>(value));
        if (static_cast<float>(std::strtod(text, nullptr)) == value) {
            return text;
        }
    }
    std::snprintf(text, sizeof text, "%.9g", static_cast<double>(value));
    return text;
}

}  // namespace

std::string FormatMs(double ms) {
    char text[64];
    for (int decimals = 0; decimals <= 9; ++decimals) {
        std::snprintf(text, sizeof text, "%.*f", decimals, ms);
        if (std::strtod(text, nullptr) == ms) {
            return text;
        }
    }
    std::snprintf(text, sizeof text, "%.17g", ms);
    return text;
}

std::string FormatTouchAxis(int pixel, int size) {
    // script_input.cpp: x = press.x / size; pixel = u16(x * size), all in float.
    for (const float fraction : {0.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.375f, 0.625f, 0.875f}) {
        const float written = static_cast<float>(pixel) + fraction;
        const float parsed = static_cast<float>(std::strtod(FormatFloat(written).c_str(), nullptr));
        const float unit = parsed / static_cast<float>(size);
        if (unit * static_cast<float>(size) >= 65535.0f) {
            continue;  // out of the range of a pixel; the plain value below
        }
        if (static_cast<int>(static_cast<u16>(unit * static_cast<float>(size))) == pixel) {
            return FormatFloat(written);
        }
    }
    return FormatFloat(static_cast<float>(pixel));
}

std::string FormatCircleAxis(int raw) {
    // script_input.cpp: raw = s16(roundf(x * 154)).
    float value = static_cast<float>(raw) / kCirclePadLimit;
    for (int tries = 0; tries < 8; ++tries) {
        const float parsed = static_cast<float>(std::strtod(FormatFloat(value).c_str(), nullptr));
        if (static_cast<int>(std::roundf(parsed * kCirclePadLimit)) == raw) {
            break;
        }
        value = std::nextafter(value, raw >= 0 ? 1e9f : -1e9f);
    }
    return FormatFloat(value);
}

void ScriptRecorder::Close(Open& press, const std::string& head, const Stamp& at) {
    Line line;
    line.start = press.start;
    line.seq = press.seq;
    if (press.start.have_frame) {
        // At least one frame: a press the game saw is never written as nothing.
        const u32 held = std::max<u32>(at.frame - press.start.frame, 1);
        line.text = "f" + std::to_string(press.start.frame) + ":" + head;
        line.hold = "f" + std::to_string(held);
    } else {
        // The script holds the press while now < start + hold, in doubles: keep the sum from
        // rounding past the moment it ended.
        double held = std::max(at.ms - press.start.ms, 0.001);
        while (press.start.ms + held > at.ms && held > 0.001) {
            held = std::nextafter(held, 0.0);
        }
        line.text = FormatMs(press.start.ms) + ":" + head;
        line.hold = FormatMs(held);
    }
    lines_.push_back(std::move(line));
    press.active = false;
}

void ScriptRecorder::Update(const InputState& state, const Stamp& at) {
    for (int i = 0; i < kButtons; ++i) {
        const bool held = (state.buttons & kButtonNames[i].bit) != 0;
        Open& press = buttons_[i];
        if (held && !press.active) {
            press.active = true;
            press.start = at;
            press.seq = next_seq_++;
        } else if (!held && press.active) {
            Close(press, kButtonNames[i].name, at);
        }
    }

    // The circle pad: a new reading is a new press.
    const bool circle = state.circle_x != 0 || state.circle_y != 0;
    if (circle_.active && (!circle || circle_.x != state.circle_x || circle_.y != state.circle_y)) {
        Close(circle_, "CIRCLE:" + FormatCircleAxis(circle_.x) + ":" + FormatCircleAxis(circle_.y), at);
    }
    if (circle && !circle_.active) {
        circle_.active = true;
        circle_.start = at;
        circle_.seq = next_seq_++;
        circle_.x = state.circle_x;
        circle_.y = state.circle_y;
    }

    // The touch screen: the same, one press per position.
    if (touch_.active && (!state.touch_down || touch_.x != state.touch_x || touch_.y != state.touch_y)) {
        Close(touch_, "TOUCH:" + FormatTouchAxis(touch_.x, 320) + ":" + FormatTouchAxis(touch_.y, 240), at);
    }
    if (state.touch_down && !touch_.active) {
        touch_.active = true;
        touch_.start = at;
        touch_.seq = next_seq_++;
        touch_.x = state.touch_x;
        touch_.y = state.touch_y;
    }
}

std::string ScriptRecorder::Finish(const Stamp& at) {
    // What is still held was held through the last frame seen (and the samples after it): it ends
    // with the next one. In milliseconds it ends at `at`.
    Stamp end = at;
    end.frame += at.have_frame ? 1 : 0;
    Update(InputState{}, end);
    // In the order the presses started: the milliseconds ones came before the first frame.
    std::sort(lines_.begin(), lines_.end(), [](const Line& a, const Line& b) { return a.seq < b.seq; });
    std::string text =
        "# mh4u-native --record-input: the inputs the game saw, one press per line.\n"
        "# WHEN:BUTTON:HOLD, WHEN:TOUCH:x:y:HOLD, WHEN:CIRCLE:x:y:HOLD; fN is a game frame.\n";
    for (const Line& line : lines_) {
        text += line.text + ":" + line.hold + "\n";
    }
    return text;
}

}  // namespace mh4n::input_live
