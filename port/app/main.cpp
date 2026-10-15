// mh4u: the MH4U PC port's own app. It opens Monster Hunter 4 Ultimate directly in a window of
// its own (SDL2), full screen, on Azahar's 3DS core with the game's recompiled code as the CPU
// (port/src/frontend.h), with the port's screen layouts, the keyboard and a controller mapped
// on their own, and multiplayer rooms joined from its settings. It contains no game code: the
// game is the player's own dump, and the code library is built from it on their machine.
//
// usage: mh4u [GAME] [--windowed | --fullscreen] [--renderer vulkan|opengl|software]
//             [--join HOST[:PORT]] [--host PORT] [--nickname NAME] [--password PASSWORD]
//             [--user DIR] [--launcher | --no-launcher]
// Settings: mh4u-app.ini (the app) and mh4u.ini (the game's options) in Azahar's user folder.
// Before the game, the launcher (launcher/) shows the home screen, setup, mods, room, settings.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <SDL.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "config.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/ir/ir_user.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "dialog.h"
#include "frontend.h"
#include "input.h"
#include "input_common/main.h"
#include "launcher/launcher.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/verify_user.h"
#include "ui/ui.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "window.h"

#ifdef ENABLE_WEB_SERVICE
#include "network/announce_multiplayer_session.h"
#include "web_service/verify_user_jwt.h"
#endif

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
    "            [--join HOST[:PORT]] [--host PORT] [--nickname NAME] [--password PASSWORD]\n"
    "            [--user DIR] [--launcher | --no-launcher]\n"
    "GAME: your decrypted Monster Hunter 4 Ultimate (.3ds, .cci, .cxi); without it, the one in\n"
    "mh4u-app.ini, or the app asks. Settings: mh4u-app.ini and mh4u.ini in Azahar's user folder\n"
    "(--user: another one, e.g. for a portable setup). --no-launcher: straight into the game\n"
    "(launcher = off in mh4u-app.ini), unless the setup is incomplete.\n";

struct Args {
    std::string game, renderer, join, nickname, password, user;
    int fullscreen = -1;  // -1: as the settings say
    int host = -1;
    int launcher = -1;       // -1: as the settings say
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
        } else if (arg == "--host") {
            a.host = std::atoi(value().c_str());
        } else if (arg == "--nickname") {
            a.nickname = value();
        } else if (arg == "--password") {
            a.password = value();
        } else if (arg == "--user") {
            a.user = value();
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

Settings::GraphicsAPI SettingFor(App::Window::Api api) {
    switch (api) {
    case App::Window::Api::OpenGL:
        return Settings::GraphicsAPI::OpenGL;
    case App::Window::Api::Software:
        return Settings::GraphicsAPI::Software;
    default:
        return Settings::GraphicsAPI::Vulkan;
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
/// software); Settings::values.graphics_api follows.
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
        Settings::values.graphics_api = SettingFor(api);
        if (auto window = App::Window::Create(system, api, config)) {
            if (api != wanted) {
                LOG_WARNING(Frontend, "{} didn't start; using {}", NameOf(wanted), NameOf(api));
            }
            used = api;
            return window;
        }
    }
    return nullptr;
}

/// The input devices again, after the profile changed (a controller came or went), without
/// the rest of System::ApplySettings (which also restarts the sound).
void ReloadInput(Core::System& system) {
    if (const auto hid = Service::HID::GetModule(system)) {
        hid->ReloadInputDevices();
    }
    if (const auto apt = Service::APT::GetModule(system)) {
        apt->GetAppletManager()->ReloadInputDevices();
    }
    auto& sm = system.ServiceManager();
    if (const auto ir_user = sm.GetService<Service::IR::IR_USER>("ir:USER")) {
        ir_user->ReloadInputDevices();
    }
    if (const auto ir_rst = sm.GetService<Service::IR::IR_RST>("ir:rst")) {
        ir_rst->ReloadInputDevices();
    }
}

#ifdef ENABLE_WEB_SERVICE
std::shared_ptr<Network::AnnounceMultiplayerSession> announce;
#endif

/// Hosts and/or joins the room the settings ask for, before the game starts: the console's
/// local-wireless service only talks to a room that exists when it starts, and takes the
/// room's MAC address for the console's (as Azahar's Multiplayer menu does). Returns false
/// with a message for the player if that fails.
bool StartMultiplayer(Core::System& system, const App::Config& config, std::string& error) {
    if (config.join.empty() && config.host <= 0) {
        return true;
    }
    std::string nickname = config.nickname;
    if (nickname.empty()) {
        nickname = Service::CFG::GetUsername(system);
    }
    std::string host = "127.0.0.1";
    u16 port = Network::DefaultRoomPort;
    if (config.host > 0) {
        port = static_cast<u16>(config.host);
        const auto room = Network::GetRoom().lock();
        const bool listed = config.room_public && !config.lobby.empty() && !config.token.empty();
        std::unique_ptr<Network::VerifyUser::Backend> verify =
            std::make_unique<Network::VerifyUser::NullBackend>();
#ifdef ENABLE_WEB_SERVICE
        if (listed) {
            Settings::values.web_api_url = config.lobby;
            Settings::values.network_token = config.token;
            verify = std::make_unique<WebService::VerifyUserJWT>(config.lobby);
        }
#endif
        if (!room || !room->Create(config.room_name, "", "", port, config.password,
                                   static_cast<u32>(config.room_members), nickname,
                                   "Monster Hunter 4 Ultimate", 0x0004000000126300,
                                   std::move(verify))) {
            error = fmt::format("Couldn't host a room on port {} (is it in use?).", port);
            return false;
        }
        LOG_INFO(Network, "hosting room \"{}\" on port {}", config.room_name, port);
#ifdef ENABLE_WEB_SERVICE
        if (listed) {
            announce = std::make_shared<Network::AnnounceMultiplayerSession>(nickname);
            const auto result = announce->Register();
            if (result.result_code != Common::WebResult::Code::Success) {
                LOG_ERROR(Network, "the lobby at {} didn't list the room: {}", config.lobby,
                          result.result_string);
            } else {
                announce->Start();
                LOG_INFO(Network, "room listed in the lobby at {}", config.lobby);
            }
        }
#endif
    } else {
        host = config.join;
        if (const auto colon = host.rfind(':'); colon != std::string::npos) {
            port = static_cast<u16>(std::atoi(host.c_str() + colon + 1));
            host = host.substr(0, colon);
        }
    }
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        error = "The network library didn't start.";
        return false;
    }
    member->BindOnError([](const Network::RoomMember::Error& e) {
        LOG_WARNING(Network, "room: {}", Network::GetErrorStr(e));
    });
    member->BindOnStatusMessageReceived([](const Network::StatusMessageEntry& msg) {
        if (msg.type == Network::IdMemberJoin) {
            LOG_INFO(Network, "{} joined the room", msg.nickname);
        } else if (msg.type == Network::IdMemberLeave) {
            LOG_INFO(Network, "{} left the room", msg.nickname);
        }
    });
    member->Join(nickname, Service::CFG::GetConsoleIdHash(system), host.c_str(), port, 0,
                 Network::NoPreferredMac, config.password);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (member->GetState() == Network::RoomMember::State::Joining &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!member->IsConnected()) {
        error = fmt::format("Couldn't join the room at {}:{} ({}).", host, port,
                            Network::GetStateStr(member->GetState()));
        return false;
    }
    LOG_INFO(Network, "in the room at {}:{} as {}", host, port, nickname);
    return true;
}

std::string LoadError(Core::System::ResultStatus status, Core::System& system) {
    using Status = Core::System::ResultStatus;
    switch (status) {
    case Status::ErrorGetLoader:
    case Status::ErrorLoader_ErrorInvalidFormat:
        return "This file isn't a 3DS game the app can open. Choose your decrypted Monster Hunter "
               "4 Ultimate (.3ds, .cci or .cxi) in mh4u-app.ini (game = ...) or remove that line to "
               "be asked again.";
    case Status::ErrorLoader_ErrorEncrypted:
        return "The game file is encrypted. The port needs a decrypted dump of your own cartridge "
               "(GodMode9 can make one; docs/SETUP.md in the port's repository).";
    case Status::ErrorSystemFiles:
        return "Some 3DS system files are missing (Azahar's System settings can set them up).";
    default:
        return fmt::format("The game didn't start: {}", system.GetStatusDetails());
    }
}

int Run(const std::vector<std::string>& argv) {
    Common::DetachedTasks detached_tasks;
    const Args args = Parse(argv);
    if (args.help || args.bad) {
        std::fputs(USAGE, args.bad ? stderr : stdout);
        return args.bad ? 1 : 0;
    }
    if (!args.user.empty()) {
        // before anything looks at the user folder (the log is in it)
        const char last = args.user.back();
        FileUtil::SetUserPath(last == '/' || last == '\\' ? args.user : args.user + '/');
    }
    // (the launcher's code check logs to a file of its own: the launcher's process has this one)
    Common::Log::Initialize(args.check_code.empty() ? "mh4u_log.txt" : "mh4u_check_log.txt");
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(filter);
    if (!args.check_code.empty()) {
        const int status = Launcher::CheckCode(args.check_code);
        Common::Log::Stop();
        return status;
    }

    LOG_INFO(Frontend, "MH4U PC port app, Azahar {} | {}-{}", Common::g_build_fullname,
             Common::g_scm_branch, Common::g_scm_desc);

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
    if (!FileUtil::Exists(Port::OptionsPath())) {
        FileUtil::WriteStringToFile(true, Port::OptionsPath(), Port::OptionsTemplate());
    }

    if (!App::Window::InitSDL()) {
        return 1;
    }
    InputCommon::Init();
    App::Input::Register();
    Network::Init();

    // ── the launcher (launcher/): the home screen, setup, mods, the room and settings; it
    // leaves the game file and the room in `config`. Without a window of its own, as before.
    if (!args.game.empty()) {
        config.game = args.game;
    }
    const bool show_launcher = args.launcher >= 0 ? args.launcher == 1 : config.launcher;
    if (Launcher::Run(config, show_launcher) == Launcher::Result::Quit) {
        Network::Shutdown();
        InputCommon::Shutdown();
        Common::Log::Stop();
        SDL_Quit();
        return 0;
    }
    // ──

    std::string game = config.game;
    if (game.empty() || !FileUtil::Exists(game)) {
        if (!game.empty()) {
            LOG_WARNING(Frontend, "{} isn't there any more", game);
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

    App::Apply(config);
    LOG_INFO(Frontend, "{}", App::Input::ControllerDescription());

    auto& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.ApplySettings();

    if (std::string error; !StartMultiplayer(system, config, error)) {
        App::Dialog::Message(TITLE, error + " The game starts without the room.", true);
    }

    LOG_INFO(Frontend, "{}", Port::PrepareBackendFor(system, game));
    if (const std::string notice = Port::BackendNotice(); !notice.empty()) {
        App::Dialog::Message(TITLE, notice);
    }

    App::Window::Api api = ApiFor(config.renderer);
    auto window = MakeWindow(system, api, config, api);
    if (!window) {
        App::Dialog::Message(TITLE, "No renderer could open a window (see mh4u_log.txt).", true);
        return 1;
    }
    std::unique_ptr<App::Window> secondary;
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows &&
        api != App::Window::Api::Software) {
        secondary = App::Window::Create(system, api, config, true);
        if (!secondary) {
            Settings::values.layout_option = Settings::LayoutOption::CustomLayout;
        }
    }
    LOG_INFO(Frontend, "renderer: {}", NameOf(api));

    // the in-game menu (Esc, a controller's Guide or Back+Start), and the 3DS software keyboard
    // drawn in it instead of Azahar's default one (ui/)
    App::Ui::Start(system, *window, config);

    const auto scope = window->Acquire();
    Settings::LogSettings();
    const auto status = system.Load(*window, game, secondary.get());
    if (status != Core::System::ResultStatus::Success) {
        App::Dialog::Message(TITLE, LoadError(status, system), true);
        Network::Shutdown();
        return 1;
    }

    std::thread present([&window] { window->Present(); });
    std::thread present_secondary([&secondary] {
        if (secondary) {
            secondary->Present();
        }
    });

    u64 program_id = 0;
    system.GetAppLoader().ReadProgramId(program_id);
    system.GPU().ApplyPerProgramSettings(program_id);
    std::atomic_bool stop_loading{false};
    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(
        stop_loading, [](VideoCore::LoadCallbackStage, std::size_t value, std::size_t total,
                         const std::string&) {
            if (total && (value == total || value % 256 == 0)) {
                LOG_DEBUG(Frontend, "shader cache {}/{}", value, total);
            }
        });

    while (window->IsOpen() && (!secondary || secondary->IsOpen())) {
        const auto result = system.RunLoop();
        if (result == Core::System::ResultStatus::ShutdownRequested) {
            break;
        }
        if (result != Core::System::ResultStatus::Success) {
            LOG_ERROR(Frontend, "emulation stopped: {}", system.GetStatusDetails());
            App::Dialog::Message(TITLE, "The game stopped: " + system.GetStatusDetails(), true);
            break;
        }
        if (window->TakeControllersChanged()) {
            App::Input::BuildProfile(config);
            ReloadInput(system);
            LOG_INFO(Frontend, "{}", App::Input::ControllerDescription());
        }
    }
    window->RequestClose();
    if (secondary) {
        secondary->RequestClose();
    }
    present.join();
    present_secondary.join();

#ifdef ENABLE_WEB_SERVICE
    if (announce) {
        announce->Stop();
        announce.reset();
    }
#endif
    system.Shutdown();
    App::Ui::Stop(); // after the renderer, before SDL and the room go
    if (const auto member = Network::GetRoomMember().lock(); member && member->IsConnected()) {
        member->Leave();
    }
    if (const auto room = Network::GetRoom().lock(); room && room->GetState() == Network::Room::State::Open) {
        room->Destroy();
    }
    Network::Shutdown();
    InputCommon::Shutdown();
    detached_tasks.WaitForAllTasks();
    Common::Log::Stop();
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
        args.push_back(Common::UTF16ToUTF8(std::wstring(wargv[i])));
    }
    LocalFree(wargv);
    return Run(args);
#else
    return Run(std::vector<std::string>(argv, argv + argc));
#endif
}
