// Derived from Azahar src/core/frontend/applets/swkbd.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later): the software keyboard's request (KeyboardConfig and the modes, button
// layouts and filters in it, with the default button texts) and the reasons the game turns a text
// down (ValidationError), as the app's own types.
// Copyright 2018 Citra Emulator Project
//
// What the 3DS's software keyboard applet asks of the app and what it answers. The runtime
// converts its applet's types to these (runtime_azahar/azahar_keyboard.cpp).
#pragma once

#include <string>
#include <vector>

#include "util/types.h"

namespace App {

/// What kinds of text the game accepts.
enum class KeyboardAccept {
    Anything,
    NotEmpty,
    NotEmptyAndNotBlank,  ///< neither empty nor only spaces
    NotBlank,             ///< empty is fine, only spaces is not
    FixedLength,          ///< exactly max_text_length characters
};

/// The buttons the game shows. The value of each is the number of the rightmost button (the OK
/// one): Single 0, Dual 1, Triple 2; the buttons are numbered from the left.
enum class KeyboardButtons {
    Single,  ///< OK
    Dual,    ///< Cancel, OK
    Triple,  ///< Cancel, I Forgot, OK
    None,
};

/// The default text of the buttons, in English.
constexpr char KEYBOARD_BUTTON_OKAY[] = "Ok";
constexpr char KEYBOARD_BUTTON_CANCEL[] = "Cancel";
constexpr char KEYBOARD_BUTTON_FORGOT[] = "I Forgot";

/// What the game asks the keyboard for.
struct KeyboardConfig {
    KeyboardButtons button_config = KeyboardButtons::Single;
    KeyboardAccept accept_mode = KeyboardAccept::Anything;
    bool multiline_mode = false;
    u16 max_text_length = 0;  ///< in UTF-16 units, the 3DS's measure
    u16 max_digits = 0;
    std::string hint_text;           ///< shown in the field before the player types
    std::vector<std::string> button_text;  ///< the game's own button texts, by button; may be short or empty
    struct Filters {
        bool prevent_digit = false;      ///< no more than max_digits digits
        bool prevent_at = false;
        bool prevent_percent = false;
        bool prevent_backslash = false;
        bool prevent_profanity = false;
        bool enable_callback = false;
    };
    Filters filters;
};

/// Why the game didn't take a text (None: it did).
enum class KeyboardError {
    None,
    ButtonOutOfRange,
    MaxDigitsExceeded,
    AtSignNotAllowed,
    PercentNotAllowed,
    BackslashNotAllowed,
    ProfanityNotAllowed,
    CallbackFailed,
    FixedLengthRequired,
    MaxLengthExceeded,
    BlankInputNotAllowed,
    EmptyInputNotAllowed,
};

} // namespace App
