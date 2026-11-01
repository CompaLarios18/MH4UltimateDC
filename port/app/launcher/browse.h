// The launcher's own file browser, drawn like the rest of it and used with a controller (the
// D-pad, A and B), the mouse or the keyboard: folders first, then the files the picker asks for,
// the folder above, places to start from (home, Desktop, Downloads, the last folder, removable
// drives such as a Steam Deck's SD card, the drive letters) and a line to type or paste a path
// into. It stands in for the system's dialog where that can't be used (Steam's Game Mode, a
// controller in hand) or didn't work, and is a choice next to it on Setup and Mods.
#pragma once

#include <string>
#include <vector>

#include "launcher/paths.h"
#include "launcher/pick.h"

namespace Launcher::Browse {

/// How a frame of the browser ended.
enum class Outcome {
    Open,       ///< still open
    Chosen,     ///< a file or folder was chosen: Browser::Chosen()
    Cancelled,  ///< closed without one
    System,     ///< the player asked for the system's dialog instead: Browser::Asked()
};

class Browser {
public:
    /// `request` as for the system's dialog; `note` a line over the list (why it opened);
    /// `last` the folder of the last choice (a place of its own); `system`: the system has a
    /// dialog to offer instead.
    Browser(Pick::Request request, std::string note, std::string last, bool system);

    /// Draws it, a window over the launcher (each frame while it is open). `pad`: the last
    /// button pressed was a controller's (its hints, and the focus shown).
    Outcome Draw(bool pad);

    /// After Outcome::Chosen: the file or folder.
    const std::string& Chosen() const {
        return chosen;
    }
    /// The request, opening where the browser is now (for the system's dialog).
    Pick::Request Asked() const;

private:
    bool Files() const;
    bool Go(const std::string& folder, bool remember);
    void Up();
    bool Back();
    void Open(const std::string& start);
    void Submit(Outcome& outcome);
    void DrawPlaces();
    void DrawEntries(bool pad, Outcome& outcome);
    std::string Summary() const;

    Pick::Request request;
    std::string note;
    bool system = false;
    std::string home;
    std::vector<Paths::Place> places;
    Paths::Listing listing;
    std::vector<std::string> history;  ///< the folders before this one (B goes back to them)
    std::string typed;                 ///< the path line
    std::string message;               ///< what went wrong last ("" when nothing did)
    std::string focus_name;            ///< the entry to give the focus next (the folder left)
    bool focus_first = true;           ///< else the first entry
    bool focus_field = false;          ///< the path line (Ctrl+L)
    bool scroll_top = true;
    bool opened = false;
    std::string chosen;
};

} // namespace Launcher::Browse
