// mh4u: the MH4U PC port's own app. It opens Monster Hunter 4 Ultimate directly in a window of
// its own (SDL2), full screen, on Azahar's 3DS core with the game's recompiled code as the CPU
// (port/src/frontend.h), with the port's screen layouts, the keyboard and a controller mapped
// on their own, and multiplayer rooms joined from its settings. It contains no game code: the
// game is the player's own dump, and the code library is built from it on their machine.
//
// usage: mh4u [GAME] [--windowed | --fullscreen] [--renderer vulkan|opengl|software]
//             [--join HOST[:PORT] | --join mh4u://INVITE] [--host PORT] [--nickname NAME]
//             [--password PASSWORD] [--join-after SECONDS] [--new-console-id]
//             [--user DIR] [--launcher | --no-launcher] [--log FILTER]
// Settings: mh4u-app.ini (the app) and mh4u.ini (the game's options) in Azahar's user folder.
// Before the game, the launcher (launcher/) shows the home screen, setup, mods, room, settings.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <SDL.h>
#include <fmt/format.h>

#include "config.h"
#include "dialog.h"
#include "input.h"
#include "invite.h"
#include "launcher/launcher.h"
#include "launcher/process.h"
#include "multiplayer.h"
#include "runtime_azahar/azahar_runtime.h"
#include "ui/ui.h"
#include "util/file.h"
#include "util/log.h"
#include "util/strings.h"
#include "window.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
extern "C" {
// laptops with two GPUs: run on the dedicated one
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

constexpr const char* TITLE = "MH4U PC port";
constexpr const char* USAGE =
    "usage: mh4u [GAME] [--windowed | --fullscreen] [--renderer vulkan|opengl|software]\n"
    "            [--join HOST[:PORT] | --join mh4u://INVITE] [--host PORT] [--nickname NAME]\n"
    "            [--password PASSWORD] [--join-after SECONDS] [--new-console-id]\n"
    "            [--user DIR] [--launcher | --no-launcher] [--log FILTER]\n"
    "GAME: your decrypted Monster Hunter 4 Ultimate (.3ds, .cci, .cxi); without it, the one in\n"
    "mh4u-app.ini, or the app asks. Settings: mh4u-app.ini and mh4u.ini in Azahar's user folder\n"
    "(--user: another one, e.g. for a portable setup). --no-launcher: straight into the game\n"
    "(launcher = off in mh4u-app.ini), unless the setup is incomplete.\n"
    "--join: a room's address or an invite code; the room is joined before the game starts, or\n"
    "with --join-after that many seconds after it started (as the menu's Join does).\n"
    "--new-console-id: give this console a new ID first (a room lets in one player per console\n"
    "ID, and copies of one Azahar user folder have the same one).\n"
    "Rooms can also be joined, hosted and left in the game: Esc > Multiplayer.\n";

struct Args {
    std::string game, renderer, join, nickname, password, user, log;
    int fullscreen = -1;  // -1: as the settings say
    int host = -1;
    int launcher = -1;       // -1: as the settings say
    double join_after = -1;  // seconds after the game started (the menu's way); -1: before
    bool new_console_id = false;
    std::string check_code;  // the launcher's code check, in a process of its own
    bool help = false, bad = false;
};

Args Parse(const std::vector<std::string>& argv) {
    Args a;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 < argv.size()) {
                return argv[++i];
            }
            a.bad = true;
            return "";
        };
        if (arg == "-h" || arg == "--help") {
            a.help = true;
        } else if (arg == "--windowed") {
            a.fullscreen = 0;
        } else if (arg == "--fullscreen" || arg == "-f") {
            a.fullscreen = 1;
        } else if (arg == "--renderer") {
            a.renderer = value();
        } else if (arg == "--join") {
            a.join = value();
        } else if (arg == "--join-after") {
            a.join_after = std::atof(value().c_str());
            if (a.join_after < 0) {
                a.bad = true;
            }
        } else if (arg == "--new-console-id") {
            a.new_console_id = true;
        } else if (arg == "--host") {
            a.host = std::atoi(value().c_str());
        } else if (arg == "--nickname") {
            a.nickname = value();
        } else if (arg == "--password") {
            a.password = value();
        } else if (arg == "--user") {
            a.user = value();
        } else if (arg == "--log") {
            a.log = value();  // Azahar's log filter, e.g. "*:Info Render.OpenGL:Debug"
        } else if (arg == "--launcher" || arg == "--no-launcher") {
            a.launcher = arg == "--launcher" ? 1 : 0;
        } else if (arg == "--check-code") {
            a.check_code = value();
        } else if (!arg.empty() && arg[0] == '-') {
            a.bad = true;
        } else {
            a.game = arg;
        }
    }
    return a;
}

App::Window::Api ApiFor(const std::string& name) {
    if (name == "opengl") {
        return App::Window::Api::OpenGL;
    }
    if (name == "software") {
        return App::Window::Api::Software;
    }
    return App::Window::Api::Vulkan;
}

App::Renderer RendererOf(App::Window::Api api) {
    switch (api) {
    case App::Window::Api::OpenGL:
        return App::Renderer::OpenGL;
    case App::Window::Api::Software:
        return App::Renderer::Software;
    default:
        return App::Renderer::Vulkan;
    }
}

const char* NameOf(App::Window::Api api) {
    switch (api) {
    case App::Window::Api::OpenGL:
        return "OpenGL";
    case App::Window::Api::Software:
        return "software";
    default:
        return "Vulkan";
    }
}

/// The window for the chosen renderer, or for the next one that works (Vulkan, OpenGL,
/// software); the runtime's active renderer follows.
std::unique_ptr<App::Window> MakeWindow(Core::System& system, App::Window::Api wanted,
                                        const App::Config& config, App::Window::Api& used) {
    std::vector<App::Window::Api> order{wanted};
    for (const auto api : {App::Window::Api::Vulkan, App::Window::Api::OpenGL,
                           App::Window::Api::Software}) {
        if (api != wanted) {
            order.push_back(api);
        }
    }
    for (const auto api : order) {
        App::Runtime().SetActiveRenderer(RendererOf(api));
        if (auto window = App::Window::Create(system, api, config)) {
            if (api != wanted) {
                APP_LOG_WARNING("{} didn't start; using {}", NameOf(wanted), NameOf(api));
            }
            used = api;
            return window;
        }
    }
    return nullptr;
}

std::string LoadError(App::LoadResult result, const std::string& details) {
    switch (result) {
    case App::LoadResult::NotAGame:
        return "This file isn't a 3DS game the app can open. Choose your decrypted Monster Hunter "
               "4 Ultimate (.3ds, .cci or .cxi) in mh4u-app.ini (game = ...) or remove that line to "
               "be asked again.";
    case App::LoadResult::Encrypted:
        return "The game file is encrypted. The port needs a decrypted dump of your own cartridge "
               "(GodMode9 can make one; docs/SETUP.md in the port's repository).";
    case App::LoadResult::MissingSystemFiles:
        return "Some 3DS system files are missing (Azahar's System settings can set them up).";
    default:
        return fmt::format("The game didn't start: {}", details);
    }
}

int Run(const std::vector<std::string>& argv) {
    const auto runtime = std::make_unique<App::Azahar::AzaharRuntime>();
    App::SetRuntime(runtime.get());
    const Args args = Parse(argv);
    if (args.help || args.bad) {
        std::fputs(USAGE, args.bad ? stderr : stdout);
        return args.bad ? 1 : 0;
    }
    if (!args.user.empty()) {
        // before anything looks at the user folder (the log is in it)
        const char last = args.user.back();
        runtime->SetUserDir(last == '/' || last == '\\' ? args.user : args.user + '/');
    }
    // (the launcher's code check logs to a file of its own: the launcher's process has this one)
    runtime->StartLogging(args.check_code.empty() ? "mh4u_log.txt" : "mh4u_check_log.txt", args.log);
    if (!args.check_code.empty()) {
        const int status = Launcher::CheckCode(args.check_code);
        runtime->StopLogging();
        return status;
    }

    const App::BuildInfo build = runtime->Build();
    APP_LOG_INFO("MH4U PC port app, Azahar {} | {}-{}", build.name, build.branch, build.description);

    App::Config config = App::Load();
    if (args.fullscreen >= 0) {
        config.fullscreen = args.fullscreen == 1;
    }
    if (!args.renderer.empty()) {
        config.renderer = args.renderer;
    }
    if (!args.join.empty()) {
        config.join = args.join;
        config.host = 0;
    }
    if (args.host >= 0) {
        config.host = args.host;
    }
    if (!args.nickname.empty()) {
        config.nickname = args.nickname;
    }
    if (!args.password.empty()) {
        config.password = args.password;
    }
    // the game's options, with the recommended settings when there are none yet (as the Qt
    // frontend's Tools > MH4U Port Options... does)
    if (!App::Util::Exists(runtime->GameOptionsPath())) {
        App::Util::WriteStringToFile(true, runtime->GameOptionsPath(), runtime->GameOptionsTemplate());
    }

    if (!App::Window::InitSDL()) {
        return 1;
    }
    runtime->InitInput();
    runtime->InitNetwork();
    // (the windows still take Azahar's system until N2b; Multiplayer::Init and Ui::Start ignore it)
    Core::System& system = *runtime->HostSystem();
    App::Multiplayer::Init(system, config);

    // ── the launcher (launcher/): the home screen, setup, mods, the room and settings; it
    // leaves the game file and the room in `config`. Without a window of its own, as before.
    if (!args.game.empty()) {
        config.game = args.game;
    }
    const bool show_launcher = args.launcher >= 0 ? args.launcher == 1 : config.launcher;
    if (const auto result = Launcher::Run(config, show_launcher);
        result == Launcher::Result::Quit || result == Launcher::Result::Restart) {
        if (result == Launcher::Result::Restart) {
            // an update was put in place: the new app starts on its own once this one has let go
            std::string error;
            const auto command = Launcher::RestartCommand();
            if (!Launcher::StartDetached(command, error)) {
                APP_LOG_ERROR("the updated app didn't start ({}): start it again yourself", error);
                App::Dialog::Message(TITLE, "The update is in place, but the app couldn't start it: open it "
                                            "again yourself.");
            } else {
                APP_LOG_INFO("restarting after the update: {}", command.empty() ? "" : command.front());
            }
        }
        runtime->ShutdownNetwork();
        runtime->ShutdownInput();
        runtime->StopLogging();
        SDL_Quit();
        return 0;
    }
    // ──

    std::string game = config.game;
    if (game.empty() || !App::Util::Exists(game)) {
        if (!game.empty()) {
            APP_LOG_WARNING("{} isn't there any more", game);
        }
        game = App::Dialog::OpenFile(
            "Choose your Monster Hunter 4 Ultimate (decrypted .3ds, .cci or .cxi)",
            {"3ds", "cci", "cxi", "app"});
        if (game.empty()) {
            App::Dialog::Message(TITLE,
                                 "No game chosen. Put the path of your decrypted Monster Hunter 4 "
                                 "Ultimate in " + App::ConfigPath() + " (game = ...), or start "
                                 "the app with the file as its argument.",
                                 true);
            return 1;
        }
        config.game = game;
        App::Save(config);
    }

    runtime->ApplyConfig(config);
    APP_LOG_INFO("{}", App::Input::ControllerDescription());

    runtime->CommitOptions();

    if (args.new_console_id) {
        runtime->NewConsoleId();
    }
    if (args.join_after >= 0) {
        // --join-after: the room is joined from the game, by the menu's own Join
        if (const auto room = App::Invite::Parse(config.join); room.ok) {
            App::Multiplayer::JoinRequest request;
            request.host = room.host;
            request.port = room.port;
            request.password = room.code ? room.password : config.password;
            App::Multiplayer::JoinLater(request, args.join_after);
        } else {
            APP_LOG_ERROR("--join-after: no room to join ({})", room.error);
        }
        config.join.clear();
    }
    if (std::string error; !App::Multiplayer::StartAtBoot(error)) {
        App::Dialog::Message(TITLE, error + " The game starts without the room (Esc > Multiplayer joins "
                             "one while it runs).", true);
    }

    App::LoadRequest request;
    request.game = game;
    APP_LOG_INFO("{}", runtime->PrepareCode(request));
    // (not again when the launcher said it and the player chose to play anyway)
    if (const std::string notice = runtime->CodeNotice(); !notice.empty() && !Launcher::CodeNoticeSeen()) {
        App::Dialog::Message(TITLE, notice);
    }

    App::Window::Api api = ApiFor(config.renderer);
    auto window = MakeWindow(system, api, config, api);
    if (!window) {
        App::Dialog::Message(TITLE, "No renderer could open a window (see mh4u_log.txt).", true);
        App::Multiplayer::Shutdown();
        return 1;
    }
    std::unique_ptr<App::Window> secondary;
    if (runtime->CurrentLayout() == App::ScreenLayout::SeparateWindows &&
        api != App::Window::Api::Software) {
        secondary = App::Window::Create(system, api, config, true);
        if (!secondary) {
            runtime->SetLayout(App::ScreenLayout::Overlay);
        }
    }
    APP_LOG_INFO("renderer: {}", NameOf(api));

    // the in-game menu (Esc, a controller's Guide or Back+Start), and the 3DS software keyboard
    // drawn in it instead of Azahar's default one (ui/)
    App::Ui::Start(*window, config);

    const auto scope = window->Acquire();
    runtime->LogOptions();
    const App::LoadResult loaded = runtime->Load(request, *window, secondary.get());
    if (loaded != App::LoadResult::Ok) {
        App::Dialog::Message(TITLE, LoadError(loaded, runtime->StatusDetails()), true);
        App::Multiplayer::Shutdown();
        runtime->ShutdownNetwork();
        return 1;
    }

    std::thread present([&window] { window->Present(); });
    std::thread present_secondary([&secondary] {
        if (secondary) {
            secondary->Present();
        }
    });

    runtime->Start();

    while (window->IsOpen() && (!secondary || secondary->IsOpen())) {
        const App::RunState state = runtime->RunStep();
        if (state == App::RunState::ShutdownRequested) {
            break;
        }
        if (state == App::RunState::Failed) {
            APP_LOG_ERROR("emulation stopped: {}", runtime->StatusDetails());
            App::Dialog::Message(TITLE, "The game stopped: " + runtime->StatusDetails(), true);
            break;
        }
        App::Multiplayer::Tick();
        if (window->TakeControllersChanged()) {
            App::Input::BuildProfile(config);
            runtime->ReloadInputDevices();
            APP_LOG_INFO("{}", App::Input::ControllerDescription());
        }
    }
    window->RequestClose();
    if (secondary) {
        secondary->RequestClose();
    }
    present.join();
    present_secondary.join();

    runtime->Stop();
    App::Ui::Stop(); // after the renderer, before SDL and the room go
    App::Multiplayer::Shutdown();
    runtime->ShutdownNetwork();
    runtime->ShutdownInput();
    runtime->WaitForBackgroundTasks();
    runtime->StopLogging();
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // the arguments as UTF-16, so that paths with any characters arrive whole (MinGW's runtime
    // calls main even in a windowed program, with the arguments in the ANSI code page)
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> args;
    for (int i = 0; i < wargc; ++i) {
        args.push_back(App::Util::WideToUtf8(std::wstring(wargv[i])));
    }
    LocalFree(wargv);
    return Run(args);
#else
    return Run(std::vector<std::string>(argv, argv + argc));
#endif
}
