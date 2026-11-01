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

The compositor (N2b-05) and the present thread with its mailbox (N2b-06) exist; pacing (N2b-23) and the stages
that publish (N2b-07, N2b-13) and read (N2b-10, N2b-11) come from other packages. The picture:

```
GPU (VBlank)                                         window
  |  DisplayFrame                                      ^
  v                                                    |
ScreenSink::Publish ---> present thread's mailbox ---> compositor + UiLayer::Record ---> swapchain
  (any thread, returns at once)  (keeps only the newest)         (waits on timeline_value)
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
| `Compose(recorder, target, frame, layout, options)` | on an open recorder of the calling thread: uploads the CPU-pixel screens (`UploadToImage`, into a small pool of `Rgba8Unorm` textures reused once their submission has passed), then `BeginTargetPass` on the target with a clear to opaque black, one draw per shown screen, and `options.ui->Record(&recorder, target_w, target_h)` last, after resetting the viewport and the scissor to the whole target (the pipeline, bindings and vertex buffers are still the compositor's last: the layer binds its own). The target's pass is still open when `Compose` returns, so a `UiLayer`'s raw Vulkan commands (`CommandBuffer(recorder)`, as Dear ImGui's backend records them) are valid inside it |
| `Submit(recorder, result)` | required: `Backend::Submit` with `result.waits`, then keeps the frame's images alive (`Backend::Retire`) and marks the upload textures busy until the submission's value. A submission that skips it can see a pooled texture reused, or a frame image freed, while the GPU still reads it; `ComposeResult::waits` is exposed for inspection and tests |
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

**Tests** (`present_test`, 23 cases on lavapipe; the log names the llvmpipe device). The expected pixels are not
the compositor's: the rectangles are the layout table's (`screen_layout_expected.inc`, Azahar's own output); the
sources are a function of the screen and the displayed pixel; the rule is nearest sampling at the destination
pixel's centre, worked out from the definition (a pixel whose sample point is within 2% of a texel boundary
accepts either neighbour; a blended one there is not compared). Every pixel of every target is compared. The four corners and the centre of every shown screen (top,
bottom, each view) are also checked on their own, with the exact colour: for the 48 table cases times the four
ways of giving the screens that is 1548 strict points and 84 ambiguous ones (sample point on a texel edge, either
neighbour accepted; 48 more are hidden, blended or outside and not counted). At least 90% strict is asserted and
the numbers are printed. Five 1920x1080 and 1280x1024 cases with non-integer ratios have every point strict. The
cases: all 48 mode and size combinations of N2b-04's table with each screen as CPU pixels or a backend image at
scale 1 or 3, upright or rotated; a widened top screen (573x240) at 1:1 with exact corners and centre; opacity
0.25 to 1 within 1/255; a crop; two views, at the bottom screen's opacity; an Overlay with height 0 (rectangles
outside the target) and the small windows of the table; a missing, an invalid or a region-outside screen;
nearest against linear; a region with garbage around it under linear filtering; a `UiLayer` that draws with the
`Recorder`, one that records raw Vulkan commands, and one that sets no viewport or scissor and lands in the corner farthest from the last screen; a target that cannot be a colour attachment (`drawn == false`); a BGRA target; the waits; the upload pool; and a frame
produced on another thread, handed over and waited for at the fragment stage with the backend's owner-rule
handler counting zero breaches (with a control that drops the waits and sees `RequiredWait`, not run under the
validation layer). Run under the layer with
`python3 tools/native/vk_layer_run.py --build build/native -R '^present_test$'`: no message. After every readback
the test calls `vkDeviceWaitIdle`, because the layer 1.3.275 does not count a host wait on a timeline semaphore
as ordering (a readback buffer's memory reused as the next upload's staging buffer shows as a hazard otherwise).

## The swapchain and the present thread (N2b-06)

`native/src/swapchain/` (module `mh4n_swapchain`, headers `swapchain/presenter.h`, `swapchain/display_time.h`, namespace
`mh4n::swapchain`), tested by `native/tests/swapchain_test.cpp`. It goes through `rhi.h` and `vulkan_access.h` only (the
handles, `QueueLock`, `WrapExternalImage`) and the compositor of N2b-05; it includes nothing else from
`native/src/gpu/backend/`.

```cpp
auto presenter = swapchain::Presenter::Create(backend, swapchain::Target::Window(surface, w, h), layout_source, options);
display->AddScreenSink(presenter->get());                 // it is a present::ScreenSink
(*presenter)->SetUiLayer(&layer);  (*presenter)->Resize(w, h);  (*presenter)->SetVsync(false);  (*presenter)->Stop();
```

| Call | Does |
|---|---|
| `Create(backend, target, layout_source, options, hooks)` | starts the present thread and waits until it has made the compositor and, for a window, the first swapchain: an error (no surface on the backend, a surface with neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM, shaders that do not compile) is the result. `Target::Window(surface, w, h)`: a `VkSurfaceKHR` the caller made on the backend's instance and passed in `BackendOptions::surface`, destroyed by the caller after the presenter; `Target::Offscreen(w, h)`: an `Rgba8Unorm` image of the presenter's own |
| `Publish(frame)` (`ScreenSink`) | any thread, returns at once: the frame goes into the one-slot mailbox, replacing an older one nobody took (that one is counted dropped), and the thread is woken. The copy of the `DisplayFrame` (shared pointers) is made outside the lock |
| `LayoutSource` | `std::function<LayoutRequest()>`, called on the present thread once per composed frame: the `screen_layout::Inputs` (`window_w/h` are overwritten with the target's size) and the `present::ComposeOptions` (`ui` is overwritten by the layer given to `SetUiLayer`) |
| `SetUiLayer(layer)` / `RequestRedraw()` | the layer the compositor calls last (N2b-11); `SetUiLayer(nullptr)` waits for a composition in progress, after which the layer is not called again. Called by the layer itself, inside its `Record` on the present thread, it waits for nothing (that composition is the caller): the change applies from the next composition and this call of `Record` runs to its end. A redraw composes the last frame again with the layout source's current answer; before a first frame it does nothing. **The last frame is kept as the `DisplayFrame` was published, so a producer must not overwrite a backend image that a published frame still refers to** (N2b-13) |
| `CurrentLayout()` | a copy of the `screen_layout::Layout` of the frame composed last, all zero before the first (the shell maps clicks with `TouchTracker` on it, N2b-10) |
| `SetVsync(bool)` | takes effect with the next present (a redraw is made for it): the swapchain is recreated |
| `Resize(w, h)` / `SetMinimised(bool)` | the size the swapchain asks for when the surface does not dictate its own (below); a zero in either dimension, or minimised, presents nothing: published frames are taken and counted as dropped (D60), a redraw is not done; leaving minimised, or any resize, draws the last frame again |
| `SetRefreshHz`, `SetPresentLog(path)` | the display's rate for the log's header (call it first), and the log (below) |
| `DumpTo(dir, n)` | offscreen only: every n-th composed image as `presented_<sequence>.ppm` (the `DisplayFrame::sequence`), and the last one as `last.ppm` at `Stop` |
| `Stop()` | see "Shutdown" (called on the present thread, from a `UiLayer` or a hook, it only requests the stop); `GetStats()` returns counters (published, presented, composed, dropped, redraws, swapchain creations, out-of-date, suboptimal, acquire timeouts, the extent, the present mode, the display-time label) |

### Threads

The present thread is the one thread that records, submits and presents. It owns the swapchain, its images (wrapped with
`WrapExternalImage` on that thread, which makes it their owner), the offscreen target and the `Compositor`. Every
swapchain call is on it, because `vk.xml` marks the swapchain `externsync` in `vkAcquireNextImageKHR`,
`vkQueuePresentKHR`, `vkWaitForPresentKHR`, `vkGetPastPresentationTimingGOOGLE` and `vkDestroySwapchainKHR` (and the
surface and `oldSwapchain` in `vkCreateSwapchainKHR`); the queue is `externsync` in `vkQueueSubmit`, `vkQueuePresentKHR` and
`vkQueueWaitIdle`, so each of those is under `rhi::QueueLock`, and nothing that calls `Backend::Submit` runs while the lock is
held. With `MH4N_RHI_CHECKS` an own recording, submission or present that runs on another thread calls the owner-rule
handler with `OwnerRule::WrongThread` (`swapchain_test` forces one onto another thread with a hook and counts the three
reports). Everything the public calls do is to hand state over under one mutex; none of them waits for the GPU or the
display. The mutex is never held across a Vulkan call or a hook, and the composition's `compose_mutex` (for `SetUiLayer`)
is taken only around `Compose`.

### Acquire and present: how they are synchronised

`Backend::Submit` takes timeline waits only (`rhi::Wait`), so a frame's submission can neither wait for a binary
acquire semaphore nor signal a binary semaphore for the present. Two things follow, both on the present thread:

1. **Acquire with a fence, no semaphore.** `vkAcquireNextImageKHR(…, VK_NULL_HANDLE, fence)` in slices of 20 ms
   (`VK_TIMEOUT` and `VK_NOT_READY` leave the fence and the image untouched: "If vkAcquireNextImageKHR does not
   successfully acquire an image, semaphore and fence are unaffected", `wsi.adoc`), then `vkWaitForFences` in slices
   before the frame is recorded. The specification allows either: "The presentation engine may not have finished reading
   from the image at the time it is acquired, so the application must use semaphore and/or fence to ensure that the image
   layout and contents are not modified until the presentation engine reads have completed", and "An application must wait
   until either the semaphore or fence is signaled before accessing the image's data". The host wait happens before the
   submission is made, so it orders everything that follows (the host operation happens-before the `vkQueueSubmit`). The
   fence must be unsignaled with no signal pending: it is reset before each acquire and waited for after it. The acquire
   never blocks the renderer: it blocks only the present thread, and in slices that look at `Stop`.
2. **An empty submission signals the present's semaphore.** After the compositor's `Submit` returns (the backend's
   `Submit` has called `vkQueueSubmit` by then: `core/submit.cpp` → plume's `executeCommandListsEx`), the presenter,
   under `QueueLock`, submits an empty batch that signals a binary semaphore and presents with that semaphore as the wait.
   The specification asks for this: "The processing of the presentation happens in issue order with other queue
   operations, but semaphores must be used to ensure that prior rendering and other commands in the specified queue
   complete before the presentation begins", and the image's automatic visibility operation "happens-after the semaphore
   wait operation" (`vkQueuePresentKHR`, `wsi.adoc`). The semaphore signal's first synchronisation scope is defined by the
   command that makes it (`synchronization.adoc`, "Semaphore Signaling"); for `vkQueueSubmit` that scope includes every
   command submitted earlier on the queue (the sentence is in the command's own page, which the two chapters I had do not
   contain; N2-27's `swapchain/frames_over_a_headless_surface…` makes the same submission and the layer accepts it).
   `PrepareForPresent(image)` is the last command of the frame's recorder, so the image is in `PRESENT_SRC_KHR` by the time
   the signal runs. A submission by the renderer thread between the two is harmless: the signal covers a superset.
   There is one semaphore per swapchain image, reused when that image is acquired again (the acquire returning the
   image means the presentation engine is done with the wait: `VK_EXT_swapchain_maintenance1`'s present fence is not used).

The price: the `QueueLock` is held across the empty submission and `vkQueuePresentKHR` (the specification requires it: the queue is
externally synchronised in both, and "may block, but must return in finite time"); a driver whose present blocks in FIFO
holds up the renderer's `Backend::Submit` for that long. `swapchain_test` checks the lock from the inside
(`TestHooks::queue_locked` runs between the two calls; another thread asking for `QueueLock` stays blocked for as long as the hook
runs and gets it afterwards). The other
choice, `Backend::WaitIdle(value)` on the host before a present with no wait semaphore, would keep the lock free but stall
the present thread for the whole GPU time of the frame and still breaks the "semaphores must be used" sentence.
`vkQueueWaitIdle` (teardown, recreation, and after a `--present-dump` readback) is under the lock too.

### Out of date, suboptimal, recreation

- `VK_ERROR_OUT_OF_DATE_KHR` (or `VK_ERROR_SURFACE_LOST_KHR`) from an acquire: nothing was acquired; the swapchain is
  recreated on the present thread and the acquire is repeated. From a present: the frame was not shown ("presentation
  fails"), so it is recreated and the same frame is composed again. At most four attempts a frame; then the frame is
  dropped and the next one tries again.
- A present that fails in any other way (`vkQueueSubmit` or `vkQueuePresentKHR` returns an error) drops the frame and marks the swapchain for recreation: the acquired image is never released, so the next frame starts from a new swapchain.
- `VK_SUBOPTIMAL_KHR` from an acquire or a present: the image is good ("can still be used for presentation"), the frame is
  shown, and the swapchain is recreated before the next one.
- Recreation = device idle (`Backend::WaitIdle`, then `vkQueueWaitIdle` under the lock, then the acquire fence if its signal
  is pending, bounded to 2 s), the wrapped images dropped (the backend's rule: before the swapchain goes), the semaphores, the
  fence and the swapchain destroyed, a new swapchain made with `oldSwapchain` null. Resize and a change of vsync recreate it
  the same way.
- **The extent** (`policy.h` `ChooseExtent`): a surface whose `currentExtent` is the special value `0xFFFFFFFF` (a
  headless surface, or a window system that lets the swapchain choose) takes the size `Resize` set, clamped to
  `minImageExtent`..`maxImageExtent`; any other surface dictates `currentExtent`. A zero (minimised) presents nothing.
- Headless surfaces never answer out-of-date or suboptimal, so `TestHooks::acquire_result` / `present_result` inject them:
  the case that tests each of the four paths, and a surface that stays out of date, is `swapchain_test`'s
  `out_of_date_and_suboptimal…`.

### Present modes

`ChoosePresentMode` (pure, `policy.h`): vsync on → `FIFO`; off → `MAILBOX` if the surface offers it, else `IMMEDIATE`, else
`FIFO` (Apple's surfaces offer no MAILBOX, `rhi-spike.md` X9). The image count is `minImageCount + 1` (at most
`maxImageCount`), the format the first of B8G8R8A8_UNORM / R8G8B8A8_UNORM in the sRGB non-linear colour space (the
compositor has pipelines for exactly those two; the game's pixels are not converted), `COLOR_ATTACHMENT` usage plus
`TRANSFER_SRC` / `TRANSFER_DST` where the surface allows them, identity transform, opaque composite alpha. The extent, image count, transform, composite alpha and the single-UNDEFINED-format rule follow Azahar's `Swapchain::SetSurfaceProperties` and `FindPresentFormat` (`policy.cpp` has the D6 header and a row in `native/THIRD_PARTY.md`); the present mode choice is this port's own (Azahar prefers IMMEDIATE to MAILBOX and has a frame-limit input).

### The present log

`SetPresentLog(path)` writes, as `present_log.h` formats them:

```
# present v1 2026-11-01T12:00:00Z refresh_hz=239.76 display_time=present_wait
sequence,game_frame,published_us,composed_us,presented_us,displayed_us,dropped_before
```

then one row per presented frame, in order (a redraw is a row too, repeating the sequence, with `published_us` the time of the
request). Times are microseconds of `std::chrono::steady_clock` (its own epoch), so they line up with the process's other logs.
`composed_us` is when the composition was submitted; `presented_us` when `vkQueuePresentKHR` returned (offscreen: when the GPU
finished the frame); `dropped_before` the frames published since the previous row that were never shown (a frame that is taken and then discarded, minimised or with a zero extent, hands its own count on to the next row, so the rows' sum is `Stats::dropped` up to what was dropped after the last row). `refresh_hz` is
`SetRefreshHz`'s, else `VK_GOOGLE_display_timing`'s refresh cycle, else `unknown`. At start the presenter logs
`present wait: on|off` (from `PresentWaitAvailable()`) and `display time: <source>`.

**Where `displayed_us` comes from** (D57: only displayed times count), `ChooseDisplayTime`, the first that applies:

| Source | When | How |
|---|---|---|
| `present_wait` | `PresentWaitMeasuresDisplay()` (not merely `PresentWaitAvailable()`: MoltenVK releases the wait at command-buffer completion) | every present carries a `VkPresentIdKHR` (1, 2, 3 …); right after the present the thread calls `vkWaitForPresentKHR` for that id in 5 ms slices (it may not run elsewhere: the swapchain is externally synchronised) until it returns, at most 250 ms and never past `Stop`; `displayed_us` is the time it returned. The thread waits for each frame to be shown before it composes the next, so there is one frame in flight (FIFO shows the next frame one refresh later at the earliest): N2b-23, which reads these rows to pace the game, must know that the present wait is part of the loop's period and not an observer beside it |
| `display_timing` | `DisplayTimingAvailable()` | `VkPresentTimesInfoGOOGLE` with the id; `vkGetPastPresentationTimingGOOGLE` is polled every 4 ms and after each present, a row waits (rows stay in order) up to 500 ms for its answer; the actual present time is taken to be in the steady clock's time base, and an answer more than a second from the present call is not believed (warning once, field empty). Untested on a real device: lavapipe lists neither extension |
| a registered `DisplayTimeSource` | a registration named `Options::display_time_source` (default `platform`) | `DisplayedUs(present_id, present_host_us)` on the present thread after each present, in order; nullopt → empty |
| `none` | otherwise, and always for an offscreen target | the field is empty: the time a frame is shown is not measurable |

The label after `display_time=` is `present_wait`, `display_timing`, the source's `Name()` or `none`. **N2b-23 registers its
Windows source** (D57's ETW or DWM) in a module of its own: `MH4N_REGISTER(swapchain::DisplayTimeSourceFactory, "platform",
{[] { return std::make_unique<MySource>(); }})` (`display_time.h`); the source is made once, when the presenter starts, and
called only on the present thread.

### Shutdown

`Stop()` sets the stop flag and joins; the frame waiting in the mailbox is dropped at that moment and counted in `Stats::dropped`. Called on the present thread (a `UiLayer`'s `Record`, a test hook), `Stop` only sets the flag and drops the mailbox frame, taking no lock a joining thread may hold, because the thread cannot wait for itself and another thread inside `Stop` is waiting for it; the `Stop` or the destructor of another thread joins. Every wait on the present thread is cut into slices of at most 20 ms that look at it:
the acquire, the fence, the present wait, the idle waits of a recreation. A composition in progress finishes (it cannot be
abandoned half recorded); a frame waiting in the mailbox, and one whose acquire or wait was abandoned, are dropped. Then, on
the present thread, the device is made idle (`Backend::WaitIdle`, `vkQueueWaitIdle` under the lock) and the images, the
semaphores, the fence, the swapchain, the offscreen target and the compositor are destroyed; `last.ppm` and the log's last
rows are written first. `Publish` after `Stop` counts the frame as dropped. A test hook that blocks the thread inside a
composition must end its wait when `StopRequested()`: the hook is the test's, not the presenter's.

### Under the validation layer with a second submitting thread

`vk_layer_run.py` runs `swapchain_test` under the Khronos layer 1.3.275 with synchronization validation. The case
`another_thread_submitting_to_the_backend_while_frames_are_presented` (one thread calls `Backend::Submit` on empty recorders
in a loop while 120 frames are presented) got `SYNC-HAZARD-WRITE-AFTER-PRESENT` from `vkQueueSubmit`, on the layout-transition
barrier of a composition into a re-acquired swapchain image, "after the previous present of that image" (3 runs of 3 at full
rate). It is **the layer's bookkeeping, not a gap in the protocol**:

- The protocol is the specification's: the acquire's fence is waited for on the host before the frame is recorded and
  submitted, and the host wait happens-before the `vkQueueSubmit`.
- In the layer (`layers/sync/sync_validation.cpp`, tag v1.3.275) a presented image stays in the swapchain's "presented" list
  with its present access until an acquire takes it out; an acquire with a fence stores that image state in
  `waitable_fences_` (`RecordAcquireNextImageState` → `UpdateFenceWaitInfo`), and the access is released only when a
  successful `vkWaitForFences` or `vkGetFenceStatus` is recorded (`WaitForFence` → `ApplyAcquireWait`), which applies the
  release to the queue's **current last batch** (`ForAllQueueBatchContexts`). A `vkQueueSubmit` is recorded in two steps on
  its own thread: `ValidateQueueSubmit` builds the new batch from a copy of the queue's last batch, and `RecordQueueSubmit`
  makes it the queue's last batch (`UpdateLastBatch`). The layer holds no lock across the two, and neither step takes the
  application's `QueueLock`. When the present thread's fence wait is recorded between the other thread's two steps, the
  release goes to the old last batch, the new batch is a copy made before it, and so the image keeps its present access: the
  present thread's next submission, which inherits that batch, reports the hazard.
- Diagnostic (reverted): the fence's wait replaced by a poll of `vkGetFenceStatus` under `QueueLock`, so that the call that
  sees the signal cannot run between the other thread's two steps (`Backend::Submit` holds `QueueLock` across `vkQueueSubmit`):
  the same case, 4 runs of 4 clean (120 frames each). The same poll **without** the lock: 3 runs of 3 report the hazard. So it
  is the exclusion with the other thread's `vkQueueSubmit`, not the poll, that removes it.
- Clean already when the other thread submits strictly between the present thread's frames (no overlap), and in every
  other `swapchain_test` and `present_test` case (one submitting thread at a time).

`Options::acquire_wait_under_queue_lock` (default **off**) does that poll in production code, for runs under the layer:
`vkGetFenceStatus` under `QueueLock` every 200 us instead of a blocking `vkWaitForFences`. A real driver needs it not, and the
poll adds up to 200 us to the wait; the stress case turns it on exactly when `VK_INSTANCE_LAYERS` names the validation layer
(`vk_layer_run.py` sets it), and a second case (`the_acquire_fence_observed_under_the_queue_lock_…`) runs it always, so the polling
path is exercised without the layer too. **N2b-09's tests with the renderer and the present thread on one queue under the layer
need the option on** (or a layer without the race; later versions of the layer were not checked).

### Tests

`swapchain_test` (32 cases; 7 need no device): the pure choices on synthetic lists; the mailbox and the log's text; and, on a
`VK_EXT_headless_surface` swapchain, frames in order; three frames published while a hook holds the thread inside a
composition show only the newest and count two dropped; `Publish` returns while the thread is held; resize and the next
frame's size; minimised; `Stop` with a frame pending, with the thread held in a composition, in an acquire that never
completes and in a present wait; the log's rows against the frames shown and against a `DisplayTimeSource` registered by
the test; vsync off against the modes the surface really offers; `CurrentLayout` against `screen_layout::Compute`;
out-of-date and suboptimal from the acquire and from the present; offscreen dumps with the pixels of the screens' rectangles;
the owner-rule check forced onto another thread; the UI layer's thread; concurrent publishers while another thread
toggles vsync, size and redraws (the ThreadSanitizer job runs it); `Stop` and `SetUiLayer(nullptr)` called by the layer on the present thread, alone and while another thread is inside `Stop` joining (each under a watchdog that ends the process); the rows' `dropped_before` adding up to `Stats::dropped` when taken frames are discarded; the queue lock held between the signalling submission and the end of the present (`TestHooks::queue_locked`); and a second thread submitting small recorders to the backend while 120 frames are presented. On lavapipe (Mesa 25.2.8) the headless surface has
`currentExtent` 0xFFFFFFFF, limits 1x1 to 16384x16384, 4 images at least and no maximum, the formats R8G8B8A8_UNORM and
B8G8R8A8_UNORM, and the modes MAILBOX and FIFO (no IMMEDIATE); it has no `VK_KHR_present_wait`, so `PresentWaitMeasuresDisplay()`
is false there: the wait's loop is tested with `TestHooks::pretend_present_wait` (the hook supplies the wait's result) and the
case on the device's own wait skips with a message, as does anything that needs `VK_GOOGLE_display_timing` (the hook
`past_presentation_times` stands in for it).
Under the validation layer (`python3 tools/native/vk_layer_run.py --build build/native -R '^swapchain_test$'`) there is
no message. The `--present-dump` readback is followed by a `vkQueueWaitIdle`, because the layer 1.3.275 does not count the
backend's host wait on a timeline semaphore as ordering (the same limitation `present_test` works around with
`vkDeviceWaitIdle`); without it the layer reports `SYNC-HAZARD-READ-AFTER-WRITE` between the readback of one dump and the
staging upload of the next frame.

## Parts the other packages add

N2b-23 (frame pacing) adds its section here.
