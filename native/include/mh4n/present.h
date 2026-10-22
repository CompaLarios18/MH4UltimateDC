// The presentation contract (N2b-26, docs/native/presentation.md).
//
// How a GPU hands the screens it displays to whoever shows them, and the process's slot for the
// one shared Vulkan backend (N2b rule 4, proposed D56). The producers are the GPUs that can show
// their screens (`null-display`, N2b-07; the renderer, N2b-13); the consumers will be the
// compositor (N2b-05), the present thread (N2b-06) and the screenshot/dump code (N2b-14). Those
// packages are written against this header and do not exist yet.
//
// This header is self-contained on purpose: it includes only the standard library and mh4n/
// headers, and no Vulkan or backend header. The backend's objects (N2-21) appear as opaque
// pointers; the producer and the consumer both link the backend and cast them back.
//
// Threads: a GPU calls ScreenSink::Publish from the thread that handles the VBlank (the game
// thread, or the renderer thread behind the `queue` wrapper). Everything else says its own rule.
//
// A wrapper that sits in front of a Gpu (gpu::GpuWrapper, like `queue`) must implement Display
// too and forward each call to the Gpu it wraps, because consumers only see the outermost
// object; FindDisplay below is how they look for it.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "mh4n/gpu.h"
#include "mh4n/registry.h"
#include "mh4n/types.h"

namespace mh4n::present {

// ---- images -----------------------------------------------------------------------------------

// 3 bytes a pixel (R, G, B), row-major, top row first.
struct RgbImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgb;

    bool empty() const {
        return rgb.empty();
    }
};

// The displayed sizes at 1x. The top screen is 400 wide, or wider when the hooks widen it
// (NativePlatform::TopScreenWidth); the bottom screen is always 320 wide.
inline constexpr u32 kTopWidth = 400;
inline constexpr u32 kTopHeight = 240;
inline constexpr u32 kBottomWidth = 320;
inline constexpr u32 kBottomHeight = 240;

// How the pixels of a backend image are laid out against the displayed screen.
//   Upright:    image column = displayed column, image row = displayed row (top row first).
//   Rotated90:  as the 3DS framebuffers are stored, column by column. The image is as wide as
//               the screen is high and as high as it is wide; image row iy holds displayed
//               column x = iy, and image column ix holds displayed row y = (displayed height - 1
//               - ix), that is, each column is stored bottom to top. (Scaled sizes: use the
//               screen's height in image pixels.)
// CPU pixels are always upright.
enum class Orientation : u32 { Upright = 0, Rotated90 = 1 };

// A pixel position in a rotated image, from a displayed one. `x` and `y` are in scaled displayed
// pixels (displayed pixels times the scale, so 0..width*scale-1 and 0..height*scale-1), and
// `scaled_height`: the displayed height in image pixels (the screen's height at 1x times the scale).
struct PixelPos {
    u32 x;
    u32 y;
    friend bool operator==(const PixelPos&, const PixelPos&) = default;
};
constexpr PixelPos ToImagePixel(Orientation orientation, u32 scaled_height, u32 x, u32 y) {
    return orientation == Orientation::Upright ? PixelPos{x, y}
                                               : PixelPos{scaled_height - 1 - y, x};
}

// A rectangle of a backend image, in image pixels, in the image's own (stored) orientation.
struct ImageRegion {
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;

    friend bool operator==(const ImageRegion&, const ImageRegion&) = default;
};

// One displayed screen. Exactly one of the two forms is set:
//  - a backend image: `image` is the backend's image object (kept alive for as long as this
//    value, or any copy of it, is), `region` the part of it that holds the screen, `scale` the
//    image pixels per displayed pixel (the renderer's --scale), `orientation` its storage
//    orientation, and `timeline_value` the value of N2-21's timeline for the submission that
//    drew it: the compositor's submission will wait on it (0: nothing to wait for);
//  - CPU pixels: `pixels` holds width * height RGBA8 pixels (4 bytes: R, G, B, A), top row
//    first, upright, at 1x (`scale` 1, `region` the whole of it).
// In both, `width` x `height` is the displayed size at 1x.
struct ScreenImage {
    std::shared_ptr<const void> image;
    ImageRegion region;
    u32 scale = 1;
    Orientation orientation = Orientation::Upright;
    u64 timeline_value = 0;

    std::shared_ptr<const std::vector<u8>> pixels;

    u32 width = 0;
    u32 height = 0;

    bool IsBackendImage() const {
        return image != nullptr;
    }
    bool IsCpu() const {
        return image == nullptr && pixels != nullptr;
    }
    // The displayed size in image pixels.
    u32 ScaledWidth() const {
        return width * scale;
    }
    u32 ScaledHeight() const {
        return height * scale;
    }
    // The size `region` must have for this screen, in the stored orientation.
    u32 StoredWidth() const {
        return orientation == Orientation::Rotated90 ? ScaledHeight() : ScaledWidth();
    }
    u32 StoredHeight() const {
        return orientation == Orientation::Rotated90 ? ScaledWidth() : ScaledHeight();
    }
    // True when exactly one form is set and its sizes agree: a nonzero displayed size and scale;
    // for a backend image, a region of StoredWidth x StoredHeight; for CPU pixels, scale 1 and
    // width * height * 4 bytes, upright.
    bool Valid() const {
        if (width == 0 || height == 0 || scale == 0 || (image != nullptr) == (pixels != nullptr)) {
            return false;
        }
        if (image != nullptr) {
            return region.width == StoredWidth() && region.height == StoredHeight();
        }
        return scale == 1 && orientation == Orientation::Upright &&
               pixels->size() == static_cast<std::size_t>(width) * height * 4;
    }
};

// What one VBlank of the top screen shows. nullopt: that screen is off or not set up.
struct DisplayFrame {
    u64 sequence = 0;   // 0, 1, 2, ... one per frame published to the sinks of one run
    u32 game_frame = 0; // the game's frame counter at the VBlank
    u64 tick = 0;       // the game clock's tick at the VBlank
    std::optional<ScreenImage> top;
    std::optional<ScreenImage> bottom;
    // A draw since the previous frame used the gas mode (fog mode 7). Azahar draws no gas, so
    // the cross-model image checks report such frames and do not judge them (N2b rule 9).
    bool gas = false;
};

// ---- the interfaces ---------------------------------------------------------------------------

// Whoever wants the frames. Thread-safe; returns at once (it keeps the frame, or drops it, and
// never waits for the game or for the display: N2b rule 3).
class ScreenSink {
public:
    virtual ~ScreenSink() = default;
    virtual void Publish(const DisplayFrame& frame) = 0;
};

// What a gpu::Gpu that can show its screens also implements; found with dynamic_cast (FindDisplay).
class Display {
public:
    virtual ~Display() = default;

    // Add a sink before the run, remove it after: neither is called while frames are being
    // published, and a sink gets every frame published between the two calls and none outside
    // them. The sink must outlive its registration. Several sinks are allowed; each gets each frame.
    virtual void AddScreenSink(ScreenSink* sink) = 0;
    virtual void RemoveScreenSink(ScreenSink* sink) = 0;

    // The screen at 1x as RGB: for a backend image, downscaled from the internal scale with a box
    // filter and rotated upright; for CPU pixels, the alpha dropped. `done` may run before
    // ReadScreen returns (CPU pixels) or later, on another thread; it runs once. A screen the
    // GPU cannot read (not Valid(), or an image it did not make) gives an empty RgbImage.
    virtual void ReadScreen(const ScreenImage& screen, std::function<void(RgbImage)> done) = 0;
};

// The Display of `gpu` (or of what it wraps), nullptr when it has none.
inline Display* FindDisplay(gpu::Gpu* gpu) {
    return dynamic_cast<Display*>(gpu);
}

// Something that draws over the composed screens (the app's UI, N2b-11).
class UiLayer {
public:
    virtual ~UiLayer() = default;
    // Called by the compositor (N2b-05) after the screens, on the thread that composes. `recorder` is the
    // backend's command recorder (N2-21), opaque here; the target is `target_width` x
    // `target_height` pixels.
    virtual void Record(void* recorder, u32 target_width, u32 target_height) = 0;
};

// ---- which GPUs show their screens ------------------------------------------------------------

// Registered by name from the module that registers the GpuFactory of that name:
//     MH4N_REGISTER(present::DisplayGpu, "null-display", {});
// A GPU registered so must implement Display. It can be asked before any GPU is made (the app
// picks its default --gpu with it).
struct DisplayGpu {};

inline bool IsDisplayGpu(std::string_view name) {
    return Registry<DisplayGpu>::Instance().Find(name) != nullptr;
}

// ---- the shared backend -----------------------------------------------------------------------

namespace detail {
struct BackendSlot {
    std::mutex mutex;
    std::shared_ptr<void> backend;
};
// Never destroyed, so that no static destructor of ours races the process's own shutdown; the
// owner empties the slot before it destroys the backend.
inline BackendSlot& Slot() {
    static BackendSlot* slot = new BackendSlot;
    return *slot;
}
} // namespace detail

// The N2-21 backend object of the process (the one Vulkan device, N2b rule 4). `mh4u-native` will
// set it before the GPU is made (N2b-09) and set an empty pointer after the run; the renderer's
// factory will use it when it is set (N2b-12) and make its own when it is not (the headless runner).
// Thread-safe. The pointee's real type is the backend's; this header never looks inside it.
inline void SetSharedBackend(std::shared_ptr<void> backend) {
    auto& slot = detail::Slot();
    std::shared_ptr<void> old;
    {
        std::lock_guard lock(slot.mutex);
        old = std::move(slot.backend);
        slot.backend = std::move(backend);
    }
    // `old` is released here, outside the lock.
}

inline std::shared_ptr<void> SharedBackend() {
    auto& slot = detail::Slot();
    std::lock_guard lock(slot.mutex);
    return slot.backend;
}

} // namespace mh4n::present
