// Headless runner: boots a 3DS executable with Azahar's core and no window, with either
// the recompiled code or one of Azahar's own CPU backends, until the program exits or an
// emulated-time limit is reached. Debug output (svcOutputDebugString) goes to stdout.
// For testing the real game it can also install a CIA (the update) first, press buttons
// and touch the screen on an emulated-time script, and save both screens as images.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "arm_recomp.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/image_interface.h"
#include "core/frontend/input.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/service.h"
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
  --speed PERCENT   emulation speed limit (default 0 = as fast as possible; 100 = real time)
  --trace-svc FILE  write every system call (thread, number, pc, lr, r0-r3, ticks) to FILE,
                    to compare CPU backends thread by thread
  --input SCRIPT    comma-separated presses at emulated times, e.g.
                    "3000:A,3500:START:200,5000:TOUCH:160:120,6000:CIRCLE:0:1:500"
                    (ms:BUTTON[:hold ms], ms:TOUCH:x:y[:hold], ms:CIRCLE:x:y[:hold];
                    hold defaults to 100 ms; buttons A B X Y UP DOWN LEFT RIGHT L R
                    START SELECT ZL ZR HOME)
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

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string code_library;
    std::string cpu = "recomp";
    std::string log_filter = "*:Warning Debug.Emulated:Debug Kernel:Info";
    double emulated_ms = 10000.0;
    std::string user_dir, shots_dir, input_script, svc_trace;
    std::vector<std::string> install;
    double shot_every = 1000.0;
    double speed = 0.0;
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
        } else if (arg == "--trace-svc" && i + 1 < argc) {
            svc_trace = argv[++i];
        } else if (arg == "--speed" && i + 1 < argc) {
            speed = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--shot-every" && i + 1 < argc) {
            shot_every = std::max(1.0, std::atof(argv[++i]));
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
    if (path.empty() || (cpu != "recomp" && cpu != "jit" && cpu != "interp")) {
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
    double next_shot = shot_every;
    int rc = 0;
    while (true) {
        const auto result = system.RunLoop();
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
    std::fflush(stdout);
    if (g_svc_trace) {
        Kernel::SetSVCObserver(nullptr);
        std::fclose(g_svc_trace);
    }
    system.Shutdown();
    Common::Log::Stop();
    return rc;
}
