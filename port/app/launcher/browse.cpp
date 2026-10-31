#include "launcher/browse.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "launcher/ui.h"

namespace Launcher::Browse {

namespace {

constexpr std::size_t HISTORY = 100;  // folders B goes back through

std::string SizeText(std::uint64_t n) {
    if (n >= (1ULL << 30)) {
        return fmt::format("{:.1f} GB", static_cast<double>(n) / static_cast<double>(1ULL << 30));
    }
    if (n >= (1ULL << 20)) {
        return fmt::format("{:.1f} MB", static_cast<double>(n) / static_cast<double>(1ULL << 20));
    }
    return fmt::format("{} KB", std::max<std::uint64_t>(1, (n + 512) / 1024));
}

std::string Count(std::size_t n, const char* one, const char* many) {
    if (n == 0) {
        return fmt::format("no {}", many);
    }
    return n == 1 ? fmt::format("1 {}", one) : fmt::format("{} {}", n, many);
}

/// The extensions to read: ".3ds, .cci or .cxi".
std::string ExtensionList(const std::vector<std::string>& extensions) {
    std::string out;
    for (std::size_t i = 0; i < extensions.size(); ++i) {
        out += (i == 0 ? "" : i + 1 == extensions.size() ? " or " : ", ") + std::string(".") + extensions[i];
    }
    return out;
}

/// A group's heading, as on the Home screen's cards.
void SmallHeading(const char* text) {
    Ui::PushFont(0.8f, true);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Ui::Color::Dim), "%s", text);
    Ui::PopFont();
}

/// `text` on one line between `min` and `max`, in the middle of their height, cut short with an
/// ellipsis when it is longer (a name is shown as it is, ## and all).
void LineText(const ImVec2& min, const ImVec2& max, const std::string& text, ImU32 color) {
    if (max.x <= min.x || text.empty()) {
        return;
    }
    const ImVec2 at(min.x, min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, ImVec2(max.x, max.y), max.x, text.c_str(),
                              text.c_str() + text.size(), nullptr);
    ImGui::PopStyleColor();
}

/// A folder `h` tall, its top left at `at`.
void FolderGlyph(ImDrawList* dl, const ImVec2& at, float h, ImU32 color) {
    const float w = h * 1.2f;
    const float r = h * 0.12f;
    const float tab = h * 0.22f;
    dl->AddRectFilled(at, ImVec2(at.x + w * 0.45f, at.y + tab * 2.0f), color, r);
    dl->AddRectFilled(ImVec2(at.x, at.y + tab), ImVec2(at.x + w, at.y + h), color, r);
}

/// A file: a page with a corner folded.
void FileGlyph(ImDrawList* dl, const ImVec2& at, float h, ImU32 color) {
    const float w = h * 0.8f;
    const float fold = w * 0.38f;
    const float t = std::max(1.0f, h * 0.09f);
    const float x = at.x + h * 0.2f;  // (in the middle of a folder's width)
    dl->PathLineTo(ImVec2(x, at.y));
    dl->PathLineTo(ImVec2(x + w - fold, at.y));
    dl->PathLineTo(ImVec2(x + w, at.y + fold));
    dl->PathLineTo(ImVec2(x + w, at.y + h));
    dl->PathLineTo(ImVec2(x, at.y + h));
    dl->PathStroke(color, t, ImDrawFlags_Closed);
    dl->PathLineTo(ImVec2(x + w - fold, at.y));
    dl->PathLineTo(ImVec2(x + w - fold, at.y + fold));
    dl->PathLineTo(ImVec2(x + w, at.y + fold));
    dl->PathStroke(color, t);
}

/// The folder above: an arrow up.
void UpGlyph(ImDrawList* dl, const ImVec2& at, float h, ImU32 color) {
    const float c = at.x + h * 0.6f;
    const float t = std::max(1.5f, h * 0.12f);
    dl->AddTriangleFilled(ImVec2(c, at.y), ImVec2(c + h * 0.42f, at.y + h * 0.45f),
                          ImVec2(c - h * 0.42f, at.y + h * 0.45f), color);
    dl->AddLine(ImVec2(c, at.y + h * 0.4f), ImVec2(c, at.y + h), color, t);
}

} // namespace

Browser::Browser(Pick::Request request_, std::string note_, std::string last, bool system_)
    : request(std::move(request_)), note(std::move(note_)), system(system_), home(Paths::Home()),
      places(Paths::Places()) {
    // the folder of the last choice, first, when it isn't one of those
    if (!last.empty() && Paths::What(last) == Paths::Kind::Folder) {
        const std::string folder = Paths::Clean(last);
        if (std::none_of(places.begin(), places.end(), [&folder](const Paths::Place& p) { return p.path == folder; })) {
            places.insert(places.begin(), Paths::Place{"Last folder", folder, false, false});
        }
    }
    Open(!request.start.empty() ? request.start : !last.empty() ? last : home);
    focus_name = request.select;
}

Pick::Request Browser::Asked() const {
    Pick::Request asked = request;
    asked.start = listing.folder;
    asked.select.clear();
    return asked;
}

bool Browser::Files() const {
    return !request.folder || !request.extensions.empty();
}

bool Browser::Go(const std::string& folder, bool remember) {
    Paths::Listing next = Paths::List(folder, request.extensions, Files());
    if (!next.error.empty()) {
        message = fmt::format("{} can't be opened: {}", Paths::Name(folder), next.error);
        return false;
    }
    if (remember && !listing.folder.empty() && listing.folder != next.folder) {
        history.push_back(listing.folder);
        if (history.size() > HISTORY) {
            history.erase(history.begin());
        }
    }
    listing = std::move(next);
    typed = listing.folder;
    message.clear();
    focus_name.clear();
    focus_first = true;
    scroll_top = true;
    return true;
}

void Browser::Up() {
    const std::string parent = Paths::Parent(listing.folder);
    if (parent.empty()) {
        return;
    }
    const std::string left = Paths::Name(listing.folder);
    if (Go(parent, true)) {
        focus_name = left;
    }
}

bool Browser::Back() {
    while (!history.empty()) {
        const std::string left = listing.folder;
        const std::string folder = history.back();
        history.pop_back();
        if (Go(folder, false)) {
            if (Paths::Parent(left) == listing.folder) {
                focus_name = Paths::Name(left);
            }
            return true;
        }
    }
    return false;
}

void Browser::Open(const std::string& start) {
    // the folder asked for, else the nearest one above it that can be read, else home
    for (std::string folder = Paths::Clean(start); !folder.empty(); folder = Paths::Parent(folder)) {
        if (Paths::What(folder) == Paths::Kind::Folder && Go(folder, false)) {
            return;
        }
    }
    if (!home.empty() && Go(home, false)) {
        return;
    }
    for (const auto& place : places) {
        if (Go(place.path, false)) {
            return;
        }
    }
}

void Browser::Submit(Outcome& outcome) {
    const std::string path = Paths::FromTyped(typed, listing.folder, home);
    if (path.empty()) {
        typed = listing.folder;
        return;
    }
    switch (Paths::What(path)) {
    case Paths::Kind::Folder:
        Go(path, true);
        break;
    case Paths::Kind::File:
        if (Files()) {
            // (any file typed, as the system's dialog takes with All files: the launcher's checks
            // say when it won't do)
            chosen = path;
            outcome = Outcome::Chosen;
        } else if (Go(Paths::Parent(path), true)) {
            focus_name = Paths::Name(path);
        }
        break;
    case Paths::Kind::Missing:
        message = fmt::format("Nothing is at {}.", path);
        break;
    }
}

std::string Browser::Summary() const {
    std::size_t folders = 0, files = 0;
    for (const auto& e : listing.entries) {
        ++(e.folder ? folders : files);
    }
    std::string text = Count(folders, "folder", "folders");
    if (Files()) {
        text += ", " + Count(files, "file", "files");
        if (!request.extensions.empty()) {
            text += " (" + ExtensionList(request.extensions) + ")";
        }
    }
    if (listing.left_out > 0) {
        text += fmt::format(", and {} more not shown: type the path", listing.left_out);
    }
    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

void Browser::DrawPlaces() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row = ImGui::GetTextLineHeight() + style.FramePadding.y * 1.2f;
    const float pad = style.FramePadding.x * 0.5f;
    std::string go;
    SmallHeading("PLACES");
    bool drives = false;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const Paths::Place& place = places[i];
        if (place.drive && !drives) {
            ImGui::Spacing();
            SmallHeading("DRIVES");
            drives = true;
        }
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable("##place", place.path == listing.folder, ImGuiSelectableFlags_NoAutoClosePopups,
                              ImVec2(0, row))) {
            go = place.path;
        }
        if (ImGui::IsItemVisible()) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            LineText(ImVec2(min.x + pad, min.y), ImVec2(max.x - pad, max.y), place.label, Ui::Color::Text);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", place.path.c_str());
        }
        ImGui::PopID();
    }
    if (!go.empty()) {
        Go(go, true);
    }
}

void Browser::DrawEntries(bool pad, Outcome& outcome) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::string parent = Paths::Parent(listing.folder);
    const int first = parent.empty() ? 0 : 1;  // the first entry's row (the folder above's before it)
    const int rows = first + static_cast<int>(listing.entries.size());
    // the row that gets the focus: the folder just left (or the one chosen last time), else the
    // first entry
    int focus = -1;
    if (!focus_name.empty()) {
        for (std::size_t i = 0; i < listing.entries.size() && focus < 0; ++i) {
            if (listing.entries[i].name == focus_name) {
                focus = first + static_cast<int>(i);
            }
        }
        focus_name.clear();
        focus_first = focus_first || focus < 0;
    }
    if (focus < 0 && focus_first) {
        focus = rows > first ? first : rows - 1;
    }
    focus_first = false;

    const float row = ImGui::GetTextLineHeight() + style.FramePadding.y * 1.3f;
    const float glyph = ImGui::GetTextLineHeight() * 0.8f;
    const float pad_x = style.FramePadding.x * 0.6f;
    const float text_x = pad_x + glyph * 1.2f + style.ItemInnerSpacing.x * 1.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 2.0f * Ui::Scale()));
    int activated = -1;
    ImGuiListClipper clipper;
    clipper.Begin(rows, row + ImGui::GetStyle().ItemSpacing.y);
    if (focus >= 0) {
        clipper.IncludeItemByIndex(focus);
    }
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            ImGui::PushID(r);
            if (r == focus) {
                // (as FocusNext does on the screens)
                ImGui::SetKeyboardFocusHere();
                if (pad || GImGui->NavInputSource == ImGuiInputSource_Gamepad || GImGui->NavCursorVisible) {
                    ImGui::SetNavCursorVisible(true);
                }
                focus = -1;
            }
            if (ImGui::Selectable("##entry", false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(0, row))) {
                activated = r;
            }
            if (ImGui::IsItemVisible()) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                const ImVec2 at(min.x + pad_x, min.y + (max.y - min.y - glyph) * 0.5f);
                float right = max.x - pad_x;
                if (r < first) {
                    UpGlyph(dl, at, glyph, Ui::Color::Dim);
                    const float dots = ImGui::CalcTextSize("..").x + style.ItemInnerSpacing.x * 2.0f;
                    LineText(ImVec2(min.x + text_x, min.y), ImVec2(right, max.y), "..", Ui::Color::Text);
                    LineText(ImVec2(min.x + text_x + dots, min.y), ImVec2(right, max.y),
                             "the folder above: " + Paths::Name(parent), Ui::Color::Dim);
                } else {
                    const Paths::Entry& e = listing.entries[static_cast<std::size_t>(r - first)];
                    if (e.folder) {
                        FolderGlyph(dl, at, glyph, Ui::Color::Accent);
                    } else {
                        FileGlyph(dl, at, glyph, Ui::Color::Info);
                        const std::string size = SizeText(e.size);
                        const float width = ImGui::CalcTextSize(size.c_str()).x;
                        LineText(ImVec2(right - width, min.y), ImVec2(right, max.y), size, Ui::Color::Dim);
                        right -= width + style.ItemSpacing.x * 2.0f;
                    }
                    LineText(ImVec2(min.x + text_x, min.y), ImVec2(right, max.y), e.name, Ui::Color::Text);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
    if (listing.entries.empty()) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);
        if (!Files()) {
            Ui::TextDim("No folders in here. To pick this one, press Choose this folder.");
        } else if (request.extensions.empty()) {
            Ui::TextDim("Nothing in here.");
        } else {
            Ui::TextDim(fmt::format("No folders in here, and no {} files.", ExtensionList(request.extensions)));
        }
    }
    // (after the list: a folder opened replaces the entries it was drawn from)
    if (activated >= 0) {
        if (activated < first) {
            Up();
        } else {
            const Paths::Entry entry = listing.entries[static_cast<std::size_t>(activated - first)];
            if (entry.folder) {
                Go(entry.path, true);
            } else {
                chosen = entry.path;
                outcome = Outcome::Chosen;
            }
        }
    }
}

Outcome Browser::Draw(bool pad) {
    if (!opened) {
        ImGui::OpenPopup("##browse");
        opened = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = Ui::Scale();
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(display.x * 0.94f, 1180.0f * scale), display.y * 0.9f),
                             ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##browse", nullptr,
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return Outcome::Cancelled;  // closed from elsewhere
    }
    Outcome outcome = Outcome::Open;
    const ImGuiStyle& style = ImGui::GetStyle();

    // the keys first (a folder they open is drawn this frame), not while the path line is typed in:
    // Esc or B back to the folder before, and out at the first one; Backspace the folder above
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (!Back()) {
                outcome = Outcome::Cancelled;
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            Up();
        } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L)) {
            focus_field = true;
        }
    }

    Ui::Heading(request.title);
    if (!note.empty()) {
        Ui::TextColored(Ui::Color::Warn, note);
    }
    ImGui::Spacing();

    // the path line: typed or pasted, then Enter or Go
    const float go_width = ImGui::CalcTextSize("Go").x + style.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - go_width - style.ItemSpacing.x, 1.0f));
    if (focus_field) {
        ImGui::SetKeyboardFocusHere();
        focus_field = false;
    }
    const bool entered = Ui::InputText("##path", &typed, ImGuiInputTextFlags_EnterReturnsTrue,
                                       "A folder or a file: type or paste its path");
    ImGui::SameLine();
    if (ImGui::Button("Go") || entered) {
        Submit(outcome);
    }
    ImGui::Spacing();

    // the places on the left, the folder's entries on the right
    const float bottom = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f;
    const float height = std::max(ImGui::GetContentRegionAvail().y - bottom, ImGui::GetFrameHeight() * 3.0f);
    const float side = std::clamp(ImGui::GetContentRegionAvail().x * 0.27f, 150.0f * scale, 320.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Ui::Color::Card);
    if (ImGui::BeginChild("##places", ImVec2(side, height), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened)) {
        DrawPlaces();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (scroll_top) {
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        scroll_top = false;
    }
    if (ImGui::BeginChild("##entries", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened)) {
        DrawEntries(pad, outcome);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // what the folder has, or what went wrong
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    LineText(at, ImVec2(at.x + width, at.y + line), message.empty() ? Summary() : message,
             message.empty() ? Ui::Color::Dim : Ui::Color::Error);
    ImGui::Dummy(ImVec2(width, line));

    // the buttons, then the keys
    const float line_y = ImGui::GetCursorPosY();
    if (request.folder) {
        if (Ui::AccentButton("Choose this folder")) {
            chosen = listing.folder;
            outcome = Outcome::Chosen;
        }
        ImGui::SameLine();
    }
    if (system) {
        if (ImGui::Button("System file dialog...")) {
            outcome = Outcome::System;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) {
        outcome = Outcome::Cancelled;
    }
    if (ImGui::IsWindowAppearing() && listing.entries.empty() && Paths::Parent(listing.folder).empty()) {
        ImGui::SetItemDefaultFocus();  // (no row to take the focus)
    }
    const auto hint = [&](const char* key, const char* action) {
        ImGui::SameLine(0, 24.0f * scale);
        ImGui::SetCursorPosY(line_y + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
        Ui::KeyHint(key, action);
    };
    hint(pad ? "A" : "Enter", "Open");
    hint(pad ? "B" : "Esc", "Back");
    if (!pad) {
        hint("Backspace", "Folder above");
    }

    if (outcome != Outcome::Open) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return outcome;
}

} // namespace Launcher::Browse
