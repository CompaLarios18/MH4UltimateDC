# Vulkan headers (N2-24, N2-25)

The tree's one copy of the Vulkan API declarations (`mh4n_vkprobe`, N2-24; the vendored plume, volk and
VulkanMemoryAllocator, N2-25), unmodified, from
[KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) at the tag
**`v1.3.296`** (`VK_HEADER_VERSION 296`):

- `include/vulkan/vulkan_core.h` and `include/vulkan/vk_platform.h`;
- `include/vulkan/vulkan.h` and `include/vulkan/vulkan_beta.h` (N2-25): volk and VulkanMemoryAllocator include
  `<vulkan/vulkan.h>`, and plume defines `VK_ENABLE_BETA_EXTENSIONS` for `VK_KHR_portability_subset`, which
  lives in `vulkan_beta.h`. No `VK_USE_PLATFORM_*` is defined anywhere in `native/`, so `vulkan.h` includes no
  platform header (`vulkan_win32.h`, `vulkan_xlib.h`, ...), and those are not vendored. Git blob ids at the tag
  (`git rev-parse v1.3.296:include/vulkan/<file>`): `vulkan.h` `ef94006bb319d65f46ef228af8a8804939352dfc`,
  `vulkan_beta.h` `df18b4042b177db542d3161d346f4ce9a21ae267`, `vulkan_core.h` `e6c16498af5373040e5ec3cb8af821dc6a3572be`;
- `include/vk_video/*.h`: the nine video-codec headers that `vulkan_core.h` includes
  unconditionally (the probe does not use them, but the header does not compile without them).

Licence: **Apache-2.0 OR MIT** (each file carries `SPDX-License-Identifier: Apache-2.0`; the
Vulkan-Headers `LICENSE.md`, kept here, says every file falls under one of the two).

Used with `VK_NO_PROTOTYPES` (volk defines it): nothing links against a Vulkan loader. The probe loads the loader
(`vulkan-1.dll`, `libvulkan.so.1`, `libvulkan.1.dylib`) at run time (`native/src/vkprobe/live.cpp`).
Updating: replace the files from the new tag, change this file and the row of
`native/THIRD_PARTY.md`. `VK_KHR_portability_subset` lives in `vulkan_beta.h`: `live.cpp` (N2-24) declares the
one structure it needs from the specification itself and does not define `VK_ENABLE_BETA_EXTENSIONS`, so
it does not include `vulkan_beta.h`. plume's own `contrib/Vulkan-Headers`
(v1.4.335) is not used: plume, volk and VulkanMemoryAllocator compile against v1.3.296.
