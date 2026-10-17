// What the port's hooks (hooks.cpp) need from the program that runs the game: a small interface
// with no Azahar type in it, so that the hooks build for any host. The Azahar build implements
// it in platform_azahar.cpp; the native runtime brings an implementation of its own.
//
// Threading: the hooks call these on the thread that runs the guest, as they called Azahar
// before. An implementation must not assume another thread unless a method says so.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Port {

/// Severity of a line sent to Platform::Log.
enum class LogLevel { Debug, Info, Warning, Error };

/// The size of the 3DS's bottom (touch) screen in pixels.
constexpr int kBottomScreenWidth = 320;
constexpr int kBottomScreenHeight = 240;

/// The custom layout as the port sets it up instead of fixed positions: the main screen fills
/// the window at its aspect ratio and the other one is drawn over it in a corner, `height` times
/// the window's height (0: off, the host's own layout applies). When the bottom screen is the one
/// in the corner, it can show just a part of it: the crop rectangle, in fractions of the bottom
/// screen's width and height (touches map to that part).
struct ScreenOverlay {
    enum class Corner : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };
    float height = 0.0f;
    float opacity = 1.0f;
    Corner corner = Corner::BottomRight;
    float crop_left = 0.0f, crop_top = 0.0f, crop_right = 1.0f, crop_bottom = 1.0f;
    /// A place in the window instead of the corner, when both are 0..1 (negative: the corner):
    /// where the overlay's left and top edges go in the room the window leaves beside and above
    /// it, 0 at the window's left (top) edge, 1 with its right (bottom) edge at the window's. So
    /// it stays in the window at any size; (1, 0) is the top-right corner.
    float x = -1.0f, y = -1.0f;

    /// Further parts of the bottom screen drawn on their own over the main screen, at the same
    /// opacity (the port's quest HUD: the target camera panel beside the map). Only while the
    /// bottom screen is the one in the corner, in landscape orientation. A touch on one maps to
    /// its part of the bottom screen.
    struct View {
        float height = 0.0f;       ///< times the window's height; 0: off
        float x = 1.0f, y = 0.0f;  ///< as ScreenOverlay::x, y (0..1)
        float crop_left = 0.0f, crop_top = 0.0f, crop_right = 1.0f, crop_bottom = 1.0f;
    };
    static constexpr std::size_t MAX_VIEWS = 2;
    std::array<View, MAX_VIEWS> views{};
};

/// How the port consoles of a network room keep their clocks together (the net_sync option).
enum class NetSyncMode {
    Off,
    Session,  ///< with the port consoles this one plays local wireless with: the default
    All,      ///< with every port console in the room (for tests without local wireless)
};

/// "on"/"session", "all" or "off" (anything else: the default).
inline NetSyncMode ParseNetSyncMode(const std::string& value) {
    if (value == "0" || value == "off" || value == "false" || value == "no") {
        return NetSyncMode::Off;
    }
    if (value == "all") {
        return NetSyncMode::All;
    }
    return NetSyncMode::Session;
}

class Platform {
public:
    virtual ~Platform() = default;

    /// One line for the host's log.
    virtual void Log(LogLevel level, const std::string& message) = 0;

    /// The folder the port's options file ("mh4u.ini") lives in, with its trailing separator.
    virtual std::string UserDir() = 0;

    /// The emulated screen's refresh rate in Hz; 0: the 3DS's own (59.83 Hz).
    virtual void SetRefreshRate(double hz) = 0;

    /// Rows of the top screen's framebuffer as the game sets it up (400 on a 3DS; more with the
    /// port's true widescreen).
    virtual void SetTopScreenRows(std::uint32_t rows) = 0;

    /// Seconds of game time since the game started: the clock the hooks' timers use. It stops
    /// when the game is paused and runs at the game's speed, whatever the host's.
    virtual double GameSeconds() = 0;

    /// Whether the game is running (its screens exist, the layout can be worked out).
    virtual bool IsRunning() = 0;

    /// Works the screen layout out again (after the overlay changed).
    virtual void RefreshScreenLayout() = 0;

    /// The overlay layout (see ScreenOverlay). The host keeps its own copy and may clamp it.
    virtual void SetScreenOverlay(const ScreenOverlay& overlay) = 0;

    /// The width the layouts give the top screen at its 240-pixel height: 400, as on a 3DS,
    /// unless the port shows it wider (427 for 16:9, say). 0: the 3DS's.
    virtual void SetTopScreenWidth(std::uint32_t width) = 0;

    /// The host's own settings that the hooks read or set, by name. "aspect_ratio": "stretch",
    /// "4:3", "16:9", "16:10" or "21:9", how the single-screen layout fits the top screen to the
    /// window. SetOption returns false for a name or value the host doesn't know; GetOption
    /// returns "" for a name it doesn't know.
    virtual bool SetOption(const std::string& key, const std::string& value) = 0;
    virtual std::string GetOption(const std::string& key) = 0;

    /// Starts (or, for Off, stops) keeping the emulated clocks of the port's consoles in a room
    /// together. `slack_ms`: how far ahead of a peer this console may get before it waits for it,
    /// in emulated ms. A host with no room for it does nothing. Lines for the log go to `log`.
    virtual void InstallNetSync(NetSyncMode mode, double slack_ms, std::vector<std::string>& log) = 0;

    /// Whether this console's player asks to end the wait after a quest (the skip, agreed by
    /// everyone), and whether every port console this one plays with, and this one, asks too
    /// (true with no room, no peers, or peers that can't vote).
    virtual void VoteEndSkip(bool vote) = 0;
    virtual bool EndSkipAgreed() = 0;
};

/// The Azahar build's implementation (platform_azahar.cpp), which uses Azahar's single
/// Core::System. Only programs linked with Azahar can call this.
Platform& AzaharPlatform();

}  // namespace Port
