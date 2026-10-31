// The in-game menu's search: the field in the header (Ctrl+F, a click, or Y on a controller), the
// results that stand in for the tab's page while the field has a word, each a link to its setting,
// and the keys on the screen a controller types with (the menu's own; ui/keyboard.cpp is the
// game's keyboard).
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <imgui.h>

#include "ui/layer.h"
#include "ui/menu.h"
#include "ui/menu_parts.h"
#include "ui/nav.h"

namespace App::Ui {

using namespace MenuParts;

namespace {

using R = Index::RowId;
using K = Menu::Focus::Kind;

} // namespace

bool Menu::Searching() const {
    return keys_up || searchable;
}

void Menu::UpdateSearch() {
    // the search runs only when the text changes; its words for the key rows (the keys bound
    // now) are built with the other texts (RefreshKeyTexts)
    if (query == search) {
        return;
    }
    query = search;
    searchable = Index::Searchable(query);
    results = searchable ? Index::Search(query, search_extra) : std::vector<Index::Result>{};
}

void Menu::ClearSearch() {
    // the field lets go of its text in the next frame (Draw): this runs as the menu opens and
    // closes too, outside the frames, without the layer's lock on Dear ImGui's context
    drop_search_field = true;
    search[0] = '\0';
    query.clear();
    searchable = false;
    results.clear();
    keys_up = false;
    keys_focus_q = false;
}

void Menu::DrawSearchField(float width) {
    // Ctrl+F from anywhere in the menu activates it, as a click does
    ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteGlobal);
    ImGui::SetNextItemWidth(width);
    const bool enter = ImGui::InputTextWithHint("##search", layer.PadLast() ? "Search (Y)" : SEARCH_HINT, search,
                                                sizeof(search),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll);
    const bool activated = ImGui::IsItemActivated();
    const bool active = ImGui::IsItemActive();
    search_id = ImGui::GetItemID();
    ItemHelp(R::HeaderSearch);
    if (activated && (Nav::ActivatedByPad() || Nav::ActivatedByTouch())) {
        // A on the field with a controller, or a finger on it (a Steam Deck's): the keys on the
        // screen, not a keyboard nobody may have (a real one types into them too)
        Nav::LeaveTextField();
        OpenKeys();
        return;
    }
    if (active && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Esc clears the text and lets go of the field in one press (EscapeClearsAll alone takes
        // two); the next Esc closes the menu
        search[0] = '\0';
        Nav::LeaveTextField();
    } else if (active && ImGui::IsKeyPressed(ImGuiKey_DownArrow) && searchable) {
        // Down: from the field to the first result
        Nav::LeaveTextField();
        focus = {};
        focus.kind = K::Result;
        focus.cursor = true;
    }
    if (enter) {
        UpdateSearch();
        if (!results.empty()) {
            pending_jump = static_cast<RowId>(results.front().row); // Enter: the first result
        }
    }
}

void Menu::DrawResults(float footer) {
    GatherFacts();
    verdict = Rules::Evaluate(facts); // for every tab: a result says when it can't apply
    in_page = true;
    in_results = true;
    item = {};
    anchors.clear();
    current_section = SectionId::Count;
    ImGui::BeginChild("##page_results", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened);
    if (keys_up) {
        DrawSearchKeys();
    }
    if (!searchable) {
        ImGui::TextDisabled("Type a setting's name or a word like fps, map, kick or bloom.");
    } else {
        BackendLine();
        const ImGuiStyle& style = ImGui::GetStyle();
        const std::string heading =
            results.empty() ? fmt::format("Nothing matches \"{}\". Try a word like fps, map, kick, bloom or volume.", query)
            : results.front().tier == 2 ? fmt::format("No setting is called that; these mention \"{}\":", query)
            : results.size() == 1       ? fmt::format("1 setting matches \"{}\"", query)
                                        : fmt::format("{} settings match \"{}\"", results.size(), query);
        const char* clear = "Clear##results";
        const float clear_width = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2;
        const float start = ImGui::GetCursorPosX();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::PushTextWrapPos(start + std::max(1.0f, avail - clear_width - style.ItemSpacing.x));
        ImGui::TextUnformatted(heading.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(start + avail - clear_width);
        item.result = results.empty(); // with nothing found, the cursor waits on Clear
        if (ImGui::SmallButton(clear)) {
            ClearSearch();
            FocusRemembered(ImGui::GetIO().NavVisible);
        }
        layer.Help("Clears the search: the tab's page comes back.");
        item = {};
        // one link a result: its label, then where it is (and why it doesn't show or apply now)
        if (!results.empty() && ImGui::BeginTable("##results", 2, ImGuiTableFlags_SizingStretchProp)) {
            // labels are 30 characters at most; where they are, and why, takes more
            ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("where", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            for (std::size_t i = 0; i < results.size(); ++i) {
                const auto row = static_cast<RowId>(results[i].row);
                const auto section = Index::SectionOf(row);
                std::string where = Index::TabName(Index::TabOf(section));
                if (*Index::Get(section).title) {
                    where += std::string(" > ") + Index::Get(section).title;
                }
                if (verdict.Hidden(row)) {
                    RowId parent = Index::ParentOf(row);
                    while (parent != RowId::Count && verdict.Hidden(parent)) {
                        parent = Index::ParentOf(parent);
                    }
                    if (parent != RowId::Count) {
                        where += " (shows with " + LabelOf(parent) + ")";
                    }
                } else if (verdict.Disabled(row)) {
                    where += " (can't apply now)";
                }
                ImGui::TableNextRow();
                // where first, to know the row's height: the link spans both columns
                ImGui::TableSetColumnIndex(1);
                const float height = std::max(ImGui::GetTextLineHeight(),
                                              ImGui::CalcTextSize(where.c_str(), nullptr, false,
                                                                  ImGui::GetContentRegionAvail().x)
                                                  .y);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", where.c_str());
                ImGui::PopTextWrapPos();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(static_cast<int>(i));
                item = {};
                item.row = row;
                item.result = i == 0;
                if (ImGui::Selectable(LabelOf(row).c_str(), false, ImGuiSelectableFlags_SpanAllColumns,
                                      ImVec2(0.0f, height))) {
                    pending_jump = row;
                }
                ItemHelp(row);
                item = {};
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    in_page = false;
    in_results = false;
    AgeFocus();
}

// ── The keys on the screen ──

void Menu::OpenKeys() {
    keys_up = true;
    keys_focus_q = true; // the cursor starts on q
    keys_start_armed = false;
}

void Menu::FinishKeys() {
    keys_up = false;
    UpdateSearch();
    if (searchable) {
        focus = {};
        focus.kind = K::Result; // the cursor to the first result
        focus.cursor = true;
    } else {
        FocusRemembered(true); // nothing typed: back to the tab
    }
}

void Menu::TypeKey(char c) {
    const std::size_t length = std::char_traits<char>::length(search);
    if (length + 1 < sizeof(search)) {
        search[length] = c;
        search[length + 1] = '\0';
    }
}

void Menu::DrawSearchKeys() {
    static constexpr const char* ROWS[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const ImGuiStyle& style = ImGui::GetStyle();
    const float unit = ImGui::GetFontSize() / 13.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float key = std::floor((avail - style.ItemSpacing.x * 9.0f) / 10.0f);
    const float wide = std::floor((avail - style.ItemSpacing.x * 3.0f) / 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 4.0f * unit));
    item = {};
    item.first = false;
    const char* key_help = "Types it in the search. LB deletes, RB types a space, Y or Done goes to the results.";
    for (const char* row : ROWS) {
        for (const char* c = row; *c; ++c) {
            if (c != row) {
                ImGui::SameLine();
            }
            const char label[] = {*c, '#', '#', 'k', *c, '\0'};
            if (ImGui::Button(label, ImVec2(key, 0.0f))) {
                TypeKey(*c);
            }
            layer.Help(key_help);
            if (keys_focus_q && *c == 'q') {
                keys_focus_q = false;
                Nav::FocusItemNow(true);
            }
        }
    }
    if (ImGui::Button("Space##keys", ImVec2(wide, 0.0f))) {
        TypeKey(' ');
    }
    layer.Help("Types a space (RB too).");
    ImGui::SameLine();
    if (ImGui::Button("Delete##keys", ImVec2(wide, 0.0f))) {
        if (const std::size_t length = std::char_traits<char>::length(search)) {
            search[length - 1] = '\0';
        }
    }
    layer.Help("Deletes the last letter (LB too).");
    ImGui::SameLine();
    if (ImGui::Button("Clear##keys", ImVec2(wide, 0.0f))) {
        search[0] = '\0';
    }
    layer.Help("Deletes the whole text.");
    ImGui::SameLine();
    if (ImGui::Button("Done##keys", ImVec2(wide, 0.0f))) {
        FinishKeys();
    }
    layer.Help("Goes to the results (Y or Start too); B closes the keys and keeps the text.");
    ImGui::PopStyleVar();
    item = {};
    ImGui::Spacing();
}

void Menu::HandleSearchKeys() {
    ImGuiIO& io = ImGui::GetIO();
    // text from any keyboard goes into the search: a Steam Deck's (Steam+X), a real one
    for (const ImWchar c : io.InputQueueCharacters) {
        if (c >= 32 && c < 127) {
            TypeKey(static_cast<char>(c));
        }
    }
    io.InputQueueCharacters.resize(0);
    // Space and Enter of a real keyboard are its text and its Done, not a key button's press
    Nav::TakeKey(ImGuiKey_Space, pad_owner);
    Nav::TakeKey(ImGuiKey_Enter, pad_owner);
    Nav::TakeKey(ImGuiKey_KeypadEnter, pad_owner);
    const std::size_t length = std::char_traits<char>::length(search);
    if (length > 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        search[length - 1] = '\0'; // LB deletes, as in the game's keyboard
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
        TypeKey(' '); // RB types a space
    }
    if (Nav::KeyPressed(ImGuiKey_GamepadFaceUp, pad_owner) || Nav::KeyPressed(ImGuiKey_Enter, pad_owner) ||
        Nav::KeyPressed(ImGuiKey_KeypadEnter, pad_owner)) {
        FinishKeys();
        return;
    }
    // Start finishes when it is let go, unless Back joined it (Back and Start together close the
    // menu), as in the game's keyboard
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
        keys_start_armed = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_GamepadBack)) {
        keys_start_armed = false;
    }
    if (keys_start_armed && ImGui::IsKeyReleased(ImGuiKey_GamepadStart)) {
        keys_start_armed = false;
        FinishKeys();
    }
}

} // namespace App::Ui
