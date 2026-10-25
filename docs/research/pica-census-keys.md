# PICA census: pipeline-key groups and the union across captures

Written from the output of `tools/native/pica_census.py --union` (N0-40), made on the seven census captures: G1, G2, G3, G4 (the golden scenes) and G9a, G9b, G9c (the census scenes). The sections below the horizontal rule are the tool's own markdown, copied unchanged except for the omission of its `vertex_stage` list (the vertex layouts are in the tool's output); nothing in this file is typed by hand but this introduction. It holds counts, register values, Azahar's enumerator names and hashes only: no memory contents, shader instructions or texture bytes. Program and key hashes are 6-byte BLAKE2b digests of register or shader words.

The command (the design's section 7, `docs/native/gpu-fragment-design.md`; it writes to `build/`, never over `docs/research/pica-census.md`, which this package neither regenerates nor edits):

```
python3 tools/native/pica_census.py \
    'G1=build/golden/G1/gpu-*.mgpu' 'G2=build/golden/G2/gpu-*.mgpu' \
    'G3=build/golden/G3/gpu-*.mgpu' 'G4=build/golden/G4/gpu-*.mgpu' \
    'G9a=build/golden/G9a/gpu-*.mgpu' 'G9b=build/golden/G9b/gpu-*.mgpu' 'G9c=build/golden/G9c/gpu-*.mgpu' \
    --union --json build/pica-census-union.json --md build/pica-census-union.md
```

Without `--union` the tool's JSON and markdown on these seven captures are byte-identical to those of the tool at the base commit of N0-40 (checked by file hash).

## What the numbers are

- A draw is a draw trigger, as in `pica-census.md` (array, indexed, and one per immediate-mode batch). Each distinct group value is counted once per label and once in the union; "only in this capture's label" counts the values no other label has.
- `fs_key` is the canonical fragment key of design 4.3, read from the registers as `FSConfig::FSConfig(regs)` (`pica_fs_config.cpp`) reads them: the framebuffer part (scissor mode, depth-buffering mode, shadow rendering, the alpha test function, blending on, `requested_logic_op` zero while blending is on, a half's requested blend zero unless its equation is Min or Max), the texture part (unit 0 type register, `texture2_use_coord1`, the combiner-buffer update masks, fog mode, `fog_flip`, the shadow orthographic flag, the wrap modes of all three units as Azahar reads them even when a unit is disabled, the six TEV stages with the Dot3_RGBA masking), the lighting part (per-light slot and flags, LUT enable, absolute flag, input and scale, the `config0` flags) and the procedural-texture part. The user word and the gas word of the design are not registers and are not in it. Its parts are counted as `fs_key.framebuffer`, `fs_key.texture`, `fs_key.lighting` and `fs_key.proctex`.
- `fixed_function`: blend equations and factors (or the logic op when blending is off), colour write mask (the four enables, none when `allow_color_write` is zero, as `vk_rasterizer.cpp` builds it), depth test, compare and write, stencil function and operations, cull mode and the front face `PicaToVK::FrontFace` derives from it. Stencil masks and reference, the blend constant colour and the depth bias are dynamic state in the design and are not in it.
- `vertex_stage`: the vertex program hash (code and swizzle words), the vertex layout and the topology. The entry point never separated two draws of the same program hash in these captures.
- `pipeline_tuple`: `fs_key`, `fixed_function`, `vertex_stage`, the pass kind (Normal, or ShadowWrite for the Shadow fragment operation mode) and the logical colour and depth formats; `pipeline_tuple+fixup` adds the fixup stage (lighting on and the vertex stage is the hardware one, geometry-shader bit clear). The geometry-shader program hash is not part of the tuple; it never split a tuple in these captures.
- `proctex_config`: the `ProcTexConfig` fields of the draws that have texture 3 enabled.

## Reading the tables

- Across the seven captures: 108 distinct `fs_key`, 21 `fixed_function`, 26 `vertex_stage`, 146 `pipeline_tuple` (146 with the fixup stage too: the fixup stage adds no tuple). Per capture (G1, G2, G3, G4, G9a, G9b, G9c): `fs_key` 6, 21, 47, 39, 51, 6, 6; `fixed_function` 5, 6, 13, 15, 11, 5, 5; `vertex_stage` 4, 9, 16, 21, 18, 4, 5; `pipeline_tuple` 7, 24, 62, 48, 66, 7, 7. Only 3 `fs_key` values and 3 `pipeline_tuple` values are in every label.
- Lit draws: 2,787 in G2, G3, G4 and G9a together, of which 1,129 set the geometry-shader bit (234, 440, 375, 80) and 1,658 do not (63, 850, 370, 375); the 1,658 are the draws the GPU fix-up applies to. G1, G9b and G9c have none.
- Procedural texture: one `ProcTexConfig` in all seven captures (the `proctex_config` row), so the design's "one" holds on these captures.
- Blend: every draw of the seven captures blends with the Add equation for both halves (no Min or Max), so no blend factor is in `fs_key`; the factors that appear are in the tables below (6 distinct colour-half pairs, 5 alpha-half pairs, 7 distinct pairs over both halves, 11 distinct blend states). Cull mode is KeepAll or KeepCounterClockWise only; scissor mode Include and depth-buffering mode ZBuffering in every draw; `fog_flip` clear in every draw; a ClampToBorder wrap occurs on unit 0 in G3 and G9a, ClampToBorder2 nowhere; the colour write mask is RGBA except 10 draws of G4 that write alpha only.

---

## Distinct values per group

Cells are "distinct values (of them only in this capture's label)"; the draws are all draws of the label that have the group (every draw for all groups but `proctex_config`).

| group | G1 | G2 | G3 | G4 | G9a | G9b | G9c | union | in every label |
|---|---|---|---|---|---|---|---|---|---|
| fixed_function | 5 (0) | 6 (0) | 13 (3) | 15 (4) | 11 (1) | 5 (1) | 5 (1) | 21 | 4 |
| fs_key | 6 (0) | 21 (9) | 47 (15) | 39 (25) | 51 (17) | 6 (0) | 6 (0) | 108 | 3 |
| fs_key.framebuffer | 1 (0) | 2 (0) | 3 (0) | 3 (0) | 3 (0) | 1 (0) | 1 (0) | 3 | 1 |
| fs_key.lighting | 1 (0) | 3 (2) | 7 (1) | 9 (4) | 8 (1) | 1 (0) | 1 (0) | 15 | 1 |
| fs_key.proctex | 2 (0) | 2 (0) | 2 (0) | 2 (0) | 2 (0) | 2 (0) | 2 (0) | 2 | 2 |
| fs_key.texture | 6 (0) | 19 (7) | 40 (9) | 29 (14) | 47 (14) | 6 (0) | 6 (0) | 85 | 3 |
| pipeline_tuple | 7 (0) | 24 (11) | 62 (25) | 48 (33) | 66 (27) | 7 (1) | 7 (1) | 146 | 3 |
| pipeline_tuple+fixup | 7 (0) | 24 (11) | 62 (25) | 48 (33) | 66 (27) | 7 (1) | 7 (1) | 146 | 3 |
| proctex_config | 1 (0) | 1 (0) | 1 (0) | 1 (0) | 1 (0) | 1 (0) | 1 (0) | 1 | 1 |
| vertex_stage | 4 (0) | 9 (0) | 16 (2) | 21 (5) | 18 (1) | 4 (0) | 5 (0) | 26 | 4 |

| draws with the group | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| fixed_function | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| fs_key | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| fs_key.framebuffer | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| fs_key.lighting | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| fs_key.proctex | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| fs_key.texture | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| pipeline_tuple | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| pipeline_tuple+fixup | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |
| proctex_config | 140 | 668 | 2560 | 425 | 1127 | 1007 | 1041 |
| vertex_stage | 155 | 794 | 3650 | 1390 | 1572 | 1025 | 1059 |

## Lit draws against geometry shader

Cells are "draws (frames)".

| lighting + geometry shader | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| lighting off + geometry shader yes | 140 (3 fr) | 434 (3 fr) | 2120 (10 fr) | 167 (10 fr) | 1051 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |
| lighting on + geometry shader no | 0 | 63 (3 fr) | 850 (10 fr) | 370 (10 fr) | 375 (3 fr) | 0 | 0 |
| lighting on + geometry shader yes | 0 | 234 (3 fr) | 440 (10 fr) | 375 (10 fr) | 80 (3 fr) | 0 | 0 |
| lighting off + geometry shader no | 15 (3 fr) | 63 (3 fr) | 240 (10 fr) | 478 (10 fr) | 66 (3 fr) | 18 (3 fr) | 18 (3 fr) |

## Blend

| blend_factors_rgb | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| Add SourceAlpha/OneMinusSourceAlpha | 135 (3 fr) | 665 (3 fr) | 2570 (10 fr) | 570 (10 fr) | 1137 (3 fr) | 1007 (3 fr) | 1044 (3 fr) |
| Add One/Zero | 12 (3 fr) | 120 (3 fr) | 990 (10 fr) | 750 (10 fr) | 417 (3 fr) | 12 (3 fr) | 12 (3 fr) |
| Add SourceAlpha/One | 5 (3 fr) | 3 (3 fr) | 30 (10 fr) | 40 (10 fr) | 12 (3 fr) | 0 | 0 |
| Add One/One | 3 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) | 6 (3 fr) | 3 (3 fr) | 3 (3 fr) |
| Add Zero/One | 0 | 0 | 30 (10 fr) | 0 | 0 | 3 (3 fr) | 0 |
| Add DestAlpha/OneMinusDestAlpha | 0 | 0 | 10 (10 fr) | 10 (10 fr) | 0 | 0 | 0 |

| blend_factors_a | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| Add One/Zero | 150 (3 fr) | 791 (3 fr) | 3600 (10 fr) | 1215 (10 fr) | 1565 (3 fr) | 1022 (3 fr) | 1056 (3 fr) |
| Add SourceAlpha/OneMinusSourceAlpha | 0 | 0 | 0 | 125 (10 fr) | 4 (3 fr) | 0 | 3 (3 fr) |
| Add SourceAlpha/One | 5 (3 fr) | 3 (3 fr) | 30 (10 fr) | 50 (10 fr) | 3 (3 fr) | 0 | 0 |
| Add Zero/One | 0 | 0 | 10 (10 fr) | 0 | 0 | 3 (3 fr) | 0 |
| Add DestAlpha/Zero | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 | 0 |

| blend_factor_pair | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| One/Zero | 150 (3 fr) | 791 (3 fr) | 3600 (10 fr) | 1215 (10 fr) | 1565 (3 fr) | 1022 (3 fr) | 1056 (3 fr) |
| SourceAlpha/OneMinusSourceAlpha | 135 (3 fr) | 665 (3 fr) | 2570 (10 fr) | 570 (10 fr) | 1137 (3 fr) | 1007 (3 fr) | 1044 (3 fr) |
| SourceAlpha/One | 5 (3 fr) | 3 (3 fr) | 30 (10 fr) | 50 (10 fr) | 12 (3 fr) | 0 | 0 |
| One/One | 3 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) | 6 (3 fr) | 3 (3 fr) | 3 (3 fr) |
| Zero/One | 0 | 0 | 40 (10 fr) | 0 | 0 | 3 (3 fr) | 0 |
| DestAlpha/OneMinusDestAlpha | 0 | 0 | 10 (10 fr) | 10 (10 fr) | 0 | 0 | 0 |
| DestAlpha/Zero | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 | 0 |

| blend_factor | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| One | 155 (3 fr) | 794 (3 fr) | 3650 (10 fr) | 1265 (10 fr) | 1568 (3 fr) | 1025 (3 fr) | 1056 (3 fr) |
| Zero | 150 (3 fr) | 791 (3 fr) | 3620 (10 fr) | 1215 (10 fr) | 1565 (3 fr) | 1025 (3 fr) | 1056 (3 fr) |
| SourceAlpha | 140 (3 fr) | 668 (3 fr) | 2600 (10 fr) | 620 (10 fr) | 1149 (3 fr) | 1007 (3 fr) | 1044 (3 fr) |
| OneMinusSourceAlpha | 135 (3 fr) | 665 (3 fr) | 2570 (10 fr) | 570 (10 fr) | 1137 (3 fr) | 1007 (3 fr) | 1044 (3 fr) |
| DestAlpha | 0 | 0 | 20 (10 fr) | 10 (10 fr) | 0 | 0 | 0 |
| OneMinusDestAlpha | 0 | 0 | 10 (10 fr) | 10 (10 fr) | 0 | 0 | 0 |

| blend_state | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| rgb Add(SourceAlpha/OneMinusSourceAlpha); a Add(One/Zero) | 135 (3 fr) | 665 (3 fr) | 2570 (10 fr) | 445 (10 fr) | 1133 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |
| rgb Add(One/Zero); a Add(One/Zero) | 12 (3 fr) | 120 (3 fr) | 990 (10 fr) | 750 (10 fr) | 417 (3 fr) | 12 (3 fr) | 12 (3 fr) |
| rgb Add(SourceAlpha/OneMinusSourceAlpha); a Add(SourceAlpha/OneMinusSourceAlpha) | 0 | 0 | 0 | 125 (10 fr) | 4 (3 fr) | 0 | 3 (3 fr) |
| rgb Add(SourceAlpha/One); a Add(SourceAlpha/One) | 5 (3 fr) | 3 (3 fr) | 30 (10 fr) | 40 (10 fr) | 3 (3 fr) | 0 | 0 |
| rgb Add(One/One); a Add(One/Zero) | 3 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) | 6 (3 fr) | 3 (3 fr) | 3 (3 fr) |
| rgb Add(Zero/One); a Add(One/Zero) | 0 | 0 | 20 (10 fr) | 0 | 0 | 0 | 0 |
| rgb Add(DestAlpha/OneMinusDestAlpha); a Add(SourceAlpha/One) | 0 | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 |
| rgb Add(DestAlpha/OneMinusDestAlpha); a Add(Zero/One) | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 | 0 |
| rgb Add(Zero/One); a Add(DestAlpha/Zero) | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 | 0 |
| rgb Add(SourceAlpha/One); a Add(One/Zero) | 0 | 0 | 0 | 0 | 9 (3 fr) | 0 | 0 |
| rgb Add(Zero/One); a Add(Zero/One) | 0 | 0 | 0 | 0 | 0 | 3 (3 fr) | 0 |

## Fixed-function and rasterizer state

| colour_write_mask | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| RGBA | 155 (3 fr) | 794 (3 fr) | 3650 (10 fr) | 1380 (10 fr) | 1572 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |
| A | 0 | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 |

| cull_mode | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| KeepAll | 155 (3 fr) | 731 (3 fr) | 2810 (10 fr) | 960 (10 fr) | 1200 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |
| KeepCounterClockWise | 0 | 63 (3 fr) | 840 (10 fr) | 430 (10 fr) | 372 (3 fr) | 0 | 0 |

| front_face | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| CounterClockwise | 155 (3 fr) | 731 (3 fr) | 2810 (10 fr) | 960 (10 fr) | 1200 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |
| Clockwise | 0 | 63 (3 fr) | 840 (10 fr) | 430 (10 fr) | 372 (3 fr) | 0 | 0 |

| scissor_mode | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| Include | 155 (3 fr) | 794 (3 fr) | 3650 (10 fr) | 1390 (10 fr) | 1572 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |

| depthmap_enable | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| ZBuffering | 155 (3 fr) | 794 (3 fr) | 3650 (10 fr) | 1390 (10 fr) | 1572 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |

## Texture and fog fields of FSConfig

| texture0_type_register | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| Texture2D | 155 (3 fr) | 794 (3 fr) | 3070 (10 fr) | 1390 (10 fr) | 1341 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |
| Shadow2D | 0 | 0 | 580 (10 fr) | 0 | 231 (3 fr) | 0 | 0 |

| texture2_use_coord1 | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| False | 155 (3 fr) | 731 (3 fr) | 2930 (10 fr) | 1270 (10 fr) | 1410 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |
| True | 0 | 63 (3 fr) | 720 (10 fr) | 120 (10 fr) | 162 (3 fr) | 0 | 0 |

| fog_flip | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| False | 155 (3 fr) | 794 (3 fr) | 3650 (10 fr) | 1390 (10 fr) | 1572 (3 fr) | 1025 (3 fr) | 1059 (3 fr) |

| shadow_orthographic | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| False | 155 (3 fr) | 794 (3 fr) | 221 (1 fr) | 1390 (10 fr) | 467 (1 fr) | 1025 (3 fr) | 1059 (3 fr) |
| True | 0 | 0 | 3429 (10 fr) | 0 | 1105 (3 fr) | 0 | 0 |

| combiner_buffer_input | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 0 | 149 (3 fr) | 716 (3 fr) | 2810 (10 fr) | 1190 (10 fr) | 1341 (3 fr) | 1019 (3 fr) | 1053 (3 fr) |
| 247 | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| 241 | 6 (3 fr) | 6 (3 fr) | 20 (10 fr) | 20 (10 fr) | 6 (3 fr) | 6 (3 fr) | 6 (3 fr) |
| 221 | 0 | 3 (3 fr) | 10 (10 fr) | 10 (10 fr) | 3 (3 fr) | 0 | 0 |
| 34 | 0 | 3 (3 fr) | 10 (10 fr) | 10 (10 fr) | 3 (3 fr) | 0 | 0 |
| 255 | 0 | 0 | 0 | 10 (10 fr) | 0 | 0 | 0 |
| 85 | 0 | 3 (3 fr) | 0 | 0 | 0 | 0 | 0 |

| wrap_mode_unit | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| unit0:s=ClampToEdge/t=ClampToEdge | 146 (3 fr) | 716 (3 fr) | 2620 (10 fr) | 665 (10 fr) | 1169 (3 fr) | 1013 (3 fr) | 1047 (3 fr) |
| unit1:s=ClampToEdge/t=ClampToEdge | 140 (3 fr) | 689 (3 fr) | 2630 (10 fr) | 575 (10 fr) | 1148 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |
| unit1:s=Repeat/t=Repeat | 0 | 63 (3 fr) | 800 (10 fr) | 410 (10 fr) | 351 (3 fr) | 0 | 0 |
| unit2:s=ClampToEdge/t=ClampToEdge | 0 | 21 (3 fr) | 70 (10 fr) | 180 (10 fr) | 45 (3 fr) | 0 | 0 |
| unit0:s=Repeat/t=Repeat | 3 (3 fr) | 3 (3 fr) | 120 (10 fr) | 165 (10 fr) | 10 (3 fr) | 3 (3 fr) | 3 (3 fr) |
| unit2:s=Repeat/t=Repeat | 0 | 0 | 80 (10 fr) | 0 | 33 (3 fr) | 0 | 0 |
| unit0:s=ClampToBorder/t=ClampToBorder | 0 | 0 | 90 (10 fr) | 0 | 21 (3 fr) | 0 | 0 |

| border_wrap_kind | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| ClampToBorder | 0 | 0 | 90 (10 fr) | 0 | 21 (3 fr) | 0 | 0 |

## Procedural texture (ProcTexConfig), draws with texture3 enabled

| proctex_coord | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 2 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_u_clamp | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| ToEdge | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_v_clamp | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| ToEdge | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_color_combiner | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| U | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_alpha_combiner | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| U | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_separate_alpha | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 0 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_noise_enable | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 0 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_u_shift | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| None | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_v_shift | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| None | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_lut_width | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 128 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_lut_offsets | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 0/128/192/224 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_lod_min | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 0 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_lod_max | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| 6 | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

| proctex_lut_filter | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| Linear | 140 (3 fr) | 668 (3 fr) | 2560 (10 fr) | 425 (10 fr) | 1127 (3 fr) | 1007 (3 fr) | 1041 (3 fr) |

## Lighting (LightConfig), lit draws

| light_config0_flag | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| clamp_highlights | 0 | 297 (3 fr) | 1290 (10 fr) | 745 (10 fr) | 455 (3 fr) | 0 | 0 |
| enable_secondary_alpha | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| enable_shadow | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| enable_primary_alpha | 0 | 63 (3 fr) | 750 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |

| light_bump_selector | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| texture 0 | 0 | 297 (3 fr) | 1240 (10 fr) | 745 (10 fr) | 455 (3 fr) | 0 | 0 |
| texture 2 | 0 | 0 | 50 (10 fr) | 0 | 0 | 0 | 0 |

| light_shadow_flag | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| shadow_alpha | 0 | 0 | 90 (10 fr) | 0 | 21 (3 fr) | 0 | 0 |
| shadow_primary | 0 | 0 | 90 (10 fr) | 0 | 21 (3 fr) | 0 | 0 |
| shadow_secondary | 0 | 0 | 90 (10 fr) | 0 | 21 (3 fr) | 0 | 0 |

| light_flag | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| directional | 0 | 297 (3 fr) | 1285 (10 fr) | 724 (10 fr) | 447 (3 fr) | 0 | 0 |
| shadow_enable | 0 | 297 (3 fr) | 320 (10 fr) | 435 (10 fr) | 156 (3 fr) | 0 | 0 |
| dist_atten_enable | 0 | 0 | 0 | 435 (10 fr) | 0 | 0 | 0 |

| light_lut_input | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| sp:NH | 0 | 234 (3 fr) | 490 (10 fr) | 595 (10 fr) | 236 (3 fr) | 0 | 0 |
| rb:NH | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rg:NH | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rr:NH | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| sp:SP | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| fr:NV | 0 | 63 (3 fr) | 800 (10 fr) | 110 (10 fr) | 195 (3 fr) | 0 | 0 |
| d0:NH | 0 | 0 | 0 | 40 (10 fr) | 24 (3 fr) | 0 | 0 |
| fr:NH | 0 | 0 | 0 | 40 (10 fr) | 24 (3 fr) | 0 | 0 |

| light_lut_abs_input | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| sp:signed | 0 | 234 (3 fr) | 490 (10 fr) | 595 (10 fr) | 236 (3 fr) | 0 | 0 |
| rb:abs | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rg:abs | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rr:abs | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| sp:abs | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| fr:abs | 0 | 63 (3 fr) | 800 (10 fr) | 110 (10 fr) | 195 (3 fr) | 0 | 0 |
| d0:abs | 0 | 0 | 0 | 40 (10 fr) | 24 (3 fr) | 0 | 0 |
| fr:signed | 0 | 0 | 0 | 40 (10 fr) | 24 (3 fr) | 0 | 0 |

| light_lut_scale | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| sp:Scale1 | 0 | 297 (3 fr) | 1290 (10 fr) | 745 (10 fr) | 455 (3 fr) | 0 | 0 |
| fr:Scale1 | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rb:Scale1 | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rg:Scale1 | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| rr:Scale1 | 0 | 63 (3 fr) | 800 (10 fr) | 150 (10 fr) | 219 (3 fr) | 0 | 0 |
| d0:Scale1 | 0 | 0 | 0 | 40 (10 fr) | 24 (3 fr) | 0 | 0 |

## `fixed_function` values

Cells are draws.

| value | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| blend:Add(SourceAlpha/OneMinusSourceAlpha),Add(One/Zero);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 135 | 665 | 2520 | 425 | 1127 | 1007 | 1041 |
| blend:Add(One/Zero),Add(One/Zero);mask=RGBA;depth=LessThanOrEqual/w;stencil=off[];cull=KeepCounterClockWise;front=Clockwise | 0 | 63 | 740 | 370 | 333 | 0 | 0 |
| blend:Add(One/Zero),Add(One/Zero);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 6 | 51 | 170 | 310 | 51 | 6 | 6 |
| blend:Add(SourceAlpha/OneMinusSourceAlpha),Add(SourceAlpha/OneMinusSourceAlpha);mask=RGBA;depth=LessThanOrEqual/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 0 | 115 | 4 | 0 | 0 |
| blend:Add(One/Zero),Add(One/Zero);mask=RGBA;depth=off/-;stencil=off[];cull=KeepCounterClockWise;front=Clockwise | 0 | 0 | 50 | 20 | 24 | 0 | 0 |
| blend:Add(SourceAlpha/OneMinusSourceAlpha),Add(One/Zero);mask=RGBA;depth=LessThanOrEqual/-;stencil=off[];cull=KeepCounterClockWise;front=Clockwise | 0 | 0 | 50 | 20 | 6 | 0 | 0 |
| blend:Add(One/Zero),Add(One/Zero);mask=RGBA;depth=Always/w;stencil=Always[Keep/Keep/Replace];cull=KeepAll;front=CounterClockwise | 6 | 6 | 20 | 20 | 6 | 6 | 6 |
| blend:Add(One/One),Add(One/Zero);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 3 | 6 | 20 | 20 | 6 | 3 | 3 |
| blend:Add(SourceAlpha/One),Add(SourceAlpha/One);mask=RGBA;depth=LessThanOrEqual/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 20 | 30 | 0 | 0 | 0 |
| blend:Add(SourceAlpha/One),Add(SourceAlpha/One);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 5 | 3 | 10 | 10 | 3 | 0 | 0 |
| blend:Add(One/Zero),Add(One/Zero);mask=RGBA;depth=LessThanOrEqual/w;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 10 | 10 | 3 | 0 | 0 |
| blend:Add(Zero/One),Add(One/Zero);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 20 | 0 | 0 | 0 | 0 |
| blend:Add(DestAlpha/OneMinusDestAlpha),Add(SourceAlpha/One);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 0 | 10 | 0 | 0 | 0 |
| blend:Add(DestAlpha/OneMinusDestAlpha),Add(Zero/One);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 10 | 0 | 0 | 0 | 0 |
| blend:Add(One/Zero),Add(One/Zero);mask=A;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 0 | 10 | 0 | 0 | 0 |
| blend:Add(One/Zero),Add(One/Zero);mask=RGBA;depth=LessThanOrEqual/-;stencil=off[];cull=KeepCounterClockWise;front=Clockwise | 0 | 0 | 0 | 10 | 0 | 0 | 0 |
| blend:Add(SourceAlpha/OneMinusSourceAlpha),Add(SourceAlpha/OneMinusSourceAlpha);mask=RGBA;depth=LessThanOrEqual/w;stencil=off[];cull=KeepCounterClockWise;front=Clockwise | 0 | 0 | 0 | 10 | 0 | 0 | 0 |
| blend:Add(Zero/One),Add(DestAlpha/Zero);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 10 | 0 | 0 | 0 | 0 |
| blend:Add(SourceAlpha/One),Add(One/Zero);mask=RGBA;depth=LessThanOrEqual/w;stencil=off[];cull=KeepCounterClockWise;front=Clockwise | 0 | 0 | 0 | 0 | 9 | 0 | 0 |
| blend:Add(SourceAlpha/OneMinusSourceAlpha),Add(SourceAlpha/OneMinusSourceAlpha);mask=RGBA;depth=off/-;stencil=off[];cull=KeepAll;front=CounterClockwise | 0 | 0 | 0 | 0 | 0 | 0 | 3 |
| blend:Add(Zero/One),Add(Zero/One);mask=RGBA;depth=Always/w;stencil=Always[Keep/Keep/Replace];cull=KeepAll;front=CounterClockwise | 0 | 0 | 0 | 0 | 0 | 3 | 0 |

## `proctex_config` values

Cells are draws.

| value | G1 | G2 | G3 | G4 | G9a | G9b | G9c |
|---|---|---|---|---|---|---|---|
| coord=2;u_clamp=ToEdge;v_clamp=ToEdge;color_combiner=U;alpha_combiner=U;separate_alpha=0;noise_enable=0;u_shift=None;v_shift=None;lut_width=128;lut_offsets=0/128/192/224;lod_min=0;lod_max=6;lut_filter=Linear | 140 | 668 | 2560 | 425 | 1127 | 1007 | 1041 |
