# Vulkan headers (N2-24)

The Vulkan API declarations `mh4n_vkprobe` needs, unmodified, from
[KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) at the tag
**`v1.3.296`** (`VK_HEADER_VERSION 296`):

- `include/vulkan/vulkan_core.h` and `include/vulkan/vk_platform.h`;
- `include/vk_video/*.h`: the nine video-codec headers that `vulkan_core.h` includes
  unconditionally (the probe does not use them, but the header does not compile without them).

Licence: **Apache-2.0 OR MIT** (each file carries `SPDX-License-Identifier: Apache-2.0`; the
Vulkan-Headers `LICENSE.md`, kept here, says every file falls under one of the two).

Used with `VK_NO_PROTOTYPES`: nothing links against a Vulkan loader. The probe loads the loader
(`vulkan-1.dll`, `libvulkan.so.1`, `libvulkan.1.dylib`) at run time (`native/src/vkprobe/live.cpp`).
Updating: replace the files from the new tag, change this file and the row of
`native/THIRD_PARTY.md`. `VK_KHR_portability_subset` lives in `vulkan_beta.h`, which is not vendored:
`live.cpp` declares the one structure it needs from the specification.
