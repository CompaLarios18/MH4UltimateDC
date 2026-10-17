// Derived from Azahar src/core/frontend/applets/swkbd.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the number of the OK button for each button layout (Single 0, Dual 1, Triple
// 2), which is the order of ButtonConfig there.
// Copyright 2018 Citra Emulator Project
//
// The app's software keyboard (ui/keyboard.cpp) in a headless Dear ImGui context, against a fake
// runtime and a fake SDL clock: what a request limits, which button finishes it, what the game's
// refusal does, and when a notice goes. Synthetic data only.
// Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand against doctest, {fmt},
// Dear ImGui's core files and the SDL2 headers (no SDL library: the clock is defined here).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include <SDL.h>
#include <imgui.h>

#include "ui/keyboard.h"
#include "util/tests/fake_runtime.h"

namespace {
Uint32 ticks = 1000;
} // namespace

extern "C" Uint32 SDL_GetTicks(void) {
    return ticks;
}

using namespace App;
using fake::Env;

namespace {

/// A Dear ImGui context with no renderer: frames can be built and their input fed.
struct Imgui {
    ImGuiContext* context;
    Imgui() {
        context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefaultVector();
    }
    ~Imgui() {
        ImGui::DestroyContext(context);
    }
    /// One frame of the keyboard's window (and the notice when no keyboard is up).
    void Frame(Ui::Keyboard& keyboard) {
        ImGui::NewFrame();
        if (keyboard.Active()) {
            keyboard.Draw();
        } else if (keyboard.HasNotice()) {
            keyboard.DrawNotice();
        }
        ImGui::Render();
    }
    /// Frames until the text field has the focus (the first frame asks for it).
    void Settle(Ui::Keyboard& keyboard) {
        for (int i = 0; i < 3; ++i) {
            Frame(keyboard);
        }
    }
    /// Typed a character a frame, as a person does (the game's limits are checked against the
    /// text as the last frame left it).
    void Type(Ui::Keyboard& keyboard, const std::string& utf8) {
        for (std::size_t i = 0; i < utf8.size();) {
            std::size_t length = 1;
            while (i + length < utf8.size() && (static_cast<unsigned char>(utf8[i + length]) & 0xC0) == 0x80) {
                ++length;
            }
            ImGui::GetIO().AddInputCharactersUTF8(utf8.substr(i, length).c_str());
            Frame(keyboard);
            Frame(keyboard);
            i += length;
        }
    }
    void Enter(Ui::Keyboard& keyboard) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Enter, true);
        Frame(keyboard);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        Frame(keyboard);
    }
};

KeyboardConfig Request(KeyboardButtons buttons, u16 length) {
    KeyboardConfig config;
    config.button_config = buttons;
    config.max_text_length = length;
    config.hint_text = "Name";
    return config;
}

} // namespace

TEST_CASE("a name is typed within the game's limit and handed over with the OK button") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    keyboard.Execute(Request(KeyboardButtons::Single, 4));
    CHECK(keyboard.Active());
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "Hunter");  // two characters too many
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 1);
    CHECK(env.runtime.finished[0].text == "Hunt");
    CHECK(env.runtime.finished[0].button == 0);
    CHECK_FALSE(keyboard.Active());
}

TEST_CASE("Enter is the rightmost button: its number follows the layout") {
    for (const auto [buttons, number] : {std::pair{KeyboardButtons::Single, 0}, std::pair{KeyboardButtons::Dual, 1},
                                         std::pair{KeyboardButtons::Triple, 2}}) {
        Env env;
        Imgui imgui;
        Ui::Keyboard keyboard;
        keyboard.Execute(Request(buttons, 8));
        imgui.Settle(keyboard);
        imgui.Type(keyboard, "abc");
        imgui.Enter(keyboard);
        REQUIRE(env.runtime.finished.size() == 1);
        CHECK(env.runtime.finished[0].button == number);
    }
}

TEST_CASE("the game turning the text down keeps the keyboard up with the text") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    keyboard.Execute(Request(KeyboardButtons::Dual, 8));
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "abc");
    env.runtime.finish_result = KeyboardError::FixedLengthRequired;
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 1);
    CHECK(keyboard.Active());
    // the game asks again after saying why: the player goes on from the same text
    env.runtime.finish_result = KeyboardError::None;
    keyboard.ShowError("longer, please");
    keyboard.Execute(Request(KeyboardButtons::Dual, 8));
    imgui.Settle(keyboard);
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 2);
    CHECK(env.runtime.finished[1].text == "abc");
    CHECK(env.runtime.finished[1].button == 1);
    CHECK_FALSE(keyboard.Active());
}

TEST_CASE("characters the game filters out are not typed") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    KeyboardConfig config = Request(KeyboardButtons::Single, 20);
    config.filters.prevent_at = true;
    config.filters.prevent_percent = true;
    config.filters.prevent_backslash = true;
    keyboard.Execute(config);
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "a@b%c\\d");
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 1);
    CHECK(env.runtime.finished[0].text == "abcd");
}

TEST_CASE("digits stop at the game's limit when it asks") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    KeyboardConfig config = Request(KeyboardButtons::Single, 20);
    config.filters.prevent_digit = true;
    config.max_digits = 2;
    keyboard.Execute(config);
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "1a2b3c");
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 1);
    CHECK(env.runtime.finished[0].text == "1a2bc");
}

TEST_CASE("the length is the 3DS's: UTF-16 units") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    keyboard.Execute(Request(KeyboardButtons::Single, 2));
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "\xc3\xa9\xc3\xa8\xc3\xa0");  // three letters of one unit each
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 1);
    CHECK(env.runtime.finished[0].text == "\xc3\xa9\xc3\xa8");
}

TEST_CASE("a new request keeps the text only after a refusal") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    keyboard.Execute(Request(KeyboardButtons::Single, 8));
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "abc");
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 1);
    CHECK(env.runtime.finished[0].text == "abc");
    // asked again with nothing said: a fresh field
    keyboard.Execute(Request(KeyboardButtons::Single, 8));
    imgui.Settle(keyboard);
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 2);
    CHECK(env.runtime.finished[1].text == "");

    // asked again after the game said why it refused: the text stays, cut to the new limit
    keyboard.Execute(Request(KeyboardButtons::Single, 8));
    imgui.Settle(keyboard);
    imgui.Type(keyboard, "abcdef");
    env.runtime.finish_result = KeyboardError::MaxLengthExceeded;
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 3);
    CHECK(keyboard.Active());
    env.runtime.finish_result = KeyboardError::None;
    keyboard.ShowError("too long");
    keyboard.Execute(Request(KeyboardButtons::Single, 4));
    imgui.Settle(keyboard);
    imgui.Enter(keyboard);
    REQUIRE(env.runtime.finished.size() == 4);
    CHECK(env.runtime.finished[3].text == "abcd");
}

TEST_CASE("an error with no keyboard up is a notice that goes after five seconds") {
    Env env;
    Imgui imgui;
    Ui::Keyboard keyboard;
    ticks = 1000;
    CHECK_FALSE(keyboard.HasNotice());
    keyboard.ShowError("Not allowed");
    CHECK(keyboard.HasNotice());
    CHECK_FALSE(keyboard.Active());
    imgui.Frame(keyboard);
    ticks = 5999;
    CHECK(keyboard.HasNotice());
    ticks = 6000;
    CHECK_FALSE(keyboard.HasNotice());
    // no reason given: the notice has words all the same
    ticks = 10000;
    keyboard.ShowError("");
    CHECK(keyboard.HasNotice());
}
