// A fake IRuntime for the app's unit tests (app_test.cpp, multiplayer_test.cpp): it records what
// the app asks and answers from what the test sets, with synthetic data. The button names are
// made up ("key0"), so what the settings file says about them comes from the runtime, not from
// the app.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "config.h"
#include "runtime.h"
#include "util/log.h"

namespace fake {

namespace fs = std::filesystem;
using namespace App;

struct FakeRuntime final : IRuntime {
    ~FakeRuntime() override {
        for (std::thread& t : network_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // what the tests set
    std::string user_dir;
    GameHeader header;
    InstalledTitle installed;
    CiaResult cia_check = CiaResult::Ok;
    std::optional<CiaInfo> cia_info;
    CiaResult cia_install = CiaResult::Ok;
    std::vector<std::string> asked;  ///< the paths the checks asked about

    std::string PrepareCode(const LoadRequest&) override { return ""; }
    std::string CodeNotice() override { return ""; }
    LoadResult Load(const LoadRequest&, IWindow&, IWindow*) override { return LoadResult::Ok; }
    std::string StatusDetails() const override { return ""; }
    void Start() override {}
    RunState RunStep() override { return RunState::Running; }
    void Stop() override {}
    PerfStats TakePerfStats() override { return {}; }
    void WaitForBackgroundTasks() override {}
    void InitInput() override {}
    void ShutdownInput() override {}
    // what the app asked of the input layer
    struct ProfileCall {
        bool controller_given = false;
        ControllerInfo controller;
    };
    std::vector<ProfileCall> profiles;
    std::vector<bool> mutes;
    std::vector<int> keys_down, keys_up;
    int keys_released = 0;
    void BuildInputProfile(const Config&, const ControllerInfo* controller) override {
        profiles.push_back({controller != nullptr, controller ? *controller : ControllerInfo{}});
    }
    void ReloadInputDevices() override {}
    void SetInputMuted(bool muted) override { mutes.push_back(muted); }
    void KeyDown(int scancode) override { keys_down.push_back(scancode); }
    void KeyUp(int scancode) override { keys_up.push_back(scancode); }
    void ReleaseAllKeys() override { ++keys_released; }
    // the mouse as the camera
    std::vector<std::pair<float, float>> mouse_sticks;  ///< what the app pushed the C-stick with
    bool camera_turn_active = false;                    ///< what CameraTurnActive answers
    std::vector<std::pair<float, float>> camera_turns;  ///< (yaw, pitch) the app added
    int camera_turn_clears = 0;
    void SetMouseStick(float x, float y) override { mouse_sticks.push_back({x, y}); }
    bool CameraTurnActive() override { return camera_turn_active; }
    void AddCameraTurn(float yaw, float pitch) override { camera_turns.push_back({yaw, pitch}); }
    void ClearCameraTurn() override { ++camera_turn_clears; }
    void SetSoftwareKeyboard(std::shared_ptr<ISoftwareKeyboard>) override {}
    struct Finished {
        std::string text;
        int button = 0;
    };
    std::vector<Finished> finished;  ///< what the keyboard handed over
    KeyboardError finish_result = KeyboardError::None;
    KeyboardError FinishKeyboard(const std::string& text, std::uint8_t button) override {
        finished.push_back({text, button});
        return finish_result;
    }
    // ── the room, scripted ──
    // What a join attempt comes to: the room lets this console in, or it doesn't (the state goes
    // back to Idle first and the reason follows a moment later, as the real client does).
    struct JoinOutcome {
        bool joins = true;
        std::optional<RoomError> error;
    };
    bool rooms_available = true;
    std::vector<JoinOutcome> join_script;  ///< attempt n uses entry n; later ones use default_outcome
    JoinOutcome default_outcome;
    int join_delay_ms = 15;  ///< how long the room takes to answer a join
    RoomOpenResult open_result{true, RoomOpenResult::Listing::None, ""};
    std::string console_username = "Hunter";
    std::string console_id = "0123456789abcdef";
    MacAddress console_mac{0x40, 0xf4, 0x07, 0x11, 0x22, 0x33};
    MacAddress assigned_mac{0x40, 0xf4, 0x07, 0xaa, 0xbb, 0xcc};  ///< what a room gives when none is asked for
    bool game_running = true;
    RoomDetails details{"Room", 4};
    std::vector<RoomMemberInfo> members;
    // what the app did (read with `room_mutex` or after the jobs ended)
    std::mutex room_mutex;
    std::vector<RoomJoinRequest> joins;
    std::vector<RoomOpenRequest> opens;
    std::vector<MacAddress> mac_sets;
    int leaves = 0, closes = 0, binds = 0, unbinds = 0, console_data_prepared = 0;
    MacAddress console_mac_now{console_mac};
    RoomCallbacks callbacks;
    RoomState room_state = RoomState::Idle;
    bool hosting = false;
    std::string joined_nickname;
    MacAddress room_mac{};
    std::vector<std::thread> network_threads;

    /// Runs `action` on a thread of its own, after `delay_ms`: the network's thread.
    void OnNetworkThread(int delay_ms, std::function<void()> action) {
        std::scoped_lock lock{thread_mutex};
        network_threads.emplace_back([delay_ms, action = std::move(action)] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            action();
        });
    }
    std::size_t JoinCount() {
        std::scoped_lock lock{room_mutex};
        return joins.size();
    }
    RoomCallbacks Callbacks() {
        std::scoped_lock lock{room_mutex};
        return callbacks;
    }

    void InitNetwork() override {}
    void ShutdownNetwork() override {}
    bool RoomsAvailable() const override { return rooms_available; }
    void PrepareConsoleData() override { ++console_data_prepared; }
    std::string ConsoleUsername() const override { return console_username; }
    std::string ConsoleIdHash() const override { return console_id; }
    MacAddress ConsoleMac() const override { return console_mac; }
    bool IsGameRunning() const override { return game_running; }
    bool SetConsoleMac(const MacAddress& mac) override {
        std::scoped_lock lock{room_mutex};
        if (!game_running || console_mac_now == mac) {
            return false;
        }
        console_mac_now = mac;
        mac_sets.push_back(mac);
        return true;
    }
    void BindRoomCallbacks(RoomCallbacks c) override {
        std::scoped_lock lock{room_mutex};
        callbacks = std::move(c);
        ++binds;
    }
    void UnbindRoomCallbacks() override {
        std::scoped_lock lock{room_mutex};
        callbacks = {};
        ++unbinds;
    }
    void JoinRoom(const RoomJoinRequest& request) override {
        JoinOutcome outcome;
        {
            std::scoped_lock lock{room_mutex};
            outcome = joins.size() < join_script.size() ? join_script[joins.size()] : default_outcome;
            joins.push_back(request);
            room_state = RoomState::Joining;
            joined_nickname = request.nickname;
            room_mac = request.preferred_mac ? *request.preferred_mac : assigned_mac;
        }
        OnNetworkThread(join_delay_ms, [this, outcome] {
            if (outcome.joins) {
                {
                    std::scoped_lock lock{room_mutex};
                    room_state = RoomState::Joined;
                }
                const RoomCallbacks c = Callbacks();
                if (c.on_state) {
                    c.on_state(RoomState::Joined);
                }
                if (c.on_info) {
                    c.on_info(details, members);
                }
                return;
            }
            {
                std::scoped_lock lock{room_mutex};
                room_state = RoomState::Idle;
            }
            const RoomCallbacks c = Callbacks();
            if (c.on_state) {
                c.on_state(RoomState::Idle);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (outcome.error && c.on_error) {
                c.on_error(*outcome.error);
            }
        });
    }
    void LeaveRoom() override {
        {
            std::scoped_lock lock{room_mutex};
            ++leaves;
            room_state = RoomState::Idle;
        }
        OnNetworkThread(0, [this] {
            const RoomCallbacks c = Callbacks();
            if (c.on_state) {
                c.on_state(RoomState::Idle);
            }
        });
    }
    RoomState RoomMemberState() const override {
        std::scoped_lock lock{const_cast<std::mutex&>(room_mutex)};
        return room_state;
    }
    bool RoomConnected() const override {
        const RoomState state = RoomMemberState();
        return state == RoomState::Joined || state == RoomState::Moderator;
    }
    MacAddress RoomMacAddress() const override {
        std::scoped_lock lock{const_cast<std::mutex&>(room_mutex)};
        return room_mac;
    }
    std::string RoomNickname() const override {
        std::scoped_lock lock{const_cast<std::mutex&>(room_mutex)};
        return joined_nickname;
    }
    RoomDetails RoomInformation() const override { return details; }
    std::vector<RoomMemberInfo> RoomMembers() const override { return members; }
    std::vector<std::string> EndSkipPending() override { return {}; }
    bool HostingRoom() const override {
        std::scoped_lock lock{const_cast<std::mutex&>(room_mutex)};
        return hosting;
    }
    RoomOpenResult OpenRoom(const RoomOpenRequest& request) override {
        std::scoped_lock lock{room_mutex};
        opens.push_back(request);
        hosting = open_result.ok;
        return open_result;
    }
    void CloseRoom() override {
        std::scoped_lock lock{room_mutex};
        ++closes;
        hosting = false;
    }
    std::mutex thread_mutex;
    void NewConsoleId() override {}
    void CommitOptions() override {}

    std::string UserDir() const override { return user_dir; }
    void SetUserDir(const std::string& dir) override { user_dir = dir; }
    std::string GameOptionsPath() const override { return user_dir + "mh4u.ini"; }
    std::string GameOptionsTemplate() const override { return ""; }
    BuildInfo Build() const override { return {"fake 1", "main", "abc"}; }
    void StartLogging(const std::string&, const std::string&) override {}
    void StopLogging() override {}
    void LogOptions() override {}

    void ApplyConfig(const Config&) override {}
    Renderer ActiveRenderer() const override { return Renderer::Vulkan; }
    void SetActiveRenderer(Renderer) override {}
    ScreenLayout CurrentLayout() const override { return ScreenLayout::Overlay; }
    void SetLayout(ScreenLayout) override {}
    void ApplyLayout(ScreenLayout) override {}
    void SetResolution(int) override {}
    void SetLinearFilter(bool) override {}
    void SetVsync(bool) override {}
    void SetVolume(int) override {}
    void SetCpuClock(int) override {}
    bool SetGameOption(const std::string&, const std::string&) override { return false; }
    void SetLayoutEditing(bool) override {}

    const char* ButtonSettingName(int button) const override {
        static std::vector<std::string> names;
        if (names.empty()) {
            for (int i = 0; i < Button::Count; ++i) {
                names.push_back(fmt::format("key{}", i));
            }
        }
        return names[static_cast<std::size_t>(button)].c_str();
    }
    const char* StickSettingName(int stick) const override {
        return stick == Analog::CirclePad ? "stickA" : "stickB";
    }
    std::string ButtonBinding(int) const override { return ""; }
    std::string StickBinding(int) const override { return ""; }
    std::string DescribeControllerInput(const std::string&) const override { return ""; }

    GameHeader ReadGameHeader(const std::string& path) const override {
        const_cast<FakeRuntime*>(this)->asked.push_back(path);
        return header;
    }
    InstalledTitle FindInstalledTitle(std::uint64_t) const override { return installed; }
    CiaResult CheckCia(const std::string&, std::optional<CiaInfo>& info) const override {
        info = cia_info;
        return cia_check;
    }
    CiaResult InstallCia(const std::string&,
                         const std::function<void(std::size_t, std::size_t)>& progress) override {
        if (progress) {
            progress(0, 0);  // nothing to measure yet
            progress(50, 100);
        }
        return cia_install;
    }
};

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("mh4u_app_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str(const std::string& name = "") const {
        return name.empty() ? path.generic_string() + "/" : (path / name).generic_string();
    }
};

/// Installs a fake as the app's runtime for a test, with a user folder of its own.
struct Env {
    TempDir dir;
    FakeRuntime runtime;
    std::vector<std::string> logged;
    std::mutex log_mutex;
    Env() {
        runtime.user_dir = dir.str();
        SetRuntime(&runtime);
        collector = this;
        Util::SetLogSink([](Util::LogLevel, const char*, unsigned, const char*, const std::string& message) {
            std::scoped_lock lock{collector->log_mutex};
            collector->logged.push_back(message);
        });
    }
    ~Env() {
        Util::SetLogSink(nullptr);
        SetRuntime(nullptr);
        collector = nullptr;
    }
    bool Logged(const std::string& part) {
        std::scoped_lock lock{log_mutex};
        for (const std::string& line : logged) {
            if (line.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    static inline Env* collector = nullptr;
};


} // namespace fake
