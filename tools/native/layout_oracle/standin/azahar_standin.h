// Derived in part from Azahar src/common/settings.h, src/common/assert.h and src/core/frontend/input.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the enumerations and the fields of Settings::values that the screen layouts read, the ASSERT and UNREACHABLE macros, and the touch device types of Input, as minimal stand-ins.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (settings.h)
// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project (assert.h)
// Copyright 2017 Citra Emulator Project (input.h)
//
// The stand-ins of the layout oracle (tools/native/layout_oracle/layout_oracle.py). Our own code,
// compiled in place of the Azahar headers that framebuffer_layout.cpp and emu_window.cpp include:
//   common/settings.h        -> only the enumerations and the Settings::values fields the layouts
//                               read, with Azahar's defaults; a field is a plain value holder
//   common/assert.h          -> ASSERT, UNREACHABLE and LOG_ERROR (which only count)
//   core/frontend/input.h    -> Input::TouchDevice, Input::Factory and the registry that
//                               EmuWindow::CreateTouchState registers its touch state in
// The harness writes a forwarding header with the Azahar name for each, so the copied sources
// compile unchanged. common_types.h, math_util.h and core/3ds.h are Azahar's own, copied as they
// are into the temporary folder.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "common/common_types.h"

namespace Common {
/// The parameter package of a device factory: nothing the touch state reads.
class ParamPackage {};
} // namespace Common

namespace Settings {

enum class LayoutOption : u32 {
    Default,
    SingleScreen,
    LargeScreen,
    SideScreen,
    SeparateWindows,
    HybridScreen,
    CustomLayout,
};

enum class PortraitLayoutOption : u32 {
    PortraitTopFullWidth,
    PortraitCustomLayout,
    PortraitOriginal
};

enum class SecondaryDisplayLayout : u32 {
    None,
    TopScreenOnly,
    BottomScreenOnly,
    SideBySide,
    OppositeScreenOnly,
    Original,
    Hybrid,
    LargeScreen
};

enum class SmallScreenPosition : u32 {
    TopRight,
    MiddleRight,
    BottomRight,
    TopLeft,
    MiddleLeft,
    BottomLeft,
    AboveLarge,
    BelowLarge
};

enum class StereoRenderOption : u32 {
    Off = 0,
    SideBySide = 1,
    SideBySideFull = 2,
    Anaglyph = 3,
    Interlaced = 4,
    ReverseInterlaced = 5,
    CardboardVR = 6
};

enum class AspectRatio : u32 {
    Default = 0,
    R16_9 = 1,
    R4_3 = 2,
    R21_9 = 3,
    R16_10 = 4,
    Stretch = 5,
};

/// A setting: a value with the accessors the layouts use.
template <typename T>
struct Setting {
    T value{};
    constexpr Setting() = default;
    constexpr Setting(T v) : value(v) {}
    T GetValue() const { return value; }
    void SetValue(T v) { value = v; }
    Setting& operator=(T v) {
        value = v;
        return *this;
    }
    operator T() const { return value; }
};

/// The fields of Azahar's Settings::Values that the layouts read, with its defaults.
struct Values {
    Setting<LayoutOption> layout_option{LayoutOption::Default};
    Setting<PortraitLayoutOption> portrait_layout_option{PortraitLayoutOption::PortraitTopFullWidth};
    Setting<SecondaryDisplayLayout> secondary_display_layout{SecondaryDisplayLayout::None};
    Setting<SmallScreenPosition> small_screen_position{SmallScreenPosition::TopRight};
    Setting<StereoRenderOption> render_3d{StereoRenderOption::Off};
    Setting<AspectRatio> aspect_ratio{AspectRatio::Default};
    Setting<bool> use_integer_scaling{false};
    Setting<bool> swap_screen{false};
    Setting<bool> upright_screen{false};
    Setting<float> large_screen_proportion{4.f};
    Setting<int> screen_gap{0};
    Setting<u16> custom_top_x{0};
    Setting<u16> custom_top_y{0};
    Setting<u16> custom_top_width{800};
    Setting<u16> custom_top_height{480};
    Setting<u16> custom_bottom_x{80};
    Setting<u16> custom_bottom_y{500};
    Setting<u16> custom_bottom_width{640};
    Setting<u16> custom_bottom_height{480};
    Setting<u16> custom_second_layer_opacity{100};
    Setting<u16> custom_portrait_top_x{0};
    Setting<u16> custom_portrait_top_y{0};
    Setting<u16> custom_portrait_top_width{800};
    Setting<u16> custom_portrait_top_height{480};
    Setting<u16> custom_portrait_bottom_x{0};
    Setting<u16> custom_portrait_bottom_y{480};
    Setting<u16> custom_portrait_bottom_width{640};
    Setting<u16> custom_portrait_bottom_height{480};
    Setting<bool> screen_top_stretch{false};
    Setting<u16> screen_top_leftright_padding{0};
    Setting<u16> screen_top_topbottom_padding{0};
    Setting<bool> screen_bottom_stretch{false};
    Setting<u16> screen_bottom_leftright_padding{0};
    Setting<u16> screen_bottom_topbottom_padding{0};
    Setting<u32> cardboard_screen_size{85};
    Setting<s32> cardboard_x_shift{0};
    Setting<s32> cardboard_y_shift{0};
};

inline Values values;

} // namespace Settings

namespace Input {

template <typename StatusType>
class InputDevice {
public:
    virtual ~InputDevice() = default;
    virtual StatusType GetStatus() const { return {}; }
};

template <typename InputDeviceType>
class Factory {
public:
    virtual ~Factory() = default;
    virtual std::unique_ptr<InputDeviceType> Create(const Common::ParamPackage&) = 0;
};

using TouchDevice = InputDevice<std::tuple<float, float, bool>>;

namespace Impl {
template <typename InputDeviceType>
inline std::unordered_map<std::string, std::shared_ptr<Factory<InputDeviceType>>> factories;
} // namespace Impl

template <typename InputDeviceType>
void RegisterFactory(const std::string& name, std::shared_ptr<Factory<InputDeviceType>> factory) {
    Impl::factories<InputDeviceType>[name] = std::move(factory);
}

/// The device a registered factory makes (the harness reads the touch point through it, as the
/// HID module does).
template <typename InputDeviceType>
std::unique_ptr<InputDeviceType> CreateDevice(const std::string& name) {
    const auto it = Impl::factories<InputDeviceType>.find(name);
    return it == Impl::factories<InputDeviceType>.end() ? nullptr
                                                        : it->second->Create(Common::ParamPackage{});
}

} // namespace Input

/// Failed ASSERTs and UNREACHABLEs end the harness: a case that hits one is a bad case.
#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
#define UNREACHABLE()                                                                              \
    do {                                                                                           \
        std::fprintf(stderr, "UNREACHABLE (%s:%d)\n", __FILE__, __LINE__);                         \
        std::abort();                                                                              \
    } while (0)
#define LOG_ERROR(cls, ...) std::fprintf(stderr, "LOG_ERROR in " #cls "\n")
