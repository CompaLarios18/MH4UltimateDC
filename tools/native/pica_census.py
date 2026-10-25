#!/usr/bin/env python3
# Derived from Azahar src/video_core/pica/regs_internal.h, regs_pipeline.h, regs_rasterizer.h, regs_texturing.h,
# regs_framebuffer.h, regs_lighting.h, regs_shader.h, pica_core.h, pica_core.cpp, packed_attribute.h,
# vertex_loader.cpp, src/video_core/shader/generator/pica_fs_config.cpp, src/video_core/renderer_vulkan/vk_rasterizer.cpp
# src/video_core/renderer_opengl/gl_rasterizer.cpp and src/video_core/shader/generator/spv_fs_shader_gen.cpp at
# 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: pica_fs_config.cpp and vk_rasterizer.cpp say "Licensed
# under GPLv3 or any later version"; the other files say "Licensed under GPLv2 or any later version"): the register
# positions, bit fields and enumerations of the PICA internal registers (the FIELDS, REGS and ENUMS tables),
# which register writes trigger a draw (pica_core.cpp: HandleSpecialReg, SubmitImmediate), the immediate-mode
# queue (pica_core.h, packed_attribute.h), how vertex attributes and loaders are read (vertex_loader.cpp) and
# which fields of the lighting, texturing and framebuffer state decide a draw's configuration (pica_fs_config.cpp), and
# when the depth buffer is used (using_depth_fb in vk_rasterizer.cpp and gl_rasterizer.cpp), and which logic ops the SPIR-V
# fragment generator emulates (the logic-op switch of spv_fs_shader_gen.cpp: Clear, Set, Copy, NoOp).
# N0-40 adds, from the same pica_fs_config.cpp (the constructors of FramebufferConfig, TextureConfig, LightConfig and
# ProcTexConfig) and the same regs_*.h, the fields of FSConfig that the census did not decode, and from
# src/video_core/renderer_vulkan/pica_to_vk.h (CullMode, FrontFace: GPL-2.0-or-later) and vk_graphics_pipeline.cpp and
# vk_rasterizer.cpp (the pipeline state that holds the cull mode, front face, colour write mask, blend and logic op:
# GPL-3.0-or-later) the fixed-function state of the pipeline key.
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_to_vk.h, vk_graphics_pipeline.cpp)
# Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_pipeline.h, regs_framebuffer.h, regs_shader.h)
# Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
# Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
# Copyright 2017 Citra Emulator Project (regs_lighting.h)
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, pica_core.cpp, packed_attribute.h, pica_fs_config.cpp, vk_rasterizer.cpp, spv_fs_shader_gen.cpp)
# Copyright 2023 Citra Emulator Project (vertex_loader.cpp)
# Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (gl_rasterizer.cpp)
#
"""PICA census (N0-13): which PICA features does the game use, and how often?

Replays the PICA_REG records of MGPU captures (docs/native/mgpu-format.md, tools/native/gcap.py) into a
register file, starting from each capture's INIT record, and records the GPU state at every draw trigger.

The command for the G1-G4 run (labels are explicit; several captures per label are allowed, each is
replayed from its own INIT; a pattern with * is expanded by the tool, in natural order):

    python3 tools/native/pica_census.py \\
        'G1=build/golden/G1/gpu-*.mgpu' 'G2=build/golden/G2/gpu-*.mgpu' \\
        'G3=build/golden/G3/gpu-*.mgpu' 'G4=build/golden/G4/gpu-*.mgpu' \\
        --json build/pica-census.json --md docs/research/pica-census.md

WARNING: --md defaults to docs/research/pica-census.md, the GENERATED census document, and --json to build/pica-census.json.
The command above, run as written, overwrites the document. For any other run pass --json and --md explicitly.

N0-40, the pipeline-key groups and the union across captures (nothing changes without --union: the JSON and the markdown
are then byte-identical to the N0-13 tool's). The command, which writes to separate files (labels G9a-G9c are the census
scenes; docs/native/gpu-fragment-design.md section 7):

    python3 tools/native/pica_census.py \\
        'G1=build/golden/G1/gpu-*.mgpu' 'G2=build/golden/G2/gpu-*.mgpu' \\
        'G3=build/golden/G3/gpu-*.mgpu' 'G4=build/golden/G4/gpu-*.mgpu' \\
        'G9a=build/golden/G9a/gpu-*.mgpu' 'G9b=build/golden/G9b/gpu-*.mgpu' 'G9c=build/golden/G9c/gpu-*.mgpu' \\
        --union --json build/pica-census-union.json --md build/pica-census-union.md

With --union the markdown gets sections appended and the JSON a "union" key: per label and over all labels (a configuration
seen in several captures counts once) the distinct fs_key (the canonical fragment key of design 4.3), fixed_function
(blend equations and factors or the logic op, colour write mask, depth, stencil, cull mode, front face), vertex_stage
(vertex program hash, vertex layout, topology), proctex_config, and the joint tuple (fs_key, fixed_function, vertex_stage,
pass kind, logical colour and depth formats), the blend factors, rasterizer and texture fields (decode_ext), and the lit
draws with and without a geometry shader.

Outputs: the JSON (counts per feature, unique configurations, the answers) and the markdown summary
(one section per question of docs/research/metal.md, answered YES/NO per label with draw and frame counts and
the register-level description, then tables per feature). Both hold counts, register values, enumerator
names and hashes only: no memory contents, shader instructions or texture bytes. stdout prints counts only.

Exit codes: 0 ok; 1 a capture failed its checks (unreadable, truncated, wrong magic or version, a
gcap.check_records problem): nothing is written; 2 usage (bad LABEL=PATH, a pattern or file that does not
exist); 3 nothing censused: no draw trigger in any capture (D34: "nothing compared is not a pass"), or, with
--strict, in any one label.

What counts as a draw trigger (pica_core.cpp, HandleSpecialReg and SubmitImmediate):
  * a write to register 0x22E (trigger_draw) or 0x22F (trigger_draw_indexed), whatever the byte mask;
  * immediate mode: while the default-attribute index (0x232) is 0xF, every three words written to 0x233-0x235
    form one attribute and max_input_attrib_index+1 attributes (0x242) form a vertex. The first vertex after a
    write to 0x232 (or after an array draw) is one "immediate" draw, with the state of that moment; later
    vertices of the same batch only add to the immediate vertex count.
  * a write to 0x23C or 0x23D (command_buffer.trigger) runs a stored command buffer; it is counted
    ("command buffer triggers") but is no draw: the writes it makes are PICA_REG entries of their own, if the
    recorder logged them.

Interpretation notes (every decoding is by the Azahar field it cites in FIELDS / REGS / ENUMS):
  * lighting is on when lighting.disable (0x1C6 bit 0) is clear, as Azahar's LightConfig does; 0x08F bit 0 is
    counted on the side. A LUT is enabled when it is not disabled in 0x1C4 and the lighting configuration
    supports it (IsLightingSamplerSupported); SP when supported and a lit light has spot attenuation on; DA when
    a lit light has distance attenuation on.
  * a logic op is effective when blending is off (0x100 bit 8 clear), the op is not Copy, colour writes are on
    (0x113 non-zero and a channel enabled in 0x107), the fragment operation mode is not Shadow and the colour
    buffer address (0x11D) is non-zero. Effective ops are split into those Azahar's SPIR-V path emulates (Clear,
    Set, NoOp) and the rest.
  * a capture's manifest `<capture>.json` may count `unrecorded_register_writes` (register-started command lists leave no
    PICA_REG record): the tool warns, sums it per label in the JSON and says in the markdown that NO means "not seen".
  * 24-bit depth is in use when the depth format is D24 or D24S8 and Azahar's using_depth_fb holds (vk_rasterizer.cpp):
    not shadow rendering, depth buffer address (0x11C) non-zero, and depth write (0x107 bit 12 with 0x115), depth test,
    or D24S8 with stencil test.
  * a border colour is in use when an enabled texture unit has ClampToBorder or ClampToBorder2 in a wrap mode.
  * a program hash covers the shader words from 0 to the end of the latest upload that started at offset 0
    (or INIT's biggest program size) plus the swizzle words likewise; the entry point (main_offset) is counted apart.
    Uploads are read from the unit's own registers (vs 0x2B0, gs 0x280); Azahar's sharing of unit 3 is not modelled.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter
from typing import Iterable, Iterator, NamedTuple, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcap  # noqa: E402

TOOL = "pica_census"
VERSION = 1

EXIT_OK, EXIT_FAILED, EXIT_USAGE, EXIT_NOTHING = 0, 1, 2, 3


# --------------------------------------------------------------------------
# Register tables.  Every entry names the Azahar path (RegsInternal.<path>) it comes from; the test
# tests/test_native_pica_census.py checks them against Azahar's headers when MH4N_AZAHAR_DIR is set.

class Reg(NamedTuple):
    index: int
    path: str


class Fld(NamedTuple):
    reg: int
    pos: int
    bits: int
    path: str


REGS: dict[str, Reg] = {}
FIELDS: dict[str, Fld] = {}


def _reg(name: str, index: int, path: str) -> None:
    assert name not in REGS
    REGS[name] = Reg(index, path)


def _fld(name: str, reg: int, pos: int, bits: int, path: str) -> None:
    assert name not in FIELDS
    FIELDS[name] = Fld(reg, pos, bits, path)


# regs_pipeline.h (positions: regs_internal.h, ASSERT_REG_POSITION(pipeline.*))
_reg("vertex_attributes", 0x200, "pipeline.vertex_attributes")
_reg("index_array", 0x227, "pipeline.index_array")
_reg("num_vertices", 0x228, "pipeline.num_vertices")
_reg("trigger_draw", 0x22E, "pipeline.trigger_draw")
_reg("trigger_draw_indexed", 0x22F, "pipeline.trigger_draw_indexed")
_reg("default_attr_index", 0x232, "pipeline.vs_default_attributes_setup.index")
for _n in range(3):
    _reg(f"default_attr_value{_n}", 0x233 + _n, f"pipeline.vs_default_attributes_setup.set_value[{_n}]")
for _n in range(2):
    _reg(f"command_buffer_trigger{_n}", 0x23C + _n, f"pipeline.command_buffer.trigger[{_n}]")
_fld("index_format", 0x227, 31, 1, "pipeline.index_array.format")
_fld("use_gs", 0x229, 0, 2, "pipeline.use_gs")
_fld("max_input_attrib_index", 0x242, 0, 4, "pipeline.max_input_attrib_index")
_fld("gs_mode", 0x252, 0, 8, "pipeline.gs_config.mode")
_fld("triangle_topology", 0x25E, 8, 2, "pipeline.triangle_topology")
_fld("attribute_mask", 0x202, 16, 12, "pipeline.vertex_attributes.attribute_mask")
_fld("max_attribute_index", 0x202, 28, 4, "pipeline.vertex_attributes.max_attribute_index")
for _n in range(12):
    _fld(f"attr_format{_n}", 0x201 + _n // 8, 4 * (_n % 8), 2, f"pipeline.vertex_attributes.format{_n}")
    _fld(f"attr_size{_n}", 0x201 + _n // 8, 4 * (_n % 8) + 2, 2, f"pipeline.vertex_attributes.size{_n}")
    _fld(f"loader{_n}_byte_count", 0x203 + 3 * _n + 2, 16, 8,
         f"pipeline.vertex_attributes.attribute_loaders[{_n}].byte_count")
    _fld(f"loader{_n}_component_count", 0x203 + 3 * _n + 2, 28, 4,
         f"pipeline.vertex_attributes.attribute_loaders[{_n}].component_count")

# regs_shader.h (gs at 0x280, vs at 0x2B0)
UNIT_BASE = {"gs": 0x280, "vs": 0x2B0}
for _u, _b in UNIT_BASE.items():
    _fld(f"{_u}_main_offset", _b + 0x0A, 0, 16, f"{_u}.main_offset")
    _reg(f"{_u}_program_offset", _b + 0x1B, f"{_u}.program.offset")
    _reg(f"{_u}_swizzle_offset", _b + 0x25, f"{_u}.swizzle_patterns.offset")
    for _n in range(8):
        _reg(f"{_u}_program_word{_n}", _b + 0x1C + _n, f"{_u}.program.set_word[{_n}]")
        _reg(f"{_u}_swizzle_word{_n}", _b + 0x26 + _n, f"{_u}.swizzle_patterns.set_word[{_n}]")

# regs_texturing.h
TEX_BASE = (0x81, 0x91, 0x99)
TEX_FORMAT_REG = (0x8E, 0x96, 0x9E)
for _n in range(3):
    _fld(f"tex{_n}_enable", 0x80, _n, 1, f"texturing.main_config.texture{_n}_enable")
    _reg(f"tex{_n}_border", TEX_BASE[_n], f"texturing.texture{_n}.border_color")
    _fld(f"tex{_n}_height", TEX_BASE[_n] + 1, 0, 11, f"texturing.texture{_n}.height")
    _fld(f"tex{_n}_width", TEX_BASE[_n] + 1, 16, 11, f"texturing.texture{_n}.width")
    _fld(f"tex{_n}_wrap_t", TEX_BASE[_n] + 2, 8, 3, f"texturing.texture{_n}.wrap_t")
    _fld(f"tex{_n}_wrap_s", TEX_BASE[_n] + 2, 12, 3, f"texturing.texture{_n}.wrap_s")
    _fld(f"tex{_n}_format", TEX_FORMAT_REG[_n], 0, 4, f"texturing.texture{_n}_format")
_fld("tex0_type", 0x83, 28, 3, "texturing.texture0.type")           # "only valid for texture 0"
_fld("proctex_enable", 0x80, 10, 1, "texturing.main_config.texture3_enable")
_fld("fragment_lighting_enable", 0x8F, 0, 1, "texturing.fragment_lighting_enable")
_fld("fog_mode", 0xE0, 0, 3, "texturing.fog_mode")
_fld("tev_update_rgb", 0xE0, 8, 4, "texturing.tev_combiner_buffer_input.update_mask_rgb")
_fld("tev_update_a", 0xE0, 12, 4, "texturing.tev_combiner_buffer_input.update_mask_a")
_reg("tev_buffer_color", 0xFD, "texturing.tev_combiner_buffer_color")
TEV_BASE = (0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8)
for _n in range(6):
    _reg(f"tev{_n}_sources", TEV_BASE[_n], f"texturing.tev_stage{_n}.sources_raw")
    _reg(f"tev{_n}_modifiers", TEV_BASE[_n] + 1, f"texturing.tev_stage{_n}.modifiers_raw")
    _reg(f"tev{_n}_ops", TEV_BASE[_n] + 2, f"texturing.tev_stage{_n}.ops_raw")
    _reg(f"tev{_n}_scales", TEV_BASE[_n] + 4, f"texturing.tev_stage{_n}.scales_raw")

# regs_framebuffer.h
_OM = "framebuffer.output_merger"
_fld("fragment_operation_mode", 0x100, 0, 2, f"{_OM}.fragment_operation_mode")
_fld("alphablend_enable", 0x100, 8, 1, f"{_OM}.alphablend_enable")
_fld("blend_equation_rgb", 0x101, 0, 3, f"{_OM}.alpha_blending.blend_equation_rgb")
_fld("blend_equation_a", 0x101, 8, 3, f"{_OM}.alpha_blending.blend_equation_a")
_fld("logic_op", 0x102, 0, 4, f"{_OM}.logic_op")
_fld("alpha_test_enable", 0x104, 0, 1, f"{_OM}.alpha_test.enable")
_fld("alpha_test_func", 0x104, 4, 3, f"{_OM}.alpha_test.func")
_fld("stencil_enable", 0x105, 0, 1, f"{_OM}.stencil_test.enable")
_fld("stencil_func", 0x105, 4, 3, f"{_OM}.stencil_test.func")
_fld("stencil_fail", 0x106, 0, 3, f"{_OM}.stencil_test.action_stencil_fail")
_fld("stencil_depth_fail", 0x106, 4, 3, f"{_OM}.stencil_test.action_depth_fail")
_fld("stencil_depth_pass", 0x106, 8, 3, f"{_OM}.stencil_test.action_depth_pass")
_fld("depth_test_enable", 0x107, 0, 1, f"{_OM}.depth_test_enable")
_fld("depth_test_func", 0x107, 4, 3, f"{_OM}.depth_test_func")
_fld("red_enable", 0x107, 8, 1, f"{_OM}.red_enable")
_fld("green_enable", 0x107, 9, 1, f"{_OM}.green_enable")
_fld("blue_enable", 0x107, 10, 1, f"{_OM}.blue_enable")
_fld("alpha_enable", 0x107, 11, 1, f"{_OM}.alpha_enable")
_fld("depth_write_enable", 0x107, 12, 1, f"{_OM}.depth_write_enable")
_fld("allow_color_write", 0x113, 0, 4, "framebuffer.framebuffer.allow_color_write")
_fld("allow_depth_stencil_write", 0x115, 0, 2, "framebuffer.framebuffer.allow_depth_stencil_write")
_fld("depth_buffer_address", 0x11C, 0, 28, "framebuffer.framebuffer.depth_buffer_address")
_fld("color_buffer_address", 0x11D, 0, 28, "framebuffer.framebuffer.color_buffer_address")
_fld("depth_format", 0x116, 0, 2, "framebuffer.framebuffer.depth_format")
_fld("color_format", 0x117, 16, 3, "framebuffer.framebuffer.color_format")

# regs_lighting.h
_fld("light_max_index", 0x1C2, 0, 3, "lighting.max_light_index")
_fld("light_enable_shadow", 0x1C3, 0, 1, "lighting.config0.enable_shadow")
_fld("light_config", 0x1C3, 4, 4, "lighting.config0.config")
_fld("light_bump_mode", 0x1C3, 28, 2, "lighting.config0.bump_mode")
_fld("light_disable_spot_atten", 0x1C4, 8, 8, "lighting.config1.disable_spot_atten")
_fld("light_disable_dist_atten", 0x1C4, 24, 8, "lighting.config1.disable_dist_atten")
for _name in ("d0", "d1", "fr", "rr", "rg", "rb"):
    _fld(f"light_disable_lut_{_name}", 0x1C4, {"d0": 16, "d1": 17, "fr": 19, "rr": 20, "rg": 21, "rb": 22}[_name], 1,
         f"lighting.config1.disable_lut_{_name}")
_fld("light_disable", 0x1C6, 0, 1, "lighting.disable")
for _n in range(8):
    _fld(f"light_slot{_n}", 0x1D9, 4 * _n, 3, f"lighting.light_enable.slot_{_n}")

# N0-40: the fields FSConfig::FSConfig(regs) reads that the census did not decode (pica_fs_config.cpp, the constructors of
# FramebufferConfig, TextureConfig, LightConfig and ProcTexConfig), and the fixed-function state of the pipeline key.
# They are used by decode_ext() only, so the census's own groups and tables are unchanged.
# regs_framebuffer.h: output_merger.alpha_blending (factors); regs_rasterizer.h: cull_mode, scissor_test.mode, depthmap_enable
# (their use as pipeline state: vk_rasterizer.cpp, vk_graphics_pipeline.cpp, pica_to_vk.h FrontFace/CullMode).
for _name, _azahar, _pos in (("src_rgb", "factor_source_rgb", 16), ("dst_rgb", "factor_dest_rgb", 20),
                             ("src_a", "factor_source_a", 24), ("dst_a", "factor_dest_a", 28)):
    _fld(f"blend_factor_{_name}", 0x101, _pos, 4, f"{_OM}.alpha_blending.{_azahar}")
_fld("cull_mode", 0x40, 0, 2, "rasterizer.cull_mode")
_fld("scissor_mode", 0x65, 0, 2, "rasterizer.scissor_test.mode")
_fld("depthmap_enable", 0x6D, 0, 1, "rasterizer.depthmap_enable")
# regs_texturing.h: main_config, shadow, fog_flip, the Operation of each TEV stage (Dot3_RGBA masking), proctex*
_fld("tex2_use_coord1", 0x80, 13, 1, "texturing.main_config.texture2_use_coord1")
_fld("proctex_coord", 0x80, 8, 2, "texturing.main_config.texture3_coordinates")
_fld("shadow_orthographic", 0x8B, 0, 1, "texturing.shadow.orthographic")
_fld("fog_flip", 0xE0, 16, 1, "texturing.fog_flip")
for _n in range(6):
    _fld(f"tev{_n}_color_op", TEV_BASE[_n] + 2, 0, 4, f"texturing.tev_stage{_n}.color_op")
for _name, _pos, _bits in (("u_clamp", 0, 3), ("v_clamp", 3, 3), ("color_combiner", 6, 4), ("alpha_combiner", 10, 4),
                           ("separate_alpha", 14, 1), ("noise_enable", 15, 1), ("u_shift", 16, 2), ("v_shift", 18, 2)):
    _fld(f"proctex_{_name}", 0xA8, _pos, _bits, f"texturing.proctex.{_name}")
for _name, _pos, _bits in (("filter", 0, 3), ("lod_min", 3, 4), ("lod_max", 7, 4), ("width", 11, 8)):
    _fld(f"proctex_lut_{_name}", 0xAC, _pos, _bits, f"texturing.proctex_lut.{_name}")
for _n in range(4):
    _fld(f"proctex_lut_offset{_n}", 0xAD, 8 * _n, 8, f"texturing.proctex_lut_offset.level{_n}")
# regs_lighting.h: config0 flags, config1.disable_shadow, abs_lut_input, lut_input, lut_scale, LightSrc.config of each light
for _name, _pos, _bits in (("enable_primary_alpha", 2, 1), ("enable_secondary_alpha", 3, 1), ("shadow_primary", 16, 1),
                           ("shadow_secondary", 17, 1), ("shadow_invert", 18, 1), ("shadow_alpha", 19, 1),
                           ("bump_selector", 22, 2), ("shadow_selector", 24, 2), ("clamp_highlights", 27, 1),
                           ("disable_bump_renorm", 30, 1)):
    _fld(f"light_{_name}", 0x1C3, _pos, _bits, f"lighting.config0.{_name}")
_fld("light_disable_shadow", 0x1C4, 0, 8, "lighting.config1.disable_shadow")
LUT_NAMES = ("d0", "d1", "sp", "fr", "rr", "rg", "rb")
for _name, _pos in (("d0", 0), ("d1", 4), ("sp", 8), ("fr", 12), ("rb", 16), ("rg", 20), ("rr", 24)):
    _fld(f"light_lut_input_{_name}", 0x1D1, _pos, 3, f"lighting.lut_input.{_name}")
    _fld(f"light_lut_scale_{_name}", 0x1D2, _pos, 3, f"lighting.lut_scale.{_name}")
for _name, _pos in (("d0", 1), ("d1", 5), ("sp", 9), ("fr", 13), ("rb", 17), ("rg", 21), ("rr", 25)):
    _fld(f"light_abs_disable_{_name}", 0x1D0, _pos, 1, f"lighting.abs_lut_input.disable_{_name}")
for _n in range(8):
    for _name, _pos in (("directional", 0), ("two_sided_diffuse", 1), ("geometric_factor_0", 2), ("geometric_factor_1", 3)):
        _fld(f"light{_n}_{_name}", 0x149 + 0x10 * _n, _pos, 1, f"lighting.light[{_n}].config.{_name}")

# Enumerations, by their Azahar names (the enumerator names are used verbatim in the outputs).
# docs/research/metal.md: Azahar's SPIR-V fragment generator emulates Clear, Set, Copy and NoOp; the rest draw as Copy
_LOGIC_SPIRV = ("Clear", "Set", "NoOp")
LOGIC_EMULATED = "emulated by Azahar's SPIR-V path (Clear, Set, NoOp)"
LOGIC_UNHANDLED = "not emulated: drawn as Copy (the other 11, and CopyInverted)"

ENUMS: dict[str, dict[int, str]] = {
    "VertexAttributeFormat": {0: "BYTE", 1: "UBYTE", 2: "SHORT", 3: "FLOAT"},                    # regs_pipeline.h
    "UseGS": {0: "No", 2: "Yes"},
    "GSMode": {0: "Point", 1: "VariablePrimitive", 2: "FixedPrimitive"},
    "TriangleTopology": {0: "List", 1: "Strip", 2: "Fan", 3: "Shader"},
    "IndexFormat": {0: "BYTE", 1: "SHORT"},
    "TextureType": {0: "Texture2D", 1: "TextureCube", 2: "Shadow2D", 3: "Projection2D",          # regs_texturing.h
                    4: "ShadowCube", 5: "Disabled"},
    "WrapMode": {0: "ClampToEdge", 1: "ClampToBorder", 2: "Repeat", 3: "MirroredRepeat",
                 4: "ClampToEdge2", 5: "ClampToBorder2", 6: "Repeat2", 7: "Repeat3"},
    "TextureFormat": {0: "RGBA8", 1: "RGB8", 2: "RGB5A1", 3: "RGB565", 4: "RGBA4", 5: "IA8", 6: "RG8",
                      7: "I8", 8: "A8", 9: "IA4", 10: "I4", 11: "A4", 12: "ETC1", 13: "ETC1A4"},
    "FogMode": {0: "None", 5: "Fog", 7: "Gas"},
    "FragmentOperationMode": {0: "Default", 1: "Gas", 3: "Shadow"},                              # regs_framebuffer.h
    "LogicOp": {0: "Clear", 1: "And", 2: "AndReverse", 3: "Copy", 4: "Set", 5: "CopyInverted", 6: "NoOp",
                7: "Invert", 8: "Nand", 9: "Or", 10: "Nor", 11: "Xor", 12: "Equiv", 13: "AndInverted",
                14: "OrReverse", 15: "OrInverted"},
    "BlendEquation": {0: "Add", 1: "Subtract", 2: "ReverseSubtract", 3: "Min", 4: "Max"},
    "CompareFunc": {0: "Never", 1: "Always", 2: "Equal", 3: "NotEqual", 4: "LessThan", 5: "LessThanOrEqual",
                    6: "GreaterThan", 7: "GreaterThanOrEqual"},
    "StencilAction": {0: "Keep", 1: "Zero", 2: "Replace", 3: "Increment", 4: "Decrement", 5: "Invert",
                      6: "IncrementWrap", 7: "DecrementWrap"},
    "ColorFormat": {0: "RGBA8", 1: "RGB8", 2: "RGB5A1", 3: "RGB565", 4: "RGBA4"},
    "DepthFormat": {0: "D16", 2: "D24", 3: "D24S8"},
    "LightingConfig": {0: "Config0", 1: "Config1", 2: "Config2", 3: "Config3", 4: "Config4", 5: "Config5",   # regs_lighting.h
                       6: "Config6", 8: "Config7"},
    "LightingBumpMode": {0: "None", 1: "NormalMap", 2: "TangentMap"},
    # N0-40 (the same headers): blend factors, rasterizer state, procedural texture, lighting LUT inputs and scales
    "BlendFactor": {0: "Zero", 1: "One", 2: "SourceColor", 3: "OneMinusSourceColor", 4: "DestColor",
                    5: "OneMinusDestColor", 6: "SourceAlpha", 7: "OneMinusSourceAlpha", 8: "DestAlpha",
                    9: "OneMinusDestAlpha", 10: "ConstantColor", 11: "OneMinusConstantColor", 12: "ConstantAlpha",
                    13: "OneMinusConstantAlpha", 14: "SourceAlphaSaturate"},
    "CullMode": {0: "KeepAll", 1: "KeepClockWise", 2: "KeepCounterClockWise", 3: "KeepAll2"},          # regs_rasterizer.h
    "ScissorMode": {0: "Disabled", 1: "Exclude", 3: "Include"},
    "DepthBuffering": {0: "WBuffering", 1: "ZBuffering"},
    "ProcTexClamp": {0: "ToZero", 1: "ToEdge", 2: "SymmetricalRepeat", 3: "MirroredRepeat", 4: "Pulse"},  # regs_texturing.h
    "ProcTexCombiner": {0: "U", 1: "U2", 2: "V", 3: "V2", 4: "Add", 5: "Add2", 6: "SqrtAdd2", 7: "Min", 8: "Max",
                        9: "RMax"},
    "ProcTexShift": {0: "None", 1: "Odd", 2: "Even"},
    "ProcTexFilter": {0: "Nearest", 1: "Linear", 2: "NearestMipmapNearest", 3: "LinearMipmapNearest",
                      4: "NearestMipmapLinear", 5: "LinearMipmapLinear"},
    "LightingLutInput": {0: "NH", 1: "VH", 2: "NV", 3: "LN", 4: "SP", 5: "CP"},                        # regs_lighting.h
    "LightingScale": {0: "Scale1", 1: "Scale2", 2: "Scale4", 3: "Scale8", 6: "Scale1_4", 7: "Scale1_2"},
}
# TevStageConfig::Operation (regs_texturing.h): pica_fs_config.cpp masks the stage words of a Dot3_RGBA colour operation
TEV_DOT3_RGBA = 7

# IsLightingSamplerSupported (regs_lighting.h): the lighting configurations that do NOT use a sampler.
_LUT_UNSUPPORTED = {
    "D0": ("Config1",),
    "D1": ("Config0", "Config1", "Config5"),
    "SP": ("Config2", "Config3"),
    "FR": ("Config0", "Config2", "Config4"),
    "RR": ("Config3",),
    "RG": ("Config0", "Config1", "Config2", "Config3", "Config6"),     # supported: Config4, Config5, Config7
    "RB": ("Config0", "Config1", "Config2", "Config3", "Config6"),
}

R_TRIGGER = REGS["trigger_draw"].index
R_TRIGGER_INDEXED = REGS["trigger_draw_indexed"].index
R_IMM_INDEX = REGS["default_attr_index"].index
R_IMM_VALUES = frozenset(REGS[f"default_attr_value{n}"].index for n in range(3))
R_CMDBUF = frozenset(REGS[f"command_buffer_trigger{n}"].index for n in range(2))
IMMEDIATE_MODE_INDEX = 0xF                      # pica_core.cpp SubmitImmediate

# Registers whose write leaves the snapshot unchanged.
_INERT = frozenset({R_TRIGGER, R_TRIGGER_INDEXED, R_IMM_INDEX} | set(R_IMM_VALUES) | set(R_CMDBUF)
                   | {REGS[f"{u}_{k}"].index for u in UNIT_BASE for k in ("program_offset", "swizzle_offset")}
                   | {REGS["num_vertices"].index})
# Registers that change a shader program (hash recomputed).
_PROGRAM_WORD = {REGS[f"{u}_program_word{n}"].index: u for u in UNIT_BASE for n in range(8)}
_SWIZZLE_WORD = {REGS[f"{u}_swizzle_word{n}"].index: u for u in UNIT_BASE for n in range(8)}
_PROGRAM_OFFSET = {REGS[f"{u}_program_offset"].index: u for u in UNIT_BASE}
_SWIZZLE_OFFSET = {REGS[f"{u}_swizzle_offset"].index: u for u in UNIT_BASE}
_MAIN_OFFSET = {FIELDS[f"{u}_main_offset"].reg: u for u in UNIT_BASE}

_MASK_CACHE = {m: gcap.mask_expand(m) for m in range(16)}


def enum_name(enum: str, value: int) -> str:
    return ENUMS[enum].get(value, f"?{value}")


def fld(regs: Sequence[int], name: str) -> int:
    f = FIELDS[name]
    return (regs[f.reg] >> f.pos) & ((1 << f.bits) - 1)


# --------------------------------------------------------------------------
# Register replay

class Blob:
    """Shader words of one unit (program code or swizzle patterns) with the span a program hash covers."""

    SIZE = 4096

    def __init__(self, words: list[int], span: int, limit: int = 4096) -> None:
        self.limit = limit                      # Azahar ignores a write at or past it (vs program: 512)
        self.words = words
        self.span = span
        self.sess_end = 0
        self.sess_zero = False

    @staticmethod
    def initial_span(words: Sequence[int], declared: int) -> int:
        if 0 < declared <= Blob.SIZE:
            return declared
        last = max((i for i, w in enumerate(words) if w), default=-1)
        return last + 1

    def start(self, offset: int) -> None:
        """The game wrote the offset register: a new upload begins there."""
        self.sess_end = offset
        self.sess_zero = offset == 0

    def put(self, offset: int, value: int) -> bool:
        if offset >= self.limit:
            return False                        # Azahar: "Invalid program offset", ignored
        self.words[offset] = value
        self.sess_end = max(self.sess_end, offset + 1)
        self.span = self.sess_end if self.sess_zero else max(self.span, self.sess_end)
        return True


class ShaderUnit:
    def __init__(self, setup: bytes, code_limit: int = 4096) -> None:
        entry, biggest_prog, biggest_swz = struct.unpack_from("<3I", setup, 0)
        del entry
        code = list(struct.unpack_from(f"<{Blob.SIZE}I", setup, 12))
        swz = list(struct.unpack_from(f"<{Blob.SIZE}I", setup, 12 + 4 * Blob.SIZE))
        self.code = Blob(code, Blob.initial_span(code, biggest_prog), code_limit)
        self.swz = Blob(swz, Blob.initial_span(swz, biggest_swz))
        self._hash: str | None = None

    def dirty(self) -> None:
        self._hash = None

    def program_hash(self) -> str:
        if self._hash is None:
            if self.code.span == 0:
                self._hash = "none"
            else:
                h = hashlib.blake2b(digest_size=6)
                h.update(struct.pack(f"<{self.code.span}I", *self.code.words[:self.code.span]))
                h.update(b"|")
                h.update(struct.pack(f"<{self.swz.span}I", *self.swz.words[:self.swz.span]))
                self._hash = h.hexdigest()
        return self._hash


class Replayer:
    """The PICA register file of one capture, with the shader programs and the immediate-mode queue."""

    def __init__(self, init_payload: bytes) -> None:
        blocks = dict(gcap.parse_init(init_payload))
        for kind in (gcap.K_PICA_REGS, gcap.K_VS_SETUP, gcap.K_GS_SETUP, gcap.K_IMMEDIATE):
            if kind not in blocks:
                raise gcap.GcapError(f"INIT: no {gcap.INIT_BLOCK_NAMES[kind]} block")
        self.regs: list[int] = list(struct.unpack(f"<{gcap.NUM_PICA_REGS}I", blocks[gcap.K_PICA_REGS]))
        self.units = {"vs": ShaderUnit(blocks[gcap.K_VS_SETUP], 512),   # pica_core.cpp: "Invalid VS program offset" at >= 512
                       "gs": ShaderUnit(blocks[gcap.K_GS_SETUP])}
        # ImmediateModeState: input_vertex 256 bytes, current_attribute, reset_geometry_pipeline, queue{buffer[4], index}
        imm = blocks[gcap.K_IMMEDIATE]
        self.imm_current = struct.unpack_from("<I", imm, 256)[0]
        self.imm_queue = struct.unpack_from("<I", imm, 256 + 8 + 16)[0]
        self.imm_index = self.regs[R_IMM_INDEX]
        self.version = 0
        self._snap_version = -1
        self._snap: tuple[tuple, dict] | None = None

    # -- writes ----------------------------------------------------------
    def write(self, reg: int, mask: int, value: int) -> str | None:
        """Apply one PICA_REG entry; return the event it causes: array, indexed, imm_reset, imm_vertex, cmdbuf."""
        m = _MASK_CACHE[mask]
        self.regs[reg] = (self.regs[reg] & ~m & 0xFFFFFFFF) | (value & m)
        if reg not in _INERT:
            self.version += 1
        if reg in _PROGRAM_WORD:
            self._upload(_PROGRAM_WORD[reg], True, value)
        elif reg in _SWIZZLE_WORD:
            self._upload(_SWIZZLE_WORD[reg], False, value)
        elif reg in _PROGRAM_OFFSET:
            self.units[_PROGRAM_OFFSET[reg]].code.start(self.regs[reg])
        elif reg in _SWIZZLE_OFFSET:
            self.units[_SWIZZLE_OFFSET[reg]].swz.start(self.regs[reg])
        elif reg in _MAIN_OFFSET:
            self.units[_MAIN_OFFSET[reg]].dirty()
        elif reg == R_TRIGGER:
            return "array"
        elif reg == R_TRIGGER_INDEXED:
            return "indexed"
        elif reg == R_IMM_INDEX:
            self.imm_index = self.regs[reg]
            self.imm_current = 0
            self.imm_queue = 0
            return "imm_reset"
        elif reg in R_IMM_VALUES:
            return self._immediate_word()
        elif reg in R_CMDBUF:
            return "cmdbuf"
        return None

    def _upload(self, unit: str, is_code: bool, value: int) -> None:
        base = UNIT_BASE[unit]
        off_reg = base + (0x1B if is_code else 0x25)
        blob = self.units[unit].code if is_code else self.units[unit].swz
        offset = self.regs[off_reg]
        if blob.put(offset, value & 0xFFFFFFFF):
            self.regs[off_reg] = offset + 1                     # Azahar: offset++
            self.units[unit].dirty()

    def _immediate_word(self) -> str | None:
        """SubmitImmediate: three words make an attribute (PackedAttribute::Push)."""
        self.imm_queue += 1
        if self.imm_queue < 3:
            return None
        self.imm_queue = 0
        index = self.imm_index
        if index > IMMEDIATE_MODE_INDEX:
            return None
        if index < IMMEDIATE_MODE_INDEX:
            self.imm_index += 1
            self.regs[R_IMM_INDEX] = self.imm_index
            return None
        if self.imm_current < fld(self.regs, "max_input_attrib_index"):
            self.imm_current += 1
            return None
        self.imm_current = 0
        return "imm_vertex"

    # -- the state at a draw ----------------------------------------------
    def snapshot(self) -> tuple[tuple, dict]:
        """(hashable key, dict) of the state a draw uses; cached until a relevant register changes."""
        if self._snap is None or self._snap_version != self.version:
            s = decode_state(self.regs, self.units)
            self._snap = (tuple(s.values()), s)
            self._snap_version = self.version
        return self._snap


def decode_state(regs: Sequence[int], units: dict[str, ShaderUnit]) -> dict:
    """Everything a draw's configuration depends on, as plain hashable values (see the module docstring)."""
    f = lambda name: fld(regs, name)                                    # noqa: E731
    s: dict = {}
    s["topology"] = enum_name("TriangleTopology", f("triangle_topology"))
    use_gs = f("use_gs")
    s["use_gs"] = enum_name("UseGS", use_gs) == "Yes"
    s["gs_mode"] = enum_name("GSMode", f("gs_mode")) if s["use_gs"] else ""
    s["index_format"] = enum_name("IndexFormat", f("index_format"))
    # vertex attributes (PipelineRegs::vertex_attributes, VertexLoader)
    total = f("max_attribute_index") + 1
    mask = f("attribute_mask")
    attrs = []
    for i in range(total):
        if i >= 12 or mask & (1 << i):
            attrs.append("fixed")
        else:
            attrs.append(f"{enum_name('VertexAttributeFormat', f(f'attr_format{i}'))}x{f(f'attr_size{i}') + 1}")
    s["attrs"] = tuple(attrs)
    s["strides"] = tuple(f(f"loader{i}_byte_count") for i in range(12) if f(f"loader{i}_component_count"))
    # shaders
    s["vs"] = units["vs"].program_hash()
    s["vs_entry"] = f("vs_main_offset")
    s["gs"] = units["gs"].program_hash() if s["use_gs"] else ""
    # TEV: sources, modifiers, operations, scales of the six stages and the combiner-buffer update masks
    # (TextureConfig::TextureConfig in pica_fs_config.cpp; the constant colours are data, not configuration)
    h = hashlib.blake2b(digest_size=6)
    for n in range(6):
        h.update(struct.pack("<4I", *(regs[REGS[f"tev{n}_{k}"].index]
                                      for k in ("sources", "modifiers", "ops", "scales"))))
    h.update(struct.pack("<2B", f("tev_update_rgb"), f("tev_update_a")))
    s["tev"] = h.hexdigest()
    # lighting
    lit = f("light_disable") == 0
    s["light_on"] = lit
    s["light_reg8f"] = f("fragment_lighting_enable") == 1
    nlights = f("light_max_index") + 1 if lit else 0
    cfg = enum_name("LightingConfig", f("light_config")) if lit else ""
    slots = [f(f"light_slot{k}") for k in range(nlights)]
    spot = sum(1 for n in slots if not f("light_disable_spot_atten") >> n & 1)
    dist = sum(1 for n in slots if not f("light_disable_dist_atten") >> n & 1)
    luts = []
    if lit:
        for lut in ("D0", "D1", "FR", "RR", "RG", "RB"):
            if f(f"light_disable_lut_{lut.lower()}") == 0 and cfg not in _LUT_UNSUPPORTED[lut]:
                luts.append(lut)
        if cfg not in _LUT_UNSUPPORTED["SP"] and spot:
            luts.append("SP")
        if dist:
            luts.append("DA")
    s["light_n"] = nlights
    s["light_config"] = cfg
    s["light_luts"] = tuple(sorted(luts))
    s["light_spot"] = spot
    s["light_dist"] = dist
    s["light_bump"] = enum_name("LightingBumpMode", f("light_bump_mode")) if lit else ""
    s["light_shadow"] = lit and f("light_enable_shadow") == 1
    # fog, procedural texture, fragment operations
    s["fog"] = enum_name("FogMode", f("fog_mode"))
    s["proctex"] = f("proctex_enable") == 1
    s["frag_mode"] = enum_name("FragmentOperationMode", f("fragment_operation_mode"))
    s["blend"] = f("alphablend_enable") == 1
    s["blend_eq"] = (f"{enum_name('BlendEquation', f('blend_equation_rgb'))}/"
                     f"{enum_name('BlendEquation', f('blend_equation_a'))}") if s["blend"] else ""
    s["logic_op"] = enum_name("LogicOp", f("logic_op"))
    s["color_write"] = bool(f("allow_color_write")) and bool(
        f("red_enable") or f("green_enable") or f("blue_enable") or f("alpha_enable"))
    s["alpha_test"] = enum_name("CompareFunc", f("alpha_test_func")) if f("alpha_test_enable") else "off"
    s["stencil"] = enum_name("CompareFunc", f("stencil_func")) if f("stencil_enable") else "off"
    s["stencil_ops"] = "/".join(enum_name("StencilAction", f(k)) for k in
                                ("stencil_fail", "stencil_depth_fail", "stencil_depth_pass")) if f("stencil_enable") else ""
    s["depth_test"] = enum_name("CompareFunc", f("depth_test_func")) if f("depth_test_enable") else "off"
    s["depth_write"] = f("depth_write_enable") == 1 and f("allow_depth_stencil_write") != 0
    # Azahar's using_depth_fb (vk_rasterizer.cpp, gl_rasterizer.cpp): not shadow rendering, a depth buffer address,
    # and (depth write, depth test, or stencil test with a D24S8 buffer)
    s["depth_used"] = (s["frag_mode"] != "Shadow" and f("depth_buffer_address") != 0
                       and (s["depth_write"] or f("depth_test_enable") == 1
                            or (enum_name("DepthFormat", f("depth_format")) == "D24S8" and f("stencil_enable") == 1)))
    s["color_buffer"] = f("color_buffer_address") != 0
    # textures: (unit, type, format, width, height, border colour or -1 when not in use)
    tex = []
    for u in range(3):
        if not f(f"tex{u}_enable"):
            continue
        ttype = enum_name("TextureType", f("tex0_type")) if u == 0 else "Texture2D"
        wraps = (enum_name("WrapMode", f(f"tex{u}_wrap_s")), enum_name("WrapMode", f(f"tex{u}_wrap_t")))
        border = regs[REGS[f"tex{u}_border"].index] if any(w in ("ClampToBorder", "ClampToBorder2") for w in wraps) else -1
        tex.append((u, ttype, enum_name("TextureFormat", f(f"tex{u}_format")),
                    f(f"tex{u}_width"), f(f"tex{u}_height"), border))
    s["tex"] = tuple(tex)
    s["color_format"] = enum_name("ColorFormat", f("color_format"))
    s["depth_format"] = enum_name("DepthFormat", f("depth_format"))
    s["ext"] = decode_ext(regs)
    return s


def decode_ext(regs: Sequence[int]) -> tuple:
    """N0-40: the fields of FSConfig::FSConfig(regs) and of the fixed-function state that decode_state does not hold,
    as a hashable tuple of (name, value) pairs; read with dict(s["ext"]).  Kept apart (and out of `full_state`) so that
    the census's own groups are unchanged."""
    f = lambda name: fld(regs, name)                                    # noqa: E731
    x: dict = {}
    # FramebufferConfig: scissor_test_mode, depthmap_enable, the requested blend of each half (only with blending on)
    blend = f("alphablend_enable") == 1
    for half, eq in (("rgb", "blend_equation_rgb"), ("a", "blend_equation_a")):
        x[f"blend_{half}"] = (enum_name("BlendEquation", f(eq)), enum_name("BlendFactor", f(f"blend_factor_src_{half}")),
                              enum_name("BlendFactor", f(f"blend_factor_dst_{half}"))) if blend else ()
    # vk_rasterizer.cpp: the colour write mask is the four enables, or none when allow_color_write is 0
    x["color_mask"] = ("".join(c for c, n in zip("RGBA", ("red_enable", "green_enable", "blue_enable", "alpha_enable"))
                               if f(n)) or "none") if f("allow_color_write") else "none"
    cull = f("cull_mode")
    x["cull_mode"] = enum_name("CullMode", cull)
    # PicaToVK::FrontFace (pica_to_vk.h): derived from the cull mode alone
    x["front_face"] = "Clockwise" if cull == 2 else "CounterClockwise"
    x["scissor_mode"] = enum_name("ScissorMode", f("scissor_mode"))
    x["depthmap_enable"] = enum_name("DepthBuffering", f("depthmap_enable"))
    # TextureConfig
    x["tex0_type_raw"] = enum_name("TextureType", f("tex0_type"))      # read whether or not unit 0 is enabled
    x["tex2_use_coord1"] = f("tex2_use_coord1") == 1
    x["fog_flip"] = f("fog_flip") == 1
    x["shadow_orthographic"] = f("shadow_orthographic") == 1
    x["combiner_buffer_input"] = f("tev_update_rgb") | f("tev_update_a") << 4
    x["wrap"] = tuple((enum_name("WrapMode", f(f"tex{u}_wrap_s")), enum_name("WrapMode", f(f"tex{u}_wrap_t")))
                      for u in range(3))                                # requested_wrap: all three units, as Azahar reads
    h = hashlib.blake2b(digest_size=6)
    for n in range(6):
        src, mod, ops, scl = (regs[REGS[f"tev{n}_{k}"].index] for k in ("sources", "modifiers", "ops", "scales"))
        if f(f"tev{n}_color_op") == TEV_DOT3_RGBA:                       # the masking of TextureConfig::TextureConfig
            src, mod, ops = src & 0xFFF, mod & 0xFFF, ops & 0xF
        h.update(struct.pack("<4I", src, mod, ops, scl))
    h.update(struct.pack("<B", x["combiner_buffer_input"]))
    x["tev_masked"] = h.hexdigest()
    # LightConfig (only when lighting is not disabled)
    if f("light_disable") == 0:
        x["light_flags"] = tuple((n, f(f"light_{n}")) for n in (
            "enable_primary_alpha", "enable_secondary_alpha", "bump_selector", "clamp_highlights",
            "disable_bump_renorm", "enable_shadow"))
        x["light_shadow"] = tuple((n, f(f"light_{n}")) for n in (
            "shadow_primary", "shadow_secondary", "shadow_invert", "shadow_alpha", "shadow_selector")
        ) if f("light_enable_shadow") else ()
        lights = []
        for k in range(f("light_max_index") + 1):
            num = f(f"light_slot{k}")
            lights.append((num,) + tuple(f(f"light{num}_{n}") for n in (
                "directional", "two_sided_diffuse", "geometric_factor_0", "geometric_factor_1")) + (
                int(not f("light_disable_dist_atten") >> num & 1), int(not f("light_disable_spot_atten") >> num & 1),
                int(not f("light_disable_shadow") >> num & 1)))
        x["lights"] = tuple(lights)
        luts = []
        for lut in LUT_NAMES:
            # lut_sp has no enable bit: "a dummy field due to lack of the corresponding register" (pica_fs_config.cpp)
            on = lut == "sp" or f(f"light_disable_lut_{lut}") == 0
            luts.append((lut, on, f(f"light_abs_disable_{lut}") == 0 if on else "",
                         enum_name("LightingLutInput", f(f"light_lut_input_{lut}")) if on else "",
                         enum_name("LightingScale", f(f"light_lut_scale_{lut}")) if on else ""))
        x["luts"] = tuple(luts)
    else:
        x["light_flags"] = x["light_shadow"] = x["lights"] = x["luts"] = ()
    # ProcTexConfig (only when texture3 is enabled)
    if f("proctex_enable"):
        x["proctex"] = (
            ("coord", f("proctex_coord")),
            ("u_clamp", enum_name("ProcTexClamp", f("proctex_u_clamp"))),
            ("v_clamp", enum_name("ProcTexClamp", f("proctex_v_clamp"))),
            ("color_combiner", enum_name("ProcTexCombiner", f("proctex_color_combiner"))),
            ("alpha_combiner", enum_name("ProcTexCombiner", f("proctex_alpha_combiner"))),
            ("separate_alpha", f("proctex_separate_alpha")),
            ("noise_enable", f("proctex_noise_enable")),
            ("u_shift", enum_name("ProcTexShift", f("proctex_u_shift"))),
            ("v_shift", enum_name("ProcTexShift", f("proctex_v_shift"))),
            ("lut_width", f("proctex_lut_width")),
            ("lut_offsets", tuple(f(f"proctex_lut_offset{n}") for n in range(4))),
            ("lod_min", f("proctex_lut_lod_min")),
            ("lod_max", f("proctex_lut_lod_max")),
            ("lut_filter", enum_name("ProcTexFilter", f("proctex_lut_filter"))))
    else:
        x["proctex"] = ()
    return tuple(x.items())


# --------------------------------------------------------------------------
# Features of a draw

def draw_features(s: dict, kind: str) -> dict[str, set[str]]:
    """feature -> the values it takes in this draw (a draw counts once per value)."""
    out: dict[str, set[str]] = {}

    def add(feature: str, value: object) -> None:
        out.setdefault(feature, set()).add(str(value))

    add("draw_kind", kind)
    add("topology", s["topology"])
    add("geometry_shader", "Yes" if s["use_gs"] else "No")
    if s["use_gs"]:
        add("gs_mode", s["gs_mode"])
    add("index_size", {"BYTE": "8-bit", "SHORT": "16-bit"}.get(s["index_format"], s["index_format"])
        if kind == "indexed" else "none")
    add("attr_count", len(s["attrs"]))
    for a in s["attrs"]:
        add("attr_format", a)
    add("loader_count", len(s["strides"]))
    for st in s["strides"]:
        add("loader_stride", st)
    add("vs_program", s["vs"])
    add("vs_entry", s["vs_entry"])
    if s["use_gs"]:
        add("gs_program", s["gs"])
    add("tev_config", s["tev"])
    add("lighting", "on" if s["light_on"] else "off")
    add("lighting_reg_0x08F", "set" if s["light_reg8f"] else "clear")
    if s["light_on"]:
        add("light_count", s["light_n"])
        add("light_config", s["light_config"])
        add("light_bump_mode", s["light_bump"])
        add("light_lut_set", "+".join(s["light_luts"]) or "none")
        for lut in s["light_luts"]:
            add("light_lut", lut)
        add("light_shadow_enable", "on" if s["light_shadow"] else "off")
    add("fog_mode", s["fog"])
    add("procedural_texture", "on" if s["proctex"] else "off")
    add("fragment_operation_mode", s["frag_mode"])
    if not s["color_write"]:
        om = "no colour write"
    elif s["blend"]:
        om = "blend"
    else:
        om = f"logic:{s['logic_op']}"
    add("output_merger", om)
    if s["blend"]:
        add("blend_equation", s["blend_eq"])
    add("alpha_test", s["alpha_test"])
    add("stencil_test", s["stencil"])
    if s["stencil_ops"]:
        add("stencil_ops", s["stencil_ops"])
    add("depth_test", s["depth_test"])
    add("depth_write", "on" if s["depth_write"] else "off")
    depth_used = s["depth_used"]
    add("depth_in_use", "yes" if depth_used else "no")
    add("color_format", s["color_format"])
    add("depth_format", s["depth_format"])
    add("texture_units", len(s["tex"]))
    for (u, ttype, fmt, w, h, border) in s["tex"]:
        add("texture_type", ttype)
        add("texture_type_unit", f"unit{u}:{ttype}")
        add("texture_format", fmt)
        add("texture_format_unit", f"unit{u}:{fmt}")
        add("texture_size", f"{w}x{h}")
        add("texture_size_unit", f"unit{u}:{w}x{h}")
        if border != -1:
            add("border_colour", f"0x{border:08x}")
    # one flag per question of docs/research/metal.md
    if (s["frag_mode"] != "Shadow" and not s["blend"] and s["color_write"] and s["color_buffer"]
            and s["logic_op"] != "Copy"):
        add("logic_op_effective", s["logic_op"])
        add("logic_op_class", LOGIC_EMULATED if s["logic_op"] in _LOGIC_SPIRV else LOGIC_UNHANDLED)
        add("q_logic_ops", "yes")
    if s["topology"] == "Fan":
        add("q_fans", "yes")
    if s["use_gs"]:
        add("q_geometry_shaders", "yes")
    if s["frag_mode"] == "Shadow":
        add("q_shadow_passes", "yes")
    if any(u == 0 and ttype in ("Shadow2D", "ShadowCube") for (u, ttype, *_r) in s["tex"]):
        add("q_shadow_textures", "yes")
    if s["fog"] == "Gas" or s["frag_mode"] == "Gas":
        add("q_gas", "yes")
    if s["proctex"]:
        add("q_procedural_textures", "yes")
    if s["depth_format"] in ("D24", "D24S8") and depth_used:
        add("q_depth24", "yes")
    if s["light_on"]:
        add("q_lighting", "yes")
    if s["fog"] == "Fog":
        add("q_fog", "yes")
    if s["blend"] and ("Min" in s["blend_eq"] or "Max" in s["blend_eq"]):
        add("q_blend_minmax", "yes")
    if kind == "indexed" and s["index_format"] == "BYTE":
        add("q_index8", "yes")
    if any(st % 4 for st in s["strides"]):
        add("q_stride_not_mult4", "yes")
    if any(t[5] != -1 for t in s["tex"]):
        add("q_border_colour", "yes")
    if any(t[0] == 0 and t[1] == "TextureCube" for t in s["tex"]):
        add("q_cube_texture", "yes")
    if any(t[0] == 0 and t[1] == "Projection2D" for t in s["tex"]):
        add("q_projection_texture", "yes")
    return out


def group_keys(s: dict) -> dict[str, str]:
    """Configuration groups whose unique values are counted."""
    keys = {
        "vertex_layout": f"{','.join(s['attrs'])};strides={','.join(map(str, s['strides']))};topology={s['topology']}",
        "vs_program": s["vs"],
        "tev_config": s["tev"],
        "lighting_config": (f"on;n={s['light_n']};{s['light_config']};luts={'+'.join(s['light_luts']) or 'none'};"
                            f"bump={s['light_bump']}" if s["light_on"] else "off"),
        "fragment_config": (f"{s['frag_mode']};{'blend:' + s['blend_eq'] if s['blend'] else 'logic:' + s['logic_op']};"
                            f"colour={'on' if s['color_write'] else 'off'};alpha={s['alpha_test']};"
                            f"stencil={s['stencil']};depth={s['depth_test']}/{'w' if s['depth_write'] else '-'}"),
        "texture_config": ";".join(f"u{u}:{t}:{fm}:{w}x{h}" for (u, t, fm, w, h, _b) in s["tex"]) or "none",
        "render_target": f"{s['color_format']}+{s['depth_format']}",
    }
    if s["gs"]:             # a geometry shader program exists only for draws that run one; "no program" is not a program
        keys["gs_program"] = s["gs"]
    return keys


# --------------------------------------------------------------------------
# Census of one label

class Agg:
    __slots__ = ("draws", "vertices", "frames")

    def __init__(self) -> None:
        self.draws = 0
        self.vertices = 0
        self.frames: set[int] = set()


class CaptureInfo(NamedTuple):
    file: str
    size: int
    records: int
    frame_markers: int
    pica_reg_entries: int
    draw_triggers: int
    unrecorded_register_writes: int = 0


class LabelCensus:
    def __init__(self, label: str) -> None:
        self.label = label
        self.captures: list[CaptureInfo] = []
        self.snaps: dict[tuple, dict] = {}
        self.aggs: dict[tuple, Agg] = {}            # (snapshot key, draw kind) -> Agg
        self.immediate_vertices = 0
        self.command_buffer_triggers = 0
        self.frames_total = 0                       # FRAME records
        self.frames_with_draws: set[int] = set()
        self.draws = Counter()                      # per kind
        self.manifest_notes: list[str] = []

    def add_draw(self, key: tuple, snap: dict, kind: str, frame_id: int, vertices: int) -> None:
        self.snaps.setdefault(key, snap)
        agg = self.aggs.get((key, kind))
        if agg is None:
            agg = self.aggs[(key, kind)] = Agg()
        agg.draws += 1
        agg.vertices += vertices
        agg.frames.add(frame_id)
        self.frames_with_draws.add(frame_id)
        self.draws[kind] += 1

    @property
    def total_draws(self) -> int:
        return sum(self.draws.values())


class CaptureError(Exception):
    """A capture failed: unreadable, truncated, or its structure is wrong."""


def read_manifest(path: str) -> tuple[int, list[str]]:
    """(unrecorded_register_writes, notes) of the manifest `<capture>.json`: register-started command lists
    leave no PICA_REG record (docs/native/wp/N0.md, N0-05 "As built"). A missing manifest is silent, a garbled one noted."""
    mpath = path + ".json"
    if not os.path.isfile(mpath):
        return 0, []
    try:
        with open(mpath, encoding="utf-8") as f:
            value = json.load(f)["unrecorded_register_writes"]
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValueError("not a count")
        return value, []
    except (OSError, ValueError, KeyError, TypeError):
        return 0, [f"{os.path.basename(mpath)}: manifest unreadable or without a valid unrecorded_register_writes"]


def census_capture(path: str, census: LabelCensus) -> CaptureInfo:
    """Replay one capture into `census`; raise CaptureError with a one-line message on any damage."""
    cap_idx = len(census.captures)
    st = {"records": 0, "frames": 0, "entries": 0, "draws": 0}
    rp: Replayer | None = None
    in_batch = False

    def tap(records: Iterator[gcap.Record]) -> Iterator[gcap.Record]:
        nonlocal rp, in_batch
        for rec in records:
            st["records"] += 1
            if rec.type == gcap.INIT:
                gcap.check_record(rec)
                if rp is None:
                    rp = Replayer(rec.payload)
            elif rec.type == gcap.FRAME:
                st["frames"] += 1
            elif rec.type == gcap.PICA_REG:
                if rp is None:
                    raise gcap.GcapError("PICA_REG before the INIT record")
                gcap.check_record(rec)
                frame_id = (cap_idx << 32) | rec.frame
                for reg, mask, value in gcap.parse_pica_reg(rec.payload):
                    st["entries"] += 1
                    ev = rp.write(reg, mask, value)
                    if ev is None:
                        continue
                    if ev in ("array", "indexed"):
                        in_batch = False
                        key, snap = rp.snapshot()
                        census.add_draw(key, snap, ev, frame_id, rp.regs[REGS["num_vertices"].index])
                        st["draws"] += 1
                    elif ev == "imm_reset":
                        in_batch = False
                    elif ev == "imm_vertex":
                        census.immediate_vertices += 1
                        if not in_batch:
                            in_batch = True
                            key, snap = rp.snapshot()
                            census.add_draw(key, snap, "immediate", frame_id, 0)
                            st["draws"] += 1
                    elif ev == "cmdbuf":
                        census.command_buffer_triggers += 1
            yield rec

    try:
        problems = gcap.check_records(tap(gcap.read(path)))
    except gcap.GcapError as e:
        raise CaptureError(str(e)) from None
    except OSError as e:
        raise CaptureError(f"cannot read: {e.strerror or e}") from None
    if problems:
        shown = "; ".join(problems[:5]) + (f"; ... {len(problems) - 5} more" if len(problems) > 5 else "")
        raise CaptureError(shown)
    census.frames_total += st["frames"]
    unrecorded, note = read_manifest(path)
    info = CaptureInfo(os.path.basename(path), os.path.getsize(path), st["records"], st["frames"],
                       st["entries"], st["draws"], unrecorded)
    census.manifest_notes += note
    census.captures.append(info)
    return info


# --------------------------------------------------------------------------
# Analysis

QUESTIONS = [
    # key, title, what docs/research/metal.md asks, flag feature, registers
    ("logic_ops", "Logic ops",
     "Does MH4U use logic ops without blending, and which? (Metal: no logic op in the default MoltenVK build; Azahar's SPIR-V "
     "generator draws only Clear, Set, Copy and NoOp correctly.)",
     "q_logic_ops",
     "framebuffer.output_merger: alphablend_enable (0x100 bit 8) clear means logic-op blending; logic_op is 0x102 bits 0-3 "
     "(LogicOp, Copy = 3 is the plain write); colour writes on = allow_color_write (0x113 bits 0-3) non-zero and a channel "
     "enabled in 0x107 bits 8-11; effective unless fragment operation mode (0x100 bits 0-1) is Shadow (Azahar skips the op only for shadow rendering and applies it in mode Gas), and only with a colour buffer address (0x11D) non-zero."),
    ("fans", "Triangle fans",
     "Does MH4U draw triangle fans? (Metal has no fan topology; Azahar falls back to CPU vertex shading for those draws.)",
     "q_fans",
     "pipeline.triangle_topology (0x25E bits 8-9) = Fan (2); counted at each draw trigger."),
    ("geometry_shaders", "Geometry shaders",
     "Does MH4U use PICA geometry shaders? (They are never accelerated; Metal has none.)",
     "q_geometry_shaders",
     "pipeline.use_gs (0x229 bits 0-1) = Yes (2): the draw runs a geometry shader (the first thing DrawArrays checks before it "
     "refuses to accelerate a draw); its mode is pipeline.gs_config.mode (0x252 bits 0-7: Point, VariablePrimitive, "
     "FixedPrimitive). Draws with triangle_topology = Shader (3) (0x25E bits 8-9, programmable primitive assembly) are "
     "listed apart in the same row."),
    ("shadow_passes", "Shadow passes",
     "Does MH4U render shadow maps with the PICA shadow mode, and sample shadow textures? (Needs fragment-shader interlock "
     "or raster order groups on Metal.)",
     "q_shadow_passes",
     "Shadow pass: framebuffer.output_merger.fragment_operation_mode (0x100 bits 0-1) = Shadow (3). Shadow texture: "
     "texturing.texture0.type (0x83 bits 28-30) = Shadow2D (2) or ShadowCube (4) on an enabled unit 0 "
     "(main_config 0x80 bit 0); lighting shadow (0x1C3 bit 0) is listed apart."),
    ("gas", "Gas",
     "Does MH4U use gas? (No Azahar renderer draws it.)",
     "q_gas",
     "texturing.fog_mode (0x0E0 bits 0-2) = Gas (7), or fragment operation mode (0x100 bits 0-1) = Gas (1)."),
    ("procedural_textures", "Procedural textures",
     "Does MH4U use procedural textures?",
     "q_procedural_textures",
     "texturing.main_config.texture3_enable (0x080 bit 10), the procedural texture unit."),
    ("depth_formats", "Depth formats",
     "Does MH4U use 24-bit depth (D24, D24S8)? (Apple GPUs have none; Azahar falls back to D32F.)",
     "q_depth24",
     "framebuffer.framebuffer.depth_format (0x116 bits 0-1: D16 = 0, D24 = 2, D24S8 = 3), counted when the depth buffer is "
     "used by Azahar's rule (using_depth_fb): not shadow rendering, depth_buffer_address (0x11C) non-zero, and depth write "
     "(0x107 bit 12 with 0x115 non-zero), depth test (0x107 bit 0), or D24S8 with stencil test (0x105 bit 0)."),
    ("lighting", "Lighting and LUTs",
     "Which draws use fragment lighting, how many lights, which lookup tables? (The lighting fix-up and the bandwidth cost "
     "are the likely Metal costs.)",
     "q_lighting",
     "lighting.disable (0x1C6 bit 0) clear; lights = max_light_index + 1 (0x1C2 bits 0-2); configuration 0x1C3 bits 4-7; "
     "LUT disables 0x1C4 bits 16-22 (a LUT is listed as enabled only when its disable bit is clear and the lighting "
     "configuration uses that sampler, Azahar's IsLightingSamplerSupported in regs_lighting.h, so the lists below can hold "
     "fewer LUTs than the clear bits of 0x1C4 do); per-light spot and distance attenuation disables 0x1C4 bits 8-15 and "
     "24-31; light slots 0x1D9. The lights, configuration, bump mode, LUT and shadow-enable tables count lit draws only."),
    ("fog", "Fog",
     "Does MH4U use fog? (Fog needs the fog LUT.)",
     "q_fog",
     "texturing.fog_mode (0x0E0 bits 0-2) = Fog (5)."),
]

EXTRA_QUESTIONS = [
    ("blend_minmax", "Min/max blending",
     "Blend equation Min or Max with blending on (emulated on Metal).", "q_blend_minmax"),
    ("index8", "8-bit indices", "Indexed draws with 8-bit indices (pipeline.index_array.format, 0x227 bit 31 clear).", "q_index8"),
    ("stride_not_mult4", "Vertex strides that are not multiples of 4",
     "An active attribute loader whose byte_count (loader word 2 bits 16-23) is not a multiple of 4.", "q_stride_not_mult4"),
    ("border_colour", "Custom border colours",
     "An enabled texture unit with ClampToBorder or ClampToBorder2 wrap (Metal exposes no custom border colours).", "q_border_colour"),
    ("cube_texture", "Cube textures", "Unit 0 enabled with type TextureCube.", "q_cube_texture"),
    ("projection_texture", "Projection textures", "Unit 0 enabled with type Projection2D.", "q_projection_texture"),
    ("shadow_textures", "Shadow textures", "Unit 0 enabled with type Shadow2D or ShadowCube.", "q_shadow_textures"),
]

# Grouped feature tables of the report: (title, [features])
TABLES = [
    ("Draw triggers", ["draw_kind", "topology", "geometry_shader", "gs_mode", "index_size"]),
    ("Vertex attributes", ["attr_count", "attr_format", "loader_count", "loader_stride"]),
    ("Fog, procedural texture, fragment operation mode", ["fog_mode", "procedural_texture", "fragment_operation_mode"]),
    ("Lighting", ["lighting", "lighting_reg_0x08F", "light_count", "light_config", "light_bump_mode", "light_lut",
                  "light_lut_set", "light_shadow_enable"]),
    ("Output merger", ["output_merger", "logic_op_effective", "blend_equation", "alpha_test", "stencil_test",
                       "stencil_ops", "depth_test", "depth_write", "depth_in_use"]),
    ("Textures", ["texture_units", "texture_type", "texture_type_unit", "texture_format", "texture_format_unit",
                  "texture_size", "border_colour"]),
    ("Render targets", ["color_format", "depth_format"]),
]


class Analysis(NamedTuple):
    label: str
    summary: dict
    features: dict[str, dict[str, tuple[int, int]]]       # feature -> value -> (draws, frames)
    unique: dict[str, dict]
    answers: dict[str, dict]


def analyse(c: LabelCensus) -> Analysis:
    feats: dict[str, dict[str, list]] = {}
    groups: dict[str, dict[str, list]] = {}
    for (key, kind), agg in c.aggs.items():
        snap = c.snaps[key]
        for feature, values in draw_features(snap, kind).items():
            table = feats.setdefault(feature, {})
            for v in values:
                slot = table.get(v)
                if slot is None:
                    slot = table[v] = [0, set()]
                slot[0] += agg.draws
                slot[1].update(agg.frames)
        for gname, gkey in group_keys(snap).items():
            table = groups.setdefault(gname, {})
            table[gkey] = table.get(gkey, 0) + agg.draws
        table = groups.setdefault("full_state", {})
        base_key = repr(tuple(v for k, v in snap.items() if k != "ext"))     # the N0-13 state: "ext" (N0-40) is not in it
        table[base_key] = table.get(base_key, 0) + agg.draws
    features = {f: {v: (n, len(fr)) for v, (n, fr) in sorted(t.items())} for f, t in sorted(feats.items())}
    unique = {}
    for gname, t in sorted(groups.items()):
        top = sorted(t.items(), key=lambda kv: (-kv[1], kv[0]))[:20]
        unique[gname] = {"count": len(t), "top": [] if gname == "full_state" else [{"config": k, "draws": n} for k, n in top]}
    total = c.total_draws
    vertices = sum(a.vertices for a in c.aggs.values())
    summary = {
        "captures": [dict(ci._asdict()) for ci in c.captures],
        "draws": {"total": total, "array": c.draws["array"], "indexed": c.draws["indexed"],
                  "immediate": c.draws["immediate"]},
        "vertices_array_and_indexed": vertices,
        "immediate_vertices": c.immediate_vertices,
        "command_buffer_triggers": c.command_buffer_triggers,
        "frames_with_draws": len(c.frames_with_draws),
        "frame_markers": c.frames_total,
        "unrecorded_register_writes": sum(ci.unrecorded_register_writes for ci in c.captures),
    }
    answers: dict[str, dict] = {}
    for key, _title, _ask, flag, *_rest in QUESTIONS:
        answers[key] = _answer(features, flag, total)
    answers["geometry_shaders"]["topology_shader_draws"] = features.get("topology", {}).get("Shader", (0, 0))[0]
    for key, _title, _desc, flag in EXTRA_QUESTIONS:
        answers[key] = _answer(features, flag, total)
    return Analysis(c.label, summary, features, unique, answers)


def _answer(features: dict, flag: str, total: int) -> dict:
    draws, frames = features.get(flag, {}).get("yes", (0, 0))
    if total == 0:
        answer = "n/a"
    else:
        answer = "YES" if draws else "NO"
    return {"answer": answer, "draws": draws, "frames": frames}


# --------------------------------------------------------------------------
# N0-40: the pipeline-key groups, the extra features and the union across labels (--union only)

def _h6(text: str) -> str:
    return hashlib.blake2b(text.encode(), digest_size=6).hexdigest()


def _blend_half(x: dict, half: str) -> str:
    eq, src, dst = x[f"blend_{half}"]
    return f"{eq}({src}/{dst})"


def needs_fixup(s: dict) -> bool:
    """The fixup stage of the design's 4.3 and 5.5: the fixed geometry shader applies to lit draws shaded by the
    translated (hardware) vertex shader, that is lighting on and the game's geometry shader not in use (use_gs clear,
    the draws Azahar's AccelerateDraw accepts).  Draws that set use_gs are CPU draws and take no fix-up."""
    return bool(s["light_on"] and not s["use_gs"])


def pipeline_groups(s: dict) -> dict[str, str]:
    """The groups of the design's 4.3 (docs/native/gpu-fragment-design.md): fs_key, fixed_function, vertex_stage,
    proctex_config (only for draws that sample the procedural texture), the parts fs_key is made of, and the joint
    pipeline tuples.  Values are hashes for what is long (fs_key and its parts), readable register values otherwise."""
    x = dict(s["ext"])
    blend = s["blend"]
    # the canonical fragment key (design 4.3): FramebufferConfig as pica_fs_config.cpp builds it, with requested_logic_op
    # zero while blending is on and a half's requested blend zero unless its equation is Min or Max
    half = {h: (_blend_half(x, h) if blend and x[f"blend_{h}"][0] in ("Min", "Max") else "-") for h in ("rgb", "a")}
    framebuffer = (f"scissor={x['scissor_mode']};depthmap={x['depthmap_enable']};shadow={s['frag_mode'] == 'Shadow'};"
                   f"alpha_test={s['alpha_test'] if s['alpha_test'] != 'off' else 'Always'};blend={blend};"
                   f"logic_op={'-' if blend else s['logic_op']};rgb={half['rgb']};a={half['a']}")
    texture = (f"tex0_type={x['tex0_type_raw']};tex2_coord1={x['tex2_use_coord1']};cbi={x['combiner_buffer_input']};"
               f"fog={s['fog']};fog_flip={x['fog_flip']};shadow_ortho={x['shadow_orthographic']};wrap={x['wrap']};"
               f"tev={x['tev_masked']}")
    lighting = repr((s["light_n"], s["light_config"], x["light_flags"], x["light_shadow"], x["lights"], x["luts"],
                     s["light_bump"])) if s["light_on"] else "off"
    proctex = repr(x["proctex"]) if x["proctex"] else "off"
    fs_key = _h6("|".join((framebuffer, texture, lighting, proctex)))
    if blend:
        ff_out = f"blend:{_blend_half(x, 'rgb')},{_blend_half(x, 'a')}"
    else:
        ff_out = f"logic:{s['logic_op']}"
    fixed_function = (f"{ff_out};mask={x['color_mask']};depth={s['depth_test']}/{'w' if s['depth_write'] else '-'};"
                      f"stencil={s['stencil']}[{s['stencil_ops']}];cull={x['cull_mode']};front={x['front_face']}")
    layout = f"{','.join(s['attrs'])};strides={','.join(map(str, s['strides']))};topology={s['topology']}"
    vertex_stage = f"vs={s['vs']};{layout}"
    pass_kind = "ShadowWrite" if s["frag_mode"] == "Shadow" else "Normal"
    formats = f"{s['color_format']}+{s['depth_format']}"
    tup = f"{fs_key}|{fixed_function}|{vertex_stage}|{pass_kind}|{formats}"
    fixup = needs_fixup(s)
    out = {"fs_key": fs_key, "fixed_function": fixed_function, "vertex_stage": vertex_stage,
           "fs_key.framebuffer": _h6(framebuffer), "fs_key.texture": _h6(texture), "fs_key.lighting": _h6(lighting),
           "fs_key.proctex": _h6(proctex),
           "pipeline_tuple": _h6(tup), "pipeline_tuple+fixup": _h6(f"{tup}|fixup={fixup}")}
    if x["proctex"]:
        out["proctex_config"] = ";".join(f"{k}={'/'.join(map(str, v)) if isinstance(v, tuple) else v}"
                                         for k, v in x["proctex"])
    return out


def ext_features(s: dict) -> dict[str, set[str]]:
    """The features of the fields decode_ext reads (a draw counts once per value, as draw_features does)."""
    x = dict(s["ext"])
    out: dict[str, set[str]] = {}

    def add(feature: str, value: object) -> None:
        out.setdefault(feature, set()).add(str(value))

    if s["blend"]:
        for h in ("rgb", "a"):
            eq, src, dst = x[f"blend_{h}"]
            add(f"blend_factors_{h}", f"{eq} {src}/{dst}")
            add("blend_factor_pair", f"{src}/{dst}")
            add("blend_factor", src)
            add("blend_factor", dst)
        add("blend_state", f"rgb {_blend_half(x, 'rgb')}; a {_blend_half(x, 'a')}")
    add("colour_write_mask", x["color_mask"])
    add("cull_mode", x["cull_mode"])
    add("front_face", x["front_face"])
    add("scissor_mode", x["scissor_mode"])
    add("depthmap_enable", x["depthmap_enable"])
    add("texture0_type_register", x["tex0_type_raw"])
    add("texture2_use_coord1", x["tex2_use_coord1"])
    add("fog_flip", x["fog_flip"])
    add("shadow_orthographic", x["shadow_orthographic"])
    add("combiner_buffer_input", x["combiner_buffer_input"])
    for (u, *_r) in s["tex"]:
        ws, wt = x["wrap"][u]
        add("wrap_mode_unit", f"unit{u}:s={ws}/t={wt}")
        for axis, w in (("s", ws), ("t", wt)):
            if w in ("ClampToBorder", "ClampToBorder2"):
                add("border_wrap_kind", w)
    for k, v in x["proctex"]:
        add(f"proctex_{k}", "/".join(map(str, v)) if isinstance(v, tuple) else v)
    if s["light_on"]:
        for k, v in x["light_flags"]:
            if k == "bump_selector":
                add("light_bump_selector", f"texture {v}")
            elif v:
                add("light_config0_flag", k)
        for k, v in x["light_shadow"]:
            if v:
                add("light_shadow_flag", k)
        for (num, directional, two_sided, gf0, gf1, dist, spot, shadow) in x["lights"]:
            for name, v in (("directional", directional), ("two_sided_diffuse", two_sided), ("geometric_factor_0", gf0),
                            ("geometric_factor_1", gf1), ("dist_atten_enable", dist), ("spot_atten_enable", spot),
                            ("shadow_enable", shadow)):
                if v:
                    add("light_flag", name)
        for (lut, on, absolute, typ, scale) in x["luts"]:
            if on:
                add("light_lut_input", f"{lut}:{typ}")
                add("light_lut_abs_input", f"{lut}:{'abs' if absolute else 'signed'}")
                add("light_lut_scale", f"{lut}:{scale}")
    add("lighting_x_geometry_shader",
        f"lighting {'on' if s['light_on'] else 'off'} + geometry shader {'yes' if s['use_gs'] else 'no'}")
    return out


class ExtAnalysis(NamedTuple):
    label: str
    features: dict[str, dict[str, tuple[int, int]]]       # feature -> value -> (draws, frames)
    groups: dict[str, dict[str, int]]                     # group -> value -> draws
    draws: int


def ext_analyse(c: LabelCensus) -> ExtAnalysis:
    feats: dict[str, dict[str, list]] = {}
    groups: dict[str, dict[str, int]] = {}
    for (key, kind), agg in c.aggs.items():
        snap = c.snaps[key]
        for feature, values in ext_features(snap).items():
            table = feats.setdefault(feature, {})
            for v in values:
                slot = table.get(v)
                if slot is None:
                    slot = table[v] = [0, set()]
                slot[0] += agg.draws
                slot[1].update(agg.frames)
        for gname, gkey in pipeline_groups(snap).items():
            table = groups.setdefault(gname, {})
            table[gkey] = table.get(gkey, 0) + agg.draws
    features = {f: {v: (n, len(fr)) for v, (n, fr) in sorted(t.items())} for f, t in sorted(feats.items())}
    return ExtAnalysis(c.label, features, dict(sorted(groups.items())), c.total_draws)


def union_groups(exts: Sequence[ExtAnalysis]) -> dict[str, dict]:
    """group -> {"union": distinct values over all labels, "values": {value: {label: draws}}, "per_label": {...}}:
    the same value in two captures counts once in the union (and once in each label's own count)."""
    names = sorted({g for e in exts for g in e.groups})
    out: dict[str, dict] = {}
    for g in names:
        values: dict[str, dict[str, int]] = {}
        for e in exts:
            for v, n in e.groups.get(g, {}).items():
                values.setdefault(v, {})[e.label] = n
        per_label = {}
        for e in exts:
            mine = e.groups.get(g, {})
            per_label[e.label] = {"distinct": len(mine), "only_here": sum(1 for v in mine if len(values[v]) == 1),
                                  "draws": sum(mine.values())}
        out[g] = {"union": len(values), "in_every_label": sum(1 for v in values.values() if len(v) == len(exts)),
                  "values": values, "per_label": per_label}
    return out


UNION_TOP = 20
UNION_ALL_BELOW = 40        # groups with at most this many distinct values are listed in full in the markdown


def _top_values(info: dict, limit: int) -> list[tuple[str, dict[str, int]]]:
    return sorted(info["values"].items(), key=lambda kv: (-sum(kv[1].values()), kv[0]))[:limit]


def build_union_json(exts: Sequence[ExtAnalysis]) -> dict:
    ug = union_groups(exts)
    return {
        "labels": [e.label for e in exts],
        "groups": {g: {"union": info["union"], "in_every_label": info["in_every_label"],
                       "per_label": info["per_label"],
                       "top": [{"value": v, "draws": per} for v, per in _top_values(info, UNION_TOP)]}
                   for g, info in ug.items()},
        "features": {e.label: {f: {v: {"draws": n, "frames": fr} for v, (n, fr) in t.items()}
                               for f, t in e.features.items()} for e in exts},
    }


EXT_TABLES = [
    ("Blend", ["blend_factors_rgb", "blend_factors_a", "blend_factor_pair", "blend_factor", "blend_state"]),
    ("Fixed-function and rasterizer state", ["colour_write_mask", "cull_mode", "front_face", "scissor_mode", "depthmap_enable"]),
    ("Texture and fog fields of FSConfig", ["texture0_type_register", "texture2_use_coord1", "fog_flip",
                                            "shadow_orthographic", "combiner_buffer_input", "wrap_mode_unit",
                                            "border_wrap_kind"]),
    ("Procedural texture (ProcTexConfig), draws with texture3 enabled",
     ["proctex_coord", "proctex_u_clamp", "proctex_v_clamp", "proctex_color_combiner", "proctex_alpha_combiner",
      "proctex_separate_alpha", "proctex_noise_enable", "proctex_u_shift", "proctex_v_shift", "proctex_lut_width",
      "proctex_lut_offsets", "proctex_lod_min", "proctex_lod_max", "proctex_lut_filter"]),
    ("Lighting (LightConfig), lit draws", ["light_config0_flag", "light_bump_selector", "light_shadow_flag", "light_flag",
                                           "light_lut_input", "light_lut_abs_input", "light_lut_scale"]),
]


def build_union_markdown(exts: Sequence[ExtAnalysis]) -> str:
    labels = [e.label for e in exts]
    ug = union_groups(exts)
    L: list[str] = ["", "# Pipeline-key groups and the union across captures (N0-40)", "",
                    "Written by `tools/native/pica_census.py --union`. The groups follow `docs/native/gpu-fragment-design.md` "
                    "4.3: `fs_key` is the canonical fragment key (Azahar's `FSConfig::FSConfig(regs)` fields, "
                    "`requested_logic_op` zero while blending is on, a half's requested blend zero unless its equation is "
                    "Min or Max, the Dot3_RGBA stage masking applied, the wrap modes of all three units as Azahar reads "
                    "them; the user word and the gas word are not registers and are not in it); `fixed_function` holds the "
                    "blend equations and factors (or the logic op when blending is off), the colour write mask, depth, "
                    "stencil, cull mode and front face; `vertex_stage` holds the vertex program hash, the vertex layout and "
                    "the topology; `proctex_config` the `ProcTexConfig` fields of the draws that sample texture 3. The joint "
                    "`pipeline_tuple` is (`fs_key`, `fixed_function`, `vertex_stage`, pass kind, logical colour and depth "
                    "formats); `pipeline_tuple+fixup` adds the fixup stage (lighting on and the vertex stage is the hardware one, geometry shader bit clear). "
                    "A value seen in several captures counts once in the union. Hashes are 6-byte BLAKE2b digests.", ""]
    L += ["## Distinct values per group", "",
          "Cells are \"distinct values (of them only in this capture's label)\"; the draws are all draws of the label "
          "that have the group (every draw for all groups but `proctex_config`).", ""]
    rows = []
    for g, info in ug.items():
        rows.append([g] + [f"{info['per_label'][l]['distinct']} ({info['per_label'][l]['only_here']})" for l in labels]
                    + [info["union"], info["in_every_label"]])
    L += _table(["group"] + labels + ["union", "in every label"], rows)
    L += _table(["draws with the group"] + labels,
                [[g] + [info["per_label"][l]["draws"] for l in labels] for g, info in ug.items()])
    L += ["## Lit draws against geometry shader", ""]
    L += ["Cells are \"draws (frames)\".", ""]
    L += _table(["lighting + geometry shader"] + labels, _feature_rows(exts, "lighting_x_geometry_shader"))
    for title, feats in EXT_TABLES:
        L += [f"## {title}", ""]
        for feature in feats:
            rows = _feature_rows(exts, feature, limit=30)
            if rows:
                L += _table([feature] + labels, rows)
    for g in ("fixed_function", "proctex_config", "vertex_stage"):
        info = ug.get(g)
        if not info:
            continue
        shown = _top_values(info, UNION_ALL_BELOW)
        L += [f"## `{g}` values" + ("" if len(info["values"]) <= UNION_ALL_BELOW else f" (top {UNION_ALL_BELOW} of {len(info['values'])})"),
              "", "Cells are draws.", ""]
        L += _table(["value"] + labels, [[v] + [per.get(l, 0) for l in labels] for v, per in shown])
    return "\n".join(L).rstrip() + "\n"


# --------------------------------------------------------------------------
# Output

def build_json(analyses: Sequence[Analysis]) -> dict:
    return {
        "tool": TOOL,
        "version": VERSION,
        "labels": [a.label for a in analyses],
        "per_label": {
            a.label: {"summary": a.summary, "features": {f: {v: {"draws": n, "frames": fr} for v, (n, fr) in t.items()}
                                                         for f, t in a.features.items()},
                      "unique": a.unique, "answers": a.answers}
            for a in analyses},
    }


def _table(headers: Sequence[str], rows: Iterable[Sequence[object]]) -> list[str]:
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x).replace("|", "\\|") for x in r) + " |")
    return out + [""]


def _pct(n: int, d: int) -> str:
    return f"{100.0 * n / d:.2f}%" if d else "-"


def _cell(a: Analysis, feature: str, value: str) -> str:
    n, fr = a.features.get(feature, {}).get(value, (0, 0))
    if n == 0:
        return "0"
    return f"{n} ({fr} fr)"


def _feature_rows(analyses: Sequence[Analysis], feature: str, limit: int = 0) -> list[list[str]]:
    values: dict[str, int] = {}
    for a in analyses:
        for v, (n, _fr) in a.features.get(feature, {}).items():
            values[v] = values.get(v, 0) + n

    def sort_key(v: str):
        return (-values[v], v)

    keys = sorted(values, key=sort_key)
    rows = [[v] + [_cell(a, feature, v) for a in analyses] for v in keys[:limit or None]]
    if limit and len(keys) > limit:
        rows.append([f"... {len(keys) - limit} more (see the JSON)"] + ["" for _ in analyses])
    return rows


def build_markdown(analyses: Sequence[Analysis]) -> str:
    labels = [a.label for a in analyses]
    L: list[str] = []
    L += ["# PICA census", "",
          f"Written by `tools/native/pica_census.py` (format v{VERSION}), N0-13, from MGPU captures replayed from their INIT "
          "record; nothing in this file is typed by hand. It holds counts, register values, Azahar's enumerator names and "
          "hashes only: no memory contents, shader instructions or texture bytes. Program and configuration hashes are "
          "6-byte BLAKE2b digests of register or shader words. A count \"N (M fr)\" is N draws in M frames "
          "(a frame is counted once per capture file); \"draws\" are draw triggers: writes to trigger_draw (0x22E) or "
          "trigger_draw_indexed (0x22F), and one per immediate-mode batch.", "",
          "Register positions, bit fields and enumerators are Azahar's (`src/video_core/pica/regs_*.h` at the pinned commit), "
          "checked against those headers by `tests/test_native_pica_census.py`.", ""]
    L += ["## Captures", ""]
    rows = []
    for a in analyses:
        for ci in a.summary["captures"]:
            rows.append([a.label, ci["file"], ci["size"], ci["records"], ci["frame_markers"], ci["pica_reg_entries"],
                         ci["draw_triggers"]])
    L += _table(["label", "file", "bytes", "records", "FRAME records", "PICA_REG entries", "draw triggers"], rows)
    L += _table(["label", "draws", "array", "indexed", "immediate batches", "immediate vertices", "frames with draws",
                 "draws per frame", "command buffer triggers", "unrecorded register writes"],
                [[a.label, a.summary["draws"]["total"], a.summary["draws"]["array"], a.summary["draws"]["indexed"],
                  a.summary["draws"]["immediate"], a.summary["immediate_vertices"], a.summary["frames_with_draws"],
                  f"{a.summary['draws']['total'] / a.summary['frames_with_draws']:.2f}"
                  if a.summary["frames_with_draws"] else "-",
                  a.summary["command_buffer_triggers"], a.summary["unrecorded_register_writes"]] for a in analyses])
    for a in analyses:
        if a.summary["unrecorded_register_writes"]:
            L += [f"{a.label}: {a.summary['unrecorded_register_writes']} register writes of register-started command "
                  "lists are not in the capture; draws made by them are not counted, so NO means 'not seen'.", ""]
    empty = [a.label for a in analyses if a.summary["draws"]["total"] == 0]
    if empty:
        L += [f"**No draw trigger in: {', '.join(empty)}.** Their answers are `n/a`, not NO.", ""]

    L += ["## Answers at a glance", ""]
    rows = []
    for key, title, *_r in QUESTIONS:
        rows.append([title] + [_glance(a.answers[key]) for a in analyses])
    for key, title, *_r in EXTRA_QUESTIONS:
        rows.append([title] + [_glance(a.answers[key]) for a in analyses])
    L += _table(["question"] + labels, rows)
    L += ["Notes on counting: the unique-TEV count is an upper bound, because Azahar's Dot3_RGBA masking of unused stage "
          "words (pica_fs_config.cpp) is not applied; the VS program hash ignores writes at offsets 512 and above, which "
          "Azahar ignores too (pica_core.cpp), but covers the swizzle and code words uploaded before the capture as INIT "
          "recorded them.", ""]
    L += ["YES (n) is n draws; NO means no draw of that label had the feature; `n/a` means the label has no draws.", ""]

    for i, (key, title, ask, flag, regs) in enumerate(QUESTIONS, 1):
        L += [f"## {i}. {title}", "", f"**Question** (docs/research/metal.md): {ask}", "", f"**Registers.** {regs}", ""]
        extra = key == "geometry_shaders"
        L += _table(["label", "answer", "draws", "of all draws", "frames", "of frames with draws"]
                    + (["draws with topology Shader (3), apart"] if extra else []),
                    [[a.label, a.answers[key]["answer"], a.answers[key]["draws"],
                      _pct(a.answers[key]["draws"], a.summary["draws"]["total"]), a.answers[key]["frames"],
                      _pct(a.answers[key]["frames"], a.summary["frames_with_draws"])]
                     + ([a.answers[key]["topology_shader_draws"]] if extra else []) for a in analyses])
        L += _breakdown(key, analyses)
    L += ["## Other items from the census list of docs/research/metal.md", ""]
    L += _table(["item", "what it counts"] + labels,
                [[title, desc] + [_glance(a.answers[key]) for a in analyses] for key, title, desc, _f in EXTRA_QUESTIONS])

    L += ["## Tables per feature", "", "Cells are \"draws (frames)\".", ""]
    for title, feats in TABLES:
        L += [f"### {title}", ""]
        for feature in feats:
            rows = _feature_rows(analyses, feature, limit=25)
            if not rows:
                continue
            L += _table([feature] + labels, rows)
    L += ["### Unique configurations", ""]
    gnames = sorted({g for a in analyses for g in a.unique})
    L += _table(["group"] + labels, [[g] + [a.unique.get(g, {}).get("count", 0) for a in analyses] for g in gnames])
    for a in analyses:
        for g in ("vs_program", "tev_config", "vertex_layout", "fragment_config", "texture_config"):
            top = a.unique.get(g, {}).get("top", [])[:10]
            if top:
                L += [f"Most used `{g}` of {a.label} (top {len(top)} of {a.unique[g]['count']}):", ""]
                L += _table(["config", "draws"], [[t["config"], t["draws"]] for t in top])
    return "\n".join(L).rstrip() + "\n"


def _glance(ans: dict) -> str:
    text = f"YES ({ans['draws']})" if ans["answer"] == "YES" else ans["answer"]
    if ans.get("topology_shader_draws"):
        text += f" + topology Shader ({ans['topology_shader_draws']})"
    return text


def _breakdown(key: str, analyses: Sequence[Analysis]) -> list[str]:
    L: list[str] = []
    if key == "logic_ops":
        L += ["Effective logic ops by op (draws, frames):", ""]
        L += _table(["op"] + [a.label for a in analyses], _feature_rows(analyses, "logic_op_effective")
                    or [["(none)"] + ["0" for _ in analyses]])
        L += ["Effective ops by class (Azahar's SPIR-V generator emulates Clear, Set, Copy and NoOp; the rest draw as Copy):", ""]
        L += _table(["class"] + [a.label for a in analyses], _feature_rows(analyses, "logic_op_class")
                    or [["(none)"] + ["0" for _ in analyses]])
        L += ["Output merger state of all draws (blend, logic op, or colour writes off):", ""]
        L += _table(["output merger"] + [a.label for a in analyses], _feature_rows(analyses, "output_merger", 20))
    elif key == "fans":
        L += _table(["topology"] + [a.label for a in analyses], _feature_rows(analyses, "topology"))
    elif key == "geometry_shaders":
        L += _table(["geometry shader in use"] + [a.label for a in analyses], _feature_rows(analyses, "geometry_shader"))
        rows = _feature_rows(analyses, "gs_mode")
        if rows:
            L += _table(["gs mode"] + [a.label for a in analyses], rows)
        rows = _feature_rows(analyses, "gs_program", 10)
        if rows:
            L += _table(["gs program hash"] + [a.label for a in analyses], rows)
        L += _table(["topology"] + [a.label for a in analyses], _feature_rows(analyses, "topology"))
    elif key == "shadow_passes":
        L += _table(["fragment operation mode"] + [a.label for a in analyses],
                    _feature_rows(analyses, "fragment_operation_mode"))
        L += _table(["label", "shadow textures (unit 0)"],
                    [[a.label, _glance(a.answers["shadow_textures"])] for a in analyses])
        L += ["Lighting shadow (0x1C3 bit 0) is counted over draws with lighting on only:", ""]
        L += _table(["lighting shadow enable"] + [a.label for a in analyses],
                    _feature_rows(analyses, "light_shadow_enable"))
    elif key == "gas":
        L += _table(["fog mode"] + [a.label for a in analyses], _feature_rows(analyses, "fog_mode"))
        L += _table(["fragment operation mode"] + [a.label for a in analyses],
                    _feature_rows(analyses, "fragment_operation_mode"))
    elif key == "procedural_textures":
        L += _table(["procedural texture"] + [a.label for a in analyses], _feature_rows(analyses, "procedural_texture"))
    elif key == "depth_formats":
        L += _table(["depth format (all draws)"] + [a.label for a in analyses], _feature_rows(analyses, "depth_format"))
        L += ["Depth \"in use\" follows Azahar's own `using_depth_fb` (vk_rasterizer.cpp, gl_rasterizer.cpp): not shadow "
              "rendering, depth buffer address (0x11C) non-zero, and (depth write allowed with 0x115 non-zero, or depth test "
              "enabled, or depth format D24S8 with stencil test enabled). This is the rule that decides whether a depth surface "
              "is created, so it is the one that matters for the 24-bit question.", ""]
        L += _table(["depth in use"] + [a.label for a in analyses], _feature_rows(analyses, "depth_in_use"))
    elif key == "lighting":
        L += _table(["lighting"] + [a.label for a in analyses], _feature_rows(analyses, "lighting"))
        L += _table(["lights"] + [a.label for a in analyses], _feature_rows(analyses, "light_count"))
        L += _table(["LUT enabled"] + [a.label for a in analyses], _feature_rows(analyses, "light_lut"))
        L += _table(["LUT set"] + [a.label for a in analyses], _feature_rows(analyses, "light_lut_set", 15))
    elif key == "fog":
        L += _table(["fog mode"] + [a.label for a in analyses], _feature_rows(analyses, "fog_mode"))
    return L


# --------------------------------------------------------------------------
# Command line

_LABEL_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


class UsageError(Exception):
    pass


def _natural(s: str) -> list:
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]


def parse_specs(specs: Sequence[str]) -> list[tuple[str, list[str]]]:
    """LABEL=PATH-or-pattern -> [(label, [paths])], labels in first-seen order."""
    out: dict[str, list[str]] = {}
    for spec in specs:
        label, sep, pattern = spec.partition("=")
        if not sep or not pattern or not _LABEL_RE.match(label):
            raise UsageError(f"bad capture argument {spec!r}: expected LABEL=PATH with LABEL made of letters, digits, _ . -")
        if any(ch in pattern for ch in "*?["):
            paths = sorted(glob.glob(pattern), key=_natural)
            if not paths:
                raise UsageError(f"{label}: no file matches {pattern!r}")
        else:
            if not os.path.isfile(pattern):
                raise UsageError(f"{label}: no such file {pattern!r}")
            paths = [pattern]
        out.setdefault(label, []).extend(paths)
    return list(out.items())


def run(specs: Sequence[str], json_path: str | None, md_path: str | None, strict: bool,
        out=None, err=None, union: bool = False) -> int:
    out = out or sys.stdout
    err = err or sys.stderr
    try:
        plan = parse_specs(specs)
    except UsageError as e:
        print(f"{TOOL}: {e}", file=err)
        return EXIT_USAGE
    censuses = []
    failed = 0
    for label, paths in plan:
        c = LabelCensus(label)
        censuses.append(c)
        for p in paths:
            try:
                info = census_capture(p, c)
            except CaptureError as e:
                print(f"{TOOL}: {label}: {p}: {e}", file=err)
                failed += 1
                continue
            print(f"{TOOL}: {label}: {info.file}: {info.records} records, {info.pica_reg_entries} register writes, "
                  f"{info.draw_triggers} draw triggers", file=out)
            if info.unrecorded_register_writes:
                print(f"{TOOL}: warning: {label}: {info.file}: {info.unrecorded_register_writes} register writes of "
                      f"register-started command lists are not in the capture; draws made by them are not counted",
                      file=err)
        for note in c.manifest_notes:
            print(f"{TOOL}: warning: {label}: {note}", file=err)
    if failed:
        print(f"{TOOL}: {failed} capture(s) failed their checks; nothing written", file=err)
        return EXIT_FAILED
    analyses = [analyse(c) for c in censuses]
    for a in analyses:
        d = a.summary["draws"]
        print(f"{TOOL}: {a.label}: {len(a.summary['captures'])} capture(s), {d['total']} draws "
              f"({d['array']} array, {d['indexed']} indexed, {d['immediate']} immediate) in "
              f"{a.summary['frames_with_draws']} frames", file=out)
    total = sum(a.summary["draws"]["total"] for a in analyses)
    empty = [a.label for a in analyses if a.summary["draws"]["total"] == 0]
    if total == 0:
        print(f"{TOOL}: no draw trigger in any capture: nothing censused is not a pass", file=err)
        return EXIT_NOTHING
    if empty:
        print(f"{TOOL}: warning: no draw trigger in label(s) {', '.join(empty)}", file=err)
    outputs = []
    exts = [ext_analyse(c) for c in censuses] if union else []
    if md_path:
        outputs.append((md_path, build_markdown(analyses) + (build_union_markdown(exts) if union else "")))
    if json_path:
        doc = build_json(analyses)
        if union:
            doc["union"] = build_union_json(exts)
        outputs.append((json_path, json.dumps(doc, indent=1, sort_keys=True) + "\n"))
    try:
        write_all(outputs)
    except OSError as e:
        print(f"{TOOL}: cannot write {e.filename or '?'}: {e.strerror or e}", file=err)
        return EXIT_FAILED
    for path, _text in outputs:
        print(f"{TOOL}: wrote {path}", file=out)
    for a in analyses:
        line = ", ".join(f"{k}={a.answers[k]['answer']}({a.answers[k]['draws']})" for k, *_r in QUESTIONS)
        print(f"{TOOL}: {a.label}: {line}", file=out)
    if strict and empty:
        print(f"{TOOL}: --strict: no draw trigger in label(s) {', '.join(empty)}", file=err)
        return EXIT_NOTHING
    return EXIT_OK


def write_all(outputs: Sequence[tuple[str, str]]) -> None:
    """Write every output to a temporary name, then rename them in order (the JSON last): no half set is left."""
    tmps = []
    try:
        for path, text in outputs:
            tmp = path + ".tmp"
            tmps.append(tmp)
            try:
                os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
                with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            except OSError as e:
                e.filename = path
                raise
        for (path, _text), tmp in zip(outputs, tmps):
            os.replace(tmp, path)
    except OSError:
        for tmp in tmps:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        raise


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="pica_census.py",
        description="Count the PICA features the game uses, from MGPU captures (N0-13).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=("G1-G4 run (labels are explicit; a pattern with * is expanded by the tool):\n"
                "  python3 tools/native/pica_census.py \\\n"
                "      'G1=build/golden/G1/gpu-*.mgpu' 'G2=build/golden/G2/gpu-*.mgpu' \\\n"
                "      'G3=build/golden/G3/gpu-*.mgpu' 'G4=build/golden/G4/gpu-*.mgpu' \\\n"
                "      --json build/pica-census.json --md docs/research/pica-census.md\n"
                "  (run as written, this OVERWRITES docs/research/pica-census.md: pass --json and --md for other runs)\n\n"
                "N0-40: add the G9a, G9b, G9c labels, --union, and --json build/pica-census-union.json "
                "--md build/pica-census-union.md\n\n"
                "exit codes: 0 ok; 1 a capture failed its checks (nothing written); 2 usage; "
                "3 nothing censused (no draw trigger in any capture, or in any label with --strict)"))
    ap.add_argument("captures", nargs="+", metavar="LABEL=PATH",
                    help="a capture file or a glob pattern under a label; repeat a label to add captures")
    ap.add_argument("--json", metavar="FILE", default="build/pica-census.json", help="JSON output (default %(default)s)")
    ap.add_argument("--md", metavar="FILE", default="docs/research/pica-census.md",
                    help="markdown output (default %(default)s)")
    ap.add_argument("--strict", action="store_true", help="exit 3 when any label has no draw trigger")
    ap.add_argument("--union", action="store_true",
                    help="N0-40: also count the pipeline-key groups (fs_key, fixed_function, vertex_stage, proctex_config, "
                         "the joint tuple) per label and over all labels together; the sections are appended to the markdown "
                         "and the JSON gets a \"union\" key")
    args = ap.parse_args(argv)
    return run(args.captures, args.json, args.md, args.strict, union=args.union)


if __name__ == "__main__":
    sys.exit(main())
