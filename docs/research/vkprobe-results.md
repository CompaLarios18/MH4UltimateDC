# What `mh4n_vkprobe` found on the maintainer's machines

The probe is `mh4n_vkprobe` (N2-24; how to run it is in [`vkprobe.md`](vkprobe.md)). Its raw output for a machine is in
this folder as `vkprobe-<machine>.json`. Nothing in these files is game data: they list a GPU's capabilities and the
driver's name.

## Windows PC (the reference PC)

Run on 2026-10-21 by the orchestrator's session on the PC itself, with the probe from the Windows artifact of Native CI
run 39710163710 (`mh4n-runner-windows`), live (no `--from-json`), and checked against the SDK's `vulkaninfo --json` on
the same device. File: [`vkprobe-pc.json`](vkprobe-pc.json).

- Device: AMD Radeon RX 6700 XT (discrete, vendor 0x1002, device 0x73df), AMD proprietary driver 26.10.1, Vulkan API
  1.4.315 (`vulkaninfo` names the file driver 2.0.353). 198 device extensions.
- `yes` for: logic ops, fragment shader interlock (pixel and sample), storage image writes from fragment shaders,
  geometry shaders, fragment shader barycentrics, triangle fans (a full Vulkan device), 8-bit indices, custom border
  colours, `shaderClipDistance`, `VK_EXT_shader_stencil_export`, `D32_SFLOAT_S8_UINT`, BC7, dynamic rendering,
  `VK_EXT_graphics_pipeline_library`, push descriptors, timeline semaphores, extended dynamic state 1 and 2, extended
  dynamic state 3 (18 of its 31 bits).
- `no`: `VK_EXT_rasterization_order_attachment_access` (the fallback is fragment shader interlock, which is `yes`), and
  `D24_UNORM_S8_UINT` as a depth-stencil attachment (the fallback `D32_SFLOAT_S8_UINT` is available).
- **`VK_KHR_present_id` and `VK_KHR_present_wait` are not on this device with this driver**, and neither are
  `VK_GOOGLE_display_timing` or `VK_EXT_present_timing` (checked in `vulkaninfo`'s device extension list, which has
  `VK_KHR_swapchain` and `VK_KHR_swapchain_mutable_format` as its only swapchain extensions).

### What it means

- **D4 (the RHI) and the feature fallbacks:** this machine needs almost none of them. The RHI spike (N2-01) can use
  interlock, barycentrics, geometry shaders, BC7 and dynamic rendering here, and the fallbacks stay what the spike
  evaluates for the Mac (MoltenVK) and for other drivers.
- **D57 (how "p99 ≤ 8.3 ms" is measured):** the plan (N2b-23, N2b-24) measures the time a frame is shown from
  `VK_KHR_present_wait`, and calls the verdict "not measurable" without it. On this PC, with this driver, that
  would be the case. Ways out, each to be decided with D57 and none decided here: (a) a newer driver may add the
  extensions (the probe says what a driver has, run it again after an update); (b) on Windows, measure the interval
  between frames shown with the operating system's presentation statistics (DXGI frame statistics, or the ETW
  presentation events that PresentMon uses), outside Vulkan; (c) accept "not measurable" on this machine and measure
  the 120 fps verdict on another one. The probe ran on one driver version only.

## Mac (Apple silicon)

Not run yet: it needs the `mh4n-vkprobe-macos` artifact on the maintainer's Mac (steps in `vkprobe.md`). Expected, from
MoltenVK being the portability subset: `no` for triangle fans and probably logic ops, geometry shaders and custom
border colours.
