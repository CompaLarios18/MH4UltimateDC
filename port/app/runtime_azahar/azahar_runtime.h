// The IRuntime for Azahar's core (runtime.h). Nothing here includes Azahar: the implementation
// files in this folder do.
#pragma once

#include <memory>

#include "runtime.h"

namespace Core {
class System;
}

namespace App::Azahar {

class AzaharRuntime final : public IRuntime {
public:
    AzaharRuntime();
    ~AzaharRuntime() override;

    std::string PrepareCode(const LoadRequest& request) override;
    std::string CodeNotice() override;
    /// `window` and `secondary` are App::Window (window.h), which main() made.
    LoadResult Load(const LoadRequest& request, IWindow& window, IWindow* secondary) override;
    std::string StatusDetails() const override;
    void Start() override;
    RunState RunStep() override;
    void Stop() override;
    PerfStats TakePerfStats() override;
    void WaitForBackgroundTasks() override;

    void InitInput() override;
    void ShutdownInput() override;
    void BuildInputProfile(const Config& config, const ControllerInfo* controller) override;
    void ReloadInputDevices() override;
    void SetInputMuted(bool muted) override;
    void KeyDown(int scancode) override;
    void KeyUp(int scancode) override;
    void ReleaseAllKeys() override;
    void SetMouseStick(float x, float y) override;
    bool CameraTurnActive() override;
    void AddCameraTurn(float yaw, float pitch) override;
    void ClearCameraTurn() override;
    void SetSoftwareKeyboard(std::shared_ptr<ISoftwareKeyboard> keyboard) override;
    KeyboardError FinishKeyboard(const std::string& text, std::uint8_t button) override;
    void InitNetwork() override;
    void ShutdownNetwork() override;
    bool RoomsAvailable() const override;
    void PrepareConsoleData() override;
    std::string ConsoleUsername() const override;
    std::string ConsoleIdHash() const override;
    MacAddress ConsoleMac() const override;
    bool IsGameRunning() const override;
    bool SetConsoleMac(const MacAddress& mac) override;
    void BindRoomCallbacks(RoomCallbacks callbacks) override;
    void UnbindRoomCallbacks() override;
    void JoinRoom(const RoomJoinRequest& request) override;
    void LeaveRoom() override;
    RoomState RoomMemberState() const override;
    bool RoomConnected() const override;
    MacAddress RoomMacAddress() const override;
    std::string RoomNickname() const override;
    RoomDetails RoomInformation() const override;
    std::vector<RoomMemberInfo> RoomMembers() const override;
    bool HostingRoom() const override;
    RoomOpenResult OpenRoom(const RoomOpenRequest& request) override;
    void CloseRoom() override;
    void NewConsoleId() override;
    void CommitOptions() override;

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

    /// The Azahar system the windows are made for (Window::Create). Not part of IRuntime: only
    /// the Azahar app's main() knows this runtime's kind.
    Core::System& CoreSystem();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace App::Azahar
