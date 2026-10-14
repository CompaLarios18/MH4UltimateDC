#include "hooks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>
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
constexpr u32 RENDER_DITHER = 0x300000;         // mDitherDisplay0 | mDitherDisplay1
constexpr u32 SCAMERA_INSTANCE = 0x0105729c;    // sMhCamera*
constexpr u32 SCAMERA_VIEWPORTS = 0x18;         // 8 viewports of 0x1a8 bytes
constexpr u32 VIEWPORT_SIZE = 0x1a8;
constexpr u32 VIEWPORT_CAMERA = 0x04;           // uCamera*
constexpr u32 VIEWPORT_DISPLAY = 0x16;          // u8: 0 top screen, 1 bottom screen
constexpr u32 VIEWPORT_SCENE_TEXTURE = 0x0c;    // sub-scene texture: region from its size
constexpr u32 VIEWPORT_Y0 = 0x28, VIEWPORT_Y1 = 0x30;  // region rows (the screen's width)
constexpr u32 VIEWPORT_MODE = 0x15;             // u8 mMode: 0 the whole 400 rows, 2..9 halves or
                                                // quarters of them, 1 and >= 10 a region its
                                                // owner sets (0xa: the virtual-rect mode)
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
    bool dither = true;     // dither / MH4U_DITHER: the game's dither pattern on both screens
    u32 kick_button = 0;    // kick / MH4U_KICK: 3DS button that kicks (the game's pad bits)
    u32 target_button = 0;  // target / MH4U_TARGET: 3DS button for the target camera panel
    Layout::ScreenOverlay touch_overlay;  // touch_overlay / MH4U_TOUCH_OVERLAY (custom layout)
    bool hud_edges = true;  // hud_edges / MH4U_HUD_EDGES: with wide_render, HUD to the edges
    u32 trace_layout = 0;   // trace_layout / MH4U_TRACE_LAYOUT: log HUD objects every n frames
};

/// A 3DS button name -> the bit sPad::processInput gives it in the held word.
u32 PadBit(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
    const std::pair<const char*, u32> bits[] = {
        {"A", 0x2000}, {"B", 0x4000},  {"X", 0x1000},  {"Y", 0x8000},  {"L", 0x100},
        {"R", 0x200},  {"ZL", 0x400},  {"ZR", 0x800},  {"START", 0x8}, {"UP", 0x10},
        {"DOWN", 0x40}, {"LEFT", 0x80}, {"RIGHT", 0x20}};
    for (const auto& [n, bit] : bits) {
        if (name == n) {
            return bit;
        }
    }
    return 0;
}

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
    } else if (key == "dither") {
        o.dither = IsOn(value);
    } else if (key == "render_flags_set") {
        o.render_set = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (key == "render_flags_clear") {
        o.render_clear = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 0));
    } else if (key == "trace_panels") {
        o.trace_panels = IsOn(value);
    } else if (key == "kick") {
        o.kick_button = PadBit(value);
    } else if (key == "target") {
        o.target_button = PadBit(value);
    } else if (key == "touch_overlay") {
        o.touch_overlay = ParseOverlay(value);
    } else if (key == "hud_edges") {
        o.hud_edges = IsOn(value);
    } else if (key == "trace_layout") {
        o.trace_layout = static_cast<u32>(std::max(0L, std::strtol(value.c_str(), nullptr, 10)));
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
        {"MH4U_TRACE_PANELS", "trace_panels"},
        {"MH4U_KICK", "kick"},
        {"MH4U_TARGET", "target"},
        {"MH4U_TOUCH_OVERLAY", "touch_overlay"},
        {"MH4U_HUD_EDGES", "hud_edges"},
        {"MH4U_DITHER", "dither"},
        {"MH4U_RENDER_FLAGS_SET", "render_flags_set"},
        {"MH4U_RENDER_FLAGS_CLEAR", "render_flags_clear"},
        {"MH4U_TRACE_LAYOUT", "trace_layout"}};
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
    const u32 clear = options.render_clear | (options.dither ? 0 : RENDER_DITHER);
    if (options.render_set || clear) {
        // the game reads these bits every frame (the community's quality cheats write the same
        // word); without the dither bits it draws no dither pattern, which an upscaled picture
        // shows as a screen-door grid
        if (const u32 r = rd32(ctx, SLTRENDER_INSTANCE)) {
            const u32 flags = rd32(ctx, r + SLTRENDER_RENDER_FLAGS);
            wr32(ctx, r + SLTRENDER_RENDER_FLAGS, (flags & ~clear) | options.render_set);
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

/// The rows each viewport with a region of its owner's was last given here, so that the region
/// is moved once and not again on every call.
struct OwnerRegion {
    u32 viewport, y0, y1;
};
std::vector<OwnerRegion> owner_regions;

/// sCamera::Viewport::updateRegion(this): top-screen viewports drawn into the scene target.
/// The regions updateRegion computes from the 400 rows (modes 0 and 2..9, every call) are
/// scaled to the wider target. A region its owner set (mode 1, >= 10), such as the weapon
/// preview at the smithy, stays as it is between calls; it frames a 3D view inside a HUD box,
/// and the HUD is centred in the wider picture, so it moves by half the extra rows, once (a
/// region over the whole 400 rows is widened instead).
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
    const u8 mode = rd8(ctx, viewport + VIEWPORT_MODE);
    if (mode == 0 || (mode >= 2 && mode <= 9)) {
        for (const u32 field : {VIEWPORT_Y0, VIEWPORT_Y1}) {
            const u32 y = rd32(ctx, viewport + field);
            if (y <= TOP_ROWS) {
                wr32(ctx, viewport + field, y * rows / TOP_ROWS);
            }
        }
        return;
    }
    if (mode == 0xa) {
        return; // setupDraw puts its own rectangle in (a tunable makes it W rows)
    }
    const u32 y0 = rd32(ctx, viewport + VIEWPORT_Y0);
    const u32 y1 = rd32(ctx, viewport + VIEWPORT_Y1);
    auto it = std::find_if(owner_regions.begin(), owner_regions.end(),
                           [viewport](const OwnerRegion& r) { return r.viewport == viewport; });
    if (it != owner_regions.end() && it->y0 == y0 && it->y1 == y1) {
        return; // moved already
    }
    if (y0 > y1 || y1 > TOP_ROWS) {
        return; // not a region of the native 400 rows
    }
    const bool whole = y0 == 0 && y1 == TOP_ROWS;
    const u32 shift = (rows - TOP_ROWS) / 2;
    const u32 new_y0 = whole ? 0 : y0 + shift;
    const u32 new_y1 = whole ? rows : y1 + shift;
    wr32(ctx, viewport + VIEWPORT_Y0, new_y0);
    wr32(ctx, viewport + VIEWPORT_Y1, new_y1);
    if (it == owner_regions.end()) {
        owner_regions.push_back({viewport, new_y0, new_y1});
    } else {
        it->y0 = new_y0;
        it->y1 = new_y1;
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

// ── The HUD in wide render (docs/research/widescreen.md, "2D HUD"). The rLayout HUD keeps
// its 400-pixel layout space, centred in the wider picture. Each layout object's translation
// goes to the GPU as a shader constant written by 0x00ae65a4; the objects that make up the
// sides of the hunting HUD get theirs moved by half the extra width, out to the edges. They
// are told apart by the hash at the start of each object (its name's, it seems: the same
// objects have the same hashes in every run). Layout x grows to the left: pixel = 200 - x.
constexpr u32 CDRAW_OBJECT_CONSTANTS = 0x83c;  // cDraw: where 0x00ae65a4 put (x, y, z, 1), or 0
constexpr u32 CDRAW_SCREEN = 0x1bc;             // cDraw: bits 1-5, the screen being drawn (0 top)
constexpr u32 HUD_LEFT[] = {
    0x2E7FA61D,  // clock
    0x8293D551,  // health
    0xD07F4AA2,  // stamina
    0xBD56507D,  // weapon sharpness
    0xB2DA1296,  // next to the sharpness
    0x8AC1CCC4,  // party list: the hunter
    0xFDC6FC52,  // party list: the Palico
    0x64CFADE8,  // party list: a second hunter (local play)
    0x13C89D7E,  // party list: the second hunter's Palico
    0x4639C2C3,  // party list rows that were empty when traced (after the rows above, before the
    0x60497133,  //   item bar's objects): the other players' rows in a larger party
    0x174E41A5,
    0x892AD406,
};
constexpr u32 HUD_RIGHT[] = {
    0x1833E2C8,  // item selection
    0x3C598C15,  // item name
};

/// 0x00ae65a4(object, cDraw): uploads a layout object's translation and colour.
void LayoutObjectConstants(RCtx* ctx, RFunc original) {
    const u32 object = ctx->r[0];
    const u32 draw = ctx->r[1];
    original(ctx);
    const u32 rows = options.hud_edges ? WideRows() : 0;
    const bool trace = options.trace_layout && frames % options.trace_layout == 0;
    if (!rows && !trace) {
        return;
    }
    const u32 constants = rd32(ctx, draw + CDRAW_OBJECT_CONSTANTS);
    if (constants == 0) {
        return;
    }
    const u32 screen = rd32(ctx, draw + CDRAW_SCREEN) >> 1 & 0x1f;
    const u32 hash = rd32(ctx, object);
    if (trace) {
        LOG_INFO(Frontend, "layout: frame {} object {:08X} hash {:08X} screen {} at {:.1f}, {:.1f}", frames, object,
                 hash, screen, ReadF32(ctx, constants), ReadF32(ctx, constants + 4));
    }
    if (rows == 0 || screen != 0) {
        return;
    }
    const float shift = (static_cast<float>(rows) - TOP_ROWS) / 2.0f;
    if (std::find(std::begin(HUD_LEFT), std::end(HUD_LEFT), hash) != std::end(HUD_LEFT)) {
        WriteF32(ctx, constants, ReadF32(ctx, constants) + shift);
    } else if (std::find(std::begin(HUD_RIGHT), std::end(HUD_RIGHT), hash) != std::end(HUD_RIGHT)) {
        WriteF32(ctx, constants, ReadF32(ctx, constants) - shift);
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
constexpr u32 SPAD_HELD = 0x2c + 0x60;   // u32: buttons held, as sPad::processInput encodes them
constexpr u32 ACTION_KICK = 0x0001;      // the action no button produces: panel 7, the kick
u32 last_panel_touch = 0xFFFFFFFF;
// The target camera panel (panel type 13). Each frame the quest HUD's target driver
// (0x00b94804) runs the panel's taps through 0x00b8d024, which also takes a request byte
// as a tap: with one monster it toggles the target camera on it, with two it cycles
// first, second, off (a monster not yet seen gets the "no" sound). The HUD clears the byte
// at the end of the same update (0x00b9dfe0), and the game sets it itself from a menu
// action in one control mode only. sCamera+0xed5 is the result: 0 off, n monster n.
constexpr u32 QUEST_HUD = 0x0108260c;     // static (getQuestHud 0x0013f230)
constexpr u32 HUD_TARGET_REQUEST = 0x27e;  // u8: toggle the target camera as a tap would
u32 target_held = 0;

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
/// The kick button injects the kick for the next update, like the touch panel does (the
/// button keeps its own action too, so pick one the game doesn't use).
void MhPadUpdate(RCtx* ctx, RFunc original) {
    const u32 pad = ctx->r[0];
    if (options.trace_panels) {
        const u32 gameplay = rd32(ctx, pad + SMHPAD_INJECTED);
        const u32 menu = rd32(ctx, pad + SMHPAD_INJECTED_MENU);
        if (gameplay || menu) {
            LOG_INFO(Frontend, "panels: injected actions {:04X}, menu actions {:04X}", gameplay, menu);
        }
    }
    original(ctx);
    const u32 held = rd32(ctx, pad + SPAD_HELD);
    if (options.kick_button && (held & options.kick_button)) {
        wr32(ctx, pad + SMHPAD_INJECTED, rd32(ctx, pad + SMHPAD_INJECTED) | ACTION_KICK);
    }
    if (options.target_button) {
        // a press, not a hold: one request per press, as one tap
        const u32 now = held & options.target_button;
        if (now && !target_held) {
            wr8(ctx, QUEST_HUD + HUD_TARGET_REQUEST, 1);
        }
        target_held = now;
    }
}

// ── Quest start (docs/research/system-services.md, "A loading race at the start of a
// quest"). Right before a quest starts, 0x0069b21c builds a list of quest-file resources by
// asking the resource manager for each id of a table (0x0069af14); 0x00b7c010 then has
// 0x0069b4d0 copy the quest's data out of that list and releases it (0x006bc838). A file
// that is still being set up at that moment is left out of the list. At 60 fps that is a
// quest nobody starts; at higher frame rates it is often the one being started, which
// then runs on an empty stage: the hunter falls into a void and the quest fails. Until its
// quest is in the list, startQuest is put off a frame and the list released, so that the
// game builds it again.
constexpr u32 QUEST_HOLDER = 0x00fbeffc;      // the object that holds the current quest
constexpr u32 QUEST_TABLE = 0xe604;           // holder: u16 quest ids, 0-terminated
constexpr u32 QUEST_LIST = 0xe608;            // holder: the resources found for them
constexpr u32 QUEST_LIST_COUNT = 0xe610;
constexpr u32 RESOURCE_BUFFER = 0x64;         // resource: its file contents
constexpr u32 QUEST_FLOW_PARAMS = 0x00edbe90; // [this]+0x9788: what 0x00b7c010 starts
constexpr u32 QUEST_LIST_RELEASE = 0x006bc838;
constexpr u32 QUEST_START = 0x00b7c010;
constexpr u32 QUEST_HOLD_MAX_FRAMES = 600;

/// The quest id 0x00b7c010 is about to set up (its three branches), or 0.
u32 NextQuestId(RCtx* ctx) {
    const u32 base = rd32(ctx, QUEST_FLOW_PARAMS);
    if (!base) {
        return 0;
    }
    const u32 p = base + 0x9788;
    if (rd8(ctx, p + 0x18)) {
        const u32 id = rd16(ctx, p + 0x1a);
        if (id != 0xffff) {
            return id;
        }
        const u32 which = rd8(ctx, p + 0x19);
        return which < 3 ? 0xafc8 + which : 0;
    }
    return rd8(ctx, p + 0x89) ? rd16(ctx, p + 0x8c) : rd16(ctx, p + 0x3c);
}

/// Whether the list was built from a table with quest `id` but doesn't have it: the quest's
/// file was not ready when 0x0069af14 asked for it (0x006ae870 reads an entry's quest id at
/// buffer + [buffer] + 0x20).
bool QuestLeftOut(RCtx* ctx, u32 holder, u32 id) {
    const u32 list = rd32(ctx, holder + QUEST_LIST);
    const u32 table = rd32(ctx, holder + QUEST_TABLE);
    const u32 count = list ? rd32(ctx, holder + QUEST_LIST_COUNT) : 0;
    if (!list || !table || count > 4096) {
        return false;
    }
    bool in_table = false;
    for (u32 i = 0; i < 4096; ++i) {
        const u32 t = rd16(ctx, table + 2 * i);
        if (t == 0) {
            break;
        }
        in_table = in_table || t == id;
    }
    if (!in_table) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        const u32 res = rd32(ctx, list + 4 * i);
        const u32 data = res ? rd32(ctx, res + RESOURCE_BUFFER) : 0;
        const u32 header = data ? rd32(ctx, data) : 0;
        if (header != 0 && header < 0x10000 && rd16(ctx, data + header + 0x20) == id) {
            return false;
        }
    }
    return true;
}

/// Calls a recompiled game function with one argument, leaving the caller's registers as
/// they were.
void CallGame(RCtx* ctx, u32 function, u32 arg) {
    u32 regs[16];
    std::copy(std::begin(ctx->r), std::end(ctx->r), regs);
    const u32 flags[4] = {ctx->fn, ctx->fz, ctx->fc, ctx->fv};
    const u32 expect_next = ctx->expect_next;
    ctx->expect_next = 0;
    ctx->r[0] = arg;
    ctx->r[14] = QUEST_START;
    rc_call(ctx, function);
    std::copy(std::begin(regs), std::end(regs), ctx->r);
    ctx->fn = flags[0];
    ctx->fz = flags[1];
    ctx->fc = flags[2];
    ctx->fv = flags[3];
    ctx->expect_next = expect_next;
}

u32 quest_start_held = 0;

/// QuestFlow::startQuest(this).
void StartQuest(RCtx* ctx, RFunc original) {
    const u32 holder = rd32(ctx, QUEST_HOLDER);
    const u32 id = NextQuestId(ctx);
    if (holder && id && quest_start_held < QUEST_HOLD_MAX_FRAMES && QuestLeftOut(ctx, holder, id)) {
        if (quest_start_held++ == 0) {
            LOG_INFO(Frontend, "quest {}: its file wasn't ready for the quest list; building the list again",
                     id);
        }
        CallGame(ctx, QUEST_LIST_RELEASE, holder);
        rc_hook_return(ctx);
        return;
    }
    if (quest_start_held) {
        LOG_INFO(Frontend, "quest {}: started after {} more frames{}", id, quest_start_held,
                 quest_start_held >= QUEST_HOLD_MAX_FRAMES ? " (gave up)" : "");
        quest_start_held = 0;
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
    {0x00b7c010, "QuestFlow::startQuest", StartQuest},
    {0x00ae65a4, "nLayout::objectConstants", LayoutObjectConstants},
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
            if (options.hud_edges) {
                log.push_back(fmt::format("widescreen: the hunting HUD's sides moved {} pixels out to the edges",
                                          (WideRows() - TOP_ROWS) / 2));
            }
        }
    }
    if (options.trace_every) {
        log.push_back(fmt::format("tracing frame timing every {} frames", options.trace_every));
    }
    if (options.trace_panels) {
        log.push_back("tracing touch panels");
    }
    if (options.kick_button) {
        log.push_back(fmt::format("kick on pad button {:04X}", options.kick_button));
    }
    if (options.target_button) {
        log.push_back(fmt::format("target camera on pad button {:04X}", options.target_button));
    }
    Layout::SetScreenOverlay(options.touch_overlay);
    if (const auto& overlay = options.touch_overlay; overlay.height > 0) {
        constexpr const char* corners[] = {"top left", "top right", "bottom left", "bottom right"};
        log.push_back(fmt::format("custom screen layout: the touch screen over the top screen, {}, "
                                  "{:.0f}% of the window's height, {:.0f}% opaque",
                                  corners[static_cast<int>(overlay.corner)], overlay.height * 100,
                                  overlay.opacity * 100));
    }
    if (options.trace_layout) {
        log.push_back(fmt::format("tracing HUD layout objects every {} frames", options.trace_layout));
    }
    if (!options.dither) {
        log.push_back("dithering off");
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

Layout::ScreenOverlay ParseOverlay(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    using Corner = Layout::ScreenOverlay::Corner;
    Layout::ScreenOverlay overlay{0.30f, 0.85f, Corner::TopRight};
    std::istringstream words(v);
    std::string corner;
    words >> corner;
    std::replace(corner.begin(), corner.end(), '_', '-');
    const std::pair<const char*, Corner> corners[] = {{"top-left", Corner::TopLeft},
                                                      {"top-right", Corner::TopRight},
                                                      {"bottom-left", Corner::BottomLeft},
                                                      {"bottom-right", Corner::BottomRight}};
    const auto it = std::find_if(std::begin(corners), std::end(corners),
                                 [&](const auto& c) { return corner == c.first; });
    if (it == std::end(corners)) {
        return IsOn(corner) ? overlay : Layout::ScreenOverlay{};
    }
    overlay.corner = it->second;
    std::string height, opacity;
    if (words >> height) {
        overlay.height = std::clamp(std::strtof(height.c_str(), nullptr) / 100.0f, 0.05f, 1.0f);
    }
    if (words >> opacity) {
        overlay.opacity = std::clamp(std::strtof(opacity.c_str(), nullptr) / 100.0f, 0.1f, 1.0f);
    }
    return overlay;
}

} // namespace Port::Hooks
