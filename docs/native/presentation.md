# The presentation contract (N2b-26)

How the screens the game displays get from the GPU to a window, and how the process shares one
Vulkan device between the renderer and the compositor. The contract is one header,
[`native/include/mh4n/present.h`](../../native/include/mh4n/present.h) (namespace `mh4n::present`),
tested by `native/tests/present_contract_test.cpp`. It has no implementation: the producers
(N2b-07, N2b-13) and the consumers (N2b-05, N2b-06, N2b-14) are built against it in parallel.

The header includes only the standard library and `mh4n/` headers, no Vulkan and no backend
header. The backend's objects (N2-26, N2-27) appear as `std::shared_ptr<const void>`, `std::shared_ptr<void>`
or `void*`; the producer and the consumer both link the backend and cast them back.

## Rule 4: one Vulkan device per process

([`wp/N2b.md`](wp/N2b.md) rule 4, proposed D56.) The compositor (N2b-05) will sample the renderer's images
directly, so both must use the same device. The process holds the N2-26 backend object in a slot,
`present::SetSharedBackend` / `present::SharedBackend`:

| Who | Does |
|---|---|
| `mh4u-native` (N2b-09) | will put its backend in the slot before the GPU is made, and set an empty pointer after the run |
| the renderer's factory (N2b-12) | will use the slot's backend when it is set |
| the headless runner | leaves the slot empty; the renderer then makes its own backend |

The slot is thread-safe and empty by default. The header never looks inside the pointee. The
slot lives for the whole process and is not destroyed at exit, so whoever fills it empties it
before it destroys the backend.

## What a frame is

`DisplayFrame` is what one VBlank of the top screen shows:

| Member | Meaning |
|---|---|
| `sequence` | 0, 1, 2, ... one per published frame of a run |
| `game_frame`, `tick` | the game's frame counter and the game clock's tick at the VBlank |
| `top`, `bottom` | a `ScreenImage`, or `nullopt` when that screen is off or not set up |
| `gas` | a draw since the previous frame used the gas mode (fog mode 7). Azahar draws no gas, so the cross-model image checks report such frames and do not judge them (N2b rule 9) |

A `ScreenImage` is one displayed screen, in one of two forms (`Valid()` checks that exactly one is
set and the sizes agree):

- **A backend image**: `image` (kept alive for as long as the frame, or any copy, is), the
  `region` of it that holds the screen, `scale` (image pixels per displayed pixel, the renderer's
  `--scale`), `orientation`, and `timeline_value`, the value of N2-26's timeline for the
  submission that drew it. The compositor's submission will wait on that value (0: nothing to wait for).
- **CPU pixels**: `pixels`, RGBA8, top row first, upright, 1x.

In both, `width` x `height` is the displayed size at 1x: the top screen is 400 x 240, or wider when
the hooks widen it (`NativePlatform::TopScreenWidth`); the bottom screen is 320 x 240.

**Orientation.** The 3DS stores its framebuffers column by column, each column bottom to top. A
backend image can keep that layout (`Rotated90`) so the renderer needs no extra pass. Then the
image is as wide as the screen is high and as high as it is wide (`region` is the stored size:
`StoredWidth()` x `StoredHeight()`), image row `iy` holds displayed column `x = iy`, and image
column `ix` holds displayed row `y = scaled height - 1 - ix`. `ToImagePixel` does that mapping (its
`x` and `y` are in scaled displayed pixels). `Upright` means image pixel = displayed pixel. CPU
pixels are always upright. This convention is the hardware's. It is **not** byte-equal to the
output of `DisplayShadow::BuildScreen` (`native/src/gpu_capture/image.h`), the upright image the
capture player compares against: that image carries Azahar's one-pixel shift (it reads index
`y * stride + stride - x`, so its row 0 comes from the next column), and a producer or a test must not
take the two to be the same bytes.

## Producers and consumers

None of the stages after `Publish` exists yet; the picture is the plan N2b-05, N2b-06 and N2b-23 build to.

```
GPU (VBlank)                                         window
  |  DisplayFrame                                      ^
  v                                                    |
ScreenSink::Publish ---> present thread's mailbox ---> compositor + UiLayer::Record ---> swapchain
  (any thread, returns at once)  (will keep only the newest)           (will wait on timeline_value)
```

- **`ScreenSink::Publish(const DisplayFrame&)`**: thread-safe and returns at once. It keeps the
  frame or drops it; it never waits for the game or for the display (N2b rule 3: the game never
  waits for presentation). The GPU calls it from the thread that handles the VBlank.
- **`Display`**: what a `gpu::Gpu` that can show its screens also implements, found with
  `dynamic_cast` (`present::FindDisplay(gpu)`):
  - `AddScreenSink` before the run and `RemoveScreenSink` after it. A sink gets every frame
    published in between and none outside; several sinks each get each frame; the sink outlives its registration.
  - `ReadScreen(screen, done)`: the screen at 1x as `RgbImage` (3 bytes a pixel, top row first),
    downscaled from the internal scale with a box filter and rotated upright. `done` runs once; it
    may run before `ReadScreen` returns (CPU pixels) or later on another thread (a backend image
    that must be read back). An invalid screen gives an empty image.
  - A GPU wrapper (`gpu::GpuWrapper`, such as `queue`) must implement `Display` as well and forward to the GPU it wraps, as it does `GpuDebug`.
- **`UiLayer::Record(recorder, target_width, target_height)`**: called by the compositor (N2b-05) after the
  screens, on the thread that composes, with the backend's command recorder (opaque here).

## Which GPUs show their screens

`present::DisplayGpu` is an empty tag registered by name next to the `GpuFactory` of that name:

```cpp
MH4N_REGISTER(gpu::GpuFactory, "null-display", ...);
MH4N_REGISTER(present::DisplayGpu, "null-display", {});
```

`present::IsDisplayGpu(name)` says whether such a registration exists, without making a GPU, so
`mh4u-native` will pick its default `--gpu` (N2b-09). A GPU registered so must implement `Display`.
The registrations: `null-display` (N2b-07) and `renderer` (N2b-13).

## Parts the other packages add

N2b-05 (the compositor), N2b-06 (the swapchain and the present thread) and N2b-23 (frame pacing)
add their sections here.
