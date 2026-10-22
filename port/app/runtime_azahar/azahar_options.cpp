// AzaharRuntime: the options (Azahar's Settings::values), set from the app's settings and while
// the game runs.
#include <algorithm>
#include <ctime>
#include <fmt/format.h>

#include "common/settings.h"
#include "config.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/service.h"
#include "hooks.h"
#include "input.h"
#include "perf.h"
#include "runtime_azahar/azahar_runtime.h"
#include "util/log.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_base.h"

namespace App::Azahar {

namespace {

bool IsOn(const std::string& v) {
    return v == "1" || v == "on" || v == "yes" || v == "true";
}

Settings::GraphicsAPI GraphicsApiFor(Renderer renderer) {
    switch (renderer) {
    case Renderer::OpenGL:
        return Settings::GraphicsAPI::OpenGL;
    case Renderer::Software:
        return Settings::GraphicsAPI::Software;
    default:
        return Settings::GraphicsAPI::Vulkan;
    }
}

Settings::LayoutOption LayoutOptionFor(ScreenLayout layout) {
    switch (layout) {
    case ScreenLayout::Single:
        return Settings::LayoutOption::SingleScreen;
    case ScreenLayout::Stacked:
        return Settings::LayoutOption::Default;
    case ScreenLayout::Side:
        return Settings::LayoutOption::SideScreen;
    case ScreenLayout::SeparateWindows:
        return Settings::LayoutOption::SeparateWindows;
    default:
        // the custom layout is the port's overlay when mh4u.ini's touch_overlay is on
        return Settings::LayoutOption::CustomLayout;
    }
}

/// Starts the frame-time log (src/perf.h) once, with the GPU timers on Azahar's hooks.
void StartPerfLog(const Config& c, bool new_3ds) {
    const std::string& perf_log = PerfLogPath();
    if (perf_log.empty() || Port::Perf::Enabled()) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    char date[16] = "0000-00-00";
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(date, sizeof(date), "%Y-%m-%d", utc);
    }
    const std::string header = fmt::format("perf v1 {} game=0x0004000000126300 cpu=recomp new3ds={} renderer={} "
                                           "resolution={} vsync={} screens={}",
                                           date, new_3ds ? "on" : "off", c.renderer, c.resolution,
                                           c.vsync ? "on" : "off", c.screens);
    std::string error;
    if (!Port::Perf::Start(perf_log, header, error)) {
        APP_LOG_ERROR("perf_log: {}", error);
        return;
    }
    Port::Perf::ChainGpuHooks(Pica::g_capture_hooks, [](Service::GSP::InterruptId id) {
        return id == Service::GSP::InterruptId::PDC0;
    });
    APP_LOG_INFO("writing the frame times to {}", perf_log);
}

} // namespace

void AzaharRuntime::ApplyConfig(const Config& c) {
    auto& v = Settings::values;
    v.graphics_api = GraphicsApiFor(RendererFor(c.renderer));
    v.resolution_factor = static_cast<u32>(c.resolution);
    v.use_vsync = c.vsync;
    v.filter_mode = c.linear_filter;
    v.custom_textures = c.custom_textures;
    const bool new_3ds = c.new_3ds == "auto" ? !c.custom_textures : IsOn(c.new_3ds);
    v.is_new_3ds = new_3ds;
    v.cpu_clock_percentage = static_cast<u32>(std::clamp(c.cpu_clock, 5, 400));
    v.async_custom_loading = true;
    if (!c.shader.empty()) {
        v.pp_shader_name = c.shader;
    }
    v.swap_screen = false;
    v.upright_screen = false;
    v.layout_option = LayoutOptionFor(ScreenLayoutFor(c.screens));
    v.volume = static_cast<float>(c.volume) / 100.0f;
    if (c.audio_output == "cubeb") {
        v.output_type = AudioCore::SinkType::Cubeb;
    } else if (c.audio_output == "sdl2") {
        v.output_type = AudioCore::SinkType::SDL2;
    } else if (c.audio_output == "openal") {
        v.output_type = AudioCore::SinkType::OpenAL;
    } else if (c.audio_output == "null") {
        v.output_type = AudioCore::SinkType::Null;
    } else {
        v.output_type = AudioCore::SinkType::Auto;
    }
    v.frame_limit = 100;
    // every system module high-level emulated, as Azahar's other frontends set it (Service::Init
    // looks each one up and throws on one that isn't there)
    for (const auto& module : Service::service_module_map) {
        v.lle_modules.emplace(module.name, false);
    }
    Input::BuildProfile(c);
    StartPerfLog(c, new_3ds);
}

Renderer AzaharRuntime::ActiveRenderer() const {
    switch (Settings::values.graphics_api.GetValue()) {
    case Settings::GraphicsAPI::OpenGL:
        return Renderer::OpenGL;
    case Settings::GraphicsAPI::Software:
        return Renderer::Software;
    default:
        return Renderer::Vulkan;
    }
}

void AzaharRuntime::SetActiveRenderer(Renderer renderer) {
    Settings::values.graphics_api = GraphicsApiFor(renderer);
}

ScreenLayout AzaharRuntime::CurrentLayout() const {
    switch (Settings::values.layout_option.GetValue()) {
    case Settings::LayoutOption::CustomLayout:
        return ScreenLayout::Overlay;
    case Settings::LayoutOption::Default:
        return ScreenLayout::Stacked;
    case Settings::LayoutOption::SingleScreen:
        return ScreenLayout::Single;
    case Settings::LayoutOption::SideScreen:
        return ScreenLayout::Side;
    case Settings::LayoutOption::SeparateWindows:
        return ScreenLayout::SeparateWindows;
    default:
        return ScreenLayout::Other;
    }
}

void AzaharRuntime::SetLayout(ScreenLayout layout) {
    Settings::values.layout_option = LayoutOptionFor(layout);
}

void AzaharRuntime::ApplyLayout(ScreenLayout layout) {
    Settings::values.layout_option = LayoutOptionFor(layout);
    Settings::values.swap_screen = false;
    Settings::values.upright_screen = false;
    Core::System::GetInstance().GPU().Renderer().UpdateCurrentFramebufferLayout();
}

void AzaharRuntime::SetResolution(int factor) {
    Settings::values.resolution_factor = static_cast<u32>(factor);
}

void AzaharRuntime::SetLinearFilter(bool on) {
    Settings::values.filter_mode = on;
}

void AzaharRuntime::SetVsync(bool on) {
    Settings::values.use_vsync = on;
}

void AzaharRuntime::SetVolume(int percent) {
    Settings::values.volume = static_cast<float>(percent) / 100.0f;
}

void AzaharRuntime::SetCpuClock(int percent) {
    Settings::values.cpu_clock_percentage = static_cast<u32>(percent);
    Core::System::GetInstance().CoreTiming().UpdateClockSpeed(static_cast<u32>(percent));
}

bool AzaharRuntime::SetGameOption(const std::string& key, const std::string& value) {
    return Port::Hooks::SetOption(key, value);
}

void AzaharRuntime::SetLayoutEditing(bool on) {
    Port::Hooks::SetLayoutEditing(on);
}

SkipInfo AzaharRuntime::SkipState() const {
    // the menu's frames are built on the hooks' thread in this build (ui.h), so the getters,
    // which read mh4u.ini's options without atomics, are read where the hooks write them
    SkipInfo info;
    info.offered = Port::Hooks::SkipOffered();
    info.hint = Port::Hooks::SkipHint();
    if (!info.offered || info.hint == 0) {
        return info;
    }
    info.skip_button = Port::Hooks::SkipButton();
    info.end_buttons = Port::Hooks::SkipEndButtons();
    info.end_hold = Port::Hooks::EndSkipHold();
    info.end_requested = Port::Hooks::EndSkipRequested();
    return info;
}

} // namespace App::Azahar
