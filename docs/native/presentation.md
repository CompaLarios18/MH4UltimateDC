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

## The compositor (N2b-05)

`native/src/present/` (module `mh4n_present`, header `present/compositor.h`, namespace `mh4n::present`),
tested by `native/tests/present_test.cpp`. It draws one `DisplayFrame` into a backend image with a
`screen_layout::Layout` (N2b-04). It goes through `rhi.h` only: it includes no Vulkan header and
uses neither `vulkan_access.h` nor anything under `native/src/gpu/backend/` besides the public
headers (`rhi.h`, and `targets/upload.h` for `UploadToImage`). Written for the present thread
(N2b-06); the swapchain, the mailbox and the frame sources are other packages.

```cpp
auto compositor = present::Compositor::Create(backend);            // rhi::Result<unique_ptr<Compositor>>
rhi::Recorder rec = backend->BeginRecording();
present::ComposeResult done = (*compositor)->Compose(rec, *target, frame, layout, options);
// a swapchain image: rec.PrepareForPresent(*target) goes here, after Compose
rhi::TimelineValue value = (*compositor)->Submit(std::move(rec), std::move(done));
```

| | |
|---|---|
| `Create(backend)` | compiles the two shaders (GLSL, N2-06's glslang, `present/shader_compile.h`: a vertex and a fragment stage), makes the pipelines for `Rgba8Unorm` and `Bgra8Unorm` targets, the nearest and the linear sampler, and a 256 KiB vertex ring. A pipeline for another target format is made on first use; a format that cannot be a colour attachment gives `ComposeResult::drawn == false` and leaves the recorder untouched. The app's own UI and blits are not in N2-16's pipeline cache; these pipelines are made here, at start-up |
| `Compose(recorder, target, frame, layout, options)` | on an open recorder of the calling thread: uploads the CPU-pixel screens (`UploadToImage`, into a small pool of `Rgba8Unorm` textures reused once their submission has passed), then `BeginTargetPass` on the target with a clear to opaque black, one draw per shown screen, and `options.ui->Record(&recorder, target_w, target_h)` last. The target's pass is still open when `Compose` returns, so a `UiLayer`'s raw Vulkan commands (`CommandBuffer(recorder)`, as Dear ImGui's backend records them) are valid inside it |
| `Submit(recorder, result)` | `Backend::Submit` with `result.waits`, then keeps the frame's images alive (`Backend::Retire`) and marks the upload textures busy until the submission's value |
| `ComposeOptions` | `linear_filter` (the app's `linear_filter`: bilinear, else nearest) and `ui` (the `UiLayer`, or null) |

**What is drawn.** Order: the top screen (`top_shown`, `top` rectangle, `top_opacity`), then, when `bottom_shown`,
the bottom screen (`bottom` rectangle, `bottom_crop`, `bottom_opacity`), then each of the layout's further views
(`views[0..view_count)`, the last on top) with its own crop, at the bottom screen's opacity (`platform.h`:
"at the same opacity"). A screen that is `nullopt`, or not `Valid()`, or whose `region` reaches outside its image,
is left black (the invalid ones log a warning once). A layer is a quad of a triangle strip: the destination
rectangle in the target's pixels converted to the target's normalised coordinates, the source coordinate at each
corner from the crop, the screen's size in image pixels, its orientation and its `region`; the sampler does the
rest, so `Rotated90` costs nothing (`present.h`'s mapping: image column = scaled height minus the displayed row,
image row = the displayed column). The sample is clamped to the centres of the region's edge texels, so linear
filtering never reads the neighbours outside the region. The screen is opaque whatever its alpha channel says;
the pipeline blends `src * a + dst * (1 - a)` on the colour with `a` the layer's opacity (1 replaces exactly) and
keeps the target's alpha at what the clear made it (1).

**Clipping.** Every draw's scissor is its rectangle clipped to the target, and a draw with nothing left is skipped,
so a layout whose rectangles lie outside `Layout::width` and `height` is safe: an Overlay layout with an overlay
height of 0 (`screen_layout.h`: top `{0,0,800,480}` and bottom `{80,500,720,980}` in any window) draws the part that
is inside the target, and rectangles of billions of pixels draw nothing out of bounds. `Layout::width/height` are
not used for anything: the target's own size is the window.

**The owner rule and the layout** (`rhi-spike.md` 9.4 item 5, `rhi.md`). The compositor records only on images its
thread owns (the target it is given, its own upload textures) and only *samples* the frame's images
(`BindingSet::SetSampledImage`): it records no barrier, copy or layout change on them. A renderer's image is
sampled in `GENERAL`, the backend's one layout. The submission must wait for the value of the submission that drew
the screen at the stage that reads it: `ComposeResult::waits` is one `Wait{value, Stage::FragmentShader}` (the
highest `timeline_value` of the frame's backend images; none for CPU pixels or a value of 0), and `Submit` passes it
(a wait at the default stage reproduced the hazard, `rhi-spike.md` 6.9 C2 and C5). A frame image produced on
another thread is the producer's to hand over (`rhi::HandOver(image, composing_thread, Wait{value,
Stage::FragmentShader})`, or settle in `GENERAL`): the compositor's first sample takes it over and the backend's
check (`MH4N_RHI_CHECKS`) wants the hand-over's wait in the submission, which `Submit` supplies. A `Compositor` and
its recorders and results belong to one thread; submit each result before the next `Compose`.

**Tests** (`present_test`, 19 cases on lavapipe; the log names the llvmpipe device). The expected pixels are not
the compositor's: the rectangles are the layout table's (`screen_layout_expected.inc`, Azahar's own output); the
sources are a function of the screen and the displayed pixel; the rule is nearest sampling at the destination
pixel's centre, worked out from the definition (a pixel whose sample point is within 2% of a texel boundary
accepts either neighbour; a blended one there is not compared). Every pixel of every target is compared. The
cases: all 48 mode and size combinations of N2b-04's table with each screen as CPU pixels or a backend image at
scale 1 or 3, upright or rotated; a widened top screen (573x240) at 1:1 with exact corners and centre; opacity
0.25 to 1 within 1/255; a crop; two views, at the bottom screen's opacity; an Overlay with height 0 (rectangles
outside the target) and the small windows of the table; a missing, an invalid or a region-outside screen;
nearest against linear; a region with garbage around it under linear filtering; a `UiLayer` that draws with the
`Recorder` and one that records raw Vulkan commands; a BGRA target; the waits; the upload pool; and a frame
produced on another thread, handed over and waited for at the fragment stage with the backend's owner-rule
handler counting zero breaches (with a control that drops the waits and sees `RequiredWait`, not run under the
validation layer). Run under the layer with
`python3 tools/native/vk_layer_run.py --build build/native -R '^present_test$'`: no message. After every readback
the test calls `vkDeviceWaitIdle`, because the layer 1.3.275 does not count a host wait on a timeline semaphore
as ordering (a readback buffer's memory reused as the next upload's staging buffer shows as a hazard otherwise).

## Parts the other packages add

N2b-06 (the swapchain and the present thread) and N2b-23 (frame pacing) add their sections here.
