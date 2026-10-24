# The RHI's third-party code (N2-25)

What the renderer's Vulkan backend stands on (decision D4). Nothing outside `native/third_party/rhi/` defines
`VOLK_IMPLEMENTATION` or `VMA_IMPLEMENTATION`: `plume/patched/plume_vulkan.cpp` does (the one translation unit of each).

| Folder | What | Pinned at | Licence |
|---|---|---|---|
| `plume/` | plume's Vulkan backend, pristine plus numbered patches ([`plume/README.md`](plume/README.md)) | `0ce184b19270e49236afbb74052a6b267494b86b` (github.com/renderbag/plume) | MIT |
| `volk/` | volk, the Vulkan meta-loader: `volk.h` and `volk.c` (`volk.h` includes `volk.c` under `VOLK_IMPLEMENTATION`) | `be3dbd49bf77052665e96b6c7484af855e7e5f67` (github.com/zeux/volk; no tags at that commit; the commit plume's `contrib/volk` submodule names at the pinned plume commit) | MIT |
| `vma/` | VulkanMemoryAllocator: `vk_mem_alloc.h` (from its `include/`) | `29b35ea4232688c0f42cdff0c10848290760a417` (github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator; the commit plume's `contrib/VulkanMemoryAllocator` names) | MIT |

The Vulkan headers are not here: the tree has one copy, `native/third_party/vulkan/` (N2-24's, with `vulkan.h` and
`vulkan_beta.h` added by N2-25 for plume and volk).

The blob ids are what `git rev-parse <commit>:<path>` gives in the upstream repository; `tools/native/rhi_patches.py blobs`
recomputes them (paths relative to this folder). The files are unmodified.

| File | Blob id |
|---|---|
| `volk/volk.h` | `b7515e17fa5ea13ede2ca52b6e2b5e57eeee1899` |
| `volk/volk.c` | `90b08e9685f02a57941250a5d63d58d9915cd774` |
| `volk/LICENSE.md` | `782051ab9dd928409984ba2a82f8dff9b7d9c11c` |
| `vma/vk_mem_alloc.h` | `6f71d5b4604d8e58aba4f340552ab092be190606` |
| `vma/LICENSE.txt` | `a080d9bf71fcf40b4714c6fab0130f6a9d973d7f` |
