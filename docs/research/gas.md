# Gas (and the fog it shares a register with)

N2-10. What is known about the PICA200's gas mode, from where, what the native renderer does with it, what is open, and how it
will be checked. The code is `native/src/gpu/fragment/fog/fog.cpp` (read its header: the same labels, the same step names), the
layout is `fog/fog_layout.h`, the checks are `native/tests/gpu_fog_test.cpp`.

## 1. Sources

**3dbrew only**, the page *GPU/Internal Registers* (revision 23943, last edited 2026-10-18 00:18; saved as wikitext for this work
and cited below by register name). **No other source was used for gas**: no SDK documentation, no other emulator, no capture of a
real console. Azahar has no gas code to copy (its generators log "Unimplemented gas mode" and write transparent black; its software
renderer's `WriteFog` acts only on fog mode `Fog`; its headers leave the gas registers as padding, `regs_framebuffer.h:15-19, 298`).

3dbrew gives the registers and their fields. It gives **no per-fragment formula**: not how the density pass combines a fragment
with the colour buffer, not how the shading pass turns a density into a colour, not what the attenuations or the shading
intensities do. So what the renderer does is the smallest model the descriptions suggest, and every step carries a label:

| label | meaning |
|---|---|
| `3dbrew` | stated there (page and register named) |
| `inference` | read from the names and the fields of the registers |
| `unverified` | a choice of ours; open until the console check (section 7) |

## 2. What 3dbrew says (all `3dbrew`, *GPU/Internal Registers*)

| register (id) | content |
|---|---|
| `GPUREG_TEXENV_UPDATE_BUFFER` (0xE0) | bits 0-2 fog mode: 0 disabled, 1 linear fog, 5 fog, **7 gas**; bit 3 "shading density source": 0 plain density, 1 depth density; bit 16 Z flip. "Shared between the gas/fog mode configuration and the texture combiner buffer inputs." |
| `GPUREG_COLOR_OPERATION` (0x100) | bits 0-1 fragment operation mode: 0 default, **1 gas**, 3 shadow; bit 8 blend (1) or logic op (0) |
| `GPUREG_COLORBUFFER_FORMAT` (0x117) | format 0 is "RGBA8/Gas"; "colour components are laid out in reverse byte order in memory, with the most significant bits used first" |
| `GPUREG_GAS_ATTENUATION` (0xE4) | bits 0-15 float1.5.10, "gas density attenuation" |
| `GPUREG_GAS_ACCMAX` (0xE5) | bits 0-15 float1.5.10, "gas maximum density accumulation" |
| `GPUREG_GAS_LIGHT_XY` (0x120) | bits 0-7 planar shading minimum intensity, 8-15 maximum intensity, 16-23 density attenuation (unsigned) |
| `GPUREG_GAS_LIGHT_Z` (0x121) | the same three fields for view shading |
| `GPUREG_GAS_LIGHT_Z_COLOR` (0x122) | bits 0-7 view shading effect in the line-of-sight direction; **bit 8 "gas colour LUT input": 0 gas density, 1 light factor** |
| `GPUREG_GAS_LUT_INDEX` (0x123), `GPUREG_GAS_LUT_DATA` (0x124) | a FIFO written one word at a time from the index; the table has 16 elements: **the first 8 are three signed bytes (red bits 0-7, green 8-15, blue 16-23), the last 8 the same three fields unsigned** |
| `GPUREG_GAS_ACCMAX_FEEDBACK` (0x125) | bits 0-15 float1.5.10 initial maximum density; bits 16-31 maximum density, read-only: "the maximum plain density (blue/alpha bytes of the colour buffer) written by gas density rendering (fragment operation mode 1)", "compared as unsigned 16-bit integers, so negative values, infinities and NaN are greater than any positive value"; "it can be read back to compute `GPUREG_GAS_ACCMAX`"; writing bits 0-15 in gas mode resets the maximum |
| `GPUREG_GAS_DELTAZ_DEPTH` (0x126) | bits 0-23 fixed0.16.8 "depth direction attenuation proportion"; bits 24-25 depth function (0 never, 1 always, 2 greater / greater or equal, 3 less / less or equal / equal / not equal) |

The census (`pica-census.md` section 5) saw gas in G4 only: 10 draws with fog mode Gas and 10 with fragment operation mode Gas, in
the same 10 frames (whether they are the same draws is not recorded). `inference`: the fragment-operation-mode draw is the density
pass (3dbrew calls mode 1 "gas density rendering") and the fog-mode draw is the shading pass.

## 3. The model

Both passes run at the `FogGas` point of the fragment generator, after the alpha test, on the TEV output, in both modes (the
specialised shader and the ubershader), and both read the colour buffer as it was **before the draw** through the feedback copy
(`Needs::dst_copy`, bound as `tex_color`).

**Density pass** (fragment operation mode Gas; key bit `kKeyDensityPass`):

| step | label | content |
|---|---|---|
| D1 | `3dbrew` | the density is in the blue and alpha bytes of the colour buffer, a float1.5.10 compared as an unsigned 16-bit integer |
| D2 | `inference` | the 16 bits are blue as the high byte and alpha as the low byte (the word is laid out in reverse byte order, so its low halfword is blue, then alpha) |
| D3 | `inference` | the fragment's density is those two bytes of its TEV output (after the byte rounding) |
| D4 | `inference` | "accumulation": the result is the fragment's density plus the colour buffer's |
| D5 | `unverified` | float1.5.10 is IEEE binary16 (bias 15); the sum is rounded by `packHalf2x16` (the device's rounding); no clamp at `GAS_ACCMAX` in this pass |
| D6 | `unverified` | red and green keep the TEV output's values |

**Shading pass** (fog mode Gas, and not the density pass):

| step | label | content |
|---|---|---|
| S1 | `inference` | the density is the colour buffer's, read as in D2 (the shading draw reads the buffer the density pass wrote) |
| S2 | `inference` / `unverified` | `t = density / ACCMAX` clamped to [0, 1] (`inference`: `ACCMAX` is read back "to compute `GPUREG_GAS_ACCMAX`"). `3dbrew`'s "compared as unsigned 16-bit integers, so negative values, infinities and NaN are greater than any positive value" is said only of the tracked maximum (`GPUREG_GAS_ACCMAX_FEEDBACK`); applying it to a shading-pass density against `ACCMAX` is `unverified`, and under it a density with bits at or above `ACCMAX`'s (negative, infinite and NaN included) gives `t = 1`. Also `unverified`: a density of +0 or -0 (0x0000, 0x8000) gives `t = 0` (so -0 is **not** `t = 1` by the unsigned compare: the zero test comes first); an `ACCMAX` that is negative or NaN gives `t = 0` |
| S3 | `3dbrew` | the colour LUT input is the gas density (bit 8 of 0x122 clear) or the light factor (set) |
| S4 | `unverified` | the light factor has no source in the descriptions (no per-fragment direction): with the light-factor input `t = 1` (a placeholder) |
| S5 | `inference` / `unverified` | the table: entries 8-15 are the colours of eight segments, entries 0-7 the signed differences to the next colour (`inference`: the fog table is value plus difference; the signedness is `3dbrew`); the 1/255 scale of a difference and the extension of the last segment to `t = 1` are `unverified`. `position = 8t`, `segment = min(floor(position), 7)`, `colour = (value[segment] + difference[segment] * (position - segment)) / 255`, clamped to [0, 1] |
| S6 | `unverified` | output RGB = the table colour; alpha = the TEV output's alpha; depth, blending and the logic op follow as for any fragment |

A fragment with both modes set runs the density pass only (`unverified`; the key word holds the density bit alone).

**Fog** (fog mode Fog, 5) is Azahar's: the software renderer's `WriteFog` on the depth the software renderer computes, with the
table `{value / 2047, difference / 2047}` of `Fog::LutEntry` in `lut_lf` and the colour in `fog_color`. One choice: the depth is
clamped to [0, 1] before the index is taken, as `sw_rasterizer.cpp` clamps it (Azahar's GLSL does not, and extrapolates the last
segment; inside [0, 1] they are the same). 3dbrew's linear fog (mode 1, `GPUREG_FOG_LINEAR_SLOPE` / `_OFFSET`) is **not** implemented:
neither Azahar nor the census has it.

## 4. Where the registers go

`FragmentKey::gas` (one word, canonical: a bit is set only when the shader reads it): bit 0 density pass; bit 1 light-factor input
(shading pass only). `FragmentBlock::gas.regs[16]`: `[0..6]` = 0x120 to 0x126, `[7]` = 0xE0, `[8]` = 0xE4, `[9]` = 0xE5, `[10..15]` = 0.
`gas.table[16]` = `PicaState::gas.lut`, raw. N2-33's comment in `uniforms.h` calls the register words "0x120-0x12F"; the corrected
text is in the work package's report. The model reads only 0xE5 (S2), the table (S5) and the key bits; the other registers are
copied raw so a later model needs no new key field or uniform.

## 5. The synthetic draw, worked out by hand

Target 8 by 8, RGBA8, every draw a full-screen triangle with a constant vertex colour; TEV stage 0 outputs the vertex colour, the
others pass through; blending off (logic op Copy); all colour channels written; no depth, no alpha test. binary16 (`unverified`,
D5): a value is (-1)^s 2^(e-15) (1 + m / 1024) with e the 5-bit exponent field and m the 10-bit mantissa field.

| bits | value | | bits | value |
|---|---|---|---|---|
| 0x3C00 | 2^0 = 1 | | 0x3E00 | 1 + 512/1024 = 1.5 |
| 0x3800 | 2^-1 = 0.5 | | 0x3F00 | 1 + 768/1024 = 1.75 |
| 0x3400 | 2^-2 = 0.25 | | 0x4400 | 2^2 = 4 |
| 0x3C80 | 1 + 128/1024 = 1.125 | | 0x4800 | 2^3 = 8 |
| 0x3480 | 2^-2 (1 + 128/1024) = 0.28125 | | 0x3DA0 | 1 + 416/1024 = 1.40625 |
| 0xBC00 | -1 (sign bit set) | | 0x7C00 | +infinity (exponent field 31, mantissa 0) |
| 0x1000 | 2^-11 | | 0x7E00 | NaN (exponent field 31, mantissa not 0) |

**Density pass.** The result is `blue = bits >> 8`, `alpha = bits & 255` of the sum, red and green from the TEV output (D6). The
colour buffer starts at (0, 0, 0, 0) unless stated.

| case | fragment (R, G, B, A) | buffer before | sum | result |
|---|---|---|---|---|
| G1a | (10, 20, 0x3C, 0x00): 1.0 | (0, 0, 0, 0): 0 | 1.0 = 0x3C00 | (10, 20, 0x3C, 0x00) |
| G1b | (30, 40, 0x38, 0x00): 0.5 | G1a: 1.0 | 1.5 = 0x3E00 | (30, 40, 0x3E, 0x00) |
| G1c | (50, 60, 0x34, 0x00): 0.25 | G1b: 1.5 | 1.75 = 0x3F00 | (50, 60, 0x3F, 0x00) |
| G2 | (1, 2, 0x34, 0x80): 0.28125 | (7, 8, 0x3C, 0x80): 1.125 | 1.40625 = 0x3DA0 | (1, 2, 0x3D, 0xA0) |
| G3 | (3, 4, 0, 0): 0 | (9, 9, 0, 0): 0 | 0 | (3, 4, 0, 0) |

**Shading pass.** Fog mode Gas, `ACCMAX` = 0x4400 (4.0) unless stated, vertex colour (9, 9, 9, 77) so the TEV alpha is 77. The
table (words as the data port receives them: bytes red, green, blue from bit 0):

| entry | meaning | bytes (R, G, B) |
|---|---|---|
| 0 | difference of segment 0 | (0, 0, 0) |
| 3 | difference of segment 3 | (20, -10, 0) = (0x14, 0xF6, 0x00) |
| 7 | difference of segment 7 | (10, -10, 100) = (0x0A, 0xF6, 0x64) |
| 5 | difference of segment 5 | (-30, 0, -40) = (0xE2, 0x00, 0xD8) |
| 8 | colour of segment 0 | (1, 2, 3) |
| 10 | colour of segment 2 | (40, 50, 60) |
| 11 | colour of segment 3 | (100, 200, 50) |
| 13 | colour of segment 5 | (200, 100, 90) |
| 15 | colour of segment 7 | (250, 5, 0) |
| others | | 0 |

| case | buffer before (blue, alpha) | t | position, segment, fraction | colour | output |
|---|---|---|---|---|---|
| H1 | 0x3F00 = 1.75 | 1.75 / 4 = 0.4375 | 3.5, 3, 0.5 | (100 + 0.5 * 20, 200 + 0.5 * -10, 50 + 0.5 * 0) = (110, 195, 50) | (110, 195, 50, 77) |
| H2 | 0x0000 = 0 | 0 (density is +-0) | 0, 0, 0 | (1, 2, 3) | (1, 2, 3, 77) |
| H3 | 0x4800 = 8 | 1 (bits 0x4800 >= 0x4400) | 8, min(8, 7) = 7, 1 | (250 + 10, 5 - 10, 0 + 100) = (260, -5, 100), clamped (255, 0, 100) | (255, 0, 100, 77) |
| H4 | 0x4400 = ACCMAX | 1 (equal) | as H3 | (255, 0, 100) | (255, 0, 100, 77) |
| H5 | 0xBC00 = -1.0 | 1 (unsigned compare, `unverified`: 0xBC00 >= 0x4400) | as H3 | (255, 0, 100) | (255, 0, 100, 77) |
| H6 | 0x3F00, light-factor input | 1 (S4 placeholder) | as H3 | (255, 0, 100) | (255, 0, 100, 77) |
| H7 | 0x3C00 = 1.0 | 1 / 4 = 0.25 | 2.0, 2, 0 | (40, 50, 60) + 0 * difference of segment 2 (zero) | (40, 50, 60, 77) |
| H8 | 0x3C00, ACCMAX 0 | 1 (0x3C00 >= 0) | as H3 | (255, 0, 100) | (255, 0, 100, 77) |
| H9 | 0x3C00, ACCMAX 0x7E00 (NaN) | 0 (S2: ACCMAX NaN) | as H2 | (1, 2, 3) | (1, 2, 3, 77) |
| H10 | 0x7C00 = +infinity | 1 (0x7C00 >= 0x4400) | as H3 | (255, 0, 100) | (255, 0, 100, 77) |
| H11 | 0x1000 = 2^-11 | 2^-11 / 4 = 2^-13 | 2^-10, 0, 2^-10 | (1, 2, 3) + 2^-10 * (0, 0, 0) | (1, 2, 3, 77) |
| H12 | 0x8000 = -0 | 0 (S2: the +-0 test comes before the compare, `unverified`; the unsigned compare alone would give 1) | as H2 | (1, 2, 3) | (1, 2, 3, 77) |
| H13 | 0x4180 = 2.75 | 2.75 / 4 = 0.6875 | 5.5, 5, 0.5 | (200 + 0.5 * -30, 100 + 0.5 * 0, 90 + 0.5 * -40) = (185, 100, 70) | (185, 100, 70, 77) |

**End to end.** G1a, G1b and G1c as three draws of one recording followed by the shading draw H1's state: the shading draw reads the
sum 1.75 that the three left (each draw's feedback copy is taken after the one before it), so the output is (110, 195, 50, 77).

All of this is asserted by `gpu_fog_test` as literals, in the specialised shader and in the ubershader (identical pixels), on
lavapipe; a small CPU model of D1 to D6 covers nine more (fragment, buffer) pairs whose sums are exact in binary16.

## 6. Open points

Each one is a choice above marked `unverified`, or something the model leaves out.

1. **The density's source (D3) and the rounding (D5).** That the TEV output's blue and alpha bytes are the fragment's density, that
   the two bytes make a binary16 in that order, and the rounding of an inexact sum (`packHalf2x16`'s, the device's), are choices.
2. **Accumulation (D4) and the clamp.** Addition without a clamp at `ACCMAX`; whether the hardware clamps, attenuates or replaces is open.
3. **`GPUREG_GAS_ATTENUATION`, `GAS_LIGHT_XY`, `GAS_LIGHT_Z`, the line-of-sight byte of `GAS_LIGHT_Z_COLOR` and `GAS_DELTAZ_DEPTH`
   (with its depth function) are not modelled.** 3dbrew names them and gives no formula. The shading density source (0xE0 bit 3,
   "depth density") is not modelled either. The words are in the gas region.
4. **The light factor (S4)** is a placeholder (`t = 1`): its source is unknown.
5. **The table (S5):** which half holds values and which differences, the scale of a difference, the last segment.
6. **The maximum the hardware tracks** (`GAS_ACCMAX_FEEDBACK` bits 16-31) is not produced: it is state that the GPU writes and the
   CPU reads back, and the renderer has no GPU-to-CPU path for it (N2-04 does not render). A game that reads it back to set
   `GAS_ACCMAX` would read the value the command list wrote.
7. **Ordering.** `dst_copy` gives the colour buffer as it was before the draw: fragments of one draw that overlap do not see each
   other, so one draw's overlapping density fragments do not accumulate (the hardware's read-modify-write is per fragment, in
   primitive order). `Needs::ordered_rmw` is not asked for: the composer has no pass for it (`native/` is outside this package).
   Draws are ordered (each takes its own copy).
8. **The fixed-function blend.** The pipeline's blend and logic op come from `fbops::MakeFixedFunctionState` (N2-13), which knows
   nothing of gas. The density pass computes the new density in the shader; if the game's registers have blending on for that draw,
   the pipeline would blend the shader's result with the buffer again. The synthetic draw has blending off. Whether MH4U's gas draws
   blend (the census line `Gas;blend:Add/Add;...` names equations, not the enable bit: the same `blend:Add/Add` is on the `Default`
   and `Shadow` lines, so it is most likely the equation registers' value and says nothing about the enable) is not known; a "gas
   draws replace" rule in `MakeFixedFunctionState` would be a change in N2-13's module.
9. **Both modes set:** the density pass wins (`unverified`). What the census (`docs/research/pica-census.md` section 5, G4) shows:
   10 draws with fog mode Gas and 10 with fragment operation mode Gas, in the same 10 frames, and the configuration line
   `Gas;blend:Add/Add;...` (operation mode Gas) is 10 draws. The census counts a draw once if either mode is Gas and reports 20
   draws for G4, so the two sets of 10 are different draws: 10 density draws (operation mode Gas) and 10 shading draws (fog mode
   Gas, operation mode not Gas: the census shows only that, so `Default` would be an inference), a density pass followed by a shading pass as the model has it, and no draw with both modes. If a
   later census counts them differently and they are the same 10 draws, this rule is the one that matters most for the console
   check, since the shading pass would never run for them.
10. **Negative or NaN densities** in the shading pass follow the unsigned compare (S2); an `ACCMAX` that is negative or NaN gives `t = 0`.

## 7. How it will be checked

Azahar draws no gas, so the image checks of N2-20 leave gas frames out (`--skip-gas-frames`) and this model is checked only
against a real 3DS: `LATER.md` N2o-01 (moved to N3-25, "the gas check on the console", `3ds`). What the console check should settle,
in order of what changes pixels most: the density's source and accumulation (1, 2, 7), the table's layout (5), whether the
attenuation and shading registers matter (3, 4), and the blend (8). N2-20 lists the gas frames of the captures here (section 8).

## 8. Gas frames found in the captures

(N2-20 fills this in.)
