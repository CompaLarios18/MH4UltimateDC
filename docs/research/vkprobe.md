# `mh4n_vkprobe`: which Vulkan features a machine offers

`mh4n_vkprobe` (N2-24) reports, for each Vulkan device, the features MH4U's rendering needs and the fallback for each one the device lacks. It is the tool the RHI spike (N2-01) measures with, built before the PICA census exists: the maintainer runs it on the PC and on the Mac, and the answers go to `rhi-spike.md`. It uses no game data and needs no Vulkan SDK.

## What it prints

For each device: name, type, driver, API version, then one line per feature, `yes`, `no (fallback: ...)` or `unknown`:

| Line | What it checks (the device must list the extension *and* report the bit) | Fallback when `no` |
|---|---|---|
| logic ops | `logicOp` | emulate the ops in the fragment shader (4 of 16 ops on MoltenVK) |
| fragment shader interlock, pixel / sample | `VK_EXT_fragment_shader_interlock`, `fragmentShader{Pixel,Sample}Interlock` | order overlapping draws with barriers |
| storage image writes from fragment shaders | `fragmentStoresAndAtomics` | draw into an attachment |
| geometry shaders | `geometryShader` | skip the quaternion fix-up, or a compute pass |
| fragment shader barycentrics | `VK_KHR_fragment_shader_barycentric` (or the NV one), `fragmentShaderBarycentric` | geometry shaders, else skip the fix-up |
| triangle fans | no `VK_KHR_portability_subset` means yes; with it, `triangleFans` | rewrite fans as triangle lists |
| 8-bit indices | `VK_EXT_index_type_uint8` (or KHR), `indexTypeUint8` | widen to 16 bits |
| custom border colours | `VK_EXT_custom_border_color`, `customBorderColors` | emulate in the shader |
| `shaderClipDistance` | the core bit | clip in the fragment shader |
| `VK_EXT_shader_stencil_export` | the extension | a second pass |
| `VK_EXT_rasterization_order_attachment_access` | the extension (or the ARM one), `rasterizationOrderColorAttachmentAccess` | interlock, else copy and barrier |
| depth-stencil `D24_UNORM_S8_UINT` | optimal tiling has the depth-stencil attachment bit (missing on AMD) | `D32_SFLOAT_S8_UINT` (its own line follows) |
| BC7 | `BC7_UNORM_BLOCK` sampled in optimal tiling, else `textureCompressionBC` | decode to RGBA8 at load |
| dynamic rendering | `dynamicRendering` | render passes and framebuffers |
| `VK_EXT_graphics_pipeline_library` | the extension, `graphicsPipelineLibrary` | a pipeline cache and worker threads |
| push descriptors | `VK_KHR_push_descriptor` (core in 1.4) | descriptor sets per draw |
| timeline semaphores | `timelineSemaphore` | binary semaphores and fences |
| extended dynamic state 1, 2, 3 | the extension and its bit; 1 and 2 are core in Vulkan 1.3; 3: the extension and at least one `extendedDynamicState3*` bit | bake state into pipelines |

The fallback column is what the spike will evaluate, not a decision. A line ends with `[why]` when the reason is not obvious (the extension that is missing, the bit that was not reported).

- **`unknown` is not `no`.** A device or file that does not report something (no extension list in a `vulkaninfo` file, a feature structure that was not queried, a format that was left out) gives `unknown`. A `yes` needs both the extension in the device's list and the bit set; a name the device did not report is never a `yes`.
- `extendedDynamicState3` is `yes` when some of its 31 bits are; the exact bits are in `vulkaninfo`, not in this table (N2-01 asks for the ones it needs).

## Running it

| Machine | Where the program is | Command |
|---|---|---|
| Windows PC | the CI artifact `mh4n-runner-windows` (a zip; `gh run download <run> -n mh4n-runner-windows`, or the run's page on GitHub): `mh4n_vkprobe.exe` beside its DLLs | `mh4n_vkprobe.exe --json > vkprobe-pc.json` |
| Mac (Apple silicon) | the CI artifact `mh4n-vkprobe-macos`: one file, `mh4n_vkprobe` | `./mh4n_vkprobe --json > vkprobe-mac.json` |
| Linux | `build/native/bin/mh4n_vkprobe` after the build | `mh4n_vkprobe --json > vkprobe-linux.json` |

Run `mh4n_vkprobe` without `--json` to read the table yourself; `--json` prints one JSON document per device (several devices: several documents in one file, which `--from-json` reads).

Artifacts exist for pushes to `native` and `wp/**` branches and for manual dispatches, not for pull requests, and for 30 days.

**The Mac file.** The zip of an artifact drops the executable bit, and a file downloaded with a browser is quarantined. In Terminal, in the folder with the file:

```
chmod +x mh4n_vkprobe
xattr -d com.apple.quarantine mh4n_vkprobe    # if macOS says it cannot verify the developer
./mh4n_vkprobe --json > vkprobe-mac.json
```

The program is an unsigned arm64 executable. On macOS 15 or later, if the quarantine attribute is not the problem, System Settings > Privacy & Security shows the blocked program with an "Open Anyway" button. It loads a Vulkan loader or MoltenVK when it runs: it tries `libvulkan.1.dylib` and `libvulkan.dylib` (the LunarG SDK, Homebrew's `vulkan-loader`), then `libMoltenVK.dylib`, in the library path and under `/opt/homebrew/lib` and `/usr/local/lib`. Without any of them it says so and exits 3: `brew install molten-vk` provides one, or point at a copy with `--loader /path/to/libMoltenVK.dylib`. (Untested on a Mac: the first run is the test.)

**Where it differs.** On a Mac it probes MoltenVK, which is the portability subset: expect `no` for triangle fans and probably for logic ops, geometry shaders and custom border colours, and `D24_UNORM_S8_UINT`. If the Mac has both the SDK's loader and MoltenVK, the loader picks the drivers; `--loader` forces one.

## What to send back

1. `vkprobe-<machine>.json` for each machine (the PC with its GPU and driver version in the message; the Mac with the chip and macOS version).
2. Where he has it, the output of the SDK's tool for the same device, as the file it writes: `vulkaninfo --json` saves `VP_VULKANINFO_<device>_<driver>.json` in the current folder (the first GPU; `--json=1` for the second) and prints nothing. `mh4n_vkprobe --from-json VP_VULKANINFO_....json` gives the same table from it, so a machine without our build can be evaluated, and the two outputs can be compared.
3. Any `unknown` line you can settle by hand.

Nothing in these files is game data: they list a GPU's capabilities and the machine's driver name.

## Until he does: reports for the same GPU

[vulkan.gpuinfo.org](https://vulkan.gpuinfo.org) (Sascha Willems' database of Vulkan device reports) has a report for most GPUs. Until the maintainer's files come, take the report for **the same GPU, the same OS and a recent driver** (the newest driver version listed for that OS, or the one the maintainer has installed), and read the features, extensions and formats pages off it by hand: a feature is a `yes` only when that report lists the extension and the bit. Quote the report's page and driver version next to every number taken from it. The site's own JSON export has a layout of its own, which `--from-json` does not read. A report for a different driver version can differ in exactly the extensions this tool looks at (`VK_EXT_fragment_shader_interlock`, `VK_EXT_graphics_pipeline_library`, the `extendedDynamicState3` bits, MoltenVK's portability subset), so say which version was used.

## `--from-json`

```
mh4n_vkprobe --from-json FILE [--json]
```

Reads `vulkaninfo --json` output, or this program's own `--json`, instead of probing. It needs no Vulkan: the reader and the report are always built. A truncated or invalid file is exit status 2 with the position of the error; a file that holds no device is exit status 2 too. Layout assumed (the Vulkan Profiles device format):

```
capabilities > device > properties   > VkPhysicalDeviceProperties (deviceName, apiVersion, deviceType, vendorID, deviceID)
                                     > VkPhysicalDeviceDriverProperties (driverName, driverInfo)
                                     features     > one object per structure: member name > true / false
                                     extensions   > extension name > spec version
                                     formats      > VK_FORMAT_... > VkFormatProperties > optimalTilingFeatures
```

Keys are read by name, a structure's nesting does not matter, and anything absent is `unknown`. The `vulkaninfo` of Vulkan SDK 1.3.275 (Linux, the Mesa lavapipe device) was read with this layout, and `--from-json` of its file gave the same table as the live probe on that device. The files of other `vulkaninfo` versions and operating systems (flag lists written as numbers rather than names, extensions written as a list) are handled but were not seen: the first Windows and Mac runs will show.

## Exit status

0 done; 2 a bad option, or a file that is missing, truncated or not JSON; 3 the live probe found no Vulkan loader or no device (the message says which; `--from-json` still works).

## How it works

`mh4n_vkprobe` has no Vulkan SDK dependency: it loads the loader at run time (`vulkan-1.dll`, `libvulkan.so.1`, the macOS names above) and takes the declarations from one vendored header (`native/third_party/vulkan`, Khronos Vulkan-Headers `v1.3.296`, built with `VK_NO_PROTOTYPES`). It makes an instance (asking for the portability enumeration when the loader lists it, which MoltenVK needs), chains only the feature structures a device's extension list and version allow, reads them for every physical device (software ones too) and destroys the instance. It creates no logical device, no surface and no window. The code is `native/src/vkprobe` (the reader, the judgement, the report and the live probe) and `native/tools/vkprobe` (`main`); `native/tests/vkprobe_test.cpp` tests all but a real device with synthetic files. CI runs it on lavapipe (`native` job) and builds it on Windows and macOS (`docs/native/CI.md`).
