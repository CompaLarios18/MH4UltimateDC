// The in-game menu's index: every tab, section and row the menu draws, in the menu's order, with
// the words its search matches and each row's help line (the footer and the search share it).
// ui/menu*.cpp draw the rows with their own code; this file holds no Dear ImGui and no Azahar, so
// tests/test_app_menu_index.py compiles it on its own.
#pragma once

#include <string>
#include <vector>

namespace App::Ui::Index {

/// The tab bar's tabs, in its order, and the header (the search, Quit game, Back to the game).
enum class Tab { Display, Graphics, TouchScreen, Controls, Keys, Gameplay, Audio, Online, Header };
inline constexpr int TAB_COUNT = 8; ///< in the tab bar (the header isn't a tab)

/// "Display", "Touch screen", ...
const char* TabName(Tab tab);

/// What a row is: a setting (a control that writes a value), a key binding, an action (a button:
/// Edit HUD layout, Quit game), a block another module draws, a link (the search's way to a part
/// of Online), a key shown but not changed here.
enum class Kind { Setting, Key, Action, Block, Link, Fixed };

struct Section {
    const char* id;    ///< "display.frame_rate"
    Tab tab;
    const char* title; ///< "Frame rate"
};

struct Row {
    const char* id;           ///< "display.fps"
    const char* section;      ///< "display.frame_rate"
    Kind kind;
    const char* label;        ///< as drawn ("    Across")
    const char* search_label; ///< as listed in the results ("Map across"); nullptr: the label
    const char* help;         ///< <= 140 ASCII characters; may hold one {key:...} placeholder
    const char* keywords;     ///< the search's words; "": not in the results
    const char* parent;       ///< the row that shows it ("" none)
    const char* shown_when;   ///< when the parent shows it, for the jump's message ("" none)
    bool next_start;          ///< applies from the next start
    bool contextual;          ///< a second place of a Keys row (Map key, Fast forward key, Pause key)
    const char* writes;       ///< "mh4u.ini fps", "mh4u-app.ini [Display] vsync", "" for actions
};

enum class SectionId : int {
    DisplayFrameRate,
    DisplayWindow,
    DisplayWidescreen,
    GraphicsQuality,
    GraphicsEffects,
    TouchScreens,
    TouchCorner,
    TouchQuests,
    ControlsInput,
    ControlsMouse,
    ControlsController,
    KeysHunter,
    KeysMoving,
    KeysCamera,
    KeysItems,
    KeysTouch,
    KeysPort,
    GameplayButtons,
    GameplayAfterQuest,
    GameplaySpeed,
    GameplayTouchTaps,
    AudioSound,
    OnlineNetSync,
    OnlineRooms,
    Header,
    Count
};

/// The rows in ROWS[] order (tests/test_app_menu_index.py checks the names against the ids).
enum class RowId : int {
    DisplayFps,
    DisplayVsync,
    DisplayFpsCounter,
    DisplayCpuClock,
    DisplayRefreshRate,
    DisplayFullscreen,
    DisplayRenderer,
    DisplayLauncher,
    DisplayAspect,
    DisplayWideRender,
    DisplayHudEdges,
    GraphicsResolution,
    GraphicsLinearFilter,
    GraphicsAntialiasing,
    GraphicsDither,
    GraphicsStereo3d,
    GraphicsCustomTextures,
    GraphicsBloom,
    GraphicsDepthOfField,
    GraphicsMotionBlur,
    GraphicsRadialBlur,
    GraphicsHaze,
    TouchScreens,
    TouchOverlay,
    TouchCorner,
    TouchSize,
    TouchOpacity,
    TouchMapOnly,
    TouchMapKey,
    TouchHudEditor,
    TouchMapSize,
    TouchMapWhere,
    TouchMapX,
    TouchMapY,
    TouchTargetView,
    TouchTargetSize,
    TouchTargetWhere,
    TouchTargetX,
    TouchTargetY,
    ControlsInputMode,
    ControlsKeyGuides,
    ControlsMouseSensitivity,
    ControlsMouseVertical,
    ControlsMouseInvertY,
    ControlsMouseInvertX,
    ControlsMouseDeadzone,
    ControlsControllerAuto,
    KeysX,
    KeysA,
    KeysB,
    KeysY,
    KeysR,
    KeysL,
    KeysZl,
    KeysZr,
    KeysMoveForward,
    KeysMoveBack,
    KeysMoveLeft,
    KeysMoveRight,
    KeysWalk,
    KeysCameraUp,
    KeysCameraDown,
    KeysCameraLeft,
    KeysCameraRight,
    KeysItemPrev,
    KeysItemNext,
    KeysStart,
    KeysSelect,
    KeysDpadUp,
    KeysDpadDown,
    KeysDpadLeft,
    KeysDpadRight,
    KeysStylus,
    KeysTouchScreen,
    KeysGuides,
    KeysPause,
    KeysFastForward,
    KeysFixedFullscreen,
    KeysFixedFpsCounter,
    KeysFixedMenu,
    KeysReset,
    GameplayKick,
    GameplayTarget,
    GameplaySkip,
    GameplaySkipEnd,
    GameplaySkipEndHold,
    GameplaySkipHint,
    GameplayFfSpeed,
    GameplayFfMode,
    GameplayFfKey,
    GameplayPauseKey,
    GameplayTouchTaps,
    AudioVolume,
    AudioOutput,
    OnlineNetSync,
    OnlineJoin,
    OnlineHost,
    OnlineLan,
    OnlineLobby,
    OnlineInvite,
    OnlineName,
    OnlineLeave,
    HeaderSearch,
    HeaderQuit,
    HeaderBack,
    Count
};

inline constexpr int SECTION_COUNT = static_cast<int>(SectionId::Count);
inline constexpr int ROW_COUNT = static_cast<int>(RowId::Count);

extern const Section SECTIONS[];
extern const Row ROWS[];

const Section& Get(SectionId section);
const Row& Get(RowId row);
/// The row's section and tab (worked out once from the ids).
SectionId SectionOf(RowId row);
Tab TabOf(RowId row);
Tab TabOf(SectionId section);
/// The row whose id is `id`, or RowId::Count.
RowId Find(const std::string& id);
/// The row's parent, or RowId::Count.
RowId ParentOf(RowId row);
/// As listed in the results: the search label, else the label without its indent.
std::string ListedLabel(RowId row);

/// ASCII lower case, every character other than a-z and 0-9 a space, words split by one space,
/// the stop words left out ("16:9" -> "16 9", "C-stick" -> "c stick", "the map" -> "map").
std::string Normalize(const std::string& text);

/// Whether the query asks for results: a word of 2 or more characters once normalized.
bool Searchable(const std::string& query);

struct Result {
    int row;  ///< RowId
    int tier; ///< 1: its names have every word; 2: only with its help
};

/// The rows that match `query`: tier 1, every word of the query the start of a word of the row's
/// names (its label, section, tab, search words and `extra` for it: the keys and buttons bound to a
/// key row); when none, tier 2, the same with its help too. Those whose label (as listed) has every
/// word come first, so that Enter goes to the setting named so; each part in the menu's order.
/// `extra` has a string per row or is empty. Rows without search words aren't listed.
std::vector<Result> Search(const std::string& query, const std::vector<std::string>& extra);

} // namespace App::Ui::Index
