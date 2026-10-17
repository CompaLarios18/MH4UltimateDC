// AzaharRuntime: the 3DS software keyboard. Azahar's applet types become the app's
// (util/swkbd.h) and back, one case at a time, so a value added on either side shows up as a
// compiler warning here.
#include "runtime_azahar/azahar_keyboard.h"

namespace App::Azahar {

// the button layouts keep Azahar's numbers: the OK button's number is the layout's value
// (ui/keyboard.cpp), and the applet validates the button against its own
static_assert(static_cast<int>(KeyboardButtons::Single) == static_cast<int>(Frontend::ButtonConfig::Single) &&
              static_cast<int>(KeyboardButtons::Dual) == static_cast<int>(Frontend::ButtonConfig::Dual) &&
              static_cast<int>(KeyboardButtons::Triple) == static_cast<int>(Frontend::ButtonConfig::Triple) &&
              static_cast<int>(KeyboardButtons::None) == static_cast<int>(Frontend::ButtonConfig::None));

namespace {

KeyboardAccept ConvertAccept(Frontend::AcceptedInput mode) {
    switch (mode) {
    case Frontend::AcceptedInput::Anything:
        return KeyboardAccept::Anything;
    case Frontend::AcceptedInput::NotEmpty:
        return KeyboardAccept::NotEmpty;
    case Frontend::AcceptedInput::NotEmptyAndNotBlank:
        return KeyboardAccept::NotEmptyAndNotBlank;
    case Frontend::AcceptedInput::NotBlank:
        return KeyboardAccept::NotBlank;
    case Frontend::AcceptedInput::FixedLength:
        return KeyboardAccept::FixedLength;
    }
    return KeyboardAccept::Anything;
}

KeyboardButtons ConvertButtons(Frontend::ButtonConfig buttons) {
    switch (buttons) {
    case Frontend::ButtonConfig::Single:
        return KeyboardButtons::Single;
    case Frontend::ButtonConfig::Dual:
        return KeyboardButtons::Dual;
    case Frontend::ButtonConfig::Triple:
        return KeyboardButtons::Triple;
    case Frontend::ButtonConfig::None:
        return KeyboardButtons::None;
    }
    return KeyboardButtons::None;
}

KeyboardError ConvertError(Frontend::ValidationError error) {
    switch (error) {
    case Frontend::ValidationError::None:
        return KeyboardError::None;
    case Frontend::ValidationError::ButtonOutOfRange:
        return KeyboardError::ButtonOutOfRange;
    case Frontend::ValidationError::MaxDigitsExceeded:
        return KeyboardError::MaxDigitsExceeded;
    case Frontend::ValidationError::AtSignNotAllowed:
        return KeyboardError::AtSignNotAllowed;
    case Frontend::ValidationError::PercentNotAllowed:
        return KeyboardError::PercentNotAllowed;
    case Frontend::ValidationError::BackslashNotAllowed:
        return KeyboardError::BackslashNotAllowed;
    case Frontend::ValidationError::ProfanityNotAllowed:
        return KeyboardError::ProfanityNotAllowed;
    case Frontend::ValidationError::CallbackFailed:
        return KeyboardError::CallbackFailed;
    case Frontend::ValidationError::FixedLengthRequired:
        return KeyboardError::FixedLengthRequired;
    case Frontend::ValidationError::MaxLengthExceeded:
        return KeyboardError::MaxLengthExceeded;
    case Frontend::ValidationError::BlankInputNotAllowed:
        return KeyboardError::BlankInputNotAllowed;
    case Frontend::ValidationError::EmptyInputNotAllowed:
        return KeyboardError::EmptyInputNotAllowed;
    }
    return KeyboardError::CallbackFailed;
}

KeyboardConfig ConvertConfig(const Frontend::KeyboardConfig& from) {
    KeyboardConfig to;
    to.button_config = ConvertButtons(from.button_config);
    to.accept_mode = ConvertAccept(from.accept_mode);
    to.multiline_mode = from.multiline_mode;
    to.max_text_length = from.max_text_length;
    to.max_digits = from.max_digits;
    to.hint_text = from.hint_text;
    to.button_text = from.button_text;
    to.filters.prevent_digit = from.filters.prevent_digit;
    to.filters.prevent_at = from.filters.prevent_at;
    to.filters.prevent_percent = from.filters.prevent_percent;
    to.filters.prevent_backslash = from.filters.prevent_backslash;
    to.filters.prevent_profanity = from.filters.prevent_profanity;
    to.filters.enable_callback = from.filters.enable_callback;
    return to;
}

} // namespace

void AzaharKeyboard::Execute(const Frontend::KeyboardConfig& config_) {
    // (the base keeps the request: Finalize validates against it)
    SoftwareKeyboard::Execute(config_);
    target->Execute(ConvertConfig(config_));
}

void AzaharKeyboard::ShowError(const std::string& error) {
    target->ShowError(error);
}

KeyboardError AzaharKeyboard::Finish(const std::string& text, std::uint8_t button) {
    return ConvertError(Finalize(text, button));
}

} // namespace App::Azahar
