// The hooks' platform (platform.h) on Azahar: every call into Azahar that hooks.cpp used to make
// itself. Azahar has one Core::System, which each method fetches as the hooks did before.
#include "platform.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#if defined(__ANDROID__)
#include "common/android_utils.h"
#endif
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/top_screen.h"
#include "netsync.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_base.h"

namespace Port {

namespace {

static_assert(kBottomScreenWidth == Core::kScreenBottomWidth);
static_assert(kBottomScreenHeight == Core::kScreenBottomHeight);

using PlainCorner = ScreenOverlay::Corner;
using LayoutCorner = Layout::ScreenOverlay::Corner;
static_assert(static_cast<int>(PlainCorner::TopLeft) == static_cast<int>(LayoutCorner::TopLeft));
static_assert(static_cast<int>(PlainCorner::TopRight) == static_cast<int>(LayoutCorner::TopRight));
static_assert(static_cast<int>(PlainCorner::BottomLeft) == static_cast<int>(LayoutCorner::BottomLeft));
static_assert(static_cast<int>(PlainCorner::BottomRight) == static_cast<int>(LayoutCorner::BottomRight));
static_assert(ScreenOverlay::MAX_VIEWS == Layout::ScreenOverlay::MAX_VIEWS);

/// The plain overlay as Azahar's layout code (our patch) takes it.
Layout::ScreenOverlay ToLayout(const ScreenOverlay& in) {
    Layout::ScreenOverlay out;
    out.height = in.height;
    out.opacity = in.opacity;
    out.corner = static_cast<LayoutCorner>(in.corner);
    out.crop_left = in.crop_left;
    out.crop_top = in.crop_top;
    out.crop_right = in.crop_right;
    out.crop_bottom = in.crop_bottom;
    out.x = in.x;
    out.y = in.y;
    for (std::size_t i = 0; i < ScreenOverlay::MAX_VIEWS; ++i) {
        const auto& view = in.views[i];
        out.views[i] = {view.height,    view.x,        view.y,          view.crop_left,
                        view.crop_top, view.crop_right, view.crop_bottom};
    }
    return out;
}

NetSync::Mode ToNetSync(NetSyncMode mode) {
    switch (mode) {
    case NetSyncMode::Off:
        return NetSync::Mode::Off;
    case NetSyncMode::All:
        return NetSync::Mode::All;
    case NetSyncMode::Session:
        break;
    }
    return NetSync::Mode::Session;
}

/// Names of Settings::AspectRatio for Platform::SetOption("aspect_ratio", ...).
constexpr std::pair<const char*, Settings::AspectRatio> ASPECT_RATIOS[] = {
    {"default", Settings::AspectRatio::Default}, {"16:9", Settings::AspectRatio::R16_9},
    {"4:3", Settings::AspectRatio::R4_3},        {"21:9", Settings::AspectRatio::R21_9},
    {"16:10", Settings::AspectRatio::R16_10},    {"stretch", Settings::AspectRatio::Stretch}};

class AzaharPlatformImpl final : public Platform {
public:
    void Log(LogLevel level, const std::string& message) override {
        switch (level) {
        case LogLevel::Debug:
            LOG_DEBUG(Frontend, "{}", message);
            break;
        case LogLevel::Info:
            LOG_INFO(Frontend, "{}", message);
            break;
        case LogLevel::Warning:
            LOG_WARNING(Frontend, "{}", message);
            break;
        case LogLevel::Error:
            LOG_ERROR(Frontend, "{}", message);
            break;
        }
    }

    std::string UserDir() override {
        return FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
    }

    bool ReadTextLines(const std::string& path, std::vector<std::string>& lines) override {
        // FileUtil::IOFile, not std::ifstream: on Android the user folder is not the host's
        // root, and only FileUtil translates the guest path to the file under it.
        FileUtil::IOFile file(path, "r");
        if (!file.IsOpen()) {
            return false;
        }
        std::string line;
        while (file.ReadLine(line)) {
            lines.push_back(line);
        }
        return true;
    }

    void SetRefreshRate(double hz) override {
        Core::System::GetInstance().GPU().SetRefreshRate(hz);
    }

    void SetTopScreenRows(std::uint32_t rows) override {
        Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config[0].height.Assign(rows);
    }

    double GameSeconds() override {
        return static_cast<double>(Core::System::GetInstance().CoreTiming().GetGlobalTicks()) /
               static_cast<double>(BASE_CLOCK_RATE_ARM11);
    }

    bool IsRunning() override {
        return Core::System::GetInstance().IsPoweredOn();
    }

    bool IsPortrait() override {
#if defined(__ANDROID__)
        return AndroidUtils::IsPortraitMode();
#else
        return false;
#endif
    }

    void RefreshScreenLayout() override {
        Core::System::GetInstance().GPU().Renderer().UpdateCurrentFramebufferLayout();
    }

    void RefreshScreenLayoutFor(bool portrait) override {
#if defined(__ANDROID__)
        Core::System::GetInstance().GPU().Renderer().UpdateCurrentFramebufferLayout(portrait);
#else
        static_cast<void>(portrait);
        RefreshScreenLayout();
#endif
    }

    void SetScreenOverlay(const ScreenOverlay& overlay) override {
        Layout::SetScreenOverlay(ToLayout(overlay));
    }

    void SetTopScreenWidth(std::uint32_t width) override {
        Layout::SetTopScreenWidth(width);
    }

    bool SetOption(const std::string& key, const std::string& value) override {
        if (key != "aspect_ratio") {
            return false;
        }
        for (const auto& [name, ratio] : ASPECT_RATIOS) {
            if (value == name) {
                Settings::values.aspect_ratio.SetValue(ratio);
                return true;
            }
        }
        return false;
    }

    std::string GetOption(const std::string& key) override {
        if (key != "aspect_ratio") {
            return {};
        }
        const auto current = Settings::values.aspect_ratio.GetValue();
        for (const auto& [name, ratio] : ASPECT_RATIOS) {
            if (ratio == current) {
                return name;
            }
        }
        return {};
    }

    void InstallNetSync(NetSyncMode mode, double slack_ms, std::vector<std::string>& log) override {
        NetSync::Install(Core::System::GetInstance(), ToNetSync(mode), slack_ms, log);
    }

    void VoteEndSkip(bool vote) override {
        NetSync::SetEndSkipVote(vote);
    }

    bool EndSkipAgreed() override {
        return NetSync::EndSkipAgreed();
    }
};

} // namespace

Platform& AzaharPlatform() {
    static AzaharPlatformImpl platform;
    return platform;
}

} // namespace Port
