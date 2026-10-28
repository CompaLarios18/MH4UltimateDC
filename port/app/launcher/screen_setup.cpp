// Setup, also the first-run wizard: the game file, its update (installed from the
// player's .cia here) and the code library (built here by tools/build_game.py).
#include <fmt/format.h>
#include <imgui.h>

#include "launcher/state.h"
#include "launcher/ui.h"

namespace Launcher {

SetupRow GameRow(const State& s) {
    using State_ = Checks::Game::State;
    switch (s.game.state) {
    case State_::Ok:
        return {Ui::Status::Ok, "Game", FileName(s.config.game)};
    case State_::Unset:
        return {Ui::Status::Error, "Game", "Not chosen yet"};
    default:
        return {Ui::Status::Error, "Game", Checks::Describe(s.game)};
    }
}

SetupRow UpdateRow(const State& s) {
    using State_ = Checks::Update::State;
    // v1.3.0, or Europe's v1.1.0
    const std::string name = Checks::UpdateName(s.game.program_id);
    switch (s.update.state) {
    case State_::Ok:
        // (a note when its version label isn't the update's: its code is)
        return {Ui::Status::Ok, "Update " + name,
                s.update.note.empty() ? "Installed in Azahar's virtual SD card" : s.update.note};
    case State_::OtherVersion:
        // (which version, by its label and its code)
        return {Ui::Status::Error, "Update " + name,
                s.update.note.empty() ? fmt::format("v{} is installed; the port is made for {}",
                                                    Checks::VersionText(s.update.version), name)
                                      : s.update.note};
    case State_::Missing:
        break;
    }
    return {Ui::Status::Error, "Update " + name, "Not installed"};
}

SetupRow CodeRow(const State& s) {
    using State_ = Checks::Code::State;
    switch (s.code.state) {
    case State_::Missing:
        return {Ui::Status::Error, "Code library", "Not built yet"};
    case State_::Checking:
        return {Ui::Status::Busy, "Code library", "Checking..."};
    case State_::Ok:
        return {Ui::Status::Ok, "Code library", "Built from the code the game runs"};
    case State_::Partial:
        return {Ui::Status::Warn, "Code library", s.code.notice};
    case State_::NotUsed:
        return {Ui::Status::Error, "Code library", s.code.notice};
    case State_::Failed:
        return {Ui::Status::Warn, "Code library", "Couldn't be checked: " + s.code.notice};
    case State_::Unknown:
        break;
    }
    return {Ui::Status::Off, "Code library",
            s.library_exists ? "Installed; checked once the game is chosen" : "Not built yet"};
}

namespace {

void StepTitle(int number, const char* title, bool done) {
    const float scale = Ui::Scale();
    const float d = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(d, d));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(at.x + d * 0.5f, at.y + d * 0.5f);
    dl->AddCircleFilled(c, d * 0.5f, done ? Ui::Color::Ok : Ui::Color::Accent);
    const std::string n = std::to_string(number);
    Ui::PushFont(1.0f, true);
    const ImVec2 size = ImGui::CalcTextSize(n.c_str());
    dl->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), Ui::Color::OnAccent, n.c_str());
    Ui::PopFont();
    ImGui::SameLine(0, 12.0f * scale);
    Ui::PushFont(1.2f, true);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (d - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted(title);
    Ui::PopFont();
}

} // namespace

void DrawSetup(State& s) {
    using Game = Checks::Game::State;
    using Update = Checks::Update::State;
    using Code = Checks::Code::State;
    const bool game_ok = s.game.state == Game::Ok;
    const bool update_ok = s.update.state == Update::Ok;
    const bool code_ok = s.code.state == Code::Ok || (s.library_exists && s.code.state == Code::Checking);
    const int current = !game_ok ? 1 : !update_ok ? 2 : !s.library_exists || !code_ok ? 3 : 0;
    if (current != s.setup_step) {
        // a step got done: on to the next one's button
        s.setup_step = current;
        s.focus_first = true;
    }

    ImGui::BeginChild("##setup_scroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    Ui::Heading(s.wizard ? "Welcome: three steps, once" : "Setup");
    Ui::TextDim("The port plays your own copy of Monster Hunter 4 Ultimate (USA or Europe) with the "
                "game's code recompiled for this computer. It needs your decrypted game, its update "
                "(USA v1.3.0, Europe v1.1.0) and the code library, built here from your update. "
                "Nothing of the game leaves this computer.");
    ImGui::Spacing();

    // 1. the game
    Ui::BeginCard("##step1", ImVec2(0, 0), current == 1);
    StepTitle(1, "Your game", game_ok);
    const auto game = GameRow(s);
    const std::string game_name =
        fmt::format("Monster Hunter 4 Ultimate ({})", Checks::RegionOf(s.game.program_id).name);
    Ui::StatusLine(game.status, game_ok ? game_name : std::string("No game yet"),
                   game_ok ? s.config.game : game.detail);
    Ui::TextDim("A decrypted dump of your cartridge or digital copy: .3ds, .cci or .cxi (GodMode9 makes "
                "one on your 3DS).");
    if (current == 1) {
        FocusNext(s);
    }
    if (game_ok ? ImGui::Button("Choose another game file...") : Ui::AccentButton("Choose the game file...")) {
        s.ChooseGame();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse in the launcher...##game")) {
        s.ChooseGame(true);
    }
    Ui::EndCard();
    ImGui::Spacing();

    // 2. the update
    Ui::BeginCard("##step2", ImVec2(0, 0), current == 2);
    StepTitle(2, ("The " + Checks::UpdateName(s.game.program_id) + " update").c_str(), update_ok);
    const auto update = UpdateRow(s);
    Ui::StatusLine(update.status, update.label, update.detail);
    Ui::TextDim("The port runs the update's code. Install it from your decrypted update (.cia; GodMode9 "
                "builds one from the update installed on your 3DS). It goes into Azahar's virtual SD "
                "card, which the port's Azahar shares.");
    ImGui::BeginDisabled(s.job.Active());
    if (current == 2) {
        FocusNext(s, !s.job.Active());
    }
    if (update_ok ? ImGui::Button("Install it again...") : Ui::AccentButton("Install the update from a .cia...")) {
        s.ChooseAndInstallUpdate();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse in the launcher...##update")) {
        s.ChooseAndInstallUpdate(true);
    }
    ImGui::EndDisabled();
    Ui::EndCard();
    ImGui::Spacing();

    // 3. the code library
    Ui::BeginCard("##step3", ImVec2(0, 0), current == 3);
    StepTitle(3, "The code library", s.code.state == Code::Ok);
    const auto code = CodeRow(s);
    Ui::StatusLine(code.status, code.label, code.detail);
    Ui::TextDim("The game's code, recompiled and compiled for this computer from the installed update by "
                "the port's tools (Python 3.10 or newer). It takes several minutes and all of the "
                "processor; the first time it also downloads a C compiler (80 to 180 MB) if there is none.");
    // the tools and Python
    if (s.probing) {
        Ui::Spinner();
        ImGui::SameLine();
        Ui::TextDim("Looking for the port's tools and Python...");
    } else if (s.tools_dir.empty()) {
        Ui::StatusLine(Ui::Status::Error, "The port's tools aren't here",
                       "Keep the package's mh4u-tools folder next to the app.");
    } else if (s.python.command.empty()) {
        Ui::StatusLine(Ui::Status::Error, "Python 3.10 or newer is needed", Tools::PythonHint(s.python));
        if (ImGui::Button("Check again")) {
            s.ProbeTools();
        }
    } else {
        Ui::StatusLine(Ui::Status::Ok, "Tools ready", fmt::format("Python {}", s.python.version));
    }
    const bool can_build = update_ok && s.ToolsReady() && !s.job.Active();
    ImGui::BeginDisabled(!can_build);
    if (current == 3) {
        FocusNext(s, can_build);
    }
    const bool rebuild = s.library_exists && s.code.state != Code::NotUsed && s.code.state != Code::Partial;
    if (rebuild ? ImGui::Button("Build it again") : Ui::AccentButton("Build the code library")) {
        s.StartBuild();
    }
    ImGui::EndDisabled();
    if (!update_ok) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Ui::TextDim("Install the update first.");
    }
    Ui::EndCard();
    ImGui::Spacing();

    if (current == 0) {
        FocusNext(s);
        if (Ui::AccentButton(s.wizard ? "All set: continue" : "Back to Home")) {
            s.wizard = false;
            s.GoTo(Screen::Home);
        }
    } else if (ImGui::Button("Back to Home")) {
        s.GoTo(Screen::Home);
    }
    ImGui::EndChild();
}

} // namespace Launcher
