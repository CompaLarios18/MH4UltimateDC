// IRuntime: what the app asks of the emulation runtime underneath it. The launcher, the in-game
// menu and main() call these methods instead of the runtime's own headers; the Azahar build
// implements them in runtime_azahar/. The window and renderer integration (EmuWindow, the
// overlay renderers) is not behind it yet.
//
// Threads: the notes say which thread the app calls each method from. In this build the main
// thread and the emulation thread are the same one (the thread that runs main(): it runs
// RunStep in a loop and the menu builds its frames inside it); the windows' present threads and
// the launcher's worker threads only call the ones marked "any thread".
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "util/room.h"
#include "util/swkbd.h"

namespace Core {
class System;
}

namespace App {

struct Config;
class Window;

/// The graphics API the game is drawn with.
enum class Renderer { Vulkan, OpenGL, Software };

/// Where the two 3DS screens go. Overlay is the game filling the window with the touch screen
/// over it in a corner (mh4u.ini's touch_overlay says where). Other is a layout of the runtime's that the app never sets.
enum class ScreenLayout { Overlay, Stacked, Single, Side, SeparateWindows, Other };

/// The renderer a settings-file name stands for (vulkan, opengl, software); any other is Vulkan.
inline Renderer RendererFor(const std::string& name) {
    if (name == "opengl") {
        return Renderer::OpenGL;
    }
    if (name == "software") {
        return Renderer::Software;
    }
    return Renderer::Vulkan;
}

/// The layout a settings-file name stands for (single, stacked, side, windows); any other is the
/// overlay.
inline ScreenLayout ScreenLayoutFor(const std::string& name) {
    if (name == "single") {
        return ScreenLayout::Single;
    }
    if (name == "stacked") {
        return ScreenLayout::Stacked;
    }
    if (name == "side") {
        return ScreenLayout::Side;
    }
    if (name == "windows") {
        return ScreenLayout::SeparateWindows;
    }
    return ScreenLayout::Overlay;
}

/// The runtime's build: `name` is its version ("Azahar 2123.3"), `branch` and `description` say
/// which commit of it this is.
struct BuildInfo {
    std::string name;
    std::string branch;
    std::string description;
};

/// A game file's header: unreadable (not a 3DS game file), readable, or readable but encrypted.
/// `program_id` is set for the last two.
struct GameHeader {
    enum class State { Unreadable, Ok, Encrypted };
    State state = State::Unreadable;
    std::uint64_t program_id = 0;
};

/// A title installed on the SD card. `content_path` is its first content file's path (empty when
/// the title has none); it may not exist on disk.
struct InstalledTitle {
    bool found = false;
    std::uint16_t version = 0;
    std::string content_path;
};

/// What reading or installing a .cia came to.
enum class CiaResult { Ok, Encrypted, Unreadable, Aborted, Invalid };

struct CiaInfo {
    std::uint64_t title_id = 0;
    std::uint16_t version = 0;
};

/// What to run. Every path is a file of the player's.
struct LoadRequest {
    std::string game;          ///< the decrypted game (.3ds, .cci, .cxi)
    std::string update;        ///< a .cia to install first; empty: the update already installed is used
    std::string code_library;  ///< the code library to run; empty: the usual lookup (environment, user folder)
};

/// How loading the game ended.
enum class LoadResult { Ok, NotAGame, Encrypted, MissingSystemFiles, Failed };

/// What one step of the run ended with.
enum class RunState { Running, ShutdownRequested, Failed };

/// How the game is running, since the last time it was asked.
struct PerfStats {
    double game_fps = 0.0;         ///< the game's frames per second
    double system_fps = 0.0;       ///< the screens' refreshes per second
    double emulation_speed = 0.0;  ///< emulated time over real time
};

/// What the game's software keyboard applet calls (the app's keyboard, ui/keyboard.h, is one).
/// Both are called on the emulation thread. Execute only takes the request and returns: the game
/// polls for the text, and the app builds its frames on the same thread, so the keyboard shows
/// itself in a frame and hands the text over with IRuntime::FinishKeyboard when the player is
/// done.
class ISoftwareKeyboard {
public:
    virtual ~ISoftwareKeyboard() = default;

    /// The game asks for a text.
    virtual void Execute(const KeyboardConfig& config) = 0;

    /// The game says why it didn't take the last text (an empty message: no reason given).
    virtual void ShowError(const std::string& error) = 0;
};

/// One member of the room.
struct RoomMemberInfo {
    std::string nickname;
    std::string username;  ///< checked by a lobby, if any
    std::string game;      ///< what it plays
    MacAddress mac{};
};

/// What the room says about itself.
struct RoomDetails {
    std::string name;
    unsigned slots = 0;
};

/// Who joins which room.
struct RoomJoinRequest {
    std::string nickname;
    std::string console_id;  ///< the console's ID hash, which a room lets in once
    std::string host;
    int port = 0;
    std::optional<MacAddress> preferred_mac;  ///< none: the room gives one
    std::string password;
};

/// A room to open on this PC.
struct RoomOpenRequest {
    std::string name;
    int port = 0;
    std::string password;
    int slots = 4;
    std::string host_nickname;
    std::string game_name;
    std::uint64_t game_id = 0;
    std::string lobby_url;  ///< the lobby to list the room in; empty: not listed
    std::string token;      ///< the player's token there
};

struct RoomOpenResult {
    enum class Listing { None, Listed, Failed };
    bool ok = false;  ///< the room is open
    Listing listing = Listing::None;
    std::string listing_error;  ///< when Failed
};

/// What the other members and the network do, as the runtime tells the app. Every function
/// runs on the network's thread, and none may bind or unbind callbacks. An empty function is
/// not called.
struct RoomCallbacks {
    std::function<void(RoomError)> on_error;
    std::function<void(RoomState)> on_state;
    /// The room's details or its members changed.
    std::function<void(const RoomDetails&, const std::vector<RoomMemberInfo>&)> on_info;
    /// A member joined or left the room.
    std::function<void(bool joined, const std::string& nickname)> on_member;
    /// A local-play frame (or a net sync report, the port's own packet type) arrived from
    /// `transmitter`.
    enum class Packet { Beacon, Data, NetSync, Other };
    std::function<void(const MacAddress& transmitter, Packet)> on_packet;
};

/// A game controller the app found, as the input profile names it: the SDL GUID, its number
/// among controllers with that GUID, and its name.
struct ControllerInfo {
    std::string guid;
    int port = 0;
    std::string name;
};

class IRuntime {
public:
    virtual ~IRuntime() = default;

    // ── Lifecycle ──
    // The order is PrepareCode, a window for Load, Load, Start, then RunStep until it says
    // ShutdownRequested or Failed, then Stop. The window and the renderer are still Azahar's:
    // Load takes the app's windows (window.h).

    /// Chooses the CPU backend for the game, before it loads: installs `request.update` when it
    /// has one, makes `request.code_library` the one to run when it has one, and returns a line
    /// for the log saying what was chosen. Main thread.
    virtual std::string PrepareCode(const LoadRequest& request) = 0;

    /// After PrepareCode: a note for the player when the game runs without the port's code
    /// library, or with one that lacks some of the port's hooks, or when the update given to
    /// PrepareCode didn't install. Empty when all is well. Any thread.
    virtual std::string CodeNotice() = 0;

    /// Loads the game, to be drawn in `window` (and its touch screen in `secondary`, if any).
    /// Main thread. On a result other than Ok, StatusDetails() says more.
    virtual LoadResult Load(const LoadRequest& request, Window& window, Window* secondary) = 0;

    /// What the runtime says about the last load or run that went wrong. Any thread.
    virtual std::string StatusDetails() const = 0;

    /// After Load, once the windows' present threads run: the settings of the game's program and
    /// the shader cache. Main thread.
    virtual void Start() = 0;

    /// Runs the game for a slice and returns (the windows pump their events inside it). The
    /// emulation thread.
    virtual RunState RunStep() = 0;

    /// Shuts the emulated system down. The emulation thread, after the last RunStep.
    virtual void Stop() = 0;

    /// The game's speed since the last call (it starts a new measure). The emulation thread.
    virtual PerfStats TakePerfStats() = 0;

    /// Waits for the work the runtime left running in the background. Main thread, at the end.
    virtual void WaitForBackgroundTasks() = 0;

    // ── Input ──
    // The 3DS's buttons and sticks come from the input profile (BuildInputProfile): each one
    // reads the app's keys, a controller, or the full parameters of the player's choice. The
    // window sends keys through KeyDown/KeyUp (the mouse's buttons too while the mouse is the
    // camera), the mouse as touches through the window itself otherwise (until N2b), and the
    // software keyboard's text goes through the keyboard (below).

    /// Starts the input layer (the keyboard and controller engines) after the app's SDL, and
    /// stops it. Main thread.
    virtual void InitInput() = 0;
    virtual void ShutdownInput() = 0;

    /// Makes the input profile: every button and stick from `config` (full parameters first,
    /// else every key of its list ("F | Mouse Right": Input::KeyCodes), the item bar's macro key
    /// of the button, and `controller`'s input when there is one, any of them pressing it), the
    /// C-stick also from the mouse's push (SetMouseStick), the mouse as the stylus, no motion
    /// sensor. Main thread before the game loads, or the emulation thread; follow it
    /// with ReloadInputDevices while the game runs.
    virtual void BuildInputProfile(const Config& config, const ControllerInfo* controller) = 0;

    /// The input devices read the profile again (a controller came or went). Emulation thread.
    virtual void ReloadInputDevices() = 0;

    /// While muted (the in-game menu is up) every button reads released and every stick
    /// centred; once unmuted, a button still held from before reads released until it is let
    /// go, and a stick still pushed reads centred until it comes back. Any thread.
    virtual void SetInputMuted(bool muted) = 0;

    /// A key (an SDL scancode, or one of input.h's codes for the mouse's buttons, the wheel and
    /// the item bar's macro) went down or up for the console, and all of them let go. The
    /// window's thread (the emulation thread).
    virtual void KeyDown(int scancode) = 0;
    virtual void KeyUp(int scancode) = 0;
    virtual void ReleaseAllKeys() = 0;

    // The keyboard-and-mouse mode (input.h): the mouse is a key on the keyboard above (the codes
    // past SDL's scancodes of input.h: Input::MOUSE_KEY + a button, Input::WHEEL_UP, ...; the item
    // bar's macro presses Input::MACRO_KEY + a button), and it turns the camera in one of two ways.

    /// Where the mouse pushes the C-stick, each part -1..1 (x to the right, y up); (0, 0) lets
    /// go. Read by the C-stick's binding that BuildInputProfile made for it. The emulation
    /// thread.
    virtual void SetMouseStick(float x, float y) = 0;

    /// The game's camera turns by the mouse itself (the port's camera hook): `yaw` degrees to
    /// the right and `pitch` up are added to the turn the hook takes next, which is only done
    /// while CameraTurnActive says the hook runs (a camera the mouse can turn, a code library
    /// built with the hook); otherwise the app pushes the C-stick (SetMouseStick). The
    /// emulation thread. ClearCameraTurn drops a turn not taken yet.
    virtual bool CameraTurnActive() = 0;
    virtual void AddCameraTurn(float yaw, float pitch) = 0;
    virtual void ClearCameraTurn() = 0;

    // ── The software keyboard ──

    /// The game's keyboard requests go to `keyboard` instead of the runtime's default one. The
    /// runtime keeps it until the end. Main thread, after CommitOptions.
    virtual void SetSoftwareKeyboard(std::shared_ptr<ISoftwareKeyboard> keyboard) = 0;

    /// The player is done: hands the game the text and the number of the button pressed (the
    /// buttons are numbered from the left, KeyboardButtons). None when the game takes it, else
    /// why it doesn't and the keyboard stays up. Emulation thread, after Execute.
    virtual KeyboardError FinishKeyboard(const std::string& text, std::uint8_t button) = 0;

    // ── Rooms ──
    // The network room of Azahar's multiplayer (multiplayer.h): one room client, which joins a
    // room at an address, and one room server, which this PC can open. The calls below are the
    // main thread's or the room job's thread's unless said otherwise; the callbacks arrive on
    // the network's thread.

    /// Starts the network layer (rooms) and stops it. Main thread.
    virtual void InitNetwork() = 0;
    virtual void ShutdownNetwork() = 0;

    /// Whether the network layer is up (between InitNetwork and ShutdownNetwork). Any thread.
    virtual bool RoomsAvailable() const = 0;

    /// Makes the folder the console's MAC address is kept in, so that the address is the same
    /// every time it is read (otherwise each read before the game starts gives a new one).
    virtual void PrepareConsoleData() = 0;

    /// The console's identity: its user name, ID hash and MAC address, as its settings say.
    virtual std::string ConsoleUsername() const = 0;
    virtual std::string ConsoleIdHash() const = 0;
    virtual MacAddress ConsoleMac() const = 0;

    /// Whether the game is loaded and not shut down. Any thread.
    virtual bool IsGameRunning() const = 0;

    /// Gives the running console this MAC address (the page the game reads it from). False when
    /// the game isn't running or the console already has it. Emulation thread.
    virtual bool SetConsoleMac(const MacAddress& mac) = 0;

    /// The room client's events go to `callbacks` until UnbindRoomCallbacks. A no-op without
    /// the network layer.
    virtual void BindRoomCallbacks(RoomCallbacks callbacks) = 0;
    virtual void UnbindRoomCallbacks() = 0;

    /// Starts joining; the answer comes as the state (RoomMemberState, on_state) and, when the
    /// room refuses, on_error. Returns at once.
    virtual void JoinRoom(const RoomJoinRequest& request) = 0;

    /// Leaves the room. Blocks for a moment.
    virtual void LeaveRoom() = 0;

    virtual RoomState RoomMemberState() const = 0;
    virtual bool RoomConnected() const = 0;
    virtual MacAddress RoomMacAddress() const = 0;
    virtual std::string RoomNickname() const = 0;
    virtual RoomDetails RoomInformation() const = 0;
    virtual std::vector<RoomMemberInfo> RoomMembers() const = 0;

    /// Whether this PC runs a room that is open.
    virtual bool HostingRoom() const = 0;

    /// Opens a room on this PC (and lists it in the lobby when `request.lobby_url` is set and
    /// the build can); blocks while the lobby answers.
    virtual RoomOpenResult OpenRoom(const RoomOpenRequest& request) = 0;

    /// Stops the lobby listing and closes the room this PC runs. Blocks for a moment.
    virtual void CloseRoom() = 0;

    /// Gives the console a new random ID, kept in the user folder (a room lets in one player
    /// per console ID). Main thread, before the game loads.
    virtual void NewConsoleId() = 0;

    /// Puts what ApplyConfig set in force: the default applets, then the system's settings.
    /// Main thread, before the game loads.
    virtual void CommitOptions() = 0;

    /// Transitional: the Azahar system that main() hands to the windows (window.h), which take
    /// it. Ui::Start and Multiplayer::Init are still given it and don't use it. N2b takes it
    /// away. Null in a runtime that isn't Azahar's.
    virtual Core::System* HostSystem() = 0;

    // ── Paths ──

    /// The user folder, with the separator at its end: where the settings, the log, the options
    /// and the code library are. Any thread.
    virtual std::string UserDir() const = 0;

    /// Another user folder (--user), before anything reads or writes in the old one. Main thread.
    virtual void SetUserDir(const std::string& dir) = 0;

    /// The game's options file (mh4u.ini) and what a new one says. Any thread.
    virtual std::string GameOptionsPath() const = 0;
    virtual std::string GameOptionsTemplate() const = 0;

    // ── Build and log ──

    virtual BuildInfo Build() const = 0;

    /// Starts the log: `file_name` in the user folder, and the console. `filter` is the log
    /// filter ("*:Info Render.OpenGL:Debug"); empty: the runtime's own setting. Main thread,
    /// before other threads log.
    virtual void StartLogging(const std::string& file_name, const std::string& filter) = 0;

    /// Writes what is queued and stops the log.
    virtual void StopLogging() = 0;

    /// The runtime's settings, into the log.
    virtual void LogOptions() = 0;

    // ── Options ──

    /// The runtime's settings for the app's: renderer, screens, sound, input profile, New 3DS
    /// mode, CPU clock, and the frame-time log. Main thread, before the game loads.
    virtual void ApplyConfig(const Config& config) = 0;

    /// The renderer in use, and the one the next window is made for.
    virtual Renderer ActiveRenderer() const = 0;
    virtual void SetActiveRenderer(Renderer renderer) = 0;

    /// The screen layout the runtime has. SetLayout only changes the setting (before the game
    /// loads); ApplyLayout also puts the screens in place at once. Emulation thread.
    virtual ScreenLayout CurrentLayout() const = 0;
    virtual void SetLayout(ScreenLayout layout) = 0;
    virtual void ApplyLayout(ScreenLayout layout) = 0;

    /// Display and sound, while the game runs. Emulation thread, or the main thread while the
    /// game isn't running.
    virtual void SetResolution(int factor) = 0;       ///< times the 3DS's; 0: from the window
    virtual void SetLinearFilter(bool on) = 0;
    virtual void SetVsync(bool on) = 0;
    virtual void SetVolume(int percent) = 0;          ///< 0-100
    virtual void SetCpuClock(int percent) = 0;        ///< % of the 3DS's

    /// Changes one of the game's options (mh4u.ini keys) while the game runs; false for a key
    /// the port's hooks read only at start. Emulation thread.
    virtual bool SetGameOption(const std::string& key, const std::string& value) = 0;

    /// The HUD layout editor of the menu is up or gone. Emulation thread.
    virtual void SetLayoutEditing(bool on) = 0;

    // ── The 3DS's buttons ──

    /// The key of a button or a stick in the settings file ("button_a", "circle_pad").
    virtual const char* ButtonSettingName(int button) const = 0;
    virtual const char* StickSettingName(int stick) const = 0;

    /// The input the runtime reads a button or a stick from, as its own parameter string, empty
    /// when it has none.
    virtual std::string ButtonBinding(int button) const = 0;
    virtual std::string StickBinding(int stick) const = 0;

    /// What a controller does for such a parameter string ("A (bottom)", "Left stick"), or empty
    /// when no controller does.
    virtual std::string DescribeControllerInput(const std::string& binding) const = 0;

    // ── Titles: the game file, and titles installed on the virtual SD card ──
    // (the launcher's setup; any thread)

    /// What the header of a game file says (.3ds, .cci, .cxi).
    virtual GameHeader ReadGameHeader(const std::string& path) const = 0;

    /// The title installed on the SD card with this id, if there is one.
    virtual InstalledTitle FindInstalledTitle(std::uint64_t title_id) const = 0;

    /// Whether a .cia can be installed, and the title it holds when the file says it.
    virtual CiaResult CheckCia(const std::string& path, std::optional<CiaInfo>& info) const = 0;

    /// Installs a .cia on the SD card, blocking; `progress` is told the bytes written and the
    /// bytes in all (it may be empty, and runs on the calling thread).
    virtual CiaResult InstallCia(const std::string& path,
                                 const std::function<void(std::size_t, std::size_t)>& progress) = 0;
};

/// The runtime the app runs on. Set once by main() before anything else uses it.
inline IRuntime*& RuntimeSlot() {
    static IRuntime* runtime = nullptr;
    return runtime;
}
inline void SetRuntime(IRuntime* runtime) {
    RuntimeSlot() = runtime;
}
inline IRuntime& Runtime() {
    return *RuntimeSlot();
}

} // namespace App
