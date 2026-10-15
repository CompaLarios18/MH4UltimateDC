// The in-game menu's window and tabs: Game (mh4u.ini, the port's hooks), Display, Audio and
// Controls (mh4u-app.ini), Multiplayer (the network room) and Quit. A change applies at once
// where the game or Azahar can take it while running, otherwise from the next start (each such
// setting says so); the files are written when the menu closes.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>

namespace App::Ui {

class Layer;

class Menu {
public:
    explicit Menu(Layer& layer);
    ~Menu();

    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    /// The menu came up: the settings files are read again.
    void Opened();
    /// It went away: what changed is written.
    void Closed();
    /// In a frame.
    void Draw();
    /// Controls: the key pressed while a binding waits for one (Esc keeps the old one, Delete
    /// clears it).
    void OnCapturedKey(int scancode);

    enum class AppSetting { Fullscreen, Screens, Resolution, LinearFilter, Vsync, Renderer, Volume, Output, Controls };

private:
    void DrawGame();
    void DrawDisplay();
    void DrawAudio();
    void DrawControls();
    void DrawMultiplayer();
    void DrawQuit();

    /// mh4u.ini: a key's value in effect (the menu's change, the file's, or the hooks' default).
    std::string Game(const std::string& key) const;
    /// Changes it: at once where the hooks take it while the game runs, and in the file later.
    void SetGame(const std::string& key, const std::string& value);
    void Changed(AppSetting setting);
    /// The controls changed: the input profile is built again from them.
    void ControlsChanged();

    Layer& layer;
    int tab = 0;
    int select_tab = -1;
    bool just_opened = false;
    std::map<std::string, std::string> game_file;    ///< mh4u.ini when the menu came up
    std::map<std::string, std::string> game_changes; ///< written when it goes
    std::set<AppSetting> app_changes;
    int capture = -1;          ///< Controls: the row waiting for a key
    std::string capture_note;  ///< why the last key wasn't taken

    struct Room;
    std::shared_ptr<Room> room; ///< what the network room's callbacks last said
};

} // namespace App::Ui
