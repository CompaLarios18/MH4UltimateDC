#include "hooks.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/frontend/top_screen.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"

// Hooks read and write guest memory through Azahar's page table, like recompiled code.
#define RC_PAGED
extern "C" {
#include "recomp.h"
}

namespace Port::Hooks {

namespace {

// USA v1.3.0 addresses and offsets (docs/research/system-services.md, widescreen.md).
constexpr u32 SMAIN_INSTANCE = 0x01057194;  // sMhMain*: the engine's main system (an sMain)
constexpr u32 SMAIN_RATE = 0x2c;            // f32: the unit of delta, in frames per second (60)
constexpr u32 SMAIN_CAP = 0x30;             // f32: frame-rate cap of the frame limiter
constexpr u32 SMAIN_FPS = 0x34;             // f32: measured frame rate, refreshed each second
constexpr u32 SMAIN_DELTA = 0x5c;           // f32: last frame's length in 1/rate s
constexpr u32 VBLANK_TOP = 0x0110d004;      // VBlank counters kept by the GX library
constexpr u32 VBLANK_BOTTOM = 0x0110d008;
constexpr u32 FRAMES_SUBMITTED = 0x010571d4;
constexpr u32 SLTRENDER_INSTANCE = 0x010572cc;
constexpr u32 SLTRENDER_DISPLAY_SKIP = 0x8aa8;  // u32[2] mDisplaySkip: top, bottom
// Render settings word: bits set by sLtRender's property setters (0x00bf685c registers them):
// 0x1 mVSyncEdit, 0x4 mTripleBufferRenderWaitEdit, 0x8 mUpsideToTopEdit, 0x10 mYFlipEdit,
// 0x40 mULCDEnableEdit, 0x100 mParallelRenderingEdit, 0x400 mFSAA_2D,
// 0x100000 mDitherDisplay0, 0x200000 mDitherDisplay1, 0x400000 mProfileRFPS
constexpr u32 SLTRENDER_RENDER_FLAGS = 0x6ba8;
constexpr u32 SCAMERA_INSTANCE = 0x0105729c;    // sMhCamera*
constexpr u32 SCAMERA_VIEWPORTS = 0x18;         // 8 viewports of 0x1a8 bytes
constexpr u32 VIEWPORT_SIZE = 0x1a8;
constexpr u32 VIEWPORT_CAMERA = 0x04;           // uCamera*
constexpr u32 VIEWPORT_DISPLAY = 0x16;          // u8: 0 top screen, 1 bottom screen
constexpr u32 VIEWPORT_SCENE_TEXTURE = 0x0c;    // sub-scene texture: region from its size
constexpr u32 VIEWPORT_Y0 = 0x28, VIEWPORT_Y1 = 0x30;  // region rows (the screen's width)
constexpr u32 SLTRENDER_SCENE = 0x6c28;         // nDraw::Scene*: the main render target
constexpr u32 SLTRENDER_TOP_TRANSFER_Y = 0x6e1c;   // u32: first row of the top screen's image in
                                                   // the composited target (512 - 400 = 112)
constexpr u32 SCENE_TARGET = 0x50, TARGET_TEXTURES = 0x34, TEXTURE_SIZE = 0x18;
constexpr u32 TOP_ROWS = 400, LCD_COLUMNS = 240;   // top screen in framebuffer orientation
constexpr float NATIVE_ASPECT = 400.0f / 240.0f;

float ReadF32(RCtx* ctx, u32 addr) {
    const u32 v = rd32(ctx, addr);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

void WriteF32(RCtx* ctx, u32 addr, float f) {
    u32 v;
    std::memcpy(&v, &f, sizeof(v));
    wr32(ctx, addr, v);
}

/// Options from <Azahar user dir>/mh4u.ini, overridden by environment variables.
struct Options {
    float fps_cap = 0;      // fps / MH4U_FPS: frame-rate cap, 0 = the game's own
    float refresh_rate = 0; // refresh_rate / MH4U_REFRESH_RATE: emulated LCD Hz, 0 = 3DS's
    float widescreen = 0;   // widescreen / MH4U_WIDESCREEN: top-screen aspect ratio, 0 = native
    bool wide_render = false;  // wide_render / MH4U_WIDE_RENDER: render the wider image itself
    u32 trace_every = 0;    // trace_frames / MH4U_TRACE_FRAMES: log frame timing every n frames
    bool trace_panels = false;  // trace_panels / MH4U_TRACE_PANELS: log touch-panel activity
    u32 render_set = 0, render_clear = 0;  // render_flags_set/_clear: bits of the render word
};

bool IsOn(const std::string& value) {
    return value == "1" || value == "true" || value == "on" || value == "yes";
}
Options options;
u64 frames = 0;

/// "16:9", "1.7778", "off"/"0" -> aspect ratio (0 = native).
float ParseAspect(const std::string& v) {
    const auto colon = v.find(':');
    if (colon != std::string::npos) {
        const float w = std::strtof(v.c_str(), nullptr);
        const float h = std::strtof(v.c_str() + colon + 1, nullptr);
        return h > 0 ? w / h : 0.0f;
    }
    return std::max(0.0f, std::strtof(v.c_str(), nullptr));
}

void Set(Options& o, const std::string& key, const std::string& value) {
    if (key == "fps") {
        o.fps_cap = std::max(0.0f, std::strtof(value.c_str(), nullptr));
    } else if (key == "refresh_rate") {
        o.refresh_rate = std::max(0.0f, std::strtof(value.c_str(), nullptr));
    } else if (key == "widescreen") {
        o.widescreen = ParseAspect(value);
    } else if (key == "wide_render") {
        o.wide_render = IsOn(value);
    } else if (key == "render_flags_set") {
        o.render_set = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (key == "render_flags_clear") {
        o.render_clear = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (key == "trace_panels") {
        o.trace_panels = IsOn(value);
    } else if (key == "trace_frames") {
        o.trace_every = static_cast<u32>(std::max(0L, std::strtol(value.c_str(), nullptr, 10)));
    }
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

Options LoadOptions(std::vector<std::string>& log) {
    Options o;
    const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "mh4u.ini";
    std::ifstream file(path);
    if (file) {
        std::string line;
        while (std::getline(file, line)) {
            line = Trim(line.substr(0, line.find('#')));
            const auto eq = line.find('=');
            if (eq != std::string::npos) {
                Set(o, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
            }
        }
        log.push_back("options from " + path);
    }
    const std::pair<const char*, const char*> env[] = {
        {"MH4U_FPS", "fps"},
        {"MH4U_REFRESH_RATE", "refresh_rate"},
        {"MH4U_WIDESCREEN", "widescreen"},
        {"MH4U_WIDE_RENDER", "wide_render"},
        {"MH4U_TRACE_FRAMES", "trace_frames"},
        {"MH4U_TRACE_PANELS", "trace_panels"}};
    for (const auto& [var, key] : env) {
        if (const char* value = std::getenv(var)) {
            Set(o, key, value);
        }
    }
    return o;
}

/// sMain::updateFrameTiming(this): the frame limiter. Delta time is counted in 1/60 s
/// whatever the cap, and gameplay scales with it, so the cap is what limits the frame rate.
void UpdateFrameTiming(RCtx* ctx, RFunc original) {
    if (options.fps_cap > 0) {
        WriteF32(ctx, ctx->r[0] + SMAIN_CAP, options.fps_cap);
    }
    original(ctx);
}

/// sMhMain::frame(this): one iteration of the main loop.
void WideTransfer(RCtx* ctx);

void Frame(RCtx* ctx, RFunc original) {
    if (frames == 0 && options.refresh_rate > 0) {
        // the game presents at most once per VBlank: more VBlanks allow more than 60 fps
        Core::System::GetInstance().GPU().SetRefreshRate(options.refresh_rate);
    }
    if (options.render_set || options.render_clear) {
        // experimental: the game reads these bits every frame (the community's quality
        // cheats write the same word)
        if (const u32 r = rd32(ctx, SLTRENDER_INSTANCE)) {
            const u32 flags = rd32(ctx, r + SLTRENDER_RENDER_FLAGS);
            wr32(ctx, r + SLTRENDER_RENDER_FLAGS, (flags & ~options.render_clear) | options.render_set);
        }
    }
    WideTransfer(ctx);
    original(ctx);
    ++frames;
    if (options.trace_every == 0 || frames % options.trace_every != 0) {
        return;
    }
    const u32 main = rd32(ctx, SMAIN_INSTANCE);
    const u32 render = rd32(ctx, SLTRENDER_INSTANCE);
    if (main == 0 || render == 0) {
        return;
    }
    LOG_INFO(Frontend,
             "frame {}: delta {:.3f} fps {:.2f} rate {:.1f} cap {:.1f} vblanks {}/{} "
             "submitted {} display skip {}/{} render flags {:08X}",
             frames, ReadF32(ctx, main + SMAIN_DELTA), ReadF32(ctx, main + SMAIN_FPS),
             ReadF32(ctx, main + SMAIN_RATE), ReadF32(ctx, main + SMAIN_CAP), rd32(ctx, VBLANK_TOP),
             rd32(ctx, VBLANK_BOTTOM), rd32(ctx, FRAMES_SUBMITTED),
             rd32(ctx, render + SLTRENDER_DISPLAY_SKIP),
             rd32(ctx, render + SLTRENDER_DISPLAY_SKIP + 4),
             rd32(ctx, render + SLTRENDER_RENDER_FLAGS));
}

/// Whether `camera` is shown on the bottom screen (only top-screen views get widened).
bool OnBottomScreen(RCtx* ctx, u32 camera) {
    const u32 scamera = rd32(ctx, SCAMERA_INSTANCE);
    if (scamera == 0) {
        return false;
    }
    for (u32 i = 0; i < 8; ++i) {
        const u32 vp = scamera + SCAMERA_VIEWPORTS + i * VIEWPORT_SIZE;
        if (rd32(ctx, vp + VIEWPORT_CAMERA) == camera && rd8(ctx, vp + VIEWPORT_DISPLAY) == 1) {
            return true;
        }
    }
    return false;
}

/// The cameras' getProjMatrix(): returns the projection by value (result pointer in r0,
/// the camera in r1). Widescreen: divide m00 so the view gets wider by target/native
/// (Hor+); culling and stereo are derived from this matrix, so they follow. The frontend
/// shows the top screen stretched to the same aspect ratio (Azahar's aspect setting).
u32 WideRows();

void GetProjMatrix(RCtx* ctx, RFunc original) {
    const u32 out = ctx->r[0];
    const u32 camera = ctx->r[1];
    original(ctx);
    // with wide_render the game's own aspect is already W/240; what is left (a target wider
    // than the widest render target) is widened here and stretched by the frontend
    const float rendered = WideRows() ? WideRows() / static_cast<float>(LCD_COLUMNS) : NATIVE_ASPECT;
    if (options.widescreen > rendered + 0.001f && !OnBottomScreen(ctx, camera)) {
        WriteF32(ctx, out, ReadF32(ctx, out) * rendered / options.widescreen);
    }
}

// ── True widescreen (wide_render, experimental): the top screen is rendered W pixels wide
// instead of 400 (docs/research/widescreen.md, "What has to grow"). The aspect ratio,
// projection, culling, GPU viewport and display transfer all follow from the sizes below.

/// W: the widescreen width rounded to whole 8-pixel tiles, or 0 when off. At most 512: the
/// scene target is a 256x512 texture (a wider one would need twice the VRAM); a wider
/// aspect ratio gets the rest from the projection (anamorphic).
constexpr u32 MAX_WIDE_ROWS = 512;
u32 WideRows() {
    if (!options.wide_render || options.widescreen <= NATIVE_ASPECT) {
        return 0;
    }
    const u32 rows = static_cast<u32>(std::lround(LCD_COLUMNS * options.widescreen / 8.0f)) * 8;
    return std::min(rows, MAX_WIDE_ROWS);
}

/// nDraw::Scene::Scene(this, params): params {columns, rows, ...}; the main scene target.
void SceneCtor(RCtx* ctx, RFunc original) {
    const u32 rows = WideRows();
    const u32 params = ctx->r[1];
    if (rows && rd32(ctx, params) == LCD_COLUMNS && rd32(ctx, params + 4) == TOP_ROWS) {
        wr32(ctx, params + 4, rows);
        LOG_INFO(Frontend, "wide render: scene target {}x{}", rows, LCD_COLUMNS);
    }
    original(ctx);
}

/// nDraw::DisplayBuffer::init(this, columns, rows, format, ...): the top screen's buffers;
/// Azahar's LCD then has to show that many rows.
void DisplayBufferInit(RCtx* ctx, RFunc original) {
    const u32 rows = WideRows();
    if (rows && ctx->r[1] == LCD_COLUMNS && ctx->r[2] == TOP_ROWS) {
        ctx->r[2] = rows;
        Core::System::GetInstance().GPU().PicaCore().regs.framebuffer_config[0].height.Assign(rows);
    }
    original(ctx);
}

/// sCamera::Viewport::updateRegion(this): top-screen viewports drawn into the scene target
/// get their rows scaled to the wider target.
void UpdateRegion(RCtx* ctx, RFunc original) {
    const u32 viewport = ctx->r[0];
    original(ctx);
    const u32 rows = WideRows();
    if (!rows || rd8(ctx, viewport + VIEWPORT_DISPLAY) != 0 ||
        rd32(ctx, viewport + VIEWPORT_SCENE_TEXTURE) != 0) {
        return;
    }
    const u32 render = rd32(ctx, SLTRENDER_INSTANCE);
    if (render == 0 || rd32(ctx, render + SLTRENDER_SCENE) == 0) {
        return;
    }
    for (const u32 field : {VIEWPORT_Y0, VIEWPORT_Y1}) {
        const u32 y = rd32(ctx, viewport + field);
        if (y <= TOP_ROWS) {
            wr32(ctx, viewport + field, y * rows / TOP_ROWS);
        }
    }
}

/// The display transfer copies the top screen from row `512 - 400` of the composited target
/// (the GPU draws from the bottom up, so the image ends at the last row). With W rows, the
/// image starts at `height - W`; the sLtRender constructor writes 112, so set it every frame.
void WideTransfer(RCtx* ctx) {
    const u32 rows = WideRows();
    const u32 render = rows ? rd32(ctx, SLTRENDER_INSTANCE) : 0;
    const u32 scene = render ? rd32(ctx, render + SLTRENDER_SCENE) : 0;
    const u32 target = scene ? rd32(ctx, scene + SCENE_TARGET) : 0;
    const u32 textures = target ? rd32(ctx, target + TARGET_TEXTURES) : 0;
    const u32 texture = textures ? rd32(ctx, textures) : 0;
    if (texture == 0) {
        return;
    }
    const u32 height = (rd32(ctx, texture + TEXTURE_SIZE) >> 16) & 0x7FFF;
    if (height >= rows && rd32(ctx, render + SLTRENDER_TOP_TRANSFER_Y) != height - rows) {
        wr32(ctx, render + SLTRENDER_TOP_TRANSFER_Y, height - rows);
        LOG_INFO(Frontend, "wide render: top screen transferred from row {} of {}", height - rows, height);
    }
}

/// Sets every tunable with this name (config/tunables.txt); returns how many there were.
u32 SetTunables(const RCodeInfo& info, std::string_view name, u32 value) {
    u32 set = 0;
    for (u32 i = 0; i < info.tunable_count; ++i) {
        if (info.tunable_names[i] && name == info.tunable_names[i]) {
            info.tunables[i] = value;
            ++set;
        }
    }
    return set;
}

/// Shows the top screen at the widescreen aspect ratio: every screen layout gets a top
/// screen that wide, and the single-screen layout uses the matching aspect setting.
void SetScreenAspect(float aspect, std::vector<std::string>& log) {
    Layout::SetTopScreenWidth(static_cast<u32>(std::lround(LCD_COLUMNS * aspect)));
    using Settings::AspectRatio;
    const std::pair<float, AspectRatio> known[] = {{16.0f / 9.0f, AspectRatio::R16_9},
                                                   {16.0f / 10.0f, AspectRatio::R16_10},
                                                   {21.0f / 9.0f, AspectRatio::R21_9},
                                                   {4.0f / 3.0f, AspectRatio::R4_3}};
    AspectRatio choice = AspectRatio::Stretch;
    for (const auto& [value, ratio] : known) {
        if (std::fabs(aspect - value) < 0.01f) {
            choice = ratio;
        }
    }
    Settings::values.aspect_ratio.SetValue(choice);
    log.push_back(choice == AspectRatio::Stretch
                      ? "screen layouts show the top screen wider; single screen stretches it to the window"
                      : "screen layouts show the top screen at that aspect ratio");
}

// ── Touch panels (docs/research/second-screen.md): tracing, to map panel ids to actions
// before binding them to buttons.
constexpr u32 PANEL_MANAGER = 0x0109a350;
constexpr u32 PANEL_FLAGS = 0xcc;     // u16: event flags of the last touch (0x0200 tapped)
constexpr u32 PANEL_ID = 0xce;        // u16: id of the panel touched
constexpr u32 SMHPAD_INJECTED = 0x3f0;   // u32: gameplay action bits set by panels
constexpr u32 SMHPAD_INJECTED_MENU = 0x3f4;
u32 last_panel_touch = 0xFFFFFFFF;

/// PanelManager::processTouch(): runs the touch groups, leaves the touched panel's id.
void PanelProcessTouch(RCtx* ctx, RFunc original) {
    original(ctx);
    if (!options.trace_panels) {
        return;
    }
    const u32 touch = rd16(ctx, PANEL_MANAGER + PANEL_ID) | rd16(ctx, PANEL_MANAGER + PANEL_FLAGS) << 16;
    if (touch != last_panel_touch && !(touch >> 16 & 0x8000)) {
        LOG_INFO(Frontend, "panels: touch on panel {} flags {:04X}", touch & 0xFFFF, touch >> 16);
    }
    last_panel_touch = touch;
}

/// PanelManager::isTapped(id): polled by the code that reacts to each panel.
void PanelIsTapped(RCtx* ctx, RFunc original) {
    const u32 id = ctx->r[0];
    const u32 caller = ctx->r[14];
    original(ctx);
    if (options.trace_panels && ctx->r[0] != 0) {
        LOG_INFO(Frontend, "panels: panel {} tapped, handled at {:08X}", id, caller);
    }
}

/// sMhPad::update(this): turns pad state and the panels' injected bits into actions.
void MhPadUpdate(RCtx* ctx, RFunc original) {
    if (options.trace_panels) {
        const u32 pad = ctx->r[0];
        const u32 gameplay = rd32(ctx, pad + SMHPAD_INJECTED);
        const u32 menu = rd32(ctx, pad + SMHPAD_INJECTED_MENU);
        if (gameplay || menu) {
            LOG_INFO(Frontend, "panels: injected actions {:04X}, menu actions {:04X}", gameplay, menu);
        }
    }
    original(ctx);
}

struct Hook {
    u32 addr;
    const char* name;
    RHookFn fn;
};
constexpr Hook HOOKS[] = {
    {0x006442f8, "sMain::updateFrameTiming", UpdateFrameTiming},
    {0x00af5250, "sMhMain::frame", Frame},
    {0x00c46380, "uMhCamera::getProjMatrix", GetProjMatrix},
    {0x002535f8, "uFreeCamera::getProjMatrix", GetProjMatrix},
    {0x0032e338, "uMotionCamera::getProjMatrix", GetProjMatrix},
    {0x00633188, "nDraw::Scene::Scene", SceneCtor},
    {0x0062e184, "nDraw::DisplayBuffer::init", DisplayBufferInit},
    {0x00aefd44, "sCamera::Viewport::updateRegion", UpdateRegion},
    {0x002951fc, "PanelManager::processTouch", PanelProcessTouch},
    {0x00288f4c, "PanelManager::isTapped", PanelIsTapped},
    {0x00694c98, "sMhPad::update", MhPadUpdate},
};

} // namespace

std::vector<std::string> Install(const RCodeInfo& info) {
    std::vector<std::string> log;
    if (!info.set_hook || info.hook_count == 0) {
        return log; // a test program, or a library from before hook points
    }
    options = LoadOptions(log);
    if (options.fps_cap > 0) {
        log.push_back(fmt::format("frame-rate cap {}", options.fps_cap));
    }
    if (options.refresh_rate > 0) {
        log.push_back(fmt::format("emulated screen refresh rate {} Hz (experimental)", options.refresh_rate));
    }
    if (options.widescreen > 0) {
        log.push_back(WideRows() ? fmt::format("widescreen: top screen rendered {}x{} (experimental)",
                                               WideRows(), LCD_COLUMNS)
                                 : fmt::format("widescreen: 3D view widened to aspect ratio {:.4f}",
                                               options.widescreen));
        SetScreenAspect(options.widescreen, log);
        if (WideRows()) {
            // the code's own 400s for the top screen (config/tunables.txt)
            const u32 set = SetTunables(info, "top_rows", WideRows());
            log.push_back(fmt::format("widescreen: {} of the code's top-screen sizes set to {}", set,
                                      WideRows()));
        }
    }
    if (options.trace_every) {
        log.push_back(fmt::format("tracing frame timing every {} frames", options.trace_every));
    }
    if (options.trace_panels) {
        log.push_back("tracing touch panels");
    }
    if (options.render_set || options.render_clear) {
        log.push_back(fmt::format("render flags: set {:08X}, clear {:08X} (experimental)",
                                  options.render_set, options.render_clear));
    }
    for (const Hook& hook : HOOKS) {
        if (!info.set_hook(hook.addr, hook.fn)) {
            log.push_back(fmt::format("no hook point for {} at {:08X} (rebuild the code library)",
                                      hook.name, hook.addr));
        }
    }
    return log;
}

} // namespace Port::Hooks
