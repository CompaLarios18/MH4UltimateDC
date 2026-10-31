# Spike S: option 1b against option 1 (N2-35)

**Verdict: 1b fails: T2, with every key slower and no key faster. The 1b-specialised pipeline's median cold creation time is 99.4, 105.4 and 111.9 ms in the three runs against option 1's 13.5, 15.8 and 15.1 ms (glslang, module and pipeline creation), and its p95 is 225, 205 and 224 ms against 40, 36 and 49 ms. T1 passes: the driver folds the constants into the same code as the literal shader (equal instruction lines and equal vector registers for all 24 keys, GPU-time ratio of 1.00 at the median).** Every figure is `measured` on one machine, one GPU and one driver version; what it means is marked `inference`; the Mac leg is not run. The decision this asks of the orchestrator: write D5's row as **option 1** (N2-08 builds the two-mode emitter and compiles specialised GLSL with N2-06's glslang; N2-34's specialisation-constant field stays unused).

The tool is `native/tools/spike_1b/` (`mh4n_spike_1b`, built only with `-DMH4N_SPIKE_1B=ON`); the spec is N2-35 in `docs/native/wp/N2a.md`, the design is `docs/native/gpu-fragment-design.md` 3.3 point 4 and 8 (j). The shaders are synthetic; no game data was read or written.

## 1. Device and driver

Printed by the tool at the start of every run (the discrete device, chosen with `--device "RX 6700"`, the substring rule of `docs/native/rhi.md`; it is the only Vulkan device on this PC and the Khronos validation layer is not installed):

```
device: AMD Radeon RX 6700 XT (vendor 0x1002, device 0x73df, discrete)
driver: AMD proprietary driver 26.10.1 (AMD proprietary shader compiler), Vulkan 1.4.315
timestamp period: 10.000 ns, VK_KHR_pipeline_executable_properties: exposed
```

`VK_KHR_pipeline_executable_properties` is **exposed** (this was `unverified` for AMD's Windows driver before this run). The statistics it gives for the fragment stage are only these five, quoted as the driver names them: `numUsedVgprs`, `numUsedSgprs`, `ldsSizePerLocalWorkGroup`, `ldsUsageSizeInBytes`, `scratchMemUsageInBytes`. **There is no instruction-count statistic: the instruction count of T1 is "not exposed"** and T1's code leg rests on the vector registers (`numUsedVgprs`) and on GPU time. As extra evidence (no threshold), the tool also captures the driver's internal representation, which is the ISA disassembly, and counts its instruction lines (section 5).

## 2. What was run

- **One process is one run**, three runs, each with its own `--salt` (section 7 says why): `n2-35-run1-4eefb8614fff`, `n2-35-run2-217b24436f05`, `n2-35-run3-0b7bbed33d80`. The GPU was otherwise idle (no game, no other replay).
- **The shader** is a skeleton of `uber_color`: six TEV stages (a `switch` on every source, modifier and operation, Azahar's enumerators as hardware values), the light loop over `src_num` lights (up to 3) with two texel fetches per enabled LUT from a `samplerBuffer` and the Config0/Config1 selection, and the output stage (alpha test, byte rounding, depth write). No procedural texture, fog or sampling, so its compile time is a lower bound of the full module's (the design accepts this). The ubershader source is written once against `cfg`; every key field is read through `f_<field>()`, which is a `cfg` read (`spec_uber` 1) or a `layout(constant_id = N) const uint` (`spec_uber` 0). It has 43 key fields (constant ids 1 to 43) and `spec_uber` (id 0); the design's 70 are the whole module's. Its SPIR-V is 11,615 words, compiled once before anything is timed with glslang 16.6.0 and no optimizer (N2-06's build has no spirv-tools).
- **Option 1's shader** for a key is emitted by a C++ function of the spike (`LiteralSource`): only the chosen case of each switch, the light loop unrolled to the key's count, the values as literals, and the identity TEV stages left out (Azahar's pass-through rule). Literal SPIR-V is 1,150 to 6,004 words.
- **Miss latency**: 1b is `vkCreateGraphicsPipelines` of the ubershader module with the key's constants and `spec_uber` 0; option 1 is glslang's compile of the literal GLSL, `vkCreateShaderModule`, then `vkCreateGraphicsPipelines`. No `VkPipelineCache` in either. The first glslang call (its table initialisation, 0.01 ms) and one warm-up pipeline are timed apart and left out; the two creations alternate in order from key to key.
- **Code quality**: GPU time between two timestamps (`TOP_OF_PIPE`, `BOTTOM_OF_PIPE`) around 20 full-screen triangles into a 3440x1440 RGBA8 target with a D32 depth target (depth test always, depth written by the shader), median of 5 repetitions, 6 untimed draws per pipeline before, the three pipelines (literal, 1b, ubershader) interleaved with a rotating order, synthetic uniforms and LUT ramps. Statistics come from pipelines created again with the capture flags (the timed pipelines have none).
- **Validity**: for each key, the 1b-specialised, the literal and the ubershader pipelines draw the same 256x256 frame; the check is ROADMAP section 6's (PSNR at least 40 dB, no 16x16 block with a mean error above 8/255), plus a count of distinct colours (at least 64, so a blank image cannot pass) and a sensitivity count (do the images of two neighbouring keys differ?). A key that failed would have stopped the run before any measurement.

### The keys (24, synthetic)

Keys 0 to 5 are unlit with 1 to 6 active TEV stages (replace, modulate, add, add-signed, interpolate, dot3). Keys 6 to 20 are lit with 4 TEV stages: 1, 2 or 3 lights times the five LUT sets of the census (none; FR+RR; DA; D0+RR; DA+FR+RR), Config1 for the two sets with FR+RR and Config0 for the others (the census does not pair them: `inference`). Keys 21 to 23 have 1, 2 and 3 lights, DA+FR+RR, bump NormalMap and 6 TEV stages (and W-buffered depth, so both depth modes occur).

| key | description | TEV stages | lights |
|---|---|---|---|
| k00 | unlit, 1 TEV stage | 1 | 0 |
| k01 | unlit, 2 TEV stages | 2 | 0 |
| k02 | unlit, 3 TEV stages | 3 | 0 |
| k03 | unlit, 4 TEV stages | 4 | 0 |
| k04 | unlit, 5 TEV stages | 5 | 0 |
| k05 | unlit, 6 TEV stages | 6 | 0 |
| k06 | lit, 1 light, no LUT, Config0, 4 TEV stages | 4 | 1 |
| k07 | lit, 1 light, FR+RR, Config1, 4 TEV stages | 4 | 1 |
| k08 | lit, 1 light, DA, Config0, 4 TEV stages | 4 | 1 |
| k09 | lit, 1 light, D0+RR, Config0, 4 TEV stages | 4 | 1 |
| k10 | lit, 1 light, DA+FR+RR, Config1, 4 TEV stages | 4 | 1 |
| k11 | lit, 2 lights, no LUT, Config0, 4 TEV stages | 4 | 2 |
| k12 | lit, 2 lights, FR+RR, Config1, 4 TEV stages | 4 | 2 |
| k13 | lit, 2 lights, DA, Config0, 4 TEV stages | 4 | 2 |
| k14 | lit, 2 lights, D0+RR, Config0, 4 TEV stages | 4 | 2 |
| k15 | lit, 2 lights, DA+FR+RR, Config1, 4 TEV stages | 4 | 2 |
| k16 | lit, 3 lights, no LUT, Config0, 4 TEV stages | 4 | 3 |
| k17 | lit, 3 lights, FR+RR, Config1, 4 TEV stages | 4 | 3 |
| k18 | lit, 3 lights, DA, Config0, 4 TEV stages | 4 | 3 |
| k19 | lit, 3 lights, D0+RR, Config0, 4 TEV stages | 4 | 3 |
| k20 | lit, 3 lights, DA+FR+RR, Config1, 4 TEV stages | 4 | 3 |
| k21 | lit, 1 light, DA+FR+RR, Config1, bump NormalMap, 6 TEV stages | 6 | 1 |
| k22 | lit, 2 lights, DA+FR+RR, Config1, bump NormalMap, 6 TEV stages | 6 | 2 |
| k23 | lit, 3 lights, DA+FR+RR, Config1, bump NormalMap, 6 TEV stages | 6 | 3 |

## 3. T2: miss latency

Milliseconds, cold (no `VkPipelineCache`, and the shader modules salted so the driver's own cache cannot answer: section 7). Option 1 is glslang + `vkCreateShaderModule` + creation, summed.

| key | 1b run 1 | 1b run 2 | 1b run 3 | option 1 run 1 | option 1 run 2 | option 1 run 3 | 1b / option 1 (medians of the runs) |
|---|---|---|---|---|---|---|---|
| k00 | 91.3 | 98.7 | 103.2 | 6.1 | 6.3 | 6.2 | 16.0 |
| k01 | 99.5 | 97.1 | 109.0 | 11.3 | 8.1 | 8.3 | 12.1 |
| k02 | 98.4 | 98.8 | 99.9 | 8.2 | 8.4 | 8.2 | 12.1 |
| k03 | 92.6 | 93.1 | 104.2 | 6.4 | 6.0 | 6.0 | 15.5 |
| k04 | 93.6 | 99.0 | 96.0 | 13.8 | 10.8 | 10.5 | 8.9 |
| k05 | 99.3 | 97.3 | 95.7 | 8.7 | 9.5 | 9.8 | 10.3 |
| k06 | 105.1 | 97.2 | 109.2 | 12.3 | 14.0 | 12.6 | 8.4 |
| k07 | 94.1 | 102.5 | 102.4 | 12.6 | 15.4 | 12.9 | 8.0 |
| k08 | 90.0 | 103.1 | 105.5 | 10.7 | 11.7 | 12.2 | 8.8 |
| k09 | 91.1 | 112.8 | 106.0 | 12.1 | 12.8 | 12.4 | 8.6 |
| k10 | 93.1 | 105.0 | 166.1 | 13.2 | 15.7 | 14.8 | 7.1 |
| k11 | 96.6 | 105.8 | 102.5 | 11.7 | 19.1 | 12.7 | 8.0 |
| k12 | 198.7 | 204.3 | 216.7 | 31.0 | 27.8 | 35.4 | 6.6 |
| k13 | 223.6 | 204.9 | 207.6 | 26.3 | 22.6 | 25.1 | 8.3 |
| k14 | 253.0 | 274.7 | 213.1 | 29.1 | 25.1 | 26.6 | 9.5 |
| k15 | 225.4 | 201.8 | 289.3 | 35.7 | 29.3 | 36.3 | 6.3 |
| k16 | 94.0 | 100.7 | 104.8 | 12.8 | 12.8 | 15.3 | 7.9 |
| k17 | 186.3 | 185.9 | 224.4 | 34.6 | 32.2 | 56.7 | 5.4 |
| k18 | 90.7 | 92.6 | 114.5 | 15.8 | 15.8 | 17.8 | 5.8 |
| k19 | 183.3 | 199.8 | 216.9 | 31.7 | 30.0 | 41.8 | 6.3 |
| k20 | 190.1 | 201.6 | 194.4 | 40.0 | 36.3 | 41.5 | 4.9 |
| k21 | 180.2 | 168.8 | 210.2 | 28.5 | 27.7 | 30.8 | 6.3 |
| k22 | 178.6 | 170.4 | 195.9 | 34.7 | 32.9 | 37.1 | 5.1 |
| k23 | 176.9 | 183.0 | 193.6 | 41.3 | 41.0 | 49.2 | 4.4 |

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| 1b median / p95 | 99.4 / 225.4 | 105.4 / 204.9 | 111.9 / 224.4 |
| option 1 median / p95 | 13.5 / 40.0 | 15.8 / 36.3 | 15.1 / 49.2 |
| median 1b / median option 1 | 7.4 | 6.7 | 7.4 |
| option 1 parts, medians over the keys: glslang / module / creation | 2.21 / 0.015 / 11.7 | 2.42 / 0.016 / 12.7 | 2.37 / 0.016 / 12.6 |
| emitting the literal GLSL (C++ strings, not in the total), median | 0.020 | 0.020 | 0.021 |

`measured`: in all 72 key-runs 1b is slower than option 1; the smallest per-key ratio is 3.9 and the largest 17.4. 1b's times fall in two groups: 90 to 166 ms for 14 keys (k00, k01, k02, k03, k04, k05, k06, k07, k08, k09, k10, k11, k16, k18) and 169 to 289 ms for 10 keys (k12, k13, k14, k15, k17, k19, k20, k21, k22, k23; median of the three runs at or above 130 ms). The module is the same for every key, so what separates the groups is what the driver has to unroll and fold for that key, not the module's size (`inference`). Option 1's total grows with the key too, from 6.0 to 56.7 ms. Neither side is dominated by glslang (1.2 to 4.5 ms for the skeleton): the cost is the driver's compile, and 1b's driver compile is the larger.

## 4. T1: code quality, GPU time

GPU time of 20 full-screen triangles into 3440x1440 RGBA8 + D32, median of 5, in milliseconds. The floor of this target (a shader that only writes its output) is 2.336, 2.346 and 2.345 ms in the three runs (about 340 GB/s of colour and depth writes, close to the card's 384 GB/s of memory bandwidth, memory), and the literal shaders of 8, 8 and 9 of the 24 keys run within 10 percent of it: for those keys GPU time cannot tell two shaders apart and the register and instruction evidence of section 5 carries the comparison.

| key | literal run 1 | literal run 2 | literal run 3 | 1b / literal run 1 | 1b / literal run 2 | 1b / literal run 3 |
|---|---|---|---|---|---|---|
| k00 | 2.369 | 2.387 | 2.358 | 0.998 | 1.122 | 0.999 |
| k01 | 2.381 | 2.378 | 2.365 | 1.151 | 0.989 | 0.993 |
| k02 | 2.400 | 2.389 | 2.377 | 0.998 | 1.125 | 1.125 |
| k03 | 2.363 | 2.363 | 2.373 | 1.000 | 1.000 | 1.000 |
| k04 | 2.411 | 2.377 | 2.388 | 0.995 | 1.000 | 1.000 |
| k05 | 2.693 | 2.676 | 2.363 | 0.888 | 0.890 | 1.000 |
| k06 | 2.447 | 2.431 | 2.416 | 0.993 | 1.131 | 0.999 |
| k07 | 3.072 | 3.069 | 3.066 | 1.000 | 1.001 | 1.000 |
| k08 | 2.464 | 2.433 | 2.429 | 1.139 | 1.000 | 1.000 |
| k09 | 2.796 | 2.792 | 2.789 | 1.110 | 1.000 | 1.000 |
| k10 | 3.609 | 3.296 | 3.296 | 1.001 | 1.000 | 1.000 |
| k11 | 2.458 | 2.432 | 2.430 | 0.995 | 1.000 | 1.000 |
| k12 | 4.181 | 3.938 | 3.937 | 0.944 | 1.000 | 1.000 |
| k13 | 2.857 | 2.847 | 2.844 | 1.000 | 1.000 | 1.000 |
| k14 | 4.094 | 3.787 | 3.786 | 1.003 | 1.000 | 1.000 |
| k15 | 4.714 | 4.397 | 4.391 | 0.983 | 1.000 | 1.000 |
| k16 | 3.170 | 2.853 | 2.849 | 0.901 | 1.000 | 1.000 |
| k17 | 5.306 | 5.027 | 5.027 | 0.949 | 1.000 | 1.000 |
| k18 | 3.683 | 3.678 | 3.677 | 1.073 | 1.000 | 1.000 |
| k19 | 4.958 | 4.954 | 4.949 | 0.996 | 1.000 | 1.000 |
| k20 | 5.975 | 5.688 | 5.761 | 0.978 | 1.010 | 1.004 |
| k21 | 4.313 | 3.992 | 3.989 | 0.999 | 1.000 | 1.000 |
| k22 | 5.227 | 5.222 | 5.220 | 1.030 | 1.000 | 1.000 |
| k23 | 6.656 | 6.633 | 6.625 | 1.022 | 1.001 | 1.003 |

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| median of the 24 ratios | 0.998 | 1.000 | 1.000 |
| largest ratio (key) | 1.151 (k01) | 1.131 (k06) | 1.125 (k02) |

The largest ratios are on keys whose code is identical (section 5) and whose time is at the floor, so they are repetition noise, not code. The tool prints the spread of the five repetitions of each pipeline, (max - min) / median, and the order of the three pipelines in a repetition rotates, so neither side is always first:

- run 1: `repetition noise, (max - min) / median over the keys: literal median 12.0% (max 16.9%), 1b median 8.9% (max 17.5%)`
- run 2: `repetition noise, (max - min) / median over the keys: literal median 0.8% (max 24.8%), 1b median 0.9% (max 34.4%)`
- run 3: `repetition noise, (max - min) / median over the keys: literal median 0.2% (max 16.1%), 1b median 0.7% (max 34.1%)`

Over the 72 key-runs the ratio goes from 0.888 to 1.151 while every pair has the same register count and the same number of ISA lines (section 5). A ratio away from 1.00 on such a pair shows in both directions and does not repeat from run to run (k05's literal shader is 12 percent slower than its 1b twin in runs 1 and 2 and equal in run 3), so it is a property of the process, such as where the driver puts the code or the clocks of the moment, not a failure to fold (`inference`). It also means the per-key cap of 1.25 has about 0.1 of margin over what identical code shows on this machine.

## 5. T1: registers and instructions

The fragment-stage statistics of key k00 as the driver names them (the other keys have the same five names):

| executable | statistic | literal | 1b specialised |
|---|---|---|---|
| ShaderProperties.ps | numUsedVgprs | 6 | 6 |
| ShaderProperties.ps | numUsedSgprs | 14 | 14 |
| ShaderProperties.ps | ldsSizePerLocalWorkGroup | 65536 | 65536 |
| ShaderProperties.ps | ldsUsageSizeInBytes | 0 | 0 |
| ShaderProperties.ps | scratchMemUsageInBytes | 0 | 0 |

The instruction count is `not exposed` as a statistic. The last three columns are the lines of the driver's ISA disassembly (its internal representation) that are instructions, not blank, not labels: **informative, with no threshold**. Registers and ISA lines were identical in the three runs, so one table:

| key | `numUsedVgprs` literal | 1b | ratio | ISA lines literal | 1b | ratio |
|---|---|---|---|---|---|---|
| k00 | 6 | 6 | 1.000 | 118 | 118 | 1.000 |
| k01 | 8 | 8 | 1.000 | 134 | 134 | 1.000 |
| k02 | 12 | 12 | 1.000 | 149 | 149 | 1.000 |
| k03 | 11 | 11 | 1.000 | 158 | 158 | 1.000 |
| k04 | 14 | 14 | 1.000 | 181 | 181 | 1.000 |
| k05 | 7 | 7 | 1.000 | 139 | 139 | 1.000 |
| k06 | 14 | 14 | 1.000 | 225 | 225 | 1.000 |
| k07 | 19 | 19 | 1.000 | 297 | 297 | 1.000 |
| k08 | 19 | 19 | 1.000 | 249 | 249 | 1.000 |
| k09 | 20 | 20 | 1.000 | 285 | 285 | 1.000 |
| k10 | 19 | 19 | 1.000 | 316 | 316 | 1.000 |
| k11 | 16 | 16 | 1.000 | 255 | 255 | 1.000 |
| k12 | 33 | 33 | 1.000 | 370 | 370 | 1.000 |
| k13 | 27 | 27 | 1.000 | 297 | 297 | 1.000 |
| k14 | 29 | 29 | 1.000 | 363 | 363 | 1.000 |
| k15 | 43 | 43 | 1.000 | 406 | 406 | 1.000 |
| k16 | 20 | 20 | 1.000 | 288 | 288 | 1.000 |
| k17 | 44 | 44 | 1.000 | 446 | 446 | 1.000 |
| k18 | 35 | 35 | 1.000 | 351 | 351 | 1.000 |
| k19 | 43 | 43 | 1.000 | 444 | 444 | 1.000 |
| k20 | 56 | 56 | 1.000 | 500 | 500 | 1.000 |
| k21 | 31 | 31 | 1.000 | 371 | 371 | 1.000 |
| k22 | 48 | 48 | 1.000 | 460 | 460 | 1.000 |
| k23 | 61 | 61 | 1.000 | 554 | 554 | 1.000 |

`measured`: the median `numUsedVgprs` ratio is 1.000 (largest 1.000) and the median ISA-line ratio is 1.000 (smallest 1.000, largest 1.000): for all 24 keys the driver's output for the 1b module with the key's constants is the same size as for the literal shader. `inference`: the driver folds the `switch` on a constant, the `if` on `spec_uber` and the light loop with a constant bound, so T1's open question (does it fold them as tight as literals) is answered yes on this driver.

## 6. Validity

256x256 RGBA8, 1b specialised against literal (the ubershader column is its largest difference from the literal image, informative). Identical in the three runs.

| key | bit-equal | max diff | PSNR dB (99 = identical) | max 16x16 block mean | distinct colours | ubershader: max diff |
|---|---|---|---|---|---|---|
| k00 | yes | 0 | 99.00 | 0.000 | 65408 | 0 |
| k01 | yes | 0 | 99.00 | 0.000 | 45529 | 0 |
| k02 | yes | 0 | 99.00 | 0.000 | 31622 | 0 |
| k03 | yes | 0 | 99.00 | 0.000 | 33251 | 0 |
| k04 | yes | 0 | 99.00 | 0.000 | 36646 | 0 |
| k05 | yes | 0 | 99.00 | 0.000 | 207 | 0 |
| k06 | yes | 0 | 99.00 | 0.000 | 24352 | 0 |
| k07 | yes | 0 | 99.00 | 0.000 | 20482 | 1 |
| k08 | yes | 0 | 99.00 | 0.000 | 24340 | 0 |
| k09 | yes | 0 | 99.00 | 0.000 | 25724 | 1 |
| k10 | yes | 0 | 99.00 | 0.000 | 20855 | 0 |
| k11 | yes | 0 | 99.00 | 0.000 | 31153 | 1 |
| k12 | yes | 0 | 99.00 | 0.000 | 22915 | 0 |
| k13 | yes | 0 | 99.00 | 0.000 | 31323 | 0 |
| k14 | yes | 0 | 99.00 | 0.000 | 28465 | 0 |
| k15 | yes | 0 | 99.00 | 0.000 | 24241 | 1 |
| k16 | yes | 0 | 99.00 | 0.000 | 30852 | 0 |
| k17 | yes | 0 | 99.00 | 0.000 | 22442 | 0 |
| k18 | yes | 0 | 99.00 | 0.000 | 31422 | 0 |
| k19 | yes | 0 | 99.00 | 0.000 | 29264 | 0 |
| k20 | yes | 0 | 99.00 | 0.000 | 22812 | 0 |
| k21 | yes | 0 | 99.00 | 0.000 | 19461 | 0 |
| k22 | yes | 0 | 99.00 | 0.000 | 27408 | 1 |
| k23 | yes | 0 | 99.00 | 0.000 | 24999 | 0 |

`measured`: all 24 keys are valid, 24 of 24 bit-equal, in all three runs, so no defect of the harness had to be fixed before measuring. The check can see a difference: the images of 23 of 23 neighbouring key pairs differ (the lighting constants are small enough not to saturate, which an earlier setting of the synthetic uniforms did, and which the distinct-colour and sensitivity columns showed). The ubershader differs from the literal image by at most 1 level of 255, on 5 keys (float contraction; within the thresholds).

## 7. The driver's own shader cache

`measured`: AMD's Windows driver keeps a cache across processes that answers a pipeline creation with no `VkPipelineCache`. A first smoke run of the tool created keys k00 to k03, and the next process, with the same SPIR-V, created them in 0.06 to 0.13 ms instead of 85 to 100 ms. The check below repeats this on all 24 keys with a fixed salt:

| process | 1b median / p95 (ms) | option 1 median / p95 (ms) |
|---|---|---|
| first, fixed salt `n2-35-cachecheck-fixed-A` | 112.0 / 203.3 | 13.7 / 38.8 |
| second, same salt | 0.07 / 0.16 | 2.38 / 4.02 |

So a rerun is a cache hit for both, and compared like that 1b would even pass T2 (0.07 ms against 2.4 ms). That is not a cold creation, which is what T2 and the miss path of design 4.5 are about (the first run on a fresh install, before any list or cache is warm). The spec's rule is to turn the cache off if the driver documents a way, and otherwise to compare first runs only. The driver's way is a setting of AMD Software (not a Vulkan call), and changing a system setting was not part of this package, so the harness does the other thing the spec allows: `--salt` adds an `OpSourceExtension` instruction with a fresh text to every SPIR-V module of the run (the vertex shader, the ubershader and each literal shader), which changes the module's hash and nothing else (the registers and ISA line counts of all 24 keys are the same with and without it: checked against an unsalted run). The three official runs use three distinct salts, so each is a first run of every pipeline. Whether AMD's cache is keyed by the module's hash is `inference` from this behaviour.

## 8. Informative: the ubershader (`spec_uber` 1, the key in the block)

No threshold. This is the input for D5's option-3 question; it is a skeleton at 20 layers of full-screen triangles, so the numbers say little about the full module.

| key | ubershader run 1 | run 2 | run 3 (ms) | ubershader / literal run 1 | run 2 | run 3 |
|---|---|---|---|---|---|---|
| k00 | 12.36 | 12.74 | 12.44 | 5.21 | 5.34 | 5.28 |
| k01 | 12.44 | 11.98 | 12.61 | 5.23 | 5.04 | 5.33 |
| k02 | 12.21 | 12.31 | 12.25 | 5.09 | 5.15 | 5.15 |
| k03 | 11.69 | 12.25 | 12.23 | 4.95 | 5.18 | 5.16 |
| k04 | 11.59 | 12.19 | 12.23 | 4.81 | 5.13 | 5.12 |
| k05 | 12.25 | 11.92 | 12.01 | 4.55 | 4.45 | 5.08 |
| k06 | 13.00 | 13.23 | 13.10 | 5.31 | 5.44 | 5.42 |
| k07 | 13.09 | 14.27 | 18.86 | 4.26 | 4.65 | 6.15 |
| k08 | 13.36 | 13.02 | 13.37 | 5.42 | 5.35 | 5.50 |
| k09 | 13.58 | 18.58 | 18.52 | 4.86 | 6.65 | 6.64 |
| k10 | 13.80 | 18.97 | 18.52 | 3.82 | 5.75 | 5.62 |
| k11 | 14.04 | 14.02 | 15.42 | 5.71 | 5.77 | 6.34 |
| k12 | 15.88 | 20.28 | 19.88 | 3.80 | 5.15 | 5.05 |
| k13 | 14.89 | 19.53 | 19.61 | 5.21 | 6.86 | 6.90 |
| k14 | 14.32 | 19.99 | 19.56 | 3.50 | 5.28 | 5.17 |
| k15 | 15.47 | 20.65 | 20.20 | 3.28 | 4.70 | 4.60 |
| k16 | 15.61 | 19.70 | 20.01 | 4.93 | 6.90 | 7.02 |
| k17 | 17.03 | 21.72 | 21.82 | 3.21 | 4.32 | 4.34 |
| k18 | 15.46 | 20.30 | 20.40 | 4.20 | 5.52 | 5.55 |
| k19 | 16.39 | 21.18 | 21.06 | 3.30 | 4.28 | 4.25 |
| k20 | 18.08 | 22.41 | 22.12 | 3.03 | 3.94 | 3.84 |
| k21 | 13.77 | 19.04 | 19.08 | 3.19 | 4.77 | 4.78 |
| k22 | 15.56 | 20.71 | 20.20 | 2.98 | 3.97 | 3.87 |
| k23 | 22.53 | 22.53 | 22.64 | 3.39 | 3.40 | 3.42 |

- Ubershader / literal GPU-time ratio: median 4.40, 5.15, 5.17 (smallest 2.98, largest 7.02) in the three runs. `inference`: this is above the 1.7 to 2.3 times of design 4.4.3 (an `estimate`), for this skeleton, because the ubershader runs all six TEV stages and the dynamic switches even for the unlit keys, and it spends 11.6 to 22.6 ms per 20 layers (0.6 to 1.1 ms per full-screen layer) against the literal shader's 2.4 to 6.7 ms.
- Ubershader creation (cold): 449, 440, 480 ms for the first creation in each run; the later creations of the same module and constants in the same process take about 0.06 ms (an in-process hit), so only the first number is a creation time.

## 9. T1 and T2 evaluated

| threshold | run 1 | run 2 | run 3 |
|---|---|---|---|
| T1 GPU time: median of the 1b / literal ratios at most 1.10 | 0.998 yes | 1.000 yes | 1.000 yes |
| T1 GPU time: no key above 1.25 | max 1.151 yes | max 1.131 yes | max 1.125 yes |
| T1 instruction-count ratio, median at most 1.10 | not exposed | not exposed | not exposed |
| T1 vector-register ratio (`numUsedVgprs`), median at most 1.10 | 1.000 yes | 1.000 yes | 1.000 yes |
| T2 median creation of 1b at most option 1's | 99.4 against 13.5 NO | 105.4 against 15.8 NO | 111.9 against 15.1 NO |
| T2 p95 creation of 1b at most option 1's | 225.4 against 40.0 NO | 204.9 against 36.3 NO | 224.4 against 49.2 NO |
| every key valid | 24 of 24 | 24 of 24 | 24 of 24 |

## 10. Verdict

**1b fails: T2, median 105 ms against 15 ms and p95 224 ms against 40 ms (medians of the three runs; the ratio of the medians is 7.4 and every key is at least 3.9 times slower). T1 passes.**

What each outcome changes is in the spec (N2-35, *What each outcome changes*); this fails, so the orchestrator writes D5's row as option 1: N2-08 builds the two-mode emitter and compiles specialised GLSL with N2-06's glslang; N2-23 and N2-09 to N2-12 write both modes of each helper; N2-16's workers generate and compile; N2-34's specialisation-constant field stays unused. The thresholds were not changed.

What limits the reading (none of it changes the verdict by itself):

- **One GPU, one driver** (the RX 6700 XT, AMD proprietary driver 26.10.1); the PC has no lavapipe, so the harness was not run on a software device. The Mac leg waits for the Mac (D95). Constants are core Vulkan, so a later failure there would make 1b slower on that device, not broken (`inference`, as the spec says); the PC's result is the one that fails already.
- **The module is a skeleton**, a lower bound of the full `uber_color` for 1b's compile time (the full one has procedural textures, fog and sampling). Option 1's literal shader grows too, so the ratio need not grow, but the absolute 1b cost will not be smaller (`inference`). N2-39 re-measures the full module only if the pass had been provisional; a fail needs no re-measure.
- **No optimizer**: N2-06's glslang has no spirv-tools, so neither the 1b module nor the literal shaders went through spirv-opt. An offline optimization of the 1b module cannot fold the key's values (they are unknown offline), so the specialisation work the driver does at creation would remain (`inference`; not measured, spirv-opt is not on this PC).
- **Creation is measured on one thread**, synchronously. N2-16 builds on worker threads, so a miss costs the time its draws stay on the ubershader, not a stall; with 1b that time is 90 to 289 ms per pipeline against 6 to 57 ms for option 1 (glslang included), and the ubershader's own first creation costs about 450 ms. Parallel creation on several workers was not measured.
- **Warm driver cache**: a rerun is a hit for both options (section 7), and then both are far below a frame. The result concerns the first creation of each pipeline on a machine, which the pipeline lists and N2-16's cache are built to move out of the play session.
- **GPU time at the floor**: 8 or 9 of the 24 literal shaders are within 10 percent of the target's bandwidth floor, where GPU time cannot show a code difference; for them the equal register counts and equal ISA lines are the evidence. The other keys are above the floor, from 1.14 to 2.85 times it, and their ratios are 1.00 within the noise.

- **Provenance**: the literal emitter of option 1 (`shaders.cpp`) follows Azahar's `src/video_core/shader/generator/glsl_fs_shader_gen.cpp` (`IsPassThroughTevStage`, `WriteTevStage`, the colour and alpha modifier and combiner expressions, `WriteAlphaTestCondition`, `WriteLighting`), simplified, at the pinned commit `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` (GPLv3 or later, so `shaders.cpp` is GPL-3.0-or-later). `key.h` takes the names and values of the TEV and lighting-input enumerations of `src/video_core/pica/regs_texturing.h` and `regs_lighting.h` (GPLv2 or later). Both files carry D6's header and need a row in `native/THIRD_PARTY.md`. An earlier version of this document and of the file header said that no Azahar file had been opened: that was wrong. The ubershader source, the uniform layout, the texture stand-ins, the LUT fetch and the harness are the spike's own. The numbers do not depend on this: the emitted GLSL is unchanged.

## 11. How to reproduce

```
cmake -S native -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON -DMH4N_SPIKE_1B=ON <the compilers of the platform>
cmake --build build/spike --target mh4n_spike_1b
build/spike/bin/mh4n_spike_1b --device "RX 6700" --salt <a text that changed since the last run>
```

`--keys N` runs the first N keys only (a smoke test), `--reps N` sets the repetitions (5), `--dump DIR` writes the GLSL of the ubershader and of every literal shader, and the ISA of the fragment stages, to DIR, `--force` measures even if a key were invalid. With the option off (the default) nothing of the spike is configured: no target `mh4n_spike_1b` exists, and the three CI jobs compile nothing of it. The build on the PC used llvm-mingw 20241217 (clang 19.1.6) with the glslang of `native/third_party/glslang` and the volk of `native/third_party/rhi/volk`.

## Appendix: the tool's tables for the three runs

### Run 1

Miss latency, milliseconds (columns as the tool prints them):

| key | 1b create | opt1 glslang | opt1 module | opt1 create | opt1 total | 1b / opt1 | uber create | emit (not in total) | literal SPIR-V words |
|---|---|---|---|---|---|---|---|---|---|
| k00 | 91.26 | 1.17 | 0.009 | 4.92 | 6.10 | 14.96 | 449.10 | 0.020 | 1150 |
| k01 | 99.55 | 2.19 | 0.035 | 9.10 | 11.32 | 8.79 | 0.06 | 0.012 | 1422 |
| k02 | 98.44 | 1.45 | 0.016 | 6.69 | 8.16 | 12.07 | 0.18 | 0.016 | 1682 |
| k03 | 92.56 | 1.42 | 0.014 | 4.94 | 6.37 | 14.54 | 0.07 | 0.009 | 1891 |
| k04 | 93.61 | 1.58 | 0.018 | 12.17 | 13.77 | 6.80 | 0.06 | 0.015 | 2265 |
| k05 | 99.29 | 1.61 | 0.016 | 7.09 | 8.72 | 11.39 | 0.08 | 0.013 | 2459 |
| k06 | 105.09 | 1.94 | 0.014 | 10.40 | 12.35 | 8.51 | 0.16 | 0.017 | 2923 |
| k07 | 94.07 | 2.05 | 0.014 | 10.53 | 12.60 | 7.47 | 0.06 | 0.013 | 3449 |
| k08 | 90.01 | 2.00 | 0.013 | 8.73 | 10.74 | 8.38 | 0.05 | 0.020 | 3325 |
| k09 | 91.11 | 1.99 | 0.013 | 10.09 | 12.09 | 7.54 | 0.06 | 0.013 | 3325 |
| k10 | 93.11 | 2.06 | 0.015 | 11.16 | 13.24 | 7.03 | 0.08 | 0.017 | 3539 |
| k11 | 96.62 | 2.15 | 0.013 | 9.49 | 11.65 | 8.30 | 0.07 | 0.024 | 3530 |
| k12 | 198.67 | 2.36 | 0.014 | 28.60 | 30.97 | 6.41 | 0.07 | 0.023 | 4270 |
| k13 | 223.63 | 2.56 | 0.019 | 23.77 | 26.34 | 8.49 | 0.18 | 0.023 | 4018 |
| k14 | 252.97 | 3.54 | 0.026 | 25.57 | 29.14 | 8.68 | 0.06 | 0.028 | 4022 |
| k15 | 225.43 | 2.51 | 0.019 | 33.17 | 35.70 | 6.31 | 0.06 | 0.020 | 4446 |
| k16 | 93.95 | 2.37 | 0.016 | 10.41 | 12.80 | 7.34 | 0.05 | 0.020 | 4138 |
| k17 | 186.34 | 2.48 | 0.014 | 32.08 | 34.58 | 5.39 | 0.07 | 0.020 | 5092 |
| k18 | 90.65 | 2.67 | 0.015 | 13.14 | 15.82 | 5.73 | 0.06 | 0.021 | 4712 |
| k19 | 183.35 | 2.39 | 0.020 | 29.27 | 31.68 | 5.79 | 0.06 | 0.019 | 4720 |
| k20 | 190.12 | 3.02 | 0.020 | 36.98 | 40.02 | 4.75 | 0.06 | 0.032 | 5354 |
| k21 | 180.20 | 2.22 | 0.015 | 26.24 | 28.48 | 6.33 | 0.06 | 0.019 | 4189 |
| k22 | 178.56 | 2.69 | 0.018 | 31.99 | 34.70 | 5.15 | 0.06 | 0.026 | 5096 |
| k23 | 176.88 | 3.06 | 0.016 | 38.18 | 41.26 | 4.29 | 0.06 | 0.024 | 6004 |

GPU time, milliseconds:

| key | literal | 1b specialised | 1b / literal | ubershader | uber / literal |
|---|---|---|---|---|---|
| k00 | 2.369 | 2.364 | 0.998 | 12.355 | 5.21 |
| k01 | 2.381 | 2.740 | 1.151 | 12.442 | 5.23 |
| k02 | 2.400 | 2.395 | 0.998 | 12.205 | 5.09 |
| k03 | 2.363 | 2.362 | 1.000 | 11.691 | 4.95 |
| k04 | 2.411 | 2.398 | 0.995 | 11.590 | 4.81 |
| k05 | 2.693 | 2.393 | 0.888 | 12.252 | 4.55 |
| k06 | 2.447 | 2.429 | 0.993 | 13.002 | 5.31 |
| k07 | 3.072 | 3.072 | 1.000 | 13.088 | 4.26 |
| k08 | 2.464 | 2.806 | 1.139 | 13.364 | 5.42 |
| k09 | 2.796 | 3.104 | 1.110 | 13.577 | 4.86 |
| k10 | 3.609 | 3.614 | 1.001 | 13.802 | 3.82 |
| k11 | 2.458 | 2.446 | 0.995 | 14.036 | 5.71 |
| k12 | 4.181 | 3.945 | 0.944 | 15.878 | 3.80 |
| k13 | 2.857 | 2.856 | 1.000 | 14.894 | 5.21 |
| k14 | 4.094 | 4.105 | 1.003 | 14.316 | 3.50 |
| k15 | 4.714 | 4.635 | 0.983 | 15.475 | 3.28 |
| k16 | 3.170 | 2.856 | 0.901 | 15.614 | 4.93 |
| k17 | 5.306 | 5.034 | 0.949 | 17.026 | 3.21 |
| k18 | 3.683 | 3.953 | 1.073 | 15.456 | 4.20 |
| k19 | 4.958 | 4.940 | 0.996 | 16.386 | 3.30 |
| k20 | 5.975 | 5.842 | 0.978 | 18.078 | 3.03 |
| k21 | 4.313 | 4.307 | 0.999 | 13.768 | 3.19 |
| k22 | 5.227 | 5.385 | 1.030 | 15.556 | 2.98 |
| k23 | 6.656 | 6.803 | 1.022 | 22.533 | 3.39 |

### Run 2

Miss latency, milliseconds (columns as the tool prints them):

| key | 1b create | opt1 glslang | opt1 module | opt1 create | opt1 total | 1b / opt1 | uber create | emit (not in total) | literal SPIR-V words |
|---|---|---|---|---|---|---|---|---|---|
| k00 | 98.70 | 1.17 | 0.009 | 5.12 | 6.30 | 15.66 | 440.43 | 0.011 | 1150 |
| k01 | 97.12 | 1.28 | 0.027 | 6.74 | 8.05 | 12.07 | 0.08 | 0.011 | 1422 |
| k02 | 98.79 | 1.38 | 0.016 | 7.00 | 8.39 | 11.77 | 0.16 | 0.012 | 1682 |
| k03 | 93.09 | 1.43 | 0.013 | 4.53 | 5.97 | 15.59 | 0.07 | 0.010 | 1891 |
| k04 | 98.97 | 1.64 | 0.014 | 9.11 | 10.76 | 9.19 | 0.05 | 0.015 | 2265 |
| k05 | 97.34 | 1.76 | 0.016 | 7.68 | 9.46 | 10.29 | 0.06 | 0.015 | 2459 |
| k06 | 97.19 | 2.08 | 0.017 | 11.91 | 14.01 | 6.94 | 0.21 | 0.017 | 2923 |
| k07 | 102.54 | 3.33 | 0.020 | 12.02 | 15.37 | 6.67 | 0.07 | 0.024 | 3449 |
| k08 | 103.13 | 2.20 | 0.016 | 9.44 | 11.66 | 8.85 | 0.06 | 0.118 | 3325 |
| k09 | 112.82 | 2.18 | 0.016 | 10.62 | 12.82 | 8.80 | 0.06 | 0.014 | 3325 |
| k10 | 105.02 | 3.51 | 0.021 | 12.15 | 15.69 | 6.70 | 0.06 | 0.026 | 3539 |
| k11 | 105.85 | 3.27 | 0.020 | 15.78 | 19.07 | 5.55 | 0.06 | 0.017 | 3530 |
| k12 | 204.33 | 2.59 | 0.016 | 25.15 | 27.76 | 7.36 | 0.06 | 0.020 | 4270 |
| k13 | 204.90 | 2.46 | 0.016 | 20.17 | 22.65 | 9.05 | 0.19 | 0.021 | 4018 |
| k14 | 274.70 | 2.62 | 0.019 | 22.48 | 25.12 | 10.94 | 0.06 | 0.313 | 4022 |
| k15 | 201.82 | 2.40 | 0.016 | 26.91 | 29.33 | 6.88 | 0.06 | 0.017 | 4446 |
| k16 | 100.74 | 2.41 | 0.015 | 10.34 | 12.77 | 7.89 | 0.06 | 0.021 | 4138 |
| k17 | 185.94 | 2.48 | 0.015 | 29.74 | 32.24 | 5.77 | 0.08 | 0.020 | 5092 |
| k18 | 92.59 | 2.63 | 0.018 | 13.18 | 15.83 | 5.85 | 0.06 | 0.023 | 4712 |
| k19 | 199.82 | 2.42 | 0.016 | 27.57 | 30.00 | 6.66 | 0.08 | 0.020 | 4720 |
| k20 | 201.56 | 2.64 | 0.017 | 33.62 | 36.29 | 5.55 | 0.06 | 0.031 | 5354 |
| k21 | 168.77 | 2.18 | 0.015 | 25.46 | 27.66 | 6.10 | 0.07 | 0.020 | 4189 |
| k22 | 170.40 | 2.62 | 0.016 | 30.22 | 32.85 | 5.19 | 0.08 | 0.026 | 5096 |
| k23 | 183.00 | 2.92 | 0.018 | 38.06 | 41.00 | 4.46 | 0.07 | 0.028 | 6004 |

GPU time, milliseconds:

| key | literal | 1b specialised | 1b / literal | ubershader | uber / literal |
|---|---|---|---|---|---|
| k00 | 2.387 | 2.677 | 1.122 | 12.743 | 5.34 |
| k01 | 2.378 | 2.351 | 0.989 | 11.979 | 5.04 |
| k02 | 2.389 | 2.688 | 1.125 | 12.311 | 5.15 |
| k03 | 2.363 | 2.362 | 1.000 | 12.249 | 5.18 |
| k04 | 2.377 | 2.377 | 1.000 | 12.194 | 5.13 |
| k05 | 2.676 | 2.382 | 0.890 | 11.917 | 4.45 |
| k06 | 2.431 | 2.750 | 1.131 | 13.229 | 5.44 |
| k07 | 3.069 | 3.070 | 1.001 | 14.270 | 4.65 |
| k08 | 2.433 | 2.432 | 1.000 | 13.018 | 5.35 |
| k09 | 2.792 | 2.792 | 1.000 | 18.579 | 6.65 |
| k10 | 3.296 | 3.297 | 1.000 | 18.969 | 5.75 |
| k11 | 2.432 | 2.432 | 1.000 | 14.020 | 5.77 |
| k12 | 3.938 | 3.938 | 1.000 | 20.283 | 5.15 |
| k13 | 2.847 | 2.846 | 1.000 | 19.530 | 6.86 |
| k14 | 3.787 | 3.787 | 1.000 | 19.991 | 5.28 |
| k15 | 4.397 | 4.396 | 1.000 | 20.653 | 4.70 |
| k16 | 2.853 | 2.851 | 1.000 | 19.696 | 6.90 |
| k17 | 5.027 | 5.029 | 1.000 | 21.718 | 4.32 |
| k18 | 3.678 | 3.678 | 1.000 | 20.298 | 5.52 |
| k19 | 4.954 | 4.953 | 1.000 | 21.181 | 4.28 |
| k20 | 5.688 | 5.747 | 1.010 | 22.410 | 3.94 |
| k21 | 3.992 | 3.993 | 1.000 | 19.039 | 4.77 |
| k22 | 5.222 | 5.223 | 1.000 | 20.708 | 3.97 |
| k23 | 6.633 | 6.642 | 1.001 | 22.529 | 3.40 |

### Run 3

Miss latency, milliseconds (columns as the tool prints them):

| key | 1b create | opt1 glslang | opt1 module | opt1 create | opt1 total | 1b / opt1 | uber create | emit (not in total) | literal SPIR-V words |
|---|---|---|---|---|---|---|---|---|---|
| k00 | 103.22 | 1.16 | 0.008 | 4.99 | 6.16 | 16.76 | 480.30 | 0.011 | 1150 |
| k01 | 108.98 | 1.63 | 0.021 | 6.61 | 8.26 | 13.19 | 0.08 | 0.008 | 1422 |
| k02 | 99.92 | 1.41 | 0.016 | 6.76 | 8.19 | 12.20 | 0.15 | 0.012 | 1682 |
| k03 | 104.19 | 1.42 | 0.014 | 4.56 | 5.99 | 17.38 | 0.08 | 0.010 | 1891 |
| k04 | 96.03 | 1.63 | 0.016 | 8.86 | 10.51 | 9.14 | 0.06 | 0.020 | 2265 |
| k05 | 95.67 | 1.92 | 0.016 | 7.91 | 9.85 | 9.71 | 0.06 | 0.014 | 2459 |
| k06 | 109.25 | 1.82 | 0.013 | 10.74 | 12.58 | 8.69 | 0.17 | 0.018 | 2923 |
| k07 | 102.39 | 2.00 | 0.016 | 10.85 | 12.87 | 7.95 | 0.06 | 0.015 | 3449 |
| k08 | 105.45 | 2.29 | 0.016 | 9.91 | 12.22 | 8.63 | 0.13 | 0.033 | 3325 |
| k09 | 105.99 | 2.08 | 0.016 | 10.27 | 12.36 | 8.58 | 0.07 | 0.015 | 3325 |
| k10 | 166.11 | 2.14 | 0.017 | 12.69 | 14.85 | 11.18 | 0.06 | 0.022 | 3539 |
| k11 | 102.46 | 3.15 | 0.021 | 9.57 | 12.73 | 8.05 | 0.07 | 0.025 | 3530 |
| k12 | 216.73 | 2.83 | 0.018 | 32.51 | 35.35 | 6.13 | 0.14 | 0.020 | 4270 |
| k13 | 207.60 | 3.41 | 0.026 | 21.63 | 25.06 | 8.28 | 0.19 | 0.033 | 4018 |
| k14 | 213.05 | 2.94 | 0.022 | 23.67 | 26.63 | 8.00 | 0.06 | 0.038 | 4022 |
| k15 | 289.31 | 2.33 | 0.014 | 33.92 | 36.27 | 7.98 | 0.07 | 0.025 | 4446 |
| k16 | 104.85 | 2.69 | 0.017 | 12.60 | 15.31 | 6.85 | 0.08 | 0.030 | 4138 |
| k17 | 224.36 | 4.47 | 0.026 | 52.22 | 56.72 | 3.96 | 0.07 | 0.035 | 5092 |
| k18 | 114.47 | 2.91 | 0.021 | 14.82 | 17.75 | 6.45 | 0.07 | 0.024 | 4712 |
| k19 | 216.94 | 2.40 | 0.020 | 39.41 | 41.83 | 5.19 | 0.06 | 0.019 | 4720 |
| k20 | 194.36 | 3.32 | 0.016 | 38.12 | 41.46 | 4.69 | 0.06 | 0.030 | 5354 |
| k21 | 210.19 | 2.52 | 0.016 | 28.25 | 30.79 | 6.83 | 0.08 | 0.020 | 4189 |
| k22 | 195.94 | 2.83 | 0.015 | 34.22 | 37.07 | 5.29 | 0.06 | 0.025 | 5096 |
| k23 | 193.57 | 3.04 | 0.018 | 46.15 | 49.21 | 3.93 | 0.07 | 0.026 | 6004 |

GPU time, milliseconds:

| key | literal | 1b specialised | 1b / literal | ubershader | uber / literal |
|---|---|---|---|---|---|
| k00 | 2.358 | 2.357 | 0.999 | 12.443 | 5.28 |
| k01 | 2.365 | 2.349 | 0.993 | 12.614 | 5.33 |
| k02 | 2.377 | 2.674 | 1.125 | 12.254 | 5.15 |
| k03 | 2.373 | 2.373 | 1.000 | 12.232 | 5.16 |
| k04 | 2.388 | 2.388 | 1.000 | 12.234 | 5.12 |
| k05 | 2.363 | 2.363 | 1.000 | 12.007 | 5.08 |
| k06 | 2.416 | 2.414 | 0.999 | 13.103 | 5.42 |
| k07 | 3.066 | 3.066 | 1.000 | 18.855 | 6.15 |
| k08 | 2.429 | 2.428 | 1.000 | 13.366 | 5.50 |
| k09 | 2.789 | 2.789 | 1.000 | 18.516 | 6.64 |
| k10 | 3.296 | 3.296 | 1.000 | 18.515 | 5.62 |
| k11 | 2.430 | 2.431 | 1.000 | 15.419 | 6.34 |
| k12 | 3.937 | 3.938 | 1.000 | 19.879 | 5.05 |
| k13 | 2.844 | 2.843 | 1.000 | 19.607 | 6.90 |
| k14 | 3.786 | 3.785 | 1.000 | 19.562 | 5.17 |
| k15 | 4.391 | 4.392 | 1.000 | 20.198 | 4.60 |
| k16 | 2.849 | 2.849 | 1.000 | 20.008 | 7.02 |
| k17 | 5.027 | 5.028 | 1.000 | 21.818 | 4.34 |
| k18 | 3.677 | 3.676 | 1.000 | 20.404 | 5.55 |
| k19 | 4.949 | 4.951 | 1.000 | 21.056 | 4.25 |
| k20 | 5.761 | 5.784 | 1.004 | 22.119 | 3.84 |
| k21 | 3.989 | 3.988 | 1.000 | 19.080 | 4.78 |
| k22 | 5.220 | 5.220 | 1.000 | 20.200 | 3.87 |
| k23 | 6.625 | 6.644 | 1.003 | 22.636 | 3.42 |

