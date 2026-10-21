// Derived from Azahar src/core/hle/applets/swkbd.h, src/core/hle/applets/swkbd.cpp,
// src/core/frontend/applets/swkbd.h and src/core/frontend/applets/swkbd.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any
// later version"): the layout of the software keyboard's configuration (0x400 bytes) and its
// enumerations, the applet's behaviour (the Request and Message signals, Start, Update, Finalize,
// the return codes), and the default keyboard (the frontend: the validation rules, the text and the
// button it answers with).
// Copyright 2015 Citra Emulator Project (hle/applets/swkbd.h, hle/applets/swkbd.cpp)
// Copyright 2018 Citra Emulator Project (frontend/applets/swkbd.h, frontend/applets/swkbd.cpp)
//
// The software keyboard applet (N1-14), HLE as in Azahar and answering as Azahar's default keyboard
// does: nobody types. The game starts it with PrepareToStartLibraryApplet (which makes the applet),
// a Request parameter (the applet answers with a shared memory block of the framebuffer's size),
// and StartLibraryApplet, whose parameter holds the configuration and whose handle is the shared
// memory block the text goes to. At the next update tick (4,468,352 ticks after the applet was made
// or, if that has passed, after the last tick) the applet writes the text (UTF-16, a null
// terminator after it, at the start of the block) and the return code into the configuration and
// sends it back with the WakeupByExit signal. With the Callback filter it sends the configuration
// as a Message parameter first and waits for the game's verdict.
//
// The text is the user name of the cfg service (Azahar: `cfg->GetUsername()`) or, for the Nth
// keyboard opened, the Nth `--swkbd-text`. The button is 0, 1 or 2 for one, two or three buttons.
//
// Departures from Azahar, all so that a bad input does not crash or overflow:
//  - The text is cut to the configuration's maximum length before the default keyboard checks it
//    (the spec of N1-14); Azahar rejects a longer text, which leaves the keyboard open for ever. A text
//    that the other rules reject (empty, blank, a digit count, '@', '%', '\', a fixed length that it
//    does not have) is still rejected as in Azahar: the keyboard never finishes, and an error is
//    logged.
//  - A text that does not fit in the shared memory block the game gave (Azahar copies it past the
//    end) is cut to fit, with a warning, and a game that gave no block gets none written.
//  - A parameter of the wrong size, a button configuration or an input rule that is not one of the
//    known ones, a framebuffer block that cannot be made: Azahar asserts; here it is logged and the
//    applet does not start (or, for the block, the Response carries no object).
//
// Threads (principle 6): scheduler thread only.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "applets/applet.h"
#include "mh4n/service.h"

namespace mh4n::apt {

// ---- the configuration (hle/applets/swkbd.h) ---------------------------------------------------------------

inline constexpr int kMaxButton = 3;                // buttons in the keyboard
inline constexpr int kMaxButtonTextLen = 16;        // button text, UTF-16 units
inline constexpr int kMaxHintTextLen = 64;          // hint text, UTF-16 units
inline constexpr int kMaxCallbackMsgLen = 256;      // filter callback error message, UTF-16 units

// Keyboard dialog buttons (SoftwareKeyboardButtonConfig).
enum class SoftwareKeyboardButtonConfig : u32 {
    SingleButton,  // Ok button
    DualButton,    // Cancel | Ok buttons
    TripleButton,  // Cancel | I Forgot | Ok buttons
    NoButton,      // no button (returned by swkbdInputText in special cases)
};

// Accepted input types (SoftwareKeyboardValidInput).
enum class SoftwareKeyboardValidInput : u32 {
    Anything,          // all inputs are accepted
    NotEmpty,          // empty inputs are not accepted
    NotEmptyNotBlank,  // empty or blank inputs (only whitespace) are not accepted
    NotBlank,          // blank inputs are not accepted, empty ones are
    FixedLen,          // the input must have the length given by max_text_length
};

// Input filtering flags (SoftwareKeyboardFilter).
enum SoftwareKeyboardFilter : u32 {
    kFilterDigits = 1,         // disallow more than max_digits digits
    kFilterAt = 1 << 1,        // disallow '@'
    kFilterPercent = 1 << 2,   // disallow '%'
    kFilterBackslash = 1 << 3, // disallow '\'
    kFilterProfanity = 1 << 4, // disallow profanity (Azahar does not check it)
    kFilterCallback = 1 << 5,  // use a callback to check the input
};

// Keyboard filter callback return values (SoftwareKeyboardCallbackResult).
enum class SoftwareKeyboardCallbackResult : u32 {
    OK,        // the input is valid
    Close,     // displays an error message, then closes the keyboard
    Continue,  // displays an error message and continues displaying the keyboard
};

// Keyboard return values (SoftwareKeyboardResult).
enum class SoftwareKeyboardResult : s32 {
    None = -1,
    InvalidInput = -2,
    OutOfMem = -3,
    D0Click = 0,  // the button was clicked in 1-button dialogs
    D1Click0,     // the left button was clicked in 2-button dialogs
    D1Click1,     // the right button was clicked in 2-button dialogs
    D2Click0,     // the left button was clicked in 3-button dialogs
    D2Click1,     // the middle button was clicked in 3-button dialogs
    D2Click2,     // the right button was clicked in 3-button dialogs
    HomePressed = 10,
    ResetPressed,
    PowerPressed,
    ParentalOK = 20,
    ParentalFail,
    BannedInput = 30,  // the filter callback returned Close
};

// The parameter of StartLibraryApplet and the Message parameters: 0x400 bytes, little-endian. The
// enumerations are kept as their u32/s32 values, since the game may send any number.
struct SoftwareKeyboardConfig {
    u32 type;
    u32 num_buttons_m1;  // SoftwareKeyboardButtonConfig
    u32 valid_input;     // SoftwareKeyboardValidInput
    u32 password_mode;
    s32 is_parental_screen;
    s32 darken_top_screen;
    u32 filter_flags;
    u32 save_state_flags;
    u16 max_text_length;
    u16 dict_word_count;
    u16 max_digits;
    u16 button_text[kMaxButton][kMaxButtonTextLen + 1];
    u16 numpad_keys[2];
    u16 hint_text[kMaxHintTextLen + 1];  // text to display when asking the user for input
    u8 predictive_input;
    u8 multiline;
    u8 fixed_width;
    u8 allow_home;
    u8 allow_reset;
    u8 allow_power;
    u8 unknown;
    u8 default_qwerty;
    u8 button_submits_text[4];
    u16 language;

    u32 initial_text_offset;  // offset of the default text in the output shared memory
    u32 dict_offset;
    u32 initial_status_offset;
    u32 initial_learning_offset;
    u32 shared_memory_size;  // size of the shared memory
    u32 version;

    s32 return_code;  // SoftwareKeyboardResult

    u32 status_offset;
    u32 learning_offset;

    u32 text_offset;  // offset in the shared memory where the output text starts
    u16 text_length;  // length in characters of the output text

    u32 callback_result;  // SoftwareKeyboardCallbackResult
    u16 callback_msg[kMaxCallbackMsgLen + 1];
    u8 skip_at_check;
    u8 padding[0xAB];
};
// "The size of this structure (0x400) has been verified via reverse engineering of multiple games
// that use the software keyboard." (swkbd.h)
static_assert(sizeof(SoftwareKeyboardConfig) == 0x400, "Software Keyboard Config size is wrong");
static_assert(offsetof(SoftwareKeyboardConfig, max_text_length) == 0x20);
static_assert(offsetof(SoftwareKeyboardConfig, button_text) == 0x26);
static_assert(offsetof(SoftwareKeyboardConfig, hint_text) == 0x90);
static_assert(offsetof(SoftwareKeyboardConfig, shared_memory_size) == 0x130);
static_assert(offsetof(SoftwareKeyboardConfig, return_code) == 0x138);
static_assert(offsetof(SoftwareKeyboardConfig, text_offset) == 0x144);
static_assert(offsetof(SoftwareKeyboardConfig, text_length) == 0x148);
static_assert(offsetof(SoftwareKeyboardConfig, callback_result) == 0x14C);
static_assert(offsetof(SoftwareKeyboardConfig, callback_msg) == 0x150);
static_assert(offsetof(SoftwareKeyboardConfig, skip_at_check) == 0x352);

// ---- the default keyboard (frontend/applets/swkbd.h) ------------------------------------------------------

// What the frontend needs of the configuration (Frontend::KeyboardConfig); the texts of the buttons
// and the hint are for a screen, which the default keyboard does not have.
struct KeyboardConfig {
    // Frontend::ButtonConfig: 0 single, 1 dual, 2 triple, 3 none.
    u32 button_config = 0;
    // Frontend::AcceptedInput: 0 anything, 1 not empty, 2 not empty and not blank, 3 not blank,
    // 4 fixed length.
    u32 accept_mode = 0;
    bool multiline_mode = false;
    u16 max_text_length = 0;
    u16 max_digits = 0;
    bool prevent_digit = false;
    bool prevent_at = false;
    bool prevent_percent = false;
    bool prevent_backslash = false;
    bool prevent_profanity = false;
    bool enable_callback = false;
};

struct KeyboardData {
    std::u16string text;
    u8 button = 0;
};

enum class ValidationError {
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
    // Not Azahar's (it asserts): a configuration value that is none of the known ones.
    UnknownConfiguration,
};

// Frontend::KeyboardConfig of `config`.
KeyboardConfig ToFrontendConfig(const SoftwareKeyboardConfig& config);

// Frontend::SoftwareKeyboard's checks and DefaultKeyboard's answer.
class DefaultKeyboard {
public:
    // `text` is what the keyboard answers with (in Azahar always the user name).
    explicit DefaultKeyboard(ServiceContext& context) : context_(context) {}

    // Makes the keyboard answer the Nth `--swkbd-text` if there is one, else with the user name.
    // Called once for each keyboard the game opens, before Execute.
    void Open();
    // Executes the keyboard with `config`: answers at once with the text and the button of the
    // default keyboard, if the checks pass (DataReady() says whether they did).
    void Execute(const KeyboardConfig& config);
    bool DataReady() const { return data_ready_; }
    // The answer, and clears the ready state.
    const KeyboardData& ReceiveData() {
        data_ready_ = false;
        return data_;
    }
    // Shows the callback's error text (logged).
    void ShowError(const std::u16string& error);

    // The validation of Frontend::SoftwareKeyboard, on UTF-16 text (the classes of characters it
    // looks at are ASCII ones, which are the same in UTF-8).
    ValidationError ValidateFilters(const std::u16string& input) const;
    ValidationError ValidateInput(const std::u16string& input) const;
    ValidationError ValidateButton(u8 button) const;
    // All the checks (only when the button is the "OK" one, as Azahar's Finalize); on success the data
    // is stored and ready.
    ValidationError Finalize(const std::u16string& text, u8 button);

    // The text the current keyboard answers with, before it is cut.
    const std::u16string& Text() const { return text_; }

private:
    ServiceContext& context_;
    KeyboardConfig config_;
    KeyboardData data_;
    bool data_ready_ = false;
    std::u16string text_;
};

// UTF-8 to UTF-16 (invalid sequences become U+FFFD) and back (unpaired surrogates become U+FFFD).
std::u16string Utf8ToUtf16(const std::string& text);
std::string Utf16ToUtf8(const std::u16string& text);

// ---- the applet (hle/applets/swkbd.h) -------------------------------------------------------------------------

class SoftwareKeyboard final : public Applet {
public:
    SoftwareKeyboard(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host,
                     ServiceContext& context);

    Result ReceiveParameterImpl(const MessageParameter& parameter) override;
    Result Start(const MessageParameter& parameter) override;
    Result Finalize() override;
    void Update() override;

    // The configuration the applet holds (tests).
    const SoftwareKeyboardConfig& Config() const { return config_; }

private:
    ServiceContext& context_;
    // Made when the Request message arrives: it holds the framebuffer info the game gets with
    // ImportDisplayCaptureInfo.
    std::shared_ptr<SharedMemory> framebuffer_memory_;
    // Where the output text goes (the object of the Wakeup parameter).
    std::shared_ptr<SharedMemory> text_memory_;
    SoftwareKeyboardConfig config_{};
    DefaultKeyboard keyboard_;
};

}  // namespace mh4n::apt
