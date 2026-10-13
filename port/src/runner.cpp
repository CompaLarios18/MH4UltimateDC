// Headless runner: boots a 3DS executable with Azahar's core and no window, with either
// the recompiled code or one of Azahar's own CPU backends, until the program exits or an
// emulated-time limit is reached. Debug output (svcOutputDebugString) goes to stdout.
// For testing the real game it can also install a CIA (the update) first, press buttons
// and touch the screen on an emulated-time script, save both screens as images, and join
// a network room (Azahar's multiplayer) so that two runners can play locally together.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>

#include "arm_recomp.h"
#include "hooks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/image_interface.h"
#include "core/frontend/input.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/service.h"
#include "network/network.h"
#include "network/verify_user.h"
#include "video_core/gpu.h"
#include "video_core/renderer_software/renderer_software.h"

namespace {

constexpr double TICKS_PER_MS = 268111.856; // ARM11 clock

const char* const USAGE = R"(usage: mh4u_runner <file> [options]
  --code LIB        recompiled code library (with --cpu recomp)
  --cpu KIND        recomp (default), jit or interp
  --ms N            stop after N ms of emulated time (default 10000)
  --log FILTER      log filter (default "*:Warning Debug.Emulated:Debug Kernel:Info")
  --user DIR        Azahar user directory (NAND, SD card, saves) to use
  --install CIA     install a CIA, e.g. the game's update, before booting (repeatable)
  --shots DIR       save the top and bottom screens as PPM images in DIR
  --shot-every MS   how often, in emulated time (default 1000)
  --shots-from MS   first screenshot at this emulated time (default: one interval in)
  --draw-window MS  draw only during the MS of emulated time before each screenshot (and
                    nothing without --shots): much faster with the software renderer, but
                    a screen built up over several frames can come out incomplete
  --speed PERCENT   emulation speed limit (default 0 = as fast as possible; 100 = real time)
  --cpu-clock PERCENT  emulated CPU clock (default 100; Azahar's CPU Clock Speed setting)
  --async-io        let file reads finish on host threads and start the clock at the
                    host's time, as Azahar does by default; the runner otherwise does reads in
                    place and starts at a fixed date, so that a run repeats exactly (an
                    asynchronous read wakes the game at an emulated time that depends on how
                    fast the host was, which moves loading screens by seconds, and the game
                    seeds its random numbers from the clock)
  --dump MS:ADDR:SIZE  at emulated time MS, write SIZE bytes of guest memory from ADDR to
                    mem_<ms>_<addr>.bin in the --shots directory (or the current one);
                    repeatable, e.g. to diff the game's state with and without an input
  --trace-svc FILE  write every system call (thread, number, pc, lr, r0-r3, ticks) to FILE,
                    to compare CPU backends thread by thread
  --input SCRIPT    comma-separated presses at emulated times, e.g.
                    "3000:A,3500:START:200,5000:TOUCH:160:120,6000:CIRCLE:0:1:500"
                    (ms:BUTTON[:hold ms], ms:TOUCH:x:y[:hold], ms:CIRCLE:x:y[:hold];
                    hold defaults to 100 ms; buttons A B X Y UP DOWN LEFT RIGHT L R
                    START SELECT ZL ZR HOME)
  --room-host PORT  host a network room (Azahar's multiplayer) on this UDP port and join it
  --room-join HOST:PORT  join a network room; the game's local wireless play then reaches
                    the other members (use --speed 100 on every runner so they keep time)
  --nick NAME       nickname in the room, 4 to 20 letters, digits, spaces, '.', '_' or '-'
                    (default "runner")
  --new-console-id  give the console a new random ID before the game starts (a room turns
                    away a second member with the same ID, e.g. two copies of a user folder)
or:    mh4u_runner --overlay-layout SPEC WIDTHxHEIGHT[:TOP_WIDTH]
                    print the custom screen layout the port's touch_overlay option SPEC
                    gives a window that size (top screen TOP_WIDTH x 240, default 400)
)";

class HeadlessWindow final : public Frontend::EmuWindow {
public:
    void PollEvents() override {}
};

int Usage() {
    std::fputs(USAGE, stderr);
    return 1;
}

s64 g_start_ticks = 0; // the emulated clock doesn't start at 0

/// Emulated time since the program was loaded.
double EmulatedMs() {
    return (Core::System::GetInstance().CoreTiming().GetGlobalTicks() - g_start_ticks) /
           TICKS_PER_MS;
}

// ── scripted input ──────────────────────────────────────────────────────────────────

struct Press {
    double start_ms, end_ms;
    int button;      // Settings::NativeButton, or TOUCH / CIRCLE
    float x, y;      // touch: pixels on the bottom screen; circle pad: -1..1
};
constexpr int TOUCH = -1, CIRCLE = -2;
std::vector<Press> g_presses;

struct Dump {
    double ms;
    u32 addr, size;
    bool done = false;
};
std::vector<Dump> g_dumps;

bool ParseDump(const std::string& arg) {
    Dump d{};
    char extra = 0;
    if (std::sscanf(arg.c_str(), "%lf:%x:%x%c", &d.ms, &d.addr, &d.size, &extra) != 3 || d.size == 0) {
        return false;
    }
    g_dumps.push_back(d);
    return true;
}

/// Writes the dumps that are due; unmapped bytes come out as zero.
void WriteDumps(Core::System& system, const std::string& dir) {
    const double now = EmulatedMs();
    for (Dump& d : g_dumps) {
        if (d.done || now < d.ms) {
            continue;
        }
        d.done = true;
        std::vector<u8> bytes(d.size);
        auto& memory = system.Memory();
        auto process = system.Kernel().GetCurrentProcess();
        for (u32 off = 0; off < d.size;) {
            const VAddr a = d.addr + off;
            const u32 chunk = std::min<u32>(d.size - off, 0x1000 - (a & 0xfff)); // page by page
            if (process && memory.IsValidVirtualAddress(*process, a)) {
                memory.ReadBlock(*process, a, bytes.data() + off, chunk);
            }
            off += chunk;
        }
        const std::string path = fmt::format("{}/mem_{:07.0f}_{:08x}.bin", dir.empty() ? "." : dir, d.ms, d.addr);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
}

const Press* Active(int button) {
    const double now = EmulatedMs();
    for (const Press& p : g_presses) {
        if (p.button == button && now >= p.start_ms && now < p.end_ms) {
            return &p;
        }
    }
    return nullptr;
}

bool ParseScript(const std::string& script) {
    static const std::pair<const char*, int> names[] = {
        {"A", Settings::NativeButton::A},         {"B", Settings::NativeButton::B},
        {"X", Settings::NativeButton::X},         {"Y", Settings::NativeButton::Y},
        {"UP", Settings::NativeButton::Up},       {"DOWN", Settings::NativeButton::Down},
        {"LEFT", Settings::NativeButton::Left},   {"RIGHT", Settings::NativeButton::Right},
        {"L", Settings::NativeButton::L},         {"R", Settings::NativeButton::R},
        {"START", Settings::NativeButton::Start}, {"SELECT", Settings::NativeButton::Select},
        {"ZL", Settings::NativeButton::ZL},       {"ZR", Settings::NativeButton::ZR},
        {"HOME", Settings::NativeButton::Home},
    };
    std::stringstream items(script);
    std::string item;
    while (std::getline(items, item, ',')) {
        std::vector<std::string> f;
        std::stringstream fields(item);
        std::string field;
        while (std::getline(fields, field, ':')) {
            f.push_back(field);
        }
        if (f.size() < 2) {
            return false;
        }
        Press p{std::atof(f[0].c_str()), 0, 0, 0, 0};
        std::size_t hold_at = 2;
        if (f[1] == "TOUCH" || f[1] == "CIRCLE") {
            if (f.size() < 4) {
                return false;
            }
            p.button = f[1] == "TOUCH" ? TOUCH : CIRCLE;
            p.x = static_cast<float>(std::atof(f[2].c_str()));
            p.y = static_cast<float>(std::atof(f[3].c_str()));
            hold_at = 4;
        } else {
            const auto it = std::find_if(std::begin(names), std::end(names),
                                         [&](const auto& n) { return f[1] == n.first; });
            if (it == std::end(names)) {
                return false;
            }
            p.button = it->second;
        }
        p.end_ms = p.start_ms + (f.size() > hold_at ? std::atof(f[hold_at].c_str()) : 100.0);
        g_presses.push_back(p);
    }
    return true;
}

class ScriptButton final : public Input::ButtonDevice {
public:
    explicit ScriptButton(int button) : button(button) {}
    bool GetStatus() const override {
        return Active(button) != nullptr;
    }

private:
    int button;
};

class ScriptCirclePad final : public Input::AnalogDevice {
public:
    std::tuple<float, float> GetStatus() const override {
        const Press* p = Active(CIRCLE);
        return p ? std::make_tuple(p->x, p->y) : std::make_tuple(0.0f, 0.0f);
    }
};

class ScriptTouch final : public Input::TouchDevice {
public:
    std::tuple<float, float, bool> GetStatus() const override {
        const Press* p = Active(TOUCH);
        return p ? std::make_tuple(p->x / 320.0f, p->y / 240.0f, true)
                 : std::make_tuple(0.0f, 0.0f, false);
    }
};

template <typename Device, typename Make>
class ScriptFactory final : public Input::Factory<Device> {
public:
    explicit ScriptFactory(Make make) : make(make) {}
    std::unique_ptr<Device> Create(const Common::ParamPackage& params) override {
        return make(params);
    }

private:
    Make make;
};

template <typename Device, typename Make>
void Register(Make make) {
    Input::RegisterFactory<Device>("script", std::make_shared<ScriptFactory<Device, Make>>(make));
}

void UseScriptedInput() {
    Register<Input::ButtonDevice>([](const Common::ParamPackage& params) {
        return std::make_unique<ScriptButton>(params.Get("button", 0));
    });
    Register<Input::AnalogDevice>(
        [](const Common::ParamPackage&) { return std::make_unique<ScriptCirclePad>(); });
    Register<Input::TouchDevice>(
        [](const Common::ParamPackage&) { return std::make_unique<ScriptTouch>(); });
    auto& profile = Settings::values.current_input_profile;
    for (std::size_t i = 0; i < profile.buttons.size(); ++i) {
        profile.buttons[i] = "engine:script,button:" + std::to_string(i);
    }
    profile.analogs[Settings::NativeAnalog::CirclePad] = "engine:script";
    profile.touch_device = "engine:script";
    profile.use_touchpad = false;
    profile.use_touch_from_button = false;
}

// ── system call trace ───────────────────────────────────────────────────────────────

FILE* g_svc_trace = nullptr;

void TraceSvc(Core::System& system, u32 imm) {
    auto& cpu = system.GetRunningCore();
    const auto* thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread();
    std::fprintf(g_svc_trace, "%u %02X pc=%08X lr=%08X %08X %08X %08X %08X t=%lld\n",
                 thread ? thread->GetThreadId() : 0u, imm, cpu.GetReg(15), cpu.GetReg(14),
                 cpu.GetReg(0), cpu.GetReg(1), cpu.GetReg(2), cpu.GetReg(3),
                 static_cast<long long>(system.CoreTiming().GetGlobalTicks() - g_start_ticks));
}

// ── screenshots ─────────────────────────────────────────────────────────────────────

/// Writes one screen of the software renderer as a binary PPM (landscape, as displayed).
bool SaveScreen(Core::System& system, VideoCore::ScreenId id, const std::string& path) {
    auto* renderer = dynamic_cast<SwRenderer::RendererSoftware*>(&system.GPU().Renderer());
    if (!renderer) {
        return false;
    }
    const auto& info = renderer->Screen(id);
    if (info.pixels.empty()) {
        return false;
    }
    // The renderer stores the screen rotated: info.height pixels per output row.
    const u32 width = info.height, height = info.width;
    std::vector<u8> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        std::memcpy(&rgb[i * 3], &info.pixels[i * 4], 3);
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

// ── network room ────────────────────────────────────────────────────────────────────

/// A new random console ID, saved in the user folder's system settings.
void NewConsoleId(Core::System& system) {
    const auto cfg = Service::CFG::GetModule(system);
    const auto [random_number, console_id] = cfg->GenerateConsoleUniqueId();
    cfg->SetConsoleUniqueId(random_number, console_id);
    cfg->UpdateConfigNANDSavegame();
    std::printf("console id %016llx\n", static_cast<unsigned long long>(console_id));
}

/// Hosts a room on `host_port` (when it isn't 0) and joins it, or joins the room at `join`
/// (HOST:PORT), the way Azahar's Multiplayer menu does. Waits until the room has accepted
/// or refused this member.
bool JoinRoom(Core::System& system, u16 host_port, const std::string& join,
              const std::string& nickname) {
    if (!Network::Init()) {
        std::fprintf(stderr, "room: the network library didn't start\n");
        return false;
    }
    std::string host = "127.0.0.1";
    u16 port = host_port;
    if (host_port) {
        const auto room = Network::GetRoom().lock();
        if (!room || !room->Create("mh4u_runner", "", "", host_port, "", 16, "", "", 0,
                                   std::make_unique<Network::VerifyUser::NullBackend>())) {
            std::fprintf(stderr, "room: could not host on port %u\n", host_port);
            return false;
        }
        std::printf("room: hosting on port %u\n", host_port);
        std::fflush(stdout);
    } else {
        const auto colon = join.rfind(':');
        host = join.substr(0, colon);
        port = static_cast<u16>(std::atoi(join.c_str() + colon + 1));
    }
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return false;
    }
    member->BindOnError([](const Network::RoomMember::Error& error) {
        std::printf("room: %s\n", Network::GetErrorStr(error));
        std::fflush(stdout);
    });
    member->Join(nickname, Service::CFG::GetConsoleIdHash(system), host.c_str(), port);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (member->GetState() == Network::RoomMember::State::Joining &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!member->IsConnected()) {
        std::fprintf(stderr, "room: could not join %s:%u (%s)\n", host.c_str(), port,
                     Network::GetStateStr(member->GetState()));
        return false;
    }
    std::printf("room: joined %s:%u as %s, MAC %s\n", host.c_str(), port, nickname.c_str(),
                Service::CFG::MacToString(member->GetMacAddress()).c_str());
    std::fflush(stdout);
    return true;
}

/// The room's members as this runner last heard of them, sorted, for the log.
void PrintRoomMembers() {
    const auto member = Network::GetRoomMember().lock();
    if (!member || !member->IsConnected()) {
        return;
    }
    std::vector<std::string> names;
    for (const auto& info : member->GetMemberInformation()) {
        names.push_back(info.nickname);
    }
    std::sort(names.begin(), names.end());
    std::string list;
    for (const auto& name : names) {
        list += (list.empty() ? "" : ", ") + name;
    }
    std::printf("room: members: %s\n", list.c_str());
}

} // namespace

/// --overlay-layout: the screens' rectangles for a touch_overlay option and window size.
int PrintOverlayLayout(const std::string& spec, const std::string& size) {
    unsigned width = 0, height = 0, top_width = Core::kScreenTopWidth;
    if (std::sscanf(size.c_str(), "%ux%u:%u", &width, &height, &top_width) < 2 || !width ||
        !height || !top_width) {
        return Usage();
    }
    Layout::SetTopScreenWidth(top_width);
    Layout::SetScreenOverlay(Port::Hooks::ParseOverlay(spec));
    const auto layout = Layout::CustomFrameLayout(width, height, false, false);
    const auto& t = layout.top_screen;
    const auto& b = layout.bottom_screen;
    std::printf("top %u,%u %u,%u opacity %.2f\nbottom %u,%u %u,%u opacity %.2f\n", t.left, t.top,
                t.right, t.bottom, layout.top_opacity, b.left, b.top, b.right, b.bottom,
                layout.bottom_opacity);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--overlay-layout") {
        return PrintOverlayLayout(argv[2], argv[3]);
    }
    std::string path;
    std::string code_library;
    std::string cpu = "recomp";
    std::string log_filter = "*:Warning Debug.Emulated:Debug Kernel:Info";
    double emulated_ms = 10000.0;
    std::string user_dir, shots_dir, input_script, svc_trace;
    std::vector<std::string> install;
    double shot_every = 1000.0;
    double shots_from = -1.0;
    double speed = 0.0;
    double draw_window = -1.0; // < 0: draw every frame
    int cpu_clock = 100;
    bool async_io = false;
    int room_host = 0;
    std::string room_join;
    std::string nickname = "runner";
    bool new_console_id = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--code" && i + 1 < argc) {
            code_library = argv[++i];
        } else if (arg == "--cpu" && i + 1 < argc) {
            cpu = argv[++i];
        } else if (arg == "--ms" && i + 1 < argc) {
            emulated_ms = std::atof(argv[++i]);
        } else if (arg == "--log" && i + 1 < argc) {
            log_filter = argv[++i];
        } else if (arg == "--user" && i + 1 < argc) {
            user_dir = argv[++i];
        } else if (arg == "--install" && i + 1 < argc) {
            install.push_back(argv[++i]);
        } else if (arg == "--shots" && i + 1 < argc) {
            shots_dir = argv[++i];
        } else if (arg == "--dump" && i + 1 < argc) {
            if (!ParseDump(argv[++i])) {
                std::fprintf(stderr, "bad --dump: %s (want MS:ADDR:SIZE, hex address and size)\n", argv[i]);
                return 1;
            }
        } else if (arg == "--trace-svc" && i + 1 < argc) {
            svc_trace = argv[++i];
        } else if (arg == "--speed" && i + 1 < argc) {
            speed = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--shots-from" && i + 1 < argc) {
            shots_from = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--shot-every" && i + 1 < argc) {
            shot_every = std::max(1.0, std::atof(argv[++i]));
        } else if (arg == "--cpu-clock" && i + 1 < argc) {
            cpu_clock = std::clamp(std::atoi(argv[++i]), 5, 400);
        } else if (arg == "--async-io") {
            async_io = true;
        } else if (arg == "--draw-window" && i + 1 < argc) {
            draw_window = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--room-host" && i + 1 < argc) {
            room_host = std::atoi(argv[++i]);
            if (room_host <= 0 || room_host > 65535) {
                return Usage();
            }
        } else if (arg == "--room-join" && i + 1 < argc) {
            room_join = argv[++i];
            const auto colon = room_join.rfind(':');
            if (colon == std::string::npos || colon == 0 || std::atoi(room_join.c_str() + colon + 1) <= 0) {
                return Usage();
            }
        } else if (arg == "--nick" && i + 1 < argc) {
            nickname = argv[++i];
            // what rooms accept; anything else comes back as a name collision
            if (nickname.size() < 4 || nickname.size() > 20 ||
                !std::all_of(nickname.begin(), nickname.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || std::strchr(" ._-", c);
                })) {
                std::fprintf(stderr, "bad --nick: %s\n", nickname.c_str());
                return 1;
            }
        } else if (arg == "--new-console-id") {
            new_console_id = true;
        } else if (arg == "--input" && i + 1 < argc) {
            input_script = argv[++i];
            if (!ParseScript(input_script)) {
                std::fprintf(stderr, "bad --input script: %s\n", input_script.c_str());
                return 1;
            }
        } else if (!arg.empty() && arg[0] != '-' && path.empty()) {
            path = arg;
        } else {
            return Usage();
        }
    }
    if (path.empty() || (cpu != "recomp" && cpu != "jit" && cpu != "interp") ||
        (room_host && !room_join.empty())) {
        return Usage();
    }

    if (!user_dir.empty()) {
        // before anything asks for a path: the other directories are derived from it once
        FileUtil::SetUserPath(user_dir.back() == '/' ? user_dir : user_dir + '/');
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::Log::Filter filter;
    filter.ParseFilterString(log_filter);
    Common::Log::SetGlobalFilter(filter);

    for (const auto& cia : install) {
        const auto result = Service::AM::InstallCIA(cia);
        if (result != Service::AM::InstallStatus::Success) {
            std::fprintf(stderr, "installing %s failed (status %u)\n", cia.c_str(),
                         static_cast<unsigned>(result));
            return 1;
        }
        std::printf("installed %s\n", cia.c_str());
    }
    UseScriptedInput();
    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Software);
    Settings::values.frame_limit.SetValue(speed); // 0: don't wait for real time
    Settings::values.cpu_clock_percentage.SetValue(cpu_clock);
    Settings::values.deterministic_async_operations.SetValue(!async_io);
    if (!async_io) {
        // the game seeds its random numbers from the clock: a fixed start date makes two runs
        // the same down to the monster's moves (2026-01-01 12:00 UTC)
        Settings::values.init_clock.SetValue(Settings::InitClock::FixedTime);
        Settings::values.init_time.SetValue(1767268800ULL);
    }
    // no sound card needed (and on Windows, cubeb's WASAPI backend asserts on a thread
    // without COM, which a runner without Qt never initialises)
    Settings::values.output_type.SetValue(AudioCore::SinkType::Null);
    Settings::values.input_type.SetValue(AudioCore::InputType::Null);
    Settings::values.use_cpu_jit = cpu == "jit";
    Settings::values.use_fastinterp = false;

    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false); // all services HLE
    }

    Core::System& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());
    std::vector<Port::ARM_Recomp*> recomp_cores;
    if (cpu == "recomp") {
        std::string error;
        if (code_library.empty() || !Port::ARM_Recomp::LoadCode(code_library, error)) {
            std::fprintf(stderr, "%s\n", code_library.empty() ? "--cpu recomp needs --code LIB" : error.c_str());
            return 1;
        }
        system.SetCPUFactory([&recomp_cores](Core::System& sys, Memory::MemorySystem& memory,
                                             u32 core_id, std::shared_ptr<Core::Timing::Timer> timer) {
            auto core = std::make_shared<Port::ARM_Recomp>(sys, memory, core_id, std::move(timer));
            recomp_cores.push_back(core.get());
            return std::shared_ptr<Core::ARM_Interface>(core);
        });
    }

    // Before loading, as Azahar's Multiplayer menu does: the system settings are read when the
    // services start, and the local wireless service only talks to a room that exists when
    // it starts (it also takes the room's MAC address for the console's).
    if (new_console_id) {
        NewConsoleId(system);
    }
    const bool in_room = room_host || !room_join.empty();
    if (in_room && !JoinRoom(system, static_cast<u16>(room_host), room_join, nickname)) {
        Network::Shutdown();
        Common::Log::Stop();
        return 5;
    }

    HeadlessWindow window;
    const auto status = system.Load(window, path);
    if (status != Core::System::ResultStatus::Success) {
        std::fprintf(stderr, "failed to load %s (status %u)\n", path.c_str(), static_cast<unsigned>(status));
        return 2;
    }
    std::weak_ptr<Kernel::Process> process = system.Kernel().GetCurrentProcess();
    // ARM11 clock: 268,111,856 Hz
    const s64 tick_limit = static_cast<s64>(emulated_ms * 268111.856);
    const s64 start_ticks = system.CoreTiming().GetGlobalTicks();
    g_start_ticks = start_ticks;
    if (!svc_trace.empty()) {
        g_svc_trace = std::fopen(svc_trace.c_str(), "w");
        if (g_svc_trace) {
            Kernel::SetSVCObserver(TraceSvc);
        }
    }

    if (!shots_dir.empty()) {
        FileUtil::CreateFullPath(shots_dir.back() == '/' ? shots_dir : shots_dir + '/');
    }
    double next_shot = shots_from >= 0.0 ? shots_from : shot_every;
    int rc = 0;
    while (true) {
        if (draw_window >= 0.0) {
            SwRenderer::SetDrawingEnabled(!shots_dir.empty() &&
                                          EmulatedMs() >= next_shot - draw_window);
        }
        const auto result = system.RunLoop();
        WriteDumps(system, shots_dir);
        if (!shots_dir.empty() && EmulatedMs() >= next_shot) {
            const auto stem = fmt::format("{}/{:07.0f}", shots_dir, next_shot);
            SaveScreen(system, VideoCore::ScreenId::TopLeft, stem + "_top.ppm");
            SaveScreen(system, VideoCore::ScreenId::Bottom, stem + "_bottom.ppm");
            next_shot += shot_every;
        }
        if (result == Core::System::ResultStatus::ShutdownRequested) {
            break;
        }
        if (result != Core::System::ResultStatus::Success) {
            std::fprintf(stderr, "emulation error (status %u)\n", static_cast<unsigned>(result));
            rc = 3;
            break;
        }
        bool failed = false;
        for (const auto* core : recomp_cores) {
            if (core->HasFailed()) {
                std::fprintf(stderr, "recompiled code stopped: %s\n", core->Failure().c_str());
                failed = true;
            }
        }
        if (failed) {
            rc = 4;
            break;
        }
        const auto p = process.lock();
        if (!p || p->status == Kernel::ProcessStatus::Exited) {
            std::printf("process exited\n");
            break;
        }
        if (system.CoreTiming().GetGlobalTicks() - start_ticks > tick_limit) {
            std::printf("time limit reached\n");
            break;
        }
    }
    std::printf("emulated time: %.1f ms\n",
                (system.CoreTiming().GetGlobalTicks() - start_ticks) / 268111.856);
    PrintRoomMembers();
    std::fflush(stdout);
    if (g_svc_trace) {
        Kernel::SetSVCObserver(nullptr);
        std::fclose(g_svc_trace);
    }
    system.Shutdown();
    if (in_room) {
        Network::Shutdown();
    }
    Common::Log::Stop();
    return rc;
}
