#include "frontend.h"

#include <cstdlib>
#include <memory>
#include <fmt/format.h>

#include "arm_recomp.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/loader/loader.h"

namespace Port {

std::string PrepareBackendFor(Core::System& system, const std::string& rom_path) {
    system.SetCPUFactory(nullptr);

    u64 program_id = 0;
    if (const auto loader = Loader::GetLoader(rom_path)) {
        if (loader->ReadProgramId(program_id) != Loader::ResultStatus::Success) {
            program_id = 0;
        }
    }

    std::string library;
    if (const char* env = std::getenv("MH4U_CODE"); env && *env) {
        library = env;
    } else if (program_id != 0) {
#if defined(_WIN32)
        constexpr const char* ext = "dll";
#elif defined(__APPLE__)
        constexpr const char* ext = "dylib";
#else
        constexpr const char* ext = "so";
#endif
        library = fmt::format("{}recomp/{:016X}.{}", FileUtil::GetUserPath(FileUtil::UserPath::UserDir),
                              program_id, ext);
        if (!FileUtil::Exists(library)) {
            return fmt::format("no recompiled code for {:016X}, using the emulated CPU", program_id);
        }
    } else {
        return "no recompiled code, using the emulated CPU";
    }

    std::string error;
    if (!ARM_Recomp::LoadCode(library, error)) {
        return fmt::format("{}; using the emulated CPU", error);
    }
    const u64 code_title = ARM_Recomp::CodeTitleId();
    if (code_title != 0 && program_id != 0 && code_title != program_id) {
        return fmt::format("{} is for {:016X}, not {:016X}; using the emulated CPU", library,
                           code_title, program_id);
    }
    system.SetCPUFactory([](Core::System& sys, Memory::MemorySystem& memory, u32 core_id,
                            std::shared_ptr<Core::Timing::Timer> timer)
                             -> std::shared_ptr<Core::ARM_Interface> {
        return std::make_shared<ARM_Recomp>(sys, memory, core_id, std::move(timer));
    });
    return fmt::format("running recompiled code from {} ({} functions)", library,
                       ARM_Recomp::FunctionCount());
}

} // namespace Port
