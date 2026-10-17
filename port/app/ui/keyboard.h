// The 3DS software keyboard (the game asks for a new hunter's name and for chat messages through
// it), drawn by the in-game menu: a text field with the game's limits and buttons, and keys on
// the screen for a controller.
//
// The game's applet calls Execute() on the emulation thread and then waits for the text on that
// thread. The app builds its frames on the same thread, so Execute only takes the request and
// returns; the menu's frame shows the keyboard and hands the text over with
// IRuntime::FinishKeyboard() when the player is done (a frontend that blocks the emulation
// thread in Execute until its dialog closes would stop the app's frames).
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "runtime.h"
#include "util/types.h"

struct ImGuiInputTextCallbackData;

namespace App::Ui {

class Keyboard final : public ISoftwareKeyboard {
public:
    void Execute(const KeyboardConfig& config) override;
    void ShowError(const std::string& error) override;

    /// The game waits for the player's text.
    bool Active() const;
    /// An error the game sent with no keyboard up, still on the screen.
    bool HasNotice() const;

    /// In a frame: the keyboard's window.
    void Draw();
    /// The keyboard takes the focus again (the menu over it closed).
    void Refocus();
    /// In a frame: the notice alone.
    void DrawNotice();

private:
    void Insert(const std::string& characters);
    void Submit(u8 button);
    /// Whether `c` may be typed now (the game's filters and lengths).
    bool Accepts(unsigned c) const;
    static int TextCallback(ImGuiInputTextCallbackData* data);

    mutable std::mutex mutex;
    KeyboardConfig config;          ///< the game's last request
    bool active = false;
    bool appeared = false;          ///< the first frame since it came up: the text field takes focus
    std::string text;
    std::vector<char> buffer;       ///< the text field's own buffer (UTF-8)
    std::string error;              ///< why the text wasn't taken
    std::string notice;
    u32 notice_until = 0;           ///< SDL ticks
    bool shift = false;
    bool start_armed = false;       ///< a controller's Start went down here: OK when it is let go
};

} // namespace App::Ui
