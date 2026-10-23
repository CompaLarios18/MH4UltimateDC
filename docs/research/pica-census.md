# PICA census

Written by `tools/native/pica_census.py` (format v1), N0-13, from MGPU captures replayed from their INIT record; nothing in this file is typed by hand. It holds counts, register values, Azahar's enumerator names and hashes only: no memory contents, shader instructions or texture bytes. Program and configuration hashes are 6-byte BLAKE2b digests of register or shader words. A count "N (M fr)" is N draws in M frames (a frame is counted once per capture file); "draws" are draw triggers: writes to trigger_draw (0x22E) or trigger_draw_indexed (0x22F), and one per immediate-mode batch.

Register positions, bit fields and enumerators are Azahar's (`src/video_core/pica/regs_*.h` at the pinned commit), checked against those headers by `tests/test_native_pica_census.py`.

## Captures

| label | file | bytes | records | FRAME records | PICA_REG entries | draw triggers |
|---|---|---|---|---|---|---|
| G1 | gpu-1500.mgpu | 144248896 | 109 | 3 | 13058 | 155 |
| G2 | gpu-2500.mgpu | 145694492 | 109 | 3 | 49745 | 794 |
| G3 | gpu-17400.mgpu | 174382220 | 357 | 10 | 311634 | 3676 |
| G4 | gpu-2400.mgpu | 173303352 | 357 | 10 | 242888 | 1386 |

| label | draws | array | indexed | immediate batches | immediate vertices | frames with draws | draws per frame | command buffer triggers | unrecorded register writes |
|---|---|---|---|---|---|---|---|---|---|
| G1 | 155 | 155 | 0 | 0 | 0 | 3 | 51.67 | 0 | 0 |
| G2 | 794 | 731 | 63 | 0 | 0 | 3 | 264.67 | 0 | 0 |
| G3 | 3676 | 2780 | 896 | 0 | 0 | 10 | 367.60 | 0 | 0 |
| G4 | 1386 | 815 | 571 | 0 | 0 | 10 | 138.60 | 0 | 0 |

## Answers at a glance

| question | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Logic ops | NO | NO | NO | NO |
| Triangle fans | NO | NO | NO | NO |
| Geometry shaders | YES (140) + topology Shader (140) | YES (668) + topology Shader (668) | YES (2560) + topology Shader (2560) | YES (541) + topology Shader (541) |
| Shadow passes | NO | NO | YES (50) | YES (20) |
| Gas | NO | NO | NO | YES (20) |
| Procedural textures | YES (140) | YES (668) | YES (2560) | YES (425) |
| Depth formats | YES (6) | YES (69) | YES (866) | YES (581) |
| Lighting and LUTs | NO | YES (297) | YES (1316) | YES (745) |
| Fog | NO | NO | YES (826) | YES (390) |
| Min/max blending | NO | NO | NO | NO |
| 8-bit indices | NO | NO | NO | NO |
| Vertex strides that are not multiples of 4 | NO | NO | NO | NO |
| Custom border colours | NO | NO | YES (90) | NO |
| Cube textures | NO | NO | NO | NO |
| Projection textures | NO | NO | NO | NO |
| Shadow textures | NO | NO | YES (90) | NO |

Notes on counting: the unique-TEV count is an upper bound, because Azahar's Dot3_RGBA masking of unused stage words (pica_fs_config.cpp) is not applied; the VS program hash ignores writes at offsets 512 and above, which Azahar ignores too (pica_core.cpp), but covers the swizzle and code words uploaded before the capture as INIT recorded them.

YES (n) is n draws; NO means no draw of that label had the feature; `n/a` means the label has no draws.

## 1. Logic ops

**Question** (docs/research/metal.md): Does MH4U use logic ops without blending, and which? (Metal: no logic op in the default MoltenVK build; Azahar's SPIR-V generator draws only Clear, Set, Copy and NoOp correctly.)

**Registers.** framebuffer.output_merger: alphablend_enable (0x100 bit 8) clear means logic-op blending; logic_op is 0x102 bits 0-3 (LogicOp, Copy = 3 is the plain write); colour writes on = allow_color_write (0x113 bits 0-3) non-zero and a channel enabled in 0x107 bits 8-11; effective unless fragment operation mode (0x100 bits 0-1) is Shadow (Azahar skips the op only for shadow rendering and applies it in mode Gas), and only with a colour buffer address (0x11D) non-zero.

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | NO | 0 | 0.00% | 0 | 0.00% |
| G2 | NO | 0 | 0.00% | 0 | 0.00% |
| G3 | NO | 0 | 0.00% | 0 | 0.00% |
| G4 | NO | 0 | 0.00% | 0 | 0.00% |

Effective logic ops by op (draws, frames):

| op | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| (none) | 0 | 0 | 0 | 0 |

Effective ops by class (Azahar's SPIR-V generator emulates Clear, Set, Copy and NoOp; the rest draw as Copy):

| class | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| (none) | 0 | 0 | 0 | 0 |

Output merger state of all draws (blend, logic op, or colour writes off):

| output merger | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| blend | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

## 2. Triangle fans

**Question** (docs/research/metal.md): Does MH4U draw triangle fans? (Metal has no fan topology; Azahar falls back to CPU vertex shading for those draws.)

**Registers.** pipeline.triangle_topology (0x25E bits 8-9) = Fan (2); counted at each draw trigger.

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | NO | 0 | 0.00% | 0 | 0.00% |
| G2 | NO | 0 | 0.00% | 0 | 0.00% |
| G3 | NO | 0 | 0.00% | 0 | 0.00% |
| G4 | NO | 0 | 0.00% | 0 | 0.00% |

| topology | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Shader | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |
| Strip | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 845 (10 fr) |

## 3. Geometry shaders

**Question** (docs/research/metal.md): Does MH4U use PICA geometry shaders? (They are never accelerated; Metal has none.)

**Registers.** pipeline.use_gs (0x229 bits 0-1) = Yes (2): the draw runs a geometry shader (the first thing DrawArrays checks before it refuses to accelerate a draw); its mode is pipeline.gs_config.mode (0x252 bits 0-7: Point, VariablePrimitive, FixedPrimitive). Draws with triangle_topology = Shader (3) (0x25E bits 8-9, programmable primitive assembly) are listed apart in the same row.

| label | answer | draws | of all draws | frames | of frames with draws | draws with topology Shader (3), apart |
|---|---|---|---|---|---|---|
| G1 | YES | 140 | 90.32% | 3 | 100.00% | 140 |
| G2 | YES | 668 | 84.13% | 3 | 100.00% | 668 |
| G3 | YES | 2560 | 69.64% | 10 | 100.00% | 2560 |
| G4 | YES | 541 | 39.03% | 10 | 100.00% | 541 |

| geometry shader in use | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Yes | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |
| No | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 845 (10 fr) |

| gs mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Point | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |

| gs program hash | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| b6f7179130d3 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 410 (10 fr) |
| 5f786f7c5175 | 0 | 0 | 0 | 102 (10 fr) |
| 6fbc29258724 | 0 | 0 | 0 | 15 (10 fr) |
| 683396221444 | 0 | 0 | 0 | 14 (10 fr) |

| topology | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Shader | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |
| Strip | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 845 (10 fr) |

## 4. Shadow passes

**Question** (docs/research/metal.md): Does MH4U render shadow maps with the PICA shadow mode, and sample shadow textures? (Needs fragment-shader interlock or raster order groups on Metal.)

**Registers.** Shadow pass: framebuffer.output_merger.fragment_operation_mode (0x100 bits 0-1) = Shadow (3). Shadow texture: texturing.texture0.type (0x83 bits 28-30) = Shadow2D (2) or ShadowCube (4) on an enabled unit 0 (main_config 0x80 bit 0); lighting shadow (0x1C3 bit 0) is listed apart.

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | NO | 0 | 0.00% | 0 | 0.00% |
| G2 | NO | 0 | 0.00% | 0 | 0.00% |
| G3 | YES | 50 | 1.36% | 10 | 100.00% |
| G4 | YES | 20 | 1.44% | 10 | 100.00% |

| fragment operation mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Default | 155 (3 fr) | 794 (3 fr) | 3626 (10 fr) | 1356 (10 fr) |
| Shadow | 0 | 0 | 50 (10 fr) | 20 (10 fr) |
| Gas | 0 | 0 | 0 | 10 (10 fr) |

| label | shadow textures (unit 0) |
|---|---|
| G1 | NO |
| G2 | NO |
| G3 | YES (90) |
| G4 | NO |

Lighting shadow (0x1C3 bit 0) is counted over draws with lighting on only:

| lighting shadow enable | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 0 | 234 (3 fr) | 490 (10 fr) | 595 (10 fr) |
| on | 0 | 63 (3 fr) | 826 (10 fr) | 150 (10 fr) |

## 5. Gas

**Question** (docs/research/metal.md): Does MH4U use gas? (No Azahar renderer draws it.)

**Registers.** texturing.fog_mode (0x0E0 bits 0-2) = Gas (7), or fragment operation mode (0x100 bits 0-1) = Gas (1).

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | NO | 0 | 0.00% | 0 | 0.00% |
| G2 | NO | 0 | 0.00% | 0 | 0.00% |
| G3 | NO | 0 | 0.00% | 0 | 0.00% |
| G4 | YES | 20 | 1.44% | 10 | 100.00% |

| fog mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| None | 155 (3 fr) | 794 (3 fr) | 2850 (10 fr) | 986 (10 fr) |
| Fog | 0 | 0 | 826 (10 fr) | 390 (10 fr) |
| Gas | 0 | 0 | 0 | 10 (10 fr) |

| fragment operation mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Default | 155 (3 fr) | 794 (3 fr) | 3626 (10 fr) | 1356 (10 fr) |
| Shadow | 0 | 0 | 50 (10 fr) | 20 (10 fr) |
| Gas | 0 | 0 | 0 | 10 (10 fr) |

## 6. Procedural textures

**Question** (docs/research/metal.md): Does MH4U use procedural textures?

**Registers.** texturing.main_config.texture3_enable (0x080 bit 10), the procedural texture unit.

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | YES | 140 | 90.32% | 3 | 100.00% |
| G2 | YES | 668 | 84.13% | 3 | 100.00% |
| G3 | YES | 2560 | 69.64% | 10 | 100.00% |
| G4 | YES | 425 | 30.66% | 10 | 100.00% |

| procedural texture | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| on | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) |
| off | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 961 (10 fr) |

## 7. Depth formats

**Question** (docs/research/metal.md): Does MH4U use 24-bit depth (D24, D24S8)? (Apple GPUs have none; Azahar falls back to D32F.)

**Registers.** framebuffer.framebuffer.depth_format (0x116 bits 0-1: D16 = 0, D24 = 2, D24S8 = 3), counted when the depth buffer is used by Azahar's rule (using_depth_fb): not shadow rendering, depth_buffer_address (0x11C) non-zero, and depth write (0x107 bit 12 with 0x115 non-zero), depth test (0x107 bit 0), or D24S8 with stencil test (0x105 bit 0).

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | YES | 6 | 3.87% | 3 | 100.00% |
| G2 | YES | 69 | 8.69% | 3 | 100.00% |
| G3 | YES | 866 | 23.56% | 10 | 100.00% |
| G4 | YES | 581 | 41.92% | 10 | 100.00% |

| depth format (all draws) | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| D24S8 | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

Depth "in use" follows Azahar's own `using_depth_fb` (vk_rasterizer.cpp, gl_rasterizer.cpp): not shadow rendering, depth buffer address (0x11C) non-zero, and (depth write allowed with 0x115 non-zero, or depth test enabled, or depth format D24S8 with stencil test enabled). This is the rule that decides whether a depth surface is created, so it is the one that matters for the 24-bit question.

| depth in use | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| no | 149 (3 fr) | 725 (3 fr) | 2810 (10 fr) | 805 (10 fr) |
| yes | 6 (3 fr) | 69 (3 fr) | 866 (10 fr) | 581 (10 fr) |

## 8. Lighting and LUTs

**Question** (docs/research/metal.md): Which draws use fragment lighting, how many lights, which lookup tables? (The lighting fix-up and the bandwidth cost are the likely Metal costs.)

**Registers.** lighting.disable (0x1C6 bit 0) clear; lights = max_light_index + 1 (0x1C2 bits 0-2); configuration 0x1C3 bits 4-7; LUT disables 0x1C4 bits 16-22 (a LUT is listed as enabled only when its disable bit is clear and the lighting configuration uses that sampler, Azahar's IsLightingSamplerSupported in regs_lighting.h, so the lists below can hold fewer LUTs than the clear bits of 0x1C4 do); per-light spot and distance attenuation disables 0x1C4 bits 8-15 and 24-31; light slots 0x1D9. The lights, configuration, bump mode, LUT and shadow-enable tables count lit draws only.

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | NO | 0 | 0.00% | 0 | 0.00% |
| G2 | YES | 297 | 37.41% | 3 | 100.00% |
| G3 | YES | 1316 | 35.80% | 10 | 100.00% |
| G4 | YES | 745 | 53.75% | 10 | 100.00% |

| lighting | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 155 (3 fr) | 497 (3 fr) | 2360 (10 fr) | 641 (10 fr) |
| on | 0 | 297 (3 fr) | 1316 (10 fr) | 745 (10 fr) |

| lights | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 1 | 0 | 297 (3 fr) | 996 (10 fr) | 310 (10 fr) |
| 3 | 0 | 0 | 0 | 435 (10 fr) |
| 2 | 0 | 0 | 320 (10 fr) | 0 |

| LUT enabled | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| RR | 0 | 63 (3 fr) | 826 (10 fr) | 150 (10 fr) |
| FR | 0 | 63 (3 fr) | 826 (10 fr) | 110 (10 fr) |
| DA | 0 | 0 | 0 | 435 (10 fr) |
| D0 | 0 | 0 | 0 | 40 (10 fr) |

| LUT set | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| none | 0 | 234 (3 fr) | 490 (10 fr) | 220 (10 fr) |
| FR+RR | 0 | 63 (3 fr) | 826 (10 fr) | 50 (10 fr) |
| DA | 0 | 0 | 0 | 375 (10 fr) |
| DA+FR+RR | 0 | 0 | 0 | 60 (10 fr) |
| D0+RR | 0 | 0 | 0 | 40 (10 fr) |

## 9. Fog

**Question** (docs/research/metal.md): Does MH4U use fog? (Fog needs the fog LUT.)

**Registers.** texturing.fog_mode (0x0E0 bits 0-2) = Fog (5).

| label | answer | draws | of all draws | frames | of frames with draws |
|---|---|---|---|---|---|
| G1 | NO | 0 | 0.00% | 0 | 0.00% |
| G2 | NO | 0 | 0.00% | 0 | 0.00% |
| G3 | YES | 826 | 22.47% | 10 | 100.00% |
| G4 | YES | 390 | 28.14% | 10 | 100.00% |

| fog mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| None | 155 (3 fr) | 794 (3 fr) | 2850 (10 fr) | 986 (10 fr) |
| Fog | 0 | 0 | 826 (10 fr) | 390 (10 fr) |
| Gas | 0 | 0 | 0 | 10 (10 fr) |

## Other items from the census list of docs/research/metal.md

| item | what it counts | G1 | G2 | G3 | G4 |
|---|---|---|---|---|---|
| Min/max blending | Blend equation Min or Max with blending on (emulated on Metal). | NO | NO | NO | NO |
| 8-bit indices | Indexed draws with 8-bit indices (pipeline.index_array.format, 0x227 bit 31 clear). | NO | NO | NO | NO |
| Vertex strides that are not multiples of 4 | An active attribute loader whose byte_count (loader word 2 bits 16-23) is not a multiple of 4. | NO | NO | NO | NO |
| Custom border colours | An enabled texture unit with ClampToBorder or ClampToBorder2 wrap (Metal exposes no custom border colours). | NO | NO | YES (90) | NO |
| Cube textures | Unit 0 enabled with type TextureCube. | NO | NO | NO | NO |
| Projection textures | Unit 0 enabled with type Projection2D. | NO | NO | NO | NO |
| Shadow textures | Unit 0 enabled with type Shadow2D or ShadowCube. | NO | NO | YES (90) | NO |

## Tables per feature

Cells are "draws (frames)".

### Draw triggers

| draw_kind | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| array | 155 (3 fr) | 731 (3 fr) | 2780 (10 fr) | 815 (10 fr) |
| indexed | 0 | 63 (3 fr) | 896 (10 fr) | 571 (10 fr) |

| topology | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Shader | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |
| Strip | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 845 (10 fr) |

| geometry_shader | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Yes | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |
| No | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 845 (10 fr) |

| gs_mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Point | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 541 (10 fr) |

| index_size | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| none | 155 (3 fr) | 731 (3 fr) | 2780 (10 fr) | 815 (10 fr) |
| 16-bit | 0 | 63 (3 fr) | 896 (10 fr) | 571 (10 fr) |

### Vertex attributes

| attr_count | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 8 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) |
| 6 | 0 | 0 | 506 (10 fr) | 360 (10 fr) |
| 1 | 15 (3 fr) | 63 (3 fr) | 210 (10 fr) | 380 (10 fr) |
| 12 | 0 | 63 (3 fr) | 370 (10 fr) | 70 (10 fr) |
| 5 | 0 | 0 | 20 (10 fr) | 141 (10 fr) |
| 2 | 0 | 0 | 10 (10 fr) | 10 (10 fr) |

| attr_format | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| UBYTEx4 | 140 (3 fr) | 731 (3 fr) | 3466 (10 fr) | 956 (10 fr) |
| FLOATx4 | 155 (3 fr) | 695 (3 fr) | 2660 (10 fr) | 625 (10 fr) |
| FLOATx3 | 0 | 63 (3 fr) | 896 (10 fr) | 571 (10 fr) |
| BYTEx4 | 0 | 63 (3 fr) | 896 (10 fr) | 469 (10 fr) |
| FLOATx2 | 0 | 63 (3 fr) | 886 (10 fr) | 440 (10 fr) |
| fixed | 0 | 63 (3 fr) | 876 (10 fr) | 430 (10 fr) |
| SHORTx4 | 0 | 36 (3 fr) | 130 (10 fr) | 321 (10 fr) |
| SHORTx2 | 0 | 0 | 0 | 156 (10 fr) |

| loader_count | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 1 | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

| loader_stride | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 80 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) |
| 28 | 0 | 45 (3 fr) | 710 (10 fr) | 350 (10 fr) |
| 16 | 15 (3 fr) | 27 (3 fr) | 100 (10 fr) | 200 (10 fr) |
| 8 | 0 | 36 (3 fr) | 110 (10 fr) | 180 (10 fr) |
| 36 | 0 | 18 (3 fr) | 150 (10 fr) | 70 (10 fr) |
| 32 | 0 | 0 | 36 (10 fr) | 141 (10 fr) |
| 12 | 0 | 0 | 10 (10 fr) | 10 (10 fr) |
| 24 | 0 | 0 | 0 | 10 (10 fr) |

### Fog, procedural texture, fragment operation mode

| fog_mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| None | 155 (3 fr) | 794 (3 fr) | 2850 (10 fr) | 986 (10 fr) |
| Fog | 0 | 0 | 826 (10 fr) | 390 (10 fr) |
| Gas | 0 | 0 | 0 | 10 (10 fr) |

| procedural_texture | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| on | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) |
| off | 15 (3 fr) | 126 (3 fr) | 1116 (10 fr) | 961 (10 fr) |

| fragment_operation_mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Default | 155 (3 fr) | 794 (3 fr) | 3626 (10 fr) | 1356 (10 fr) |
| Shadow | 0 | 0 | 50 (10 fr) | 20 (10 fr) |
| Gas | 0 | 0 | 0 | 10 (10 fr) |

### Lighting

| lighting | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 155 (3 fr) | 497 (3 fr) | 2360 (10 fr) | 641 (10 fr) |
| on | 0 | 297 (3 fr) | 1316 (10 fr) | 745 (10 fr) |

| lighting_reg_0x08F | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| clear | 155 (3 fr) | 497 (3 fr) | 2360 (10 fr) | 641 (10 fr) |
| set | 0 | 297 (3 fr) | 1316 (10 fr) | 745 (10 fr) |

| light_count | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 1 | 0 | 297 (3 fr) | 996 (10 fr) | 310 (10 fr) |
| 3 | 0 | 0 | 0 | 435 (10 fr) |
| 2 | 0 | 0 | 320 (10 fr) | 0 |

| light_config | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Config0 | 0 | 234 (3 fr) | 490 (10 fr) | 635 (10 fr) |
| Config1 | 0 | 63 (3 fr) | 826 (10 fr) | 110 (10 fr) |

| light_bump_mode | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| None | 0 | 297 (3 fr) | 1266 (10 fr) | 745 (10 fr) |
| NormalMap | 0 | 0 | 50 (10 fr) | 0 |

| light_lut | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| RR | 0 | 63 (3 fr) | 826 (10 fr) | 150 (10 fr) |
| FR | 0 | 63 (3 fr) | 826 (10 fr) | 110 (10 fr) |
| DA | 0 | 0 | 0 | 435 (10 fr) |
| D0 | 0 | 0 | 0 | 40 (10 fr) |

| light_lut_set | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| none | 0 | 234 (3 fr) | 490 (10 fr) | 220 (10 fr) |
| FR+RR | 0 | 63 (3 fr) | 826 (10 fr) | 50 (10 fr) |
| DA | 0 | 0 | 0 | 375 (10 fr) |
| DA+FR+RR | 0 | 0 | 0 | 60 (10 fr) |
| D0+RR | 0 | 0 | 0 | 40 (10 fr) |

| light_shadow_enable | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 0 | 234 (3 fr) | 490 (10 fr) | 595 (10 fr) |
| on | 0 | 63 (3 fr) | 826 (10 fr) | 150 (10 fr) |

### Output merger

| output_merger | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| blend | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

| blend_equation | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Add/Add | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

| alpha_test | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 155 (3 fr) | 791 (3 fr) | 3536 (10 fr) | 1075 (10 fr) |
| GreaterThan | 0 | 3 (3 fr) | 140 (10 fr) | 311 (10 fr) |

| stencil_test | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 149 (3 fr) | 788 (3 fr) | 3656 (10 fr) | 1366 (10 fr) |
| Always | 6 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) |

| stencil_ops | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Keep/Keep/Replace | 6 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) |

| depth_test | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 149 (3 fr) | 725 (3 fr) | 2810 (10 fr) | 805 (10 fr) |
| LessThanOrEqual | 0 | 63 (3 fr) | 846 (10 fr) | 561 (10 fr) |
| Always | 6 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) |

| depth_write | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| off | 149 (3 fr) | 725 (3 fr) | 2880 (10 fr) | 976 (10 fr) |
| on | 6 (3 fr) | 69 (3 fr) | 796 (10 fr) | 410 (10 fr) |

| depth_in_use | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| no | 149 (3 fr) | 725 (3 fr) | 2810 (10 fr) | 805 (10 fr) |
| yes | 6 (3 fr) | 69 (3 fr) | 866 (10 fr) | 581 (10 fr) |

### Textures

| texture_units | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 2 | 140 (3 fr) | 668 (3 fr) | 2630 (10 fr) | 485 (10 fr) |
| 1 | 9 (3 fr) | 93 (3 fr) | 816 (10 fr) | 631 (10 fr) |
| 3 | 0 | 21 (3 fr) | 120 (10 fr) | 130 (10 fr) |
| 0 | 6 (3 fr) | 12 (3 fr) | 110 (10 fr) | 140 (10 fr) |

| texture_type | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| Texture2D | 149 (3 fr) | 782 (3 fr) | 3566 (10 fr) | 1246 (10 fr) |
| Shadow2D | 0 | 0 | 90 (10 fr) | 0 |

| texture_type_unit | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| unit1:Texture2D | 140 (3 fr) | 752 (3 fr) | 3456 (10 fr) | 985 (10 fr) |
| unit0:Texture2D | 149 (3 fr) | 719 (3 fr) | 2740 (10 fr) | 826 (10 fr) |
| unit2:Texture2D | 0 | 21 (3 fr) | 150 (10 fr) | 180 (10 fr) |
| unit0:Shadow2D | 0 | 0 | 90 (10 fr) | 0 |

| texture_format | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| I8 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) |
| ETC1A4 | 32 (3 fr) | 135 (3 fr) | 1130 (10 fr) | 626 (10 fr) |
| A4 | 105 (3 fr) | 527 (3 fr) | 1230 (10 fr) | 0 |
| ETC1 | 3 (3 fr) | 30 (3 fr) | 526 (10 fr) | 215 (10 fr) |
| RGBA8 | 9 (3 fr) | 51 (3 fr) | 250 (10 fr) | 270 (10 fr) |
| IA4 | 0 | 0 | 410 (10 fr) | 15 (10 fr) |
| IA8 | 0 | 0 | 20 (10 fr) | 80 (10 fr) |
| RGBA4 | 0 | 36 (3 fr) | 10 (10 fr) | 30 (10 fr) |
| RGB5A1 | 0 | 0 | 50 (10 fr) | 0 |
| I4 | 0 | 0 | 10 (10 fr) | 10 (10 fr) |

| texture_format_unit | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| unit1:I8 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) |
| unit0:A4 | 105 (3 fr) | 527 (3 fr) | 1230 (10 fr) | 0 |
| unit0:ETC1A4 | 32 (3 fr) | 96 (3 fr) | 790 (10 fr) | 406 (10 fr) |
| unit1:ETC1 | 0 | 24 (3 fr) | 486 (10 fr) | 190 (10 fr) |
| unit1:ETC1A4 | 0 | 39 (3 fr) | 340 (10 fr) | 220 (10 fr) |
| unit0:RGBA8 | 9 (3 fr) | 51 (3 fr) | 250 (10 fr) | 260 (10 fr) |
| unit0:IA4 | 0 | 0 | 410 (10 fr) | 15 (10 fr) |
| unit1:RGBA8 | 0 | 21 (3 fr) | 70 (10 fr) | 150 (10 fr) |
| unit2:RGBA8 | 0 | 21 (3 fr) | 70 (10 fr) | 140 (10 fr) |
| unit0:IA8 | 0 | 0 | 20 (10 fr) | 80 (10 fr) |
| unit0:RGBA4 | 0 | 36 (3 fr) | 10 (10 fr) | 30 (10 fr) |
| unit2:ETC1 | 0 | 0 | 30 (10 fr) | 40 (10 fr) |
| unit0:I8 | 0 | 3 (3 fr) | 50 (10 fr) | 0 |
| unit0:RGB5A1 | 0 | 0 | 50 (10 fr) | 0 |
| unit2:ETC1A4 | 0 | 0 | 50 (10 fr) | 0 |
| unit0:ETC1 | 3 (3 fr) | 6 (3 fr) | 10 (10 fr) | 25 (10 fr) |
| unit0:I4 | 0 | 0 | 10 (10 fr) | 10 (10 fr) |

| texture_size | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 8x8 | 143 (3 fr) | 671 (3 fr) | 2570 (10 fr) | 435 (10 fr) |
| 1024x1024 | 105 (3 fr) | 527 (3 fr) | 1230 (10 fr) | 0 |
| 256x256 | 0 | 63 (3 fr) | 320 (10 fr) | 456 (10 fr) |
| 128x128 | 0 | 18 (3 fr) | 416 (10 fr) | 350 (10 fr) |
| 512x512 | 29 (3 fr) | 6 (3 fr) | 530 (10 fr) | 0 |
| 256x512 | 6 (3 fr) | 15 (3 fr) | 450 (10 fr) | 75 (10 fr) |
| 64x64 | 0 | 45 (3 fr) | 310 (10 fr) | 20 (10 fr) |
| 64x128 | 0 | 15 (3 fr) | 80 (10 fr) | 100 (10 fr) |
| 32x64 | 0 | 12 (3 fr) | 70 (10 fr) | 60 (10 fr) |
| 512x256 | 3 (3 fr) | 3 (3 fr) | 50 (10 fr) | 80 (10 fr) |
| 128x256 | 0 | 3 (3 fr) | 60 (10 fr) | 30 (10 fr) |
| 256x128 | 3 (3 fr) | 63 (3 fr) | 20 (10 fr) | 0 |
| 128x64 | 0 | 3 (3 fr) | 30 (10 fr) | 45 (10 fr) |
| 16x32 | 0 | 6 (3 fr) | 20 (10 fr) | 40 (10 fr) |
| 32x32 | 0 | 0 | 10 (10 fr) | 10 (10 fr) |
| 64x256 | 0 | 0 | 20 (10 fr) | 0 |
| 64x32 | 0 | 0 | 20 (10 fr) | 0 |

| border_colour | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| 0xffffffff | 0 | 0 | 90 (10 fr) | 0 |

### Render targets

| color_format | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| RGBA8 | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

| depth_format | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| D24S8 | 155 (3 fr) | 794 (3 fr) | 3676 (10 fr) | 1386 (10 fr) |

### Unique configurations

| group | G1 | G2 | G3 | G4 |
|---|---|---|---|---|
| fragment_config | 2 | 4 | 6 | 7 |
| full_state | 7 | 34 | 63 | 52 |
| gs_program | 1 | 1 | 1 | 4 |
| lighting_config | 1 | 3 | 5 | 6 |
| render_target | 1 | 1 | 1 | 1 |
| tev_config | 5 | 15 | 22 | 23 |
| texture_config | 7 | 21 | 35 | 30 |
| vertex_layout | 2 | 5 | 9 | 11 |
| vs_program | 4 | 8 | 13 | 19 |

Most used `vs_program` of G1 (top 4 of 4):

| config | draws |
|---|---|
| b6f7179130d3 | 140 |
| 09dbda60c961 | 6 |
| fa65db28444d | 6 |
| 468d88a4eb91 | 3 |

Most used `tev_config` of G1 (top 5 of 5):

| config | draws |
|---|---|
| 1ccd61e0d377 | 105 |
| acdf984097fa | 35 |
| 260186962a09 | 6 |
| 268910d516be | 6 |
| b88d913aa146 | 3 |

Most used `vertex_layout` of G1 (top 2 of 2):

| config | draws |
|---|---|
| FLOATx4,FLOATx4,UBYTEx4,UBYTEx4,UBYTEx4,UBYTEx4,FLOATx4,FLOATx4;strides=80;topology=Shader | 140 |
| FLOATx4;strides=16;topology=Strip | 15 |

Most used `fragment_config` of G1 (top 2 of 2):

| config | draws |
|---|---|
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=off/- | 149 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=Always;depth=Always/w | 6 |

Most used `texture_config` of G1 (top 7 of 7):

| config | draws |
|---|---|
| u0:Texture2D:A4:1024x1024;u1:Texture2D:I8:8x8 | 105 |
| u0:Texture2D:ETC1A4:512x512;u1:Texture2D:I8:8x8 | 29 |
| none | 6 |
| u0:Texture2D:RGBA8:256x512 | 6 |
| u0:Texture2D:ETC1:512x256;u1:Texture2D:I8:8x8 | 3 |
| u0:Texture2D:ETC1A4:256x128;u1:Texture2D:I8:8x8 | 3 |
| u0:Texture2D:RGBA8:8x8 | 3 |

Most used `vs_program` of G2 (top 8 of 8):

| config | draws |
|---|---|
| b6f7179130d3 | 668 |
| dc48a79f1f2c | 63 |
| 86fcff4ec138 | 18 |
| 09dbda60c961 | 12 |
| 2b02f780cf03 | 12 |
| fa65db28444d | 12 |
| 7d852a7e977e | 6 |
| 468d88a4eb91 | 3 |

Most used `tev_config` of G2 (top 10 of 15):

| config | draws |
|---|---|
| 1ccd61e0d377 | 533 |
| acdf984097fa | 135 |
| a385dbf4d6a2 | 30 |
| 78341797b265 | 21 |
| 3c54693b3bee | 18 |
| 268910d516be | 12 |
| 6b1451b69ed5 | 12 |
| 29f5e373ffea | 9 |
| 260186962a09 | 6 |
| 28619951b43e | 3 |

Most used `vertex_layout` of G2 (top 5 of 5):

| config | draws |
|---|---|
| FLOATx4,FLOATx4,UBYTEx4,UBYTEx4,UBYTEx4,UBYTEx4,FLOATx4,FLOATx4;strides=80;topology=Shader | 668 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,fixed,fixed,fixed,fixed,fixed,fixed,fixed,fixed;strides=28;topology=Strip | 45 |
| SHORTx4;strides=8;topology=Strip | 36 |
| FLOATx4;strides=16;topology=Strip | 27 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,BYTEx4,UBYTEx4,fixed,fixed,fixed,fixed,fixed,fixed;strides=36;topology=Strip | 18 |

Most used `fragment_config` of G2 (top 4 of 4):

| config | draws |
|---|---|
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=off/- | 725 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=LessThanOrEqual/w | 60 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=Always;depth=Always/w | 6 |
| Default;blend:Add/Add;colour=on;alpha=GreaterThan;stencil=off;depth=LessThanOrEqual/w | 3 |

Most used `texture_config` of G2 (top 10 of 21):

| config | draws |
|---|---|
| u0:Texture2D:A4:1024x1024;u1:Texture2D:I8:8x8 | 527 |
| u0:Texture2D:ETC1A4:256x128;u1:Texture2D:I8:8x8 | 63 |
| u0:Texture2D:RGBA4:256x256;u1:Texture2D:I8:8x8 | 36 |
| u0:Texture2D:ETC1A4:256x256;u1:Texture2D:I8:8x8 | 27 |
| u1:Texture2D:ETC1:64x64 | 24 |
| u1:Texture2D:ETC1A4:64x64 | 21 |
| u1:Texture2D:ETC1A4:128x128 | 18 |
| u0:Texture2D:RGBA8:256x512 | 15 |
| none | 12 |
| u0:Texture2D:ETC1A4:512x512;u1:Texture2D:I8:8x8 | 6 |

Most used `vs_program` of G3 (top 10 of 13):

| config | draws |
|---|---|
| b6f7179130d3 | 2560 |
| ba6efb81007a | 506 |
| dc48a79f1f2c | 270 |
| 86fcff4ec138 | 60 |
| 34af07c3ab7f | 50 |
| 4e8f262dc7b1 | 50 |
| fa65db28444d | 50 |
| 09dbda60c961 | 40 |
| 2b02f780cf03 | 40 |
| 71fbc4051c91 | 20 |

Most used `tev_config` of G3 (top 10 of 22):

| config | draws |
|---|---|
| 1ccd61e0d377 | 1340 |
| acdf984097fa | 1220 |
| 0a707eb3306f | 476 |
| a385dbf4d6a2 | 110 |
| 78341797b265 | 70 |
| 268910d516be | 60 |
| 3c54693b3bee | 60 |
| 3f888aa32d07 | 50 |
| cac954c2cbe5 | 50 |
| 6b1451b69ed5 | 40 |

Most used `vertex_layout` of G3 (top 9 of 9):

| config | draws |
|---|---|
| FLOATx4,FLOATx4,UBYTEx4,UBYTEx4,UBYTEx4,UBYTEx4,FLOATx4,FLOATx4;strides=80;topology=Shader | 2560 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,fixed,fixed;strides=28;topology=Strip | 490 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,fixed,fixed,fixed,fixed,fixed,fixed,fixed,fixed;strides=28;topology=Strip | 220 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,BYTEx4,UBYTEx4,fixed,fixed,fixed,fixed,fixed,fixed;strides=36;topology=Strip | 150 |
| SHORTx4;strides=8;topology=Strip | 110 |
| FLOATx4;strides=16;topology=Strip | 100 |
| FLOATx3,UBYTEx4,SHORTx4,BYTEx4,BYTEx4;strides=32;topology=Strip | 20 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,BYTEx4,fixed;strides=32;topology=Strip | 16 |
| FLOATx2,UBYTEx4;strides=12;topology=Strip | 10 |

Most used `fragment_config` of G3 (top 6 of 6):

| config | draws |
|---|---|
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=off/- | 2760 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=LessThanOrEqual/w | 706 |
| Default;blend:Add/Add;colour=on;alpha=GreaterThan;stencil=off;depth=LessThanOrEqual/- | 70 |
| Default;blend:Add/Add;colour=on;alpha=GreaterThan;stencil=off;depth=LessThanOrEqual/w | 70 |
| Shadow;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=off/- | 50 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=Always;depth=Always/w | 20 |

Most used `texture_config` of G3 (top 10 of 35):

| config | draws |
|---|---|
| u0:Texture2D:A4:1024x1024;u1:Texture2D:I8:8x8 | 1230 |
| u0:Texture2D:ETC1A4:512x512;u1:Texture2D:I8:8x8 | 520 |
| u0:Texture2D:IA4:256x512;u1:Texture2D:I8:8x8 | 410 |
| u0:Texture2D:ETC1A4:256x256;u1:Texture2D:I8:8x8 | 220 |
| u1:Texture2D:ETC1:64x64 | 200 |
| u1:Texture2D:ETC1:128x128 | 176 |
| u1:Texture2D:ETC1A4:128x128 | 150 |
| none | 110 |
| u1:Texture2D:ETC1A4:64x64 | 70 |
| u0:Shadow2D:RGBA8:256x256;u1:Texture2D:ETC1A4:256x256;u2:Texture2D:ETC1A4:512x256 | 50 |

Most used `vs_program` of G4 (top 10 of 19):

| config | draws |
|---|---|
| b6f7179130d3 | 410 |
| 91b5c81efa5b | 260 |
| 86fcff4ec138 | 120 |
| 5f786f7c5175 | 102 |
| ba6efb81007a | 100 |
| fa65db28444d | 100 |
| dc48a79f1f2c | 50 |
| 09dbda60c961 | 40 |
| 2b02f780cf03 | 40 |
| 71fbc4051c91 | 25 |

Most used `tev_config` of G4 (top 10 of 23):

| config | draws |
|---|---|
| acdf984097fa | 425 |
| ac4ee041a29b | 260 |
| 3c54693b3bee | 120 |
| 268910d516be | 110 |
| 0f53c49c9a89 | 61 |
| 0a707eb3306f | 50 |
| a530908642ab | 50 |
| b1a0dfbf9913 | 50 |
| 6b1451b69ed5 | 40 |
| ea5454ddb44d | 40 |

Most used `vertex_layout` of G4 (top 10 of 11):

| config | draws |
|---|---|
| FLOATx4,FLOATx4,UBYTEx4,UBYTEx4,UBYTEx4,UBYTEx4,FLOATx4,FLOATx4;strides=80;topology=Shader | 425 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,fixed,fixed;strides=28;topology=Strip | 310 |
| FLOATx4;strides=16;topology=Strip | 200 |
| SHORTx4;strides=8;topology=Strip | 180 |
| FLOATx3,UBYTEx4,SHORTx4,SHORTx2,UBYTEx4;strides=32;topology=Shader | 102 |
| FLOATx3,BYTEx4,FLOATx2,UBYTEx4,BYTEx4,UBYTEx4,fixed,fixed,fixed,fixed,fixed,fixed;strides=36;topology=Strip | 70 |
| FLOATx3,BYTEx4,FLOATx2,SHORTx2,fixed,fixed;strides=28;topology=Strip | 40 |
| FLOATx3,UBYTEx4,SHORTx4,BYTEx4,BYTEx4;strides=32;topology=Strip | 25 |
| FLOATx3,UBYTEx4,SHORTx4,SHORTx2,BYTEx4;strides=32;topology=Shader | 14 |
| FLOATx2,UBYTEx4;strides=12;topology=Strip | 10 |

Most used `fragment_config` of G4 (top 7 of 7):

| config | draws |
|---|---|
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=off/- | 785 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=LessThanOrEqual/w | 240 |
| Default;blend:Add/Add;colour=on;alpha=GreaterThan;stencil=off;depth=LessThanOrEqual/- | 161 |
| Default;blend:Add/Add;colour=on;alpha=GreaterThan;stencil=off;depth=LessThanOrEqual/w | 150 |
| Default;blend:Add/Add;colour=on;alpha=off;stencil=Always;depth=Always/w | 20 |
| Shadow;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=off/- | 20 |
| Gas;blend:Add/Add;colour=on;alpha=off;stencil=off;depth=LessThanOrEqual/- | 10 |

Most used `texture_config` of G4 (top 10 of 30):

| config | draws |
|---|---|
| u0:Texture2D:ETC1A4:128x128;u1:Texture2D:I8:8x8 | 250 |
| u1:Texture2D:ETC1A4:256x256 | 150 |
| none | 140 |
| u1:Texture2D:ETC1:256x256 | 100 |
| u0:Texture2D:ETC1A4:256x256;u1:Texture2D:I8:8x8 | 95 |
| u0:Texture2D:IA8:512x256 | 80 |
| u0:Texture2D:ETC1A4:256x256 | 61 |
| u0:Texture2D:RGBA8:256x512 | 60 |
| u0:Texture2D:RGBA8:16x32;u1:Texture2D:RGBA8:16x32;u2:Texture2D:RGBA8:16x32 | 40 |
| u0:Texture2D:RGBA8:32x64;u1:Texture2D:RGBA8:32x64;u2:Texture2D:RGBA8:32x64 | 40 |
