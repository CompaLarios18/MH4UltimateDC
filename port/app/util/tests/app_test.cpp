// The app's logic that sits on IRuntime (N0-20), run against a fake runtime with synthetic data:
// the settings file (config.cpp), the launcher's setup checks (launcher/checks.cpp), and the
// helpers of runtime.h. Built with -DMH4U_APP_TESTS=ON (port/app/CMakeLists.txt) or by hand
// against doctest, {fmt} and inih. The fake's button names are made up ("key0"), so what the
// settings file says about them comes from the runtime, not from the app.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "config.h"
#include "launcher/checks.h"
#include "runtime.h"
#include "util/file.h"
#include "util/log.h"

namespace fs = std::filesystem;
using namespace App;

namespace {

struct FakeRuntime final : IRuntime {
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
    LoadResult Load(const LoadRequest&, Window&, Window*) override { return LoadResult::Ok; }
    std::string StatusDetails() const override { return ""; }
    void Start() override {}
    RunState RunStep() override { return RunState::Running; }
    void SetPaused(bool) override {}
    void Stop() override {}
    RuntimeStatus Status() const override { return RuntimeStatus::Idle; }
    PerfStats TakePerfStats() override { return {}; }
    void WaitForBackgroundTasks() override {}
    void InitInput() override {}
    void ShutdownInput() override {}
    void ReloadInputDevices() override {}
    void InitNetwork() override {}
    void ShutdownNetwork() override {}
    void NewConsoleId() override {}
    void CommitOptions() override {}
    Core::System* HostSystem() override { return nullptr; }

    std::string UserDir() const override { return user_dir; }
    void SetUserDir(const std::string& dir) override { user_dir = dir; }
    std::string SdmcDir() const override { return user_dir + "sdmc/"; }
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
    Env() {
        runtime.user_dir = dir.str();
        SetRuntime(&runtime);
        collector = this;
        Util::SetLogSink([](Util::LogLevel, const char*, unsigned, const char*, const std::string& message) {
            collector->logged.push_back(message);
        });
    }
    ~Env() {
        Util::SetLogSink(nullptr);
        SetRuntime(nullptr);
        collector = nullptr;
    }
    bool Logged(const std::string& part) const {
        for (const std::string& line : logged) {
            if (line.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    static Env* collector;
};
Env* Env::collector = nullptr;

}  // namespace

// ── runtime.h ──

TEST_CASE("renderer and layout names map the way the settings file says") {
    CHECK(RendererFor("vulkan") == Renderer::Vulkan);
    CHECK(RendererFor("opengl") == Renderer::OpenGL);
    CHECK(RendererFor("software") == Renderer::Software);
    CHECK(RendererFor("") == Renderer::Vulkan);
    CHECK(RendererFor("OpenGL") == Renderer::Vulkan);  // names are lower case
    CHECK(ScreenLayoutFor("single") == ScreenLayout::Single);
    CHECK(ScreenLayoutFor("stacked") == ScreenLayout::Stacked);
    CHECK(ScreenLayoutFor("side") == ScreenLayout::Side);
    CHECK(ScreenLayoutFor("windows") == ScreenLayout::SeparateWindows);
    CHECK(ScreenLayoutFor("overlay") == ScreenLayout::Overlay);
    CHECK(ScreenLayoutFor("anything else") == ScreenLayout::Overlay);
}

TEST_CASE("the 3DS's buttons are numbered from 0 with one slot per button") {
    CHECK(Button::A == 0);
    CHECK(Button::Count == Button::Power + 1);
    CHECK(Analog::CirclePad == 0);
    CHECK(Analog::Count == 2);
}

// ── config.cpp ──

TEST_CASE("a missing settings file is written with the defaults, with the runtime's key names") {
    Env env;
    const Config c = Load();
    const Config d = Defaults();
    CHECK(c.keys == d.keys);
    CHECK(c.stick_keys == d.stick_keys);
    CHECK(ConfigPath() == env.dir.str("mh4u-app.ini"));
    REQUIRE(Util::Exists(ConfigPath()));
    std::string text;
    Util::ReadFileToString(true, ConfigPath(), text);
    CHECK(text.find("[Controls]") != std::string::npos);
    CHECK(text.find(fmt::format("key{} = E\n", static_cast<int>(Button::A))) != std::string::npos);
    CHECK(text.find("stickA = W,S,A,D,Left Ctrl\n") != std::string::npos);
    CHECK(text.find("stickB = Up,Down,Left,Right\n") != std::string::npos);
    CHECK(env.Logged("with the defaults"));
}

TEST_CASE("the defaults put the keys where the app says") {
    Env env;
    const Config d = Defaults();
    CHECK(d.keys[Button::A] == "E");
    CHECK(d.keys[Button::B] == "Space");
    CHECK(d.keys[Button::R] == "Left Shift");
    CHECK(d.keys[Button::ZR] == "C");
    CHECK(d.keys[Button::Home].empty());
    CHECK(d.stick_keys[Analog::CStick] == "Up,Down,Left,Right");
    CHECK(d.controller_auto);
    CHECK(d.renderer == "vulkan");
    CHECK(d.screens == "overlay");
}

TEST_CASE("Save then Load keeps every setting") {
    Env env;
    Config c = Defaults();
    c.game = "/games/a b.cxi";
    c.launcher = false;
    c.new_3ds = "off";
    c.cpu_clock = 150;
    c.renderer = "opengl";
    c.fullscreen = false;
    c.resolution = 3;
    c.vsync = false;
    c.screens = "side";
    c.linear_filter = false;
    c.custom_textures = true;
    c.shader = "sharp";
    c.fps_counter = "top-right";
    c.volume = 40;
    c.audio_output = "null";
    c.keys[Button::A] = "Z";
    c.stick_keys[Analog::CStick] = "I,K,J,L";
    c.button_params[Button::X] = "engine:sdl,button:2";
    c.stick_params[Analog::CirclePad] = "engine:sdl,axis_x:0,axis_y:1";
    c.controller_auto = false;
    c.touch_screen_key = "F9";
    c.nickname = "Hunter";
    c.join = "example.org:1234";
    c.password = "pw";
    c.host = 24000;
    c.room_port = 24001;
    c.room_name = "Room";
    c.room_members = 8;
    c.lobby = "http://lobby";
    c.token = "tok";
    c.room_public = false;
    c.last_room = "host";
    c.catalog_url = "http://catalog";
    Save(c);

    const Config back = Load();
    CHECK(back.game == c.game);
    CHECK(back.launcher == false);
    CHECK(back.new_3ds == "off");
    CHECK(back.cpu_clock == 150);
    CHECK(back.renderer == "opengl");
    CHECK(back.fullscreen == false);
    CHECK(back.resolution == 3);
    CHECK(back.vsync == false);
    CHECK(back.screens == "side");
    CHECK(back.linear_filter == false);
    CHECK(back.custom_textures == true);
    CHECK(back.shader == "sharp");
    CHECK(back.fps_counter == "top-right");
    CHECK(back.volume == 40);
    CHECK(back.audio_output == "null");
    CHECK(back.keys == c.keys);
    CHECK(back.stick_keys == c.stick_keys);
    CHECK(back.button_params == c.button_params);
    CHECK(back.stick_params == c.stick_params);
    CHECK(back.controller_auto == false);
    CHECK(back.touch_screen_key == "F9");
    CHECK(back.nickname == "Hunter");
    CHECK(back.join == "example.org:1234");
    CHECK(back.password == "pw");
    CHECK(back.host == 24000);
    CHECK(back.room_port == 24001);
    CHECK(back.room_name == "Room");
    CHECK(back.room_members == 8);
    CHECK(back.lobby == "http://lobby");
    CHECK(back.token == "tok");
    CHECK(back.room_public == false);
    CHECK(back.last_room == "host");
    CHECK(back.catalog_url == "http://catalog");
}

TEST_CASE("the keys of the file are the runtime's names, and a file with other names changes nothing") {
    Env env;
    Util::WriteStringToFile(true, ConfigPath(),
                            fmt::format("[Controls]\nkey{} = Q\nbutton_a = X\nstickA = A,B,C,D\n", static_cast<int>(Button::A)));
    const Config c = Load();
    CHECK(c.keys[Button::A] == "Q");     // the runtime calls the A button key0
    CHECK(c.keys[Button::B] == Defaults().keys[Button::B]);
    CHECK(c.stick_keys[Analog::CirclePad] == "A,B,C,D");
}

TEST_CASE("values are clamped and on/off words are understood") {
    Env env;
    Util::WriteStringToFile(true, ConfigPath(),
                            "[Game]\ncpu_clock = 1000\nlauncher = no\n[Display]\nresolution = 99\nfullscreen = yes\n"
                            "vsync = 1\nlinear_filter = true\ncustom_textures = on\n"
                            "[Audio]\nvolume = 500\n[Multiplayer]\nroom_members = 1\nroom_port = 0\nhost = 70000\n");
    const Config c = Load();
    CHECK(c.cpu_clock == 400);
    CHECK(c.launcher == false);   // anything but 1/on/yes/true is off
    CHECK(c.resolution == 10);
    CHECK(c.fullscreen == true);
    CHECK(c.vsync == true);
    CHECK(c.linear_filter == true);
    CHECK(c.custom_textures == true);
    CHECK(c.volume == 100);
    CHECK(c.room_members == 2);
    CHECK(c.room_port == 1);
    CHECK(c.host == 65535);
}

TEST_CASE("a line that is not understood is logged and the rest is read") {
    Env env;
    Util::WriteStringToFile(true, ConfigPath(), "[Game]\nlauncher = off\nthis is not a setting\n[Audio]\nvolume = 30\n");
    const Config c = Load();
    CHECK(env.Logged("isn't understood"));
    CHECK(c.launcher == false);
    CHECK(c.volume == 30);
}

TEST_CASE("the perf_log setting is kept for the runtime and written back") {
    Env env;
    Util::WriteStringToFile(true, ConfigPath(), "[Debug]\nperf_log = /tmp/frames.csv\n");
    Config c = Load();
    CHECK(PerfLogPath() == "/tmp/frames.csv");
    c.nickname = "n";
    Save(c);
    std::string text;
    Util::ReadFileToString(true, ConfigPath(), text);
    CHECK(text.find("perf_log = /tmp/frames.csv\n") != std::string::npos);
}

TEST_CASE("Save into a folder that can't be made logs the error and does not throw") {
    Env env;
    env.runtime.user_dir = env.dir.str("blocker/");
    Util::WriteStringToFile(false, env.dir.str("blocker"), "a file where the folder should be");
    Save(Defaults());
    CHECK(env.Logged("couldn't write"));
    CHECK_FALSE(Util::IsDirectory(env.dir.str("blocker")));
}

// ── launcher/checks.cpp ──

using namespace Launcher::Checks;

TEST_CASE("CheckGame: no path, no file, a folder, and what the header says") {
    Env env;
    CHECK(CheckGame("").state == Game::State::Unset);
    CHECK(CheckGame(env.dir.str("missing.cxi")).state == Game::State::Missing);
    CHECK(CheckGame(env.dir.str()).state == Game::State::Missing);  // a folder is not a game
    CHECK(env.runtime.asked.empty());                               // none of them reached the runtime

    Util::WriteStringToFile(false, env.dir.str("g.cxi"), "x");
    env.runtime.header = {GameHeader::State::Unreadable, 0};
    CHECK(CheckGame(env.dir.str("g.cxi")).state == Game::State::NotA3ds);
    env.runtime.header = {GameHeader::State::Ok, MH4U_USA};
    const Game ok = CheckGame(env.dir.str("g.cxi"));
    CHECK(ok.state == Game::State::Ok);
    CHECK(ok.program_id == MH4U_USA);
    env.runtime.header = {GameHeader::State::Ok, 0x0004000000123400ull};
    const Game other = CheckGame(env.dir.str("g.cxi"));
    CHECK(other.state == Game::State::OtherGame);
    CHECK(other.program_id == 0x0004000000123400ull);
    env.runtime.header = {GameHeader::State::Encrypted, MH4U_USA};
    const Game encrypted = CheckGame(env.dir.str("g.cxi"));
    CHECK(encrypted.state == Game::State::Encrypted);
    CHECK(encrypted.program_id == MH4U_USA);
    CHECK(Describe(encrypted).find("encrypted") != std::string::npos);
    CHECK(Describe(other).find("0004000000123400") != std::string::npos);
    CHECK(Describe(Game{}).find("Choose your decrypted") != std::string::npos);
}

TEST_CASE("CheckUpdate: nothing installed, no content file, another version, the right one") {
    Env env;
    CHECK(CheckUpdate().state == Update::State::Missing);  // not installed

    env.runtime.installed = {true, UPDATE_VERSION, ""};
    CHECK(CheckUpdate().state == Update::State::Missing);  // installed, but the title has no content

    env.runtime.installed = {true, UPDATE_VERSION, env.dir.str("content.app")};
    Update u = CheckUpdate();
    CHECK(u.state == Update::State::Missing);              // the content file isn't on disk
    CHECK(u.content.empty());

    Util::WriteStringToFile(false, env.dir.str("content.app"), "x");
    u = CheckUpdate();
    CHECK(u.state == Update::State::Ok);
    CHECK(u.version == UPDATE_VERSION);
    CHECK(u.content == env.dir.str("content.app"));

    env.runtime.installed.version = 1040;
    u = CheckUpdate();
    CHECK(u.state == Update::State::OtherVersion);
    CHECK(u.version == 1040);
    CHECK(u.content == env.dir.str("content.app"));
}

TEST_CASE("VersionText splits a title version into major, minor and micro") {
    CHECK(VersionText(UPDATE_VERSION) == "1.3.0");
    CHECK(VersionText(0) == "0.0.0");
    CHECK(VersionText(static_cast<u16>((2 << 10) | (5 << 4) | 7)) == "2.5.7");
}

TEST_CASE("CheckUpdateCia says why a file is not the update, and nothing when it is") {
    Env env;
    env.runtime.cia_check = CiaResult::Encrypted;
    CHECK(CheckUpdateCia("a.cia").find("encrypted") != std::string::npos);
    env.runtime.cia_check = CiaResult::Unreadable;
    CHECK(CheckUpdateCia("a.cia") == "The file can't be read.");
    env.runtime.cia_check = CiaResult::Invalid;
    CHECK(CheckUpdateCia("a.cia") == "This file isn't a CIA the app can read.");
    env.runtime.cia_check = CiaResult::Aborted;
    CHECK(CheckUpdateCia("a.cia") == "This file isn't a CIA the app can read.");

    env.runtime.cia_check = CiaResult::Ok;
    env.runtime.cia_info = std::nullopt;                       // no title information: nothing to object to
    CHECK(CheckUpdateCia("a.cia").empty());
    env.runtime.cia_info = CiaInfo{0x0004000000999900ull, UPDATE_VERSION};
    CHECK(CheckUpdateCia("a.cia").find("0004000000999900") != std::string::npos);
    env.runtime.cia_info = CiaInfo{MH4U_UPDATE, 1040};
    const std::string wrong_version = CheckUpdateCia("a.cia");
    CHECK(wrong_version.find("v1.1.0") != std::string::npos);
    CHECK(wrong_version.find("v1.3.0") != std::string::npos);
    env.runtime.cia_info = CiaInfo{MH4U_UPDATE, UPDATE_VERSION};
    CHECK(CheckUpdateCia("a.cia").empty());
}

TEST_CASE("Install reports the runtime's result and progress on its own thread") {
    struct Case {
        CiaResult result;
        const char* error;  ///< a part of the message; empty: none
    };
    const Case cases[] = {{CiaResult::Ok, ""},
                          {CiaResult::Encrypted, "encrypted"},
                          {CiaResult::Unreadable, "can't be read"},
                          {CiaResult::Aborted, "half-way"},
                          {CiaResult::Invalid, "isn't a CIA"}};
    for (const Case& c : cases) {
        Env env;
        env.runtime.cia_install = c.result;
        Install install;
        install.Start("u.cia");
        for (int i = 0; i < 500 && install.Running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE_FALSE(install.Running());
        CHECK(install.Progress() == 1.0f);
        if (std::string(c.error).empty()) {
            CHECK(install.Error().empty());
        } else {
            CHECK(install.Error().find(c.error) != std::string::npos);
        }
        CHECK(env.Logged("installing u.cia"));
    }
}

TEST_CASE("the code library is looked for in the runtime's user folder") {
    Env env;
    const std::string path = LibraryPath();
    CHECK(path.rfind(env.dir.str() + "recomp/0004000000126300.", 0) == 0);
    CHECK((path.ends_with(".so") || path.ends_with(".dll") || path.ends_with(".dylib")));
}

TEST_CASE("CodeFromCheck turns an exit status and the notice into a verdict") {
    CHECK(CodeFromCheck(0, "backend: x\n").state == Code::State::Ok);
    const Code partial = CodeFromCheck(3, "backend: x\nnotice:\nFirst paragraph.\n\nHow to build it.\n");
    CHECK(partial.state == Code::State::Partial);
    CHECK(partial.notice == "First paragraph.");
    const Code unused = CodeFromCheck(2, "backend: x\n");
    CHECK(unused.state == Code::State::NotUsed);
    CHECK(unused.notice == "The game would run without the port's code library.");
    CHECK(CodeFromCheck(2, "notice:\nWhy.\n").notice == "Why.");
    const Code failed = CodeFromCheck(9, "");
    CHECK(failed.state == Code::State::Failed);
    CHECK(failed.notice == "The check ended with status 9.");
    CHECK(CodeFromCheck(9, std::string(1000, 'x')).notice.size() == 400);
}
