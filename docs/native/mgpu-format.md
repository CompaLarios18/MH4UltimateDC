# MGPU capture format, version 1

The `MGPU` format (decision D23) records everything that crosses Azahar's `VideoCore::GPU` interface, plus the memory the GPU can see, so the native GPU (N2) can replay it and be compared with the reference output. The recorder is N0-05; readers are the censuses (N0-13, N0-14) and the capture player (N2-02). The reference implementation of this document is [`tools/native/gcap.py`](../../tools/native/gcap.py) (reader, checker, summariser, writer; tests in `tests/test_native_gcap.py`).

A capture holds textures, shaders and framebuffers: it is game data. It lives under `build/` only, never in the repository.

All integers are little-endian. "Word" means `u32`.

## 1. File layout

```
offset 0   char[4]  magic = "MGPU"
offset 4   u32      version = 1
offset 8   records, back to back, up to the end of the file
```

A record:

```
u32  type      1..12, section 3
u32  frame     the game's frame counter (u32 at 0x010571d4), read when the record is written
u64  ticks     ARM11 ticks (Azahar CoreTiming global ticks) when the record is written
u32  size      payload size in bytes
u8   payload[size]
```

The header is 20 bytes. There is no padding or alignment between records. A sibling `FILE.json` manifest (tool, version, game, frames, record counts) is written by the recorder; it is not part of this format.

**Versioning.** Any change to this document that a version-1 reader would misread (a new record type, a new INIT block kind, a layout change) bumps `version`. Readers refuse versions they do not know (`gcap.read` raises `GcapError`).

## 2. Ordering rules (what `gcap.py check` enforces)

1. The first record is the only `INIT`.
2. `frame` never decreases from one record to the next.
3. Every `GX` record whose command id is 1 (`SubmitCmdList`) is immediately followed by one `PICA_REG` record (it may have count 0). A `PICA_REG` record appears nowhere else.
4. Record types are 1..12; payload sizes are the ones below; INIT block kinds are 1..11 with the sizes below; page addresses are multiples of 4096.

The intended order of a recording, which the checker does not require: `INIT`, `MEM` (all of FCRAM, then all of VRAM), then per event `CPU_PAGES` (the pages the CPU changed since the last diff, taken before the event), the event (`GX`, `MMIO`, `SWAP`, `FILL`), `PICA_REG` after a command-list `GX`, `GPU_PAGES` after any `GX` the GPU executed, `IRQ` for each interrupt signalled, `FLUSH` where the game flushed, `FRAME` once per game frame.

## 3. Records

Addresses are physical (FCRAM at `0x20000000`, VRAM at `0x18000000`) unless a field says "virtual".

| Type | Name | Payload size |
|---|---|---|
| 1 | `INIT` | 4 + sum of (8 + block size) |
| 2 | `MEM` | 8 + size |
| 3 | `CPU_PAGES` | 4 + 4100 x count |
| 4 | `GX` | 32 |
| 5 | `PICA_REG` | 4 + 8 x count |
| 6 | `GPU_PAGES` | 4 + 4100 x count |
| 7 | `MMIO` | 8 |
| 8 | `SWAP` | 32 |
| 9 | `FILL` | 4 |
| 10 | `IRQ` | 12 |
| 11 | `FLUSH` | 12 |
| 12 | `FRAME` | 4 |

### 3.1 `INIT` (1)

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | block count N |
| 4 | blocks | N blocks, back to back |

A block is `{u32 kind, u32 size, u8 data[size]}`. Each kind appears at most once, in increasing kind order. The GPU state at the start of the capture; the block kinds are in section 4.

### 3.2 `MEM` (2)

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | physical address |
| 4 | `u32` | size in bytes, N |
| 8 | `u8[N]` | the bytes |

The address need not be page aligned. Recording starts with `MEM` records covering all of FCRAM and VRAM (more than one record when a region is larger than the recorder wants in one piece).

### 3.3 `CPU_PAGES` (3) and `GPU_PAGES` (6)

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | count N |
| 4 + 4100 i | `u32` | physical address of page i, a multiple of 4096 |
| 8 + 4100 i | `u8[4096]` | the page's new contents |

`CPU_PAGES`: pages that changed since the last diff, taken before each GX command, register write, swap and colour fill. `GPU_PAGES`: the pages the GPU changed while executing a command (the reference output). Memory at any record is `MEM`, `CPU_PAGES` and `GPU_PAGES` applied in file order (`gcap.memory_at`).

### 3.4 `GX` (4)

Eight words: the GSP command exactly as `GPU::Execute(const Service::GSP::Command&)` gets it (`src/core/hle/service/gsp/gsp_command.h`).

| Word | Field |
|---|---|
| 0 | bits 0-7 command id, bits 8-15 unknown1, bits 16-23 stop, bits 24-31 unknown2 |
| 1-7 | the command's arguments (below), unused words kept as the game wrote them |

Command ids: 0 `RequestDma`, 1 `SubmitCmdList`, 2 `MemoryFill`, 3 `DisplayTransfer`, 4 `TextureCopy`, 5 `CacheFlush`. Arguments by word:

| Id | w1 | w2 | w3 | w4 | w5 | w6 | w7 |
|---|---|---|---|---|---|---|---|
| 0 | source address | dest address | size | - | - | - | - |
| 1 | address | size | flags | unused | unused | unused | do_flush |
| 2 | start1 | value1 | end1 | start2 | value2 | end2 | control (two 16-bit halves) |
| 3 | in address | out address | in size | out size | flags | - | - |
| 4 | in address | out address | size | in width gap | out width gap | flags | - |
| 5 | region 0 address | region 0 size | region 1 address | region 1 size | region 2 address | region 2 size | - |

(Source: `DmaCommand`, `SubmitCmdListCommand`, `MemoryFillCommand`, `DisplayTransferCommand`, `TextureCopyCommand`, `CacheFlushCommand`.)

### 3.5 `PICA_REG` (5)

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | count N |
| 4 + 8 i | `u16` | internal register index, 0..0x2FF |
| 6 + 8 i | `u8` | byte mask, bit n set = byte n of the register is written; 0..0xF |
| 7 + 8 i | `u8` | 0 |
| 8 + 8 i | `u32` | value |

The internal register writes the command processor made while executing the preceding `GX` command list, in order, including the writes made by the list's own headers and consecutive-register and incrementing-write forms (each register written is one entry). New register value = (old & ~M) | (value & M), M = the mask expanded to bytes (`gcap.registers_at`).

### 3.6 `MMIO` (7)

`u32` address (virtual, as the game sees it: LCD registers at `0x1ED02000`, GPU registers at `0x1EF00000` and `0x1EF01000`), `u32` value: one call of `GPU::WriteReg(VAddr, u32)`.

### 3.7 `SWAP` (8)

Eight words: `u32` screen (0 top, 1 bottom), then the seven words of `Service::GSP::FrameBufferInfo` in order: `active_fb`, `address_left`, `address_right`, `stride`, `format`, `shown_fb`, `unknown`. Addresses are as the game wrote them (physical).

### 3.8 `FILL` (9)

`u32`: the colour-fill word of `GPU::SetColorFill(const Pica::ColorFill&)`: bits 0-7 R, 8-15 G, 16-23 B, bit 24 enabled.

### 3.9 `IRQ` (10)

`u32` interrupt id (0 PSC0, 1 PSC1, 2 PDC0, 3 PDC1, 4 PPF, 5 P3D, 6 DMA; `Service::GSP::InterruptId`), `u64` delay in nanoseconds: the delay the interrupt handler was given (for P3D, `DelayGenerator::CalculateAndResetDelay()`).

### 3.10 `FLUSH` (11)

`u32` kind (1 FlushDataCache, 2 InvalidateDataCache, 3 StoreDataCache, 4 a CacheFlush region), `u32` virtual address, `u32` size in bytes.

### 3.11 `FRAME` (12)

`u32`: the game's frame counter. Also the `frame` field of the header, written once per game frame at its start.

## 4. `INIT` block kinds

Layouts follow how Azahar holds the state at the pinned commit (`5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6`, `src/video_core/pica/pica_core.h`). Fixed-size, little-endian, no padding. `f32` is an IEEE single; Azahar's `f24` (`Pica::Float<16, 7>`) is stored in memory as one `float`, so a `Vec4<f24>` is four `f32` (16 bytes).

| Kind | Name | Size | Contents |
|---|---|---|---|
| 1 | `GPU_REGS` | 4296 | words 0..0x431 of `PicaCore::Regs::reg_array` (the GPU's external registers: hardware id, memory fill, framebuffer and display-transfer configuration), 0x432 words |
| 2 | `LCD_REGS` | 4096 | `PicaCore::regs_lcd` (`RegsLcd`), 0x400 words: colour fill and backlight of both screens |
| 3 | `PICA_REGS` | 3072 | the PICA internal registers, `Regs::internal` = words 0x432..0x731 of `reg_array`, 0x300 words (the file the `PICA_REG` records write; fog mode is register 0x0E0 bits 0-2: 0 none, 5 fog, 7 gas) |
| 4 | `VS_SETUP` | 34368 | `PicaCore::vs_setup`, shader setup (below) |
| 5 | `GS_SETUP` | 34368 | `PicaCore::gs_setup`, same layout |
| 6 | `DEFAULT_ATTRS` | 256 | `input_default_attributes`: 16 x `Vec4<f24>` as 4 x `f32` |
| 7 | `LIGHTING_LUT` | 24576 | `Lighting::luts`: 24 tables x 256 entries x `u32` (the raw `LutEntry`: bits 0-11 value, 12-22 difference, bit 23 negative-difference flag), table-major |
| 8 | `FOG_LUT` | 512 | `Fog::lut`: 128 x `u32` (raw `LutEntry`: bits 0-12 signed difference, bits 13-23 value) |
| 9 | `PROCTEX` | 3584 | `ProcTex` tables, 896 `u32` in this order: `noise_table` (128), `color_map_table` (128), `alpha_map_table` (128), `color_table` (256), `color_diff_table` (256); raw entries; the dirty flags are not stored |
| 10 | `IMMEDIATE` | 284 | `ImmediateModeState` (below) |
| 11 | `GEOMETRY_PIPELINE` | 288 | geometry pipeline state (below) |

**Shader setup (kinds 4 and 5), 34368 bytes:**

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `entry_point` |
| 4 | 4 | `biggest_program_size` (words) |
| 8 | 4 | `biggest_swizzle_size` (words) |
| 12 | 16384 | `program_code`, 4096 words |
| 16396 | 16384 | `swizzle_data`, 4096 words |
| 32780 | 1536 | float uniforms `uniforms.f`, 96 x `Vec4<f24>` (4 x `f32` each) |
| 34316 | 16 | boolean uniforms `uniforms.b`, 16 bytes, each 0 or 1 |
| 34332 | 16 | integer uniforms `uniforms.i`, 4 x `Vec4<u8>` |
| 34348 | 20 | `uniform_queue` (`PackedAttribute`): `u32 buffer[4]`, `u32 index` |

**Immediate mode (kind 10), 284 bytes:** `input_vertex` 16 x `Vec4<f24>` (256 bytes, 4 x `f32` each); `u32 current_attribute`; `u32 reset_geometry_pipeline` (0 or 1); `queue` (`PackedAttribute`): `u32 buffer[4]`, `u32 index`.

**Geometry pipeline (kind 11), 288 bytes:** the active backend of `GeometryPipeline` (`geometry_pipeline.cpp`), as the words below; fields a backend does not have are 0.

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | mode: 0 none/not configured, 1 Point, 2 VariablePrimitive, 3 FixedPrimitive |
| 4 | `u32` | `vs_output_num` |
| 8 | `u32` | buffer index (Point: `buffer_cur - attribute_buffer`; Variable and Fixed: `buffer_cur - uniforms.f`) |
| 12 | `u32` | buffer size (Point: `buffer_end - attribute_buffer`; Fixed: `buffer_end - uniforms.f`) |
| 16 | `u32` | buffer offset (Fixed: `buffer_begin - uniforms.f`) |
| 20 | `u32` | `need_index` (Variable; 0 or 1) |
| 24 | `u32` | `main_vertex_num` (Variable) |
| 28 | `u32` | `total_vertex_num` (Variable) |
| 32 | 256 | `attribute_buffer` (Point): 16 x `Vec4<f24>` as 4 x `f32` |

Not captured in version 1: the `PrimitiveAssembler` buffer and the geometry shader's emitter. Recording starts between command lists, where both are rebuilt from the registers by the next draw. If N0-05 finds otherwise, add a block kind and bump the version.

## 5. Tools

```
python3 tools/native/gcap.py summary FILE [--json]   # counts per type, GX kinds, frames, fog modes, IRQ delay statistics
python3 tools/native/gcap.py check FILE              # section 2 plus per-record sizes; exit 1 if broken
```

`summary` and `check` print counts, ids and statistics only, never buffer contents, so their output can be quoted. The fog modes are those written by `PICA_REG` entries to register 0x0E0 whose byte mask includes byte 0; the IRQ statistics are the count and the minimum, median and maximum delay in ns per interrupt id.

In Python: `gcap.read(path)` (records), `gcap.Writer` (tests and tools), `gcap.memory_at(records, index)`, `gcap.registers_at(records, index)`.
