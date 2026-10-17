// The IRuntime for Azahar's core (runtime.h). Nothing here includes Azahar: the implementation
// files in this folder do.
#pragma once

#include <memory>

#include "runtime.h"

namespace App::Azahar {

class AzaharRuntime final : public IRuntime {
public:
    AzaharRuntime();
    ~AzaharRuntime() override;

    std::string PrepareCode(const LoadRequest& request) override;
    std::string CodeNotice() override;
    LoadResult Load(const LoadRequest& request, Window& window, Window* secondary) override;
    std::string StatusDetails() const override;
    void Start() override;
    RunState RunStep() override;
    void Stop() override;
    PerfStats TakePerfStats() override;
    void WaitForBackgroundTasks() override;

    void InitInput() override;
    void ShutdownInput() override;
    void ReloadInputDevices() override;
    void InitNetwork() override;
    void ShutdownNetwork() override;
    void NewConsoleId() override;
    void CommitOptions() override;
    Core::System* HostSystem() override;

    std::string UserDir() const override;
    void SetUserDir(const std::string& dir) override;
    std::string GameOptionsPath() const override;
    std::string GameOptionsTemplate() const override;

    BuildInfo Build() const override;
    void StartLogging(const std::string& file_name, const std::string& filter) override;
    void StopLogging() override;
    void LogOptions() override;

    void ApplyConfig(const Config& config) override;
    Renderer ActiveRenderer() const override;
    void SetActiveRenderer(Renderer renderer) override;
    ScreenLayout CurrentLayout() const override;
    void SetLayout(ScreenLayout layout) override;
    void ApplyLayout(ScreenLayout layout) override;
    void SetResolution(int factor) override;
    void SetLinearFilter(bool on) override;
    void SetVsync(bool on) override;
    void SetVolume(int percent) override;
    void SetCpuClock(int percent) override;
    bool SetGameOption(const std::string& key, const std::string& value) override;
    void SetLayoutEditing(bool on) override;

    const char* ButtonSettingName(int button) const override;
    const char* StickSettingName(int stick) const override;
    std::string ButtonBinding(int button) const override;
    std::string StickBinding(int stick) const override;
    std::string DescribeControllerInput(const std::string& binding) const override;

    GameHeader ReadGameHeader(const std::string& path) const override;
    InstalledTitle FindInstalledTitle(std::uint64_t title_id) const override;
    CiaResult CheckCia(const std::string& path, std::optional<CiaInfo>& info) const override;
    CiaResult InstallCia(const std::string& path,
                         const std::function<void(std::size_t, std::size_t)>& progress) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace App::Azahar
