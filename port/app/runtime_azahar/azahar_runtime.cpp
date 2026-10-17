#include "runtime_azahar/azahar_runtime.h"

#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "frontend.h"
#include "runtime_azahar/log_sink.h"

namespace App::Azahar {

AzaharRuntime::AzaharRuntime() {
    InstallLogSink();
}

AzaharRuntime::~AzaharRuntime() = default;

std::string AzaharRuntime::UserDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
}

void AzaharRuntime::SetUserDir(const std::string& dir) {
    FileUtil::SetUserPath(dir);
}

std::string AzaharRuntime::SdmcDir() const {
    return FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
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
