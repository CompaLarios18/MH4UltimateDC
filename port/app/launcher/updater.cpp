// Updates of the port itself: the launcher asks GitHub for the latest release when it opens
// (tools/update.py check, mh4u-app.ini's check_updates), Home shows a card when there is a newer
// one, and Settings has the Updates row; Update now downloads the release's package for this
// system, puts it over the installed one (update.py install, in the job window) and starts the
// new app.
#include <fmt/format.h>
#include <imgui.h>
#include <json.hpp>
#include <SDL.h>

#include "launcher/state.h"
#include "launcher/ui.h"
#include "util/log.h"

#ifndef MH4U_PORT_COMMIT
#define MH4U_PORT_COMMIT ""
#endif

namespace Launcher {

namespace {

using Json = nlohmann::json;

std::vector<std::string> g_restart;

/// update.py --json's one JSON object (after its progress lines).
Json ParseResult(const std::string& output) {
    const auto j = Json::parse(Tools::WithoutProgress(output), nullptr, false);
    return j.is_object() ? j : Json::object();
}

std::string Str(const Json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::string Error(const Json& j, const std::string& output) {
    if (j.contains("error") && j["error"].is_object()) {
        if (std::string message = Str(j["error"], "message"); !message.empty()) {
            return message;
        }
    }
    const auto lines = Tools::WithoutProgress(output);
    return lines.empty() ? "update.py didn't say why." : lines;
}

std::string SizeText(long long bytes) {
    return bytes > 0 ? fmt::format(" ({:.0f} MB)", static_cast<double>(bytes) / 1048576.0) : "";
}

void OpenPage(State& s, const std::string& url) {
    if (url.empty() || SDL_OpenURL(url.c_str()) != 0) {
        s.Message("It can't be opened from here", "Open it yourself: " + url);
    }
}

} // namespace

std::vector<std::string> RestartCommand() {
    return g_restart;
}

void State::CheckForUpdate(bool manual) {
    if (updates.checking || !ToolsReady()) {
        return;
    }
    if (updates.root.empty()) {
        updates.root = Tools::PackageRoot(tools_dir);
    }
    if (updates.root.empty()) {
        if (manual) {
            Message("Updates", "This app wasn't installed from a release package (it was built from the source): "
                               "git pull and build it again instead.");
        }
        return;
    }
    updates.checking = true;
    updates.error.clear();
    runner.Queue(Tools::UpdateCommand(python, tools_dir,
                                      {"check", "--platform", Tools::Platform(), "--commit", MH4U_PORT_COMMIT,
                                       "--to", updates.root}),
                 "Looking for updates", [this, manual](int code, const std::string& output) {
                     updates.checking = false;
                     updates.checked = true;
                     const Json j = ParseResult(output);
                     if (code != 0 || !j.value("ok", false)) {
                         updates.status.clear();
                         updates.error = Error(j, output);
                         APP_LOG_WARNING("launcher: update check: {}", updates.error);
                         if (manual) {
                             Message("Updates", "It couldn't look for updates: " + updates.error);
                         }
                         return;
                     }
                     updates.status = Str(j, "status");
                     updates.tag = Str(j, "tag");
                     updates.name = Str(j, "name");
                     updates.url = Str(j, "url");
                     updates.size = j.contains("size") && j["size"].is_number() ? j["size"].get<long long>() : -1;
                     APP_LOG_INFO("launcher: latest release {}: {}", updates.tag, updates.status);
                     if (manual && updates.status != "update") {
                         Message("Updates", updates.status == "current"
                                                ? fmt::format("You have the latest release, {}.", updates.name)
                                                : fmt::format("The latest release is {}. This app was built from "
                                                              "another commit than a release ({}), so it can't tell "
                                                              "which is newer: Update now installs {}.",
                                                              updates.name, MH4U_PORT_COMMIT, updates.tag));
                     }
                 });
}

void State::InstallUpdate() {
    if (job.Active() || !ToolsReady() || updates.root.empty()) {
        return;
    }
    auto process = std::make_unique<Process>();
    std::string error;
    std::vector<std::string> args{"install", "--platform", Tools::Platform(), "--to", updates.root};
    if (!updates.tag.empty()) {
        args.insert(args.end(), {"--tag", updates.tag});
    }
    if (!process->Start(Tools::UpdateCommand(python, tools_dir, args), error)) {
        Message("Update", error);
        return;
    }
    const std::string title = fmt::format("Updating to {}", updates.name.empty() ? updates.tag : updates.name);
    APP_LOG_INFO("launcher: {}", title);
    job = Job{};
    job.kind = Job::Kind::Download;  // update.py's progress lines are mods.py's kind
    job.title = title;
    job.cancellable = true;
    job.close_when_done = true;
    job.stop_question = "Stop the update? Nothing is replaced until the download is complete.";
    job.keep_label = "Keep going";
    job.stop_label = "Stop it";
    job.process = std::move(process);
    job.on_done = [this](int code, const std::string& output) {
        if (job.process && job.process->WasStopped()) {
            return;
        }
        const Json j = ParseResult(output);
        if (code != 0 || !j.value("ok", false)) {
            const std::string why = Error(j, output);
            APP_LOG_WARNING("launcher: update: {}", why);
            Message("It wasn't updated", why + "\n\nThe app you have is as it was.");
            return;
        }
        updates.app = Str(j, "app");
        updates.status = "current";
        Dialog d;
        d.title = fmt::format("Updated to {}", Str(j, "name"));
        d.text = "Start the new version now? Your settings, saves, mods and code library stay as they are. "
                 "If the new version needs the code library built again, the launcher says so.";
        d.buttons = {{"Restart now",
                      [this] {
                          const std::string& app = updates.app;
                          g_restart = app.size() > 4 && app.compare(app.size() - 4, 4, ".app") == 0
                                          ? std::vector<std::string>{"open", "-n", app}
                                          : std::vector<std::string>{app};
                          result = Result::Restart;
                          done = true;
                      },
                      true},
                     {"Later", nullptr, false}};
        Show(std::move(d));
    };
}

void DrawUpdateCard(State& s) {
    if (s.updates.status != "update") {
        return;
    }
    Ui::BeginCard("##update", ImVec2(0, 0), true);
    Ui::PushFont(0.85f, true);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Ui::Color::Accent), "UPDATE");
    Ui::PopFont();
    Ui::TextColored(Ui::Color::Text, fmt::format("{} is out{}.", s.updates.name, SizeText(s.updates.size)));
    ImGui::BeginDisabled(s.job.Active() || !s.ToolsReady());
    if (Ui::AccentButton("Update now##update_now")) {
        s.InstallUpdate();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("What's new##update_notes")) {
        OpenPage(s, s.updates.url);
    }
    Ui::EndCard();
    ImGui::Spacing();
}

void DrawUpdateSettings(State& s) {
    auto& u = s.updates;
    std::string line;
    if (u.root.empty() && !s.tools_dir.empty()) {
        u.root = Tools::PackageRoot(s.tools_dir);
    }
    if (!s.ToolsReady()) {
        line = s.probing ? "Looking for the port's tools..." : "Updates need the port's tools and Python.";
    } else if (u.root.empty()) {
        line = "Built from the source: git pull and build again.";
    } else if (u.checking) {
        line = "Looking for updates...";
    } else if (!u.error.empty()) {
        line = "It couldn't look: " + u.error;
    } else if (u.status == "update") {
        line = fmt::format("{} is out{}.", u.name, SizeText(u.size));
    } else if (u.status == "current") {
        line = fmt::format("Up to date ({}).", u.name);
    } else if (u.checked) {
        line = fmt::format("The latest release is {}.", u.name);
    } else {
        line = "Not checked yet.";
    }
    Ui::TextColored(u.status == "update" ? Ui::Color::Accent : Ui::Color::Text, line);
    const bool can = s.ToolsReady() && !u.root.empty() && !u.checking;
    ImGui::BeginDisabled(!can || s.job.Active());
    if (u.status == "update" || (u.checked && u.status != "current" && !u.tag.empty())) {
        if (Ui::AccentButton("Update now##settings_update")) {
            s.InstallUpdate();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Check now##check_updates")) {
        s.CheckForUpdate(true);
    }
    ImGui::EndDisabled();
    if (!u.url.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("What's new##settings_notes")) {
            OpenPage(s, u.url);
        }
    }
    bool automatic = s.config.check_updates;
    if (Ui::Toggle(automatic ? "Look when the launcher opens##auto_updates" : "Only when I press Check now##auto_updates",
                   &automatic)) {
        s.Change([&](App::Config& cfg) { cfg.check_updates = automatic; });
    }
}

} // namespace Launcher
