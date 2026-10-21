// Derived from Azahar src/core/hle/applets/swkbd.cpp, src/core/frontend/applets/swkbd.cpp and
// src/core/frontend/applets/swkbd.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later:
// each of them says "GPLv2 or any later version"): the applet's handling of the Request and Message
// signals, Start, Update and Finalize, and the default keyboard's validation and answer (see the
// header next to this file).
// Copyright 2015 Citra Emulator Project (hle/applets/swkbd.cpp)
// Copyright 2018 Citra Emulator Project (frontend/applets/swkbd.cpp, frontend/applets/swkbd.h)
#include "applets/swkbd.h"

#include <algorithm>
#include <cstring>

#include "mh4n/cfg.h"
#include "mh4n/log.h"
#include "mh4n/options.h"

namespace mh4n::apt {

// The texts of the keyboards, in the order the game opens them. The replay tool takes the same option
// (the replay of a session that opens the keyboard needs the same text as the run it replays).
MH4N_OPTION("runner", "--swkbd-text", "TEXT",
            "the text the software keyboard answers with (repeatable: the Nth keyboard the game opens "
            "gets the Nth; without one it answers with the user name of the cfg service)");
MH4N_OPTION("ipc-replay", "--swkbd-text", "TEXT",
            "the text the software keyboard answers with (repeatable: the Nth keyboard the game opens "
            "gets the Nth; without one it answers with the user name of the cfg service)");

namespace {

constexpr const char* kLog = "Applet.SWKBD";

// How many keyboards the game has opened (all the applets of a run share it).
struct KeyboardCounter {
    u32 opened = 0;
};

// The 3DS's own ASCII classes (std::isspace and std::isdigit of the "C" locale, which is what Azahar's
// frontend sees: it calls them on the bytes of the UTF-8 text, and a byte above 0x7F is none of them).
bool IsSpace(char16_t c) {
    return c == u' ' || (c >= u'\t' && c <= u'\r');
}
bool IsDigit(char16_t c) {
    return c >= u'0' && c <= u'9';
}

// The units of a null-terminated buffer.
std::u16string BufferToString(const u16* data, std::size_t capacity) {
    std::u16string out;
    for (std::size_t i = 0; i < capacity && data[i] != 0; ++i) {
        out.push_back(static_cast<char16_t>(data[i]));
    }
    return out;
}

}  // namespace

// ---- UTF conversions ---------------------------------------------------------------------------------------------

std::u16string Utf8ToUtf16(const std::string& text) {
    std::u16string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const u8 lead = static_cast<u8>(text[i]);
        u32 code = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            code = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07u;
        } else {
            out.push_back(u'�');
            ++i;
            continue;
        }
        bool valid = i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const u8 next = static_cast<u8>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
            } else {
                code = (code << 6) | (next & 0x3Fu);
            }
        }
        // Overlong forms, surrogates and values past U+10FFFF are not text.
        if (valid && ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
                      (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)) {
            valid = false;
        }
        if (!valid) {
            out.push_back(u'�');
            ++i;
            continue;
        }
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

std::string Utf16ToUtf8(const std::u16string& text) {
    std::string out;
    const auto put = [&out](u32 code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const u32 unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
            text[i + 1] <= 0xDFFF) {
            put(0x10000 + ((unit - 0xD800) << 10) + (text[i + 1] - 0xDC00));
            ++i;
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            put(0xFFFD);
        } else {
            put(unit);
        }
    }
    return out;
}

// ---- the default keyboard -------------------------------------------------------------------------------------

KeyboardConfig ToFrontendConfig(const SoftwareKeyboardConfig& config) {
    KeyboardConfig out;
    out.button_config = config.num_buttons_m1;
    out.accept_mode = config.valid_input;
    out.multiline_mode = config.multiline != 0;
    out.max_text_length = config.max_text_length;
    out.max_digits = config.max_digits;
    out.prevent_digit = (config.filter_flags & kFilterDigits) != 0;
    out.prevent_at = (config.filter_flags & kFilterAt) != 0;
    out.prevent_percent = (config.filter_flags & kFilterPercent) != 0;
    out.prevent_backslash = (config.filter_flags & kFilterBackslash) != 0;
    out.prevent_profanity = (config.filter_flags & kFilterProfanity) != 0;
    out.enable_callback = (config.filter_flags & kFilterCallback) != 0;
    return out;
}

ValidationError DefaultKeyboard::ValidateFilters(const std::u16string& input) const {
    if (config_.prevent_digit) {
        const auto digits = std::count_if(input.begin(), input.end(), IsDigit);
        if (digits > config_.max_digits) {
            return ValidationError::MaxDigitsExceeded;
        }
    }
    if (config_.prevent_at) {
        if (input.find(u'@') != std::u16string::npos) {
            return ValidationError::AtSignNotAllowed;
        }
    }
    if (config_.prevent_percent) {
        if (input.find(u'%') != std::u16string::npos) {
            return ValidationError::PercentNotAllowed;
        }
    }
    if (config_.prevent_backslash) {
        if (input.find(u'\\') != std::u16string::npos) {
            return ValidationError::BackslashNotAllowed;
        }
    }
    if (config_.prevent_profanity) {
        // TODO: check the profanity filter (Azahar's too)
        MH4N_LOG(Info, kLog, "App requested swkbd profanity filter, but its not implemented.");
    }
    return ValidationError::None;
}

ValidationError DefaultKeyboard::ValidateInput(const std::u16string& input) const {
    const ValidationError filter_error = ValidateFilters(input);
    if (filter_error != ValidationError::None) {
        return filter_error;
    }

    // 3DS uses UTF-16 string to test string size
    if (input.size() > config_.max_text_length) {
        return ValidationError::MaxLengthExceeded;
    }

    const bool is_blank = std::all_of(input.begin(), input.end(), IsSpace);
    const bool is_empty = input.empty();
    switch (config_.accept_mode) {
    case 4:  // FixedLength
        if (input.size() != config_.max_text_length) {
            return ValidationError::FixedLengthRequired;
        }
        break;
    case 2:  // NotEmptyAndNotBlank
        if (is_blank) {
            return ValidationError::BlankInputNotAllowed;
        }
        if (is_empty) {
            return ValidationError::EmptyInputNotAllowed;
        }
        break;
    case 3:  // NotBlank
        if (is_blank) {
            return ValidationError::BlankInputNotAllowed;
        }
        break;
    case 1:  // NotEmpty
        if (is_empty) {
            return ValidationError::EmptyInputNotAllowed;
        }
        break;
    case 0:  // Anything
        return ValidationError::None;
    default:
        // Azahar: "What does hardware do in this case?", then UNREACHABLE().
        MH4N_LOG(Error, kLog, "Application requested unknown validation method. Method: {}",
                 config_.accept_mode);
        return ValidationError::UnknownConfiguration;
    }

    return ValidationError::None;
}

ValidationError DefaultKeyboard::ValidateButton(u8 button) const {
    switch (config_.button_config) {
    case 3:  // None
        return ValidationError::None;
    case 0:  // Single
        if (button != 0) {
            return ValidationError::ButtonOutOfRange;
        }
        break;
    case 1:  // Dual
        if (button > 1) {
            return ValidationError::ButtonOutOfRange;
        }
        break;
    case 2:  // Triple
        if (button > 2) {
            return ValidationError::ButtonOutOfRange;
        }
        break;
    default:
        return ValidationError::UnknownConfiguration;
    }
    return ValidationError::None;
}

ValidationError DefaultKeyboard::Finalize(const std::u16string& text, u8 button) {
    // Skip check when OK is not pressed
    if (button == static_cast<u8>(config_.button_config)) {
        ValidationError error = ValidateInput(text);
        if (error != ValidationError::None) {
            return error;
        }
        error = ValidateButton(button);
        if (error != ValidationError::None) {
            return error;
        }
    }
    data_ = {text, button};
    data_ready_ = true;
    return ValidationError::None;
}

void DefaultKeyboard::Open() {
    const std::shared_ptr<KeyboardCounter> counter = context_.State<KeyboardCounter>("swkbd");
    const std::vector<std::string> texts = context_.GetOptions().All("--swkbd-text");
    if (counter->opened < texts.size()) {
        text_ = Utf8ToUtf16(texts[counter->opened]);
    } else {
        text_ = cfg::GetUserName(context_);
    }
    ++counter->opened;
}

void DefaultKeyboard::Execute(const KeyboardConfig& config) {
    config_ = config;

    // The text is cut to the configuration's maximum length (not Azahar's: it rejects a longer one).
    std::u16string text = text_;
    if (text.size() > config_.max_text_length) {
        text.resize(config_.max_text_length);
        // A cut in the middle of a surrogate pair would leave half of a character.
        if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) {
            text.pop_back();
        }
    }

    ValidationError error = ValidationError::None;
    switch (config_.button_config) {
    case 3:  // None
    case 0:  // Single
        error = Finalize(text, 0);
        break;
    case 1:  // Dual
        error = Finalize(text, 1);
        break;
    case 2:  // Triple
        error = Finalize(text, 2);
        break;
    default:
        MH4N_LOG(Error, kLog, "Unknown button config {}", config_.button_config);
        error = ValidationError::UnknownConfiguration;
        break;
    }
    if (error != ValidationError::None) {
        // Azahar says nothing and never answers; the applet waits for ever.
        MH4N_LOG(Error, kLog,
                 "the default keyboard's text is not accepted (check {}): the keyboard never finishes",
                 static_cast<int>(error));
    }
}

void DefaultKeyboard::ShowError(const std::u16string& error) {
    MH4N_LOG(Error, kLog, "Default keyboard text is unaccepted! error: {}", Utf16ToUtf8(error));
}

// ---- the applet ---------------------------------------------------------------------------------------------------

SoftwareKeyboard::SoftwareKeyboard(AppletId id, AppletId parent, std::weak_ptr<AppletHost> host,
                                   ServiceContext& context)
    : Applet(id, parent, std::move(host)), context_(context), keyboard_(context) {}

Result SoftwareKeyboard::ReceiveParameterImpl(const MessageParameter& parameter) {
    switch (parameter.signal) {
    case SignalType::Request: {
        // The LibAppJustStarted message contains a buffer with the size of the framebuffer shared
        // memory. Create the SharedMemory that will hold the framebuffer data.
        if (parameter.buffer.size() != sizeof(CaptureBufferInfo)) {
            MH4N_LOG(Error, kLog, "the Request parameter has {} bytes, not {}", parameter.buffer.size(),
                     sizeof(CaptureBufferInfo));
            return kResultUnknown;
        }
        CaptureBufferInfo capture_info;
        std::memcpy(&capture_info, parameter.buffer.data(), sizeof(capture_info));

        // A SharedMemory that directly points to a heap block of the system region.
        framebuffer_memory_ = context_.GetKernel().NewSharedMemoryForApplet(
            0, capture_info.size, MemoryPermission::ReadWrite, MemoryPermission::ReadWrite,
            "SoftwareKeyboard Memory");
        if (!framebuffer_memory_) {
            // Azahar: "Not enough space in region to allocate shared memory!" (an assertion).
            MH4N_LOG(Error, kLog, "cannot make the framebuffer shared memory of {} bytes",
                     capture_info.size);
        }

        // Send the response message with the newly created SharedMemory
        SendParameter({
            .sender_id = id_,
            .destination_id = parent_,
            .signal = SignalType::Response,
            .object = framebuffer_memory_,
            .buffer = {},
        });

        return kResultSuccess;
    }

    case SignalType::Message: {
        // Callback result
        if (parameter.buffer.size() != sizeof(config_)) {
            MH4N_LOG(Error, kLog,
                     "The size of the parameter (SoftwareKeyboardConfig) is wrong: {} bytes",
                     parameter.buffer.size());
            return kResultUnknown;
        }

        std::memcpy(&config_, parameter.buffer.data(), parameter.buffer.size());

        switch (static_cast<SoftwareKeyboardCallbackResult>(config_.callback_result)) {
        case SoftwareKeyboardCallbackResult::OK:
            // Finish execution
            Finalize();
            return kResultSuccess;

        case SoftwareKeyboardCallbackResult::Close:
            // Let the frontend display error and quit
            keyboard_.ShowError(BufferToString(config_.callback_msg, kMaxCallbackMsgLen + 1));
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::BannedInput);
            config_.text_offset = config_.text_length = 0;
            Finalize();
            return kResultSuccess;

        case SoftwareKeyboardCallbackResult::Continue:
            // Let the frontend display error and get input again. The input will be sent for
            // validation again on next Update().
            keyboard_.ShowError(BufferToString(config_.callback_msg, kMaxCallbackMsgLen + 1));
            keyboard_.Execute(ToFrontendConfig(config_));
            return kResultSuccess;

        default:
            // Azahar: UNREACHABLE().
            MH4N_LOG(Error, kLog, "unknown callback result {}", config_.callback_result);
            return kResultUnknown;
        }
    }

    default: {
        MH4N_LOG(Error, "Service.APT", "unsupported signal {}", static_cast<u32>(parameter.signal));
        // TODO(Subv): Find the right error code
        return kResultUnknown;
    }
    }
}

Result SoftwareKeyboard::Start(const MessageParameter& parameter) {
    if (parameter.buffer.size() != sizeof(config_)) {
        // Azahar asserts.
        MH4N_LOG(Error, kLog, "The size of the parameter (SoftwareKeyboardConfig) is wrong: {} bytes",
                 parameter.buffer.size());
        return kResultUnknown;
    }

    std::memcpy(&config_, parameter.buffer.data(), parameter.buffer.size());
    text_memory_ = ObjectCast<SharedMemory>(parameter.object);
    if (!text_memory_) {
        MH4N_LOG(Warning, kLog, "the game gave no shared memory block for the text");
    }

    // DrawScreenKeyboard(): "TODO(Subv): Draw the HLE keyboard, for now just do nothing" (Azahar's too)

    keyboard_.Open();
    keyboard_.Execute(ToFrontendConfig(config_));

    return kResultSuccess;
}

void SoftwareKeyboard::Update() {
    if (!keyboard_.DataReady()) {
        return;
    }

    const KeyboardData data = keyboard_.ReceiveData();
    std::u16string text = data.text;

    // Include a null terminator. Azahar copies (length + 1) units into the block whatever its size.
    u32 units_that_fit = 0;
    if (text_memory_) {
        units_that_fit = text_memory_->Size() / 2;
    }
    if (units_that_fit == 0) {
        if (text_memory_) {
            MH4N_LOG(Warning, kLog, "the shared memory block of the text is too small for anything");
        }
        text.clear();
    } else if (text.size() + 1 > units_that_fit) {
        MH4N_LOG(Warning, kLog,
                 "the text ({} units) does not fit the shared memory block ({} bytes): cut", text.size(),
                 text_memory_->Size());
        text.resize(units_that_fit - 1);
        if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) {
            text.pop_back();
        }
    }
    if (units_that_fit != 0) {
        u8* destination = text_memory_->GetPointer();
        for (std::size_t i = 0; i <= text.size(); ++i) {
            const u16 unit = i < text.size() ? static_cast<u16>(text[i]) : u16{0};
            destination[2 * i] = static_cast<u8>(unit);
            destination[2 * i + 1] = static_cast<u8>(unit >> 8);
        }
    }

    switch (static_cast<SoftwareKeyboardButtonConfig>(config_.num_buttons_m1)) {
    case SoftwareKeyboardButtonConfig::SingleButton:
        config_.return_code = static_cast<s32>(SoftwareKeyboardResult::D0Click);
        break;
    case SoftwareKeyboardButtonConfig::DualButton:
        if (data.button == 0) {
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::D1Click0);
        } else {
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::D1Click1);
        }
        break;
    case SoftwareKeyboardButtonConfig::TripleButton:
        if (data.button == 0) {
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::D2Click0);
        } else if (data.button == 1) {
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::D2Click1);
        } else {
            config_.return_code = static_cast<s32>(SoftwareKeyboardResult::D2Click2);
        }
        break;
    case SoftwareKeyboardButtonConfig::NoButton:
        // TODO: find out what is actually returned
        config_.return_code = static_cast<s32>(SoftwareKeyboardResult::None);
        break;
    default:
        // Not reachable: Execute does not answer to a button configuration it does not know.
        MH4N_LOG(Critical, kLog, "Unknown button config {}", config_.num_buttons_m1);
        return;
    }

    config_.text_length = static_cast<u16>(text.size());
    config_.text_offset = 0;

    if (config_.filter_flags & kFilterCallback) {
        std::vector<u8> buffer(sizeof(SoftwareKeyboardConfig));
        std::memcpy(buffer.data(), &config_, buffer.size());

        // Send the message to invoke callback
        SendParameter({
            .sender_id = id_,
            .destination_id = parent_,
            .signal = SignalType::Message,
            .object = nullptr,
            .buffer = std::move(buffer),
        });
    } else {
        Finalize();
    }
}

Result SoftwareKeyboard::Finalize() {
    std::vector<u8> buffer(sizeof(SoftwareKeyboardConfig));
    std::memcpy(buffer.data(), &config_, buffer.size());
    CloseApplet(nullptr, buffer);
    text_memory_ = nullptr;
    return kResultSuccess;
}

}  // namespace mh4n::apt
