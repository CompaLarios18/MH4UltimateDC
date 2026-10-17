#include "runtime_azahar/azahar_runtime.h"

#include <atomic>
#include <cstdlib>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/ir/ir_user.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "frontend.h"
#include "input.h"
#include "input_common/main.h"
#include "network/network.h"
#include "runtime_azahar/log_sink.h"
#include "util/log.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "window.h"

namespace App::Azahar {

namespace {

const char* CiaProblem(CiaResult result) {
    switch (result) {
    case CiaResult::Encrypted:
        return "it is encrypted: the port needs a decrypted one";
    case CiaResult::Unreadable:
        return "it can't be read";
    case CiaResult::Aborted:
        return "installing stopped half-way";
    default:
        return "it isn't a CIA that can be installed";
    }
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

} // namespace

struct AzaharRuntime::Impl {
    /// Keeps the work detached from the threads that started it finished before the program ends
    /// (Azahar's rule: one of these in main, waited for at the end).
    Common::DetachedTasks detached_tasks;
    std::string update_notice;  ///< why request.update didn't install, if it didn't
};

AzaharRuntime::AzaharRuntime() : impl(std::make_unique<Impl>()) {
    InstallLogSink();
}

AzaharRuntime::~AzaharRuntime() = default;

std::string AzaharRuntime::PrepareCode(const LoadRequest& request) {
    impl->update_notice.clear();
    if (!request.update.empty()) {
        const CiaResult result = InstallCia(request.update, nullptr);
        APP_LOG_INFO("installing {}: {}", request.update, result == CiaResult::Ok ? "done" : CiaProblem(result));
        if (result != CiaResult::Ok) {
            impl->update_notice = fmt::format("The update {} wasn't installed: {}.", request.update,
                                              CiaProblem(result));
        }
    }
    if (!request.code_library.empty()) {
        // (the port's backend looks the library up in this variable first)
        SetEnvironment("MH4U_CODE", request.code_library);
    }
    return Port::PrepareBackendFor(Core::System::GetInstance(), request.game);
}

std::string AzaharRuntime::CodeNotice() {
    const std::string backend = Port::BackendNotice();
    if (impl->update_notice.empty()) {
        return backend;
    }
    return backend.empty() ? impl->update_notice : impl->update_notice + " " + backend;
}

LoadResult AzaharRuntime::Load(const LoadRequest& request, Window& window, Window* secondary) {
    using Status = Core::System::ResultStatus;
    const Status status = Core::System::GetInstance().Load(window, request.game, secondary);
    switch (status) {
    case Status::Success:
        return LoadResult::Ok;
    case Status::ErrorGetLoader:
    case Status::ErrorLoader_ErrorInvalidFormat:
        return LoadResult::NotAGame;
    case Status::ErrorLoader_ErrorEncrypted:
        return LoadResult::Encrypted;
    case Status::ErrorSystemFiles:
        return LoadResult::MissingSystemFiles;
    default:
        return LoadResult::Failed;
    }
}

std::string AzaharRuntime::StatusDetails() const {
    return Core::System::GetInstance().GetStatusDetails();
}

void AzaharRuntime::Start() {
    auto& system = Core::System::GetInstance();
    u64 program_id = 0;
    system.GetAppLoader().ReadProgramId(program_id);
    system.GPU().ApplyPerProgramSettings(program_id);
    std::atomic_bool stop_loading{false};
    system.GPU().Renderer().Rasterizer()->LoadDefaultDiskResources(
        stop_loading, [](VideoCore::LoadCallbackStage, std::size_t value, std::size_t total,
                         const std::string&) {
            if (total && (value == total || value % 256 == 0)) {
                APP_LOG_DEBUG("shader cache {}/{}", value, total);
            }
        });
}

RunState AzaharRuntime::RunStep() {
    const auto result = Core::System::GetInstance().RunLoop();
    if (result == Core::System::ResultStatus::ShutdownRequested) {
        return RunState::ShutdownRequested;
    }
    return result == Core::System::ResultStatus::Success ? RunState::Running : RunState::Failed;
}

void AzaharRuntime::Stop() {
    Core::System::GetInstance().Shutdown();
}

PerfStats AzaharRuntime::TakePerfStats() {
    const auto stats = Core::System::GetInstance().GetAndResetPerfStats();
    return {stats.game_fps, stats.system_fps, stats.emulation_speed};
}

void AzaharRuntime::WaitForBackgroundTasks() {
    impl->detached_tasks.WaitForAllTasks();
}

void AzaharRuntime::InitInput() {
    InputCommon::Init();
    Input::Register();
}

void AzaharRuntime::ShutdownInput() {
    InputCommon::Shutdown();
}

void AzaharRuntime::ReloadInputDevices() {
    auto& system = Core::System::GetInstance();
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

void AzaharRuntime::InitNetwork() {
    Network::Init();
}

void AzaharRuntime::ShutdownNetwork() {
    Network::Shutdown();
}

void AzaharRuntime::NewConsoleId() {
    const auto cfg = Service::CFG::GetModule(Core::System::GetInstance());
    const auto [random_number, console_id] = cfg->GenerateConsoleUniqueId();
    cfg->SetConsoleUniqueId(random_number, console_id);
    cfg->UpdateConfigNANDSavegame();
    APP_LOG_INFO("the console has a new ID: {:016x}", console_id);
}

void AzaharRuntime::CommitOptions() {
    auto& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.ApplySettings();
}

Core::System* AzaharRuntime::HostSystem() {
    return &Core::System::GetInstance();
}

std::string AzaharRuntime::UserDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
}

void AzaharRuntime::SetUserDir(const std::string& dir) {
    FileUtil::SetUserPath(dir);
}

std::string AzaharRuntime::GameOptionsPath() const {
    return Port::OptionsPath();
}

std::string AzaharRuntime::GameOptionsTemplate() const {
    return Port::OptionsTemplate();
}

BuildInfo AzaharRuntime::Build() const {
    return {Common::g_build_fullname, Common::g_scm_branch, Common::g_scm_desc};
}

void AzaharRuntime::StartLogging(const std::string& file_name, const std::string& filter) {
    Common::Log::Initialize(file_name);
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();
    Common::Log::Filter log_filter;
    log_filter.ParseFilterString(filter.empty() ? Settings::values.log_filter.GetValue() : filter);
    Common::Log::SetGlobalFilter(log_filter);
}

void AzaharRuntime::StopLogging() {
    Common::Log::Stop();
}

void AzaharRuntime::LogOptions() {
    Settings::LogSettings();
}

} // namespace App::Azahar
