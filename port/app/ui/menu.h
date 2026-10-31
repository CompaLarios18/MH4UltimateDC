// The in-game menu's window: a header with the search, Quit game and Back to the game, eight tabs
// that each do one job (Display, Graphics, Touch screen, Controls, Keys, Gameplay, Audio, Online;
// ui/settings_index.cpp lists every row, ui/menu_rules.cpp says when one can't take effect), and a
// footer with the help of the control under the cursor and the keys. A change applies at once where
// the game or Azahar can take it while running, otherwise from the next start (each such setting
// says so); the files are written when the menu closes.
//
// The menu's code: menu.cpp (the window, rows, focus, saving), menu_pages.cpp (the tabs),
// menu_keys.cpp (Keys and the key rows elsewhere), menu_search.cpp (the search and its keys on the
// screen), menu_editor.cpp (the HUD layout editor).
#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <imgui.h>

#include "ui/menu_rules.h"
#include "ui/settings_index.h"

namespace App::Ui {

class Layer;

/// What a controller does for an input of the input profile (Settings' parameters: "RT",
/// "A (bottom)", ...), or "" when no controller does.
std::string ControllerInputName(const std::string& params);

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
    /// The key (or Input::MOUSE_KEY + a mouse button, Input::WHEEL_UP, ...) pressed while a
    /// binding waits for one (Esc keeps the old one, Delete clears it).
    void OnCapturedKey(int code);
    /// B or Esc with nothing of Dear ImGui's to close: the keys on the screen close, else a search
    /// with text clears (the cursor goes back to the tab's last row); false when there was neither,
    /// and the menu closes.
    bool OnBack();
    /// After each item that has help (Layer::Help): the cursor the menu is waiting to put
    /// somewhere, the tab's last row, the sections LT and RT go through.
    void AfterItem();

    enum class AppSetting {
        Fullscreen,
        Screens,
        Resolution,
        LinearFilter,
        Vsync,
        Renderer,
        Volume,
        Output,
        Controls,
        FpsCounter,
        CpuClock,
        FastForward,
        CustomTextures,
        Launcher
    };

    /// The layout editor is up in place of the menu's window: frames over the game for the
    /// quest's map and target camera views, moved and resized with the mouse, a controller or the
    /// keyboard.
    bool EditingLayout() const {
        return layout_edit;
    }
    /// Esc or a controller's B in the editor: what it changed is undone and the menu comes back.
    void CancelLayoutEdit();

    /// Where the cursor goes next: taken by the first matching item drawn (AfterItem), never by
    /// SetKeyboardFocusHere (it would put a slider into typing a value).
    struct Focus {
        enum class Kind { None, First, Item, Row, Section, Note, Next, Result };
        Kind kind = Kind::None;
        ImGuiID item = 0;                         ///< Item: the tab's last row
        Index::RowId row = Index::RowId::Count;   ///< Row; Next: the fixed row before the control
        int note = -1;                            ///< Note: a row's index, or ROW_COUNT + a section's
        Index::SectionId section = Index::SectionId::Count; ///< Section
        bool cursor = false;   ///< shown there
        bool scroll = false;   ///< a jump: the page scrolls to it first, the cursor goes a frame later
        bool scrolled = false;
        int ready = 0;         ///< the frame from which it may be taken
        int frames = 0;        ///< frames it has waited
    };

private:
    using RowId = Index::RowId;
    using SectionId = Index::SectionId;
    using Tab = Index::Tab;

    // ── The window (menu.cpp) ──
    void DrawHeader();
    void DrawQuitQuestion();
    void DrawTabBar();
    void DrawPage(float footer);
    void DrawFooter();
    /// LB/RB, LT/RT, Y, B and Ctrl+Tab, after the window's items.
    void HandleButtons();
    const char* Hints() const;
    /// Another tab, on its last row (else its first control); `cursor`: shown there.
    void SelectTab(Tab to, bool cursor);
    /// What the rules look at, for every tab (cheap); the hooks' sentences only on Touch screen.
    void GatherFacts();
    /// The texts that name keys and buttons (help lines, the key rows' labels, the lists of 3DS
    /// buttons), built again on opening, after the keys change and when a controller comes or goes.
    void RefreshTexts();

    // ── Rows (menu.cpp) ──
    /// A settings table: the name, then the control; compact rows.
    bool BeginSettings(const char* id);
    void EndSettings();
    /// A section's title, the place LT and RT go to, and its note.
    void Section(SectionId section);
    /// The place LT and RT go to, with no title (Online's, Touch taps').
    void SectionAnchor(SectionId section);
    /// A row's label and markers, its control's cell ready (full width); false when it is hidden.
    bool BeginRow(RowId row);
    /// After the row's control: its help (unless the row drew its items' itself), its note.
    void EndRow(RowId row, bool help = true);
    void RowLabel(RowId row);
    /// After an item of `row`: the footer's help, the pending focus.
    void ItemHelp(RowId row);
    const char* HelpOf(RowId row) const;
    /// The label as drawn: a key row's names the port's roles of its button.
    std::string LabelOf(RowId row) const;
    /// A note (amber or dim, wrapped) and its Go to link; `key` names it for the focus: a row's
    /// index, or ROW_COUNT + a section's.
    void DrawNote(const Rules::Note& note, int key, bool link_takes_first);
    /// A row of a settings table that leads to another tab.
    void LinkRow(const char* text, Tab to, const char* help, const char* id);
    /// R17: the port's options may not work now (the code library): one line, Go to Display.
    void BackendLine();
    bool SectionDisabled(SectionId section) const;
    /// The row was changed in this opening of the menu (its "(next start)" turns amber).
    bool ChangedNow(RowId row) const;
    void FlashRow(RowId row);

    // ── Controls with steppers: left and right change them without opening them ──
    bool Choose(const char* id, int& current, const std::vector<std::string>& labels, int disabled = -1);
    bool ChooseValue(const char* id, std::string& value, std::vector<std::string> values,
                     std::vector<std::string> labels, int disabled = -1);
    bool SliderInt(const char* id, int& value, int min, int max, const char* format, int step);
    bool SliderFloat(const char* id, float& value, float min, float max, const char* format, float step);
    /// Its sliders' flags: with a controller in use, no value typed in (only the steps).
    ImGuiSliderFlags SliderFlags() const;

    // ── Focus and jumps (menu.cpp) ──
    void FocusFirst(bool cursor);
    void FocusRemembered(bool cursor);
    /// Takes the pending focus on the item just drawn when it is the one awaited.
    void TakeFocus(bool match);
    /// A pending focus nothing took: the first control after two frames, then nothing.
    void AgeFocus();
    /// The search's result or a Go to link: that row's tab, the row scrolled to and focused (its
    /// parent when it is hidden now, its note's link when it can't apply now), flashed.
    void Jump(RowId row);
    /// LT/RT: the previous or next section of the tab with something to focus.
    void JumpSection(int direction);

    // ── The tabs (menu_pages.cpp) ──
    void DrawDisplay();
    void DrawGraphics();
    void DrawTouchScreen();
    void DrawPanelViewRows(const char* key, RowId size, RowId where, RowId x, RowId y);
    void DrawControls();
    void DrawGameplay();
    /// Kick, Target camera, Skip the scene: a list of 3DS buttons, each with what presses it.
    void DrawButtonList(RowId row, const char* key);
    void DrawAudio();
    void DrawOnline();

    // ── Keys (menu_keys.cpp) ──
    void DrawKeys();
    void DrawKeyRow(RowId row, int binding);
    void DrawFixedKeyRow(RowId row);
    /// A binding's key button; `place` the row whose capture note shows the outcome.
    void KeyButton(int binding, int slot, const char* id, float width, RowId place);
    /// Map key, Fast forward key, Pause key: a Keys row's two key buttons elsewhere.
    void DrawContextKeyRow(RowId row, int binding);
    void DrawResetKeys();
    void StartCapture(int id, RowId place);
    /// The key rows' labels and the search's words for them (the keys and buttons bound now).
    void RefreshKeyTexts();
    std::string KeyRowLabel(RowId row) const;
    int BindingOf(RowId row) const;
    const std::string& PadText(int binding);

    // ── The search (menu_search.cpp) ──
    void DrawSearchField(float width);
    void DrawResults(float footer);
    void DrawSearchKeys();
    void HandleSearchKeys();
    void UpdateSearch();
    void ClearSearch();
    void OpenKeys();
    /// The keys on the screen are done: the cursor goes to the first result (or the tab's row).
    void FinishKeys();
    void TypeKey(char c);
    bool Searching() const;

    // ── The HUD layout editor (menu_editor.cpp) ──
    void DrawLayoutEditor();
    void StartLayoutEdit();
    /// The editor goes; the menu comes back unless it is closing.
    void EndLayoutEdit();
    /// The editor's controller and keyboard: LB/RB (Tab) choose a view, the D-pad (arrows) and the
    /// left stick move it, LT/RT (- +) resize it, Y (Space) shows it, X (Backspace) resets.
    void EditorKeys(float width, float height);
    std::string EditorNumbers() const;

    // ── The map view's notes (port/src/map_view.h) and the touch taps (ui/touch_taps.h) ──
    /// The hooks' sentences for In quests (R4) and Target camera by the map (R5).
    void GatherViewNotes(Rules::Facts& facts) const;
    /// The same in the editor's toolbar.
    std::string EditorViewNote() const;
    /// A touch tap waits for its key.
    bool TapCapturing() const;
    /// A touch tap's key or point stops being edited.
    void EndTapEdit();
    /// The key pressed is a touch tap's; true when it took it.
    bool TapTakesKey(int code);
    /// `key` given to a binding leaves any touch tap that had it.
    void ReleaseTapKey(const std::string& key);
    /// After Reset keys: the touch taps keep their keys, which the defaults go without.
    void SettleTaps();
    /// Gameplay > Touch taps, full width, last on the tab.
    void DrawTouchTaps();

    // ── The settings files ──
    /// mh4u.ini: a key's value in effect (the menu's change, the file's, or the hooks' default).
    std::string Game(const std::string& key) const;
    /// Changes it: at once where the hooks take it while the game runs, and in the file later.
    void SetGame(const std::string& key, const std::string& value);
    void Changed(AppSetting setting);
    /// The controls changed: the input profile is built again from them.
    void ControlsChanged();

    Layer& layer;
    Tab tab = Tab::Display;
    int bar_tab = -1;            ///< the tab the tab bar shows (it follows `tab` a frame later)
    bool just_opened = false;
    std::map<std::string, std::string> game_file;    ///< mh4u.ini when the menu came up
    std::map<std::string, std::string> game_changes; ///< written when it goes
    std::set<AppSetting> app_changes;
    std::string backend_notice;  ///< Port::BackendNotice(), read on opening
    std::string renderer_started; ///< the renderer chosen as the app started (R8)

    // the rules, this frame
    Rules::Facts facts;
    Rules::Verdict verdict;

    // the page being drawn
    bool in_page = false;        ///< the items' help takes the pending focus
    bool in_results = false;
    struct ItemContext {
        RowId row = RowId::Count; ///< the row the item belongs to
        int note = -1;            ///< a note's Go to link (DrawNote's key)
        bool first = true;        ///< it can be a page's or a section's first control
        bool result = false;      ///< the first of the search's results
    } item;
    SectionId current_section = SectionId::Count;
    struct Anchor {
        SectionId section;
        bool focusable;
    };
    std::vector<Anchor> anchors, anchors_last; ///< the sections drawn this frame and the last one
    SectionId focused_section = SectionId::Count, focused_section_last = SectionId::Count;
    bool section_armed = false;  ///< the next first control takes a section's focus
    bool next_armed = false;     ///< the next control takes a fixed row's
    bool list_open = false;      ///< one of the menu's lists is open (the footer's keys)
    bool slider_active = false;  ///< one of its sliders is being changed
    RowId pending_jump = RowId::Count; ///< a Go to link or a result chosen: after the page
    int pending_tab = -1;        ///< a Go to <tab> link chosen
    bool pending_tab_cursor = false;

    // focus
    Focus focus;
    std::array<ImGuiID, Index::TAB_COUNT> remembered{}; ///< each tab's last row, for the run
    RowId flash_row = RowId::Count;
    double flash_until = 0.0;
    bool fresh = true;           ///< the footer's default text until the cursor or the mouse moves
    ImGuiID fresh_nav = 0;
    std::string footer_note;     ///< a jump's "... shows when ...", for a while
    double footer_note_until = 0.0;
    ImGuiID footer_note_nav = 0;
    bool open_quit = false;      ///< the search chose Quit game: its question opens

    // the texts that name keys (RefreshTexts)
    bool texts_dirty = true;
    int refresh_in = 0;          ///< frames until they are built again (the input profile first)
    std::string help_fps_counter, help_fullscreen, help_key_guides;
    std::vector<std::string> key_labels;      ///< per row: a key row's label, "" for the index's
    std::vector<std::string> search_extra;    ///< per row: the keys and buttons bound to it
    std::vector<std::string> link_helps;      ///< per row: a Go to link's help ("Goes to ...")
    std::vector<std::string> button_labels;   ///< the lists of 3DS buttons: "ZR  (C / RT)", ...
    std::vector<std::string> combo_labels;    ///< End the quest's
    std::array<std::string, static_cast<int>(Rules::Buttons::Count)> list_buttons, list_unbound;
    std::string controller_description;
    struct PadCache {
        bool valid = false;
        std::string params, text;
    };
    std::vector<PadCache> pad_cache;          ///< the Controller column, per binding

    // keys
    int capture = -1;            ///< the binding * 2 + the key's column waiting for a key
    RowId capture_place = RowId::Count; ///< the row it started from, which shows the note
    std::string capture_note;    ///< why the last key wasn't taken
    bool open_reset = false;

    // the search
    char search[64] = {};
    ImGuiID search_id = 0;       ///< its field
    /// The field lets go of its text at the start of the next frame (ClearSearch is called from
    /// outside the frames too, where Dear ImGui's context isn't the menu's to touch)
    bool drop_search_field = false;
    std::string query;           ///< what the results are for
    bool searchable = false;     ///< it has a word of two letters or more: the results show
    std::vector<Index::Result> results;
    bool keys_up = false;        ///< the keys on the screen
    bool keys_focus_q = false;
    bool keys_start_armed = false;
    ImGuiID pad_owner = 0;       ///< owns the controller's Y (the search, not Dear ImGui's typing)

    // the layout editor
    bool layout_edit = false;
    /// quest_overlay's and target_overlay's entries in game_changes before it (empty: none)
    std::map<std::string, std::string> layout_before;
    std::set<std::string> layout_before_missing;
    int drag = -1;             ///< the view being dragged: 0 the map, 1 the target camera
    bool drag_resize = false;  ///< by its corner
    float drag_mouse_x = 0, drag_mouse_y = 0, drag_left = 0, drag_top = 0, drag_height = 0;
    int pick = 0;              ///< the view the controller and the keyboard move
};

} // namespace App::Ui
