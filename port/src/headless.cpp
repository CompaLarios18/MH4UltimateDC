// Headless runner: boots a 3DS executable with Azahar's core and no window, with either
// the recompiled code or one of Azahar's own CPU backends, until the program exits or an
// emulated-time limit is reached. Debug output (svcOutputDebugString) goes to stdout.
//
// usage: mh4u_headless <file> [--cpu recomp|jit|interp] [--ms N] [--log FILTER]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "arm_recomp.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/service.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"

namespace {

class HeadlessWindow final : public Frontend::EmuWindow {
public:
    void PollEvents() override {}
};

int Usage() {
    std::fprintf(stderr, "usage: mh4u_headless <file> [--cpu recomp|jit|interp] [--ms N] [--log FILTER]\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string cpu = "recomp";
    std::string log_filter = "*:Warning Debug.Emulated:Debug Kernel:Info";
    double emulated_ms = 10000.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cpu" && i + 1 < argc) {
            cpu = argv[++i];
        } else if (arg == "--ms" && i + 1 < argc) {
            emulated_ms = std::atof(argv[++i]);
        } else if (arg == "--log" && i + 1 < argc) {
            log_filter = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && path.empty()) {
            path = arg;
        } else {
            return Usage();
        }
    }
    if (path.empty() || (cpu != "recomp" && cpu != "jit" && cpu != "interp")) {
        return Usage();
    }

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::Log::Filter filter;
    filter.ParseFilterString(log_filter);
    Common::Log::SetGlobalFilter(filter);

    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Software);
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
        if (Port::ARM_Recomp::FunctionCount() == 0) {
            std::fprintf(stderr, "no recompiled code linked in\n");
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

    int rc = 0;
    while (true) {
        const auto result = system.RunLoop();
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
    std::fflush(stdout);
    system.Shutdown();
    Common::Log::Stop();
    return rc;
}
