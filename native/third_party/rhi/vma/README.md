# VulkanMemoryAllocator (vendored, N2-25)

[VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (VMA) is the GPU memory
allocator under plume's Vulkan backend ("Copyright (c) 2017-2025 Advanced Micro Devices, Inc.", MIT; the licence is
`pristine/LICENSE.txt`). This folder holds its one header as a pristine copy plus numbered patches of ours, the layout
of [`../plume/`](../plume/README.md); the manual is [`docs/native/rhi.md`](../../../../docs/native/rhi.md).
`VMA_IMPLEMENTATION` is defined in one place, `plume/patched/plume_vulkan.cpp`.

Pinned commit: `29b35ea4232688c0f42cdff0c10848290760a417`
(the commit plume's `contrib/VulkanMemoryAllocator` submodule names at plume's pinned commit; no tag; the header says
"Version 3.2.1").

## Layout

| Folder | What |
|---|---|
| `pristine/` | `vk_mem_alloc.h` (from upstream's `include/`) and `LICENSE.txt` at the pinned commit, unmodified. |
| `patches/` | `NN-<name>.diff`, unified diffs on the pristine paths, applied in number order. |
| `patched/` | `pristine/` with every patch applied: what the build compiles (the include directory of `mh4n_rhi_plume`). Written by `tools/native/rhi_patches.py apply` and committed. Unlike plume's, it holds no `mh4n_patch_list.h`. |

## Pristine files and their git blob ids

The blob id is what `git rev-parse 29b35ea:<path>` gives in VMA's repository (`include/vk_mem_alloc.h`,
`LICENSE.txt`); `tools/native/rhi_patches.py blobs` recomputes it for each file here. A row that does not match means
the pristine copy was edited: undo the edit and write a patch instead.

| File | Blob id |
|---|---|
| `vk_mem_alloc.h` | `6f71d5b4604d8e58aba4f340552ab092be190606` |
| `LICENSE.txt` | `a080d9bf71fcf40b4714c6fab0130f6a9d973d7f` |

## The patches

The changed-line figure is `git diff --numstat` (added plus removed).

| No. | File | What | Lines |
|---|---|---|---:|
| 01 | `01-upstream-4f68795-mapped-data-sync.diff` | upstream commit `4f687952b8c7aa250dfc25f78e269b9834872458` ("Fixed synchronization issue of m_pMappedData", 2026-05-22, MIT, first released in VMA 3.4.0; the fix of upstream issue #525): `VMA_ATOMIC_BOOL`, an atomic `m_IsMapped` per memory block that mirrors `m_pMappedData != VMA_NULL` (set in `Map`, cleared in `Unmap`, `PostFree` and `Destroy`), and `IsMapped()`, which `VmaBlockVector::AllocatePage` now reads instead of the pointer it read without the block's mutex (a data race with `vmaMapMemory` and `vmaUnmapMemory` of another allocation in the same block; `native-tsan` saw it, `docs/native/rhi.md`). Every hunk of the commit, re-based onto the pinned file; all keep their upstream text except `Map`'s, whose added line is indented one level deeper, because at the pinned commit the `vkMapMemory` branch of `Map` sits in an `else` | 22 |

## Adding a patch

As for plume ([`../plume/README.md`](../plume/README.md#adding-a-patch)): a new numbered file after the last one, never
an edit of an earlier file or of `patched/`; list it in the table above; `python3 tools/native/rhi_patches.py apply`,
then `check` and `blobs`; build and run the RHI tests. An upstream fix is cherry-picked by us as a new numbered patch,
not by changing the pinned commit; text before the patch's first `diff --git` line (the upstream commit, the issue and
the licence) is ignored by the applier and by `git apply`.
