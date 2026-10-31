# Derived from Azahar src/video_core/shader/generator/pica_fs_config.cpp (Dot3_RGBA masking, the canonical key inputs),
# src/video_core/renderer_vulkan/pica_to_vk.h (FrontFace), src/video_core/pica/regs_internal.h, regs_pipeline.h, regs_rasterizer.h, regs_texturing.h,
# regs_framebuffer.h, regs_lighting.h, regs_shader.h, pica_core.cpp, src/video_core/renderer_vulkan/vk_rasterizer.cpp
# src/video_core/renderer_opengl/gl_rasterizer.cpp and src/video_core/shader/generator/spv_fs_shader_gen.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
# pica_fs_config.cpp and vk_rasterizer.cpp say "Licensed under GPLv3 or any later version"; the other files, pica_to_vk.h
# included, say "Licensed under GPLv2 or any later version"): the register numbers and bit positions the synthetic register writes use, which register
# writes trigger a draw, the depth-buffer cases of using_depth_fb the depth tests expect, the logic ops the SPIR-V path emulates, and the textual
# parser of those headers that checks the tool's tables (the oracle).
# Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_internal.h, regs_pipeline.h, regs_framebuffer.h, regs_shader.h)
# Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
# Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
# Copyright 2017 Citra Emulator Project (regs_lighting.h)
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, vk_rasterizer.cpp, pica_fs_config.cpp, pica_to_vk.h)
# Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (spv_fs_shader_gen.cpp)
# Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project (gl_rasterizer.cpp)
#
"""tools/native/pica_census.py: PICA census of MGPU captures (synthetic captures only)."""
import io
import json
import os
import re
import struct
import sys

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools", "native"))

import gcap  # noqa: E402
import pica_census as pc  # noqa: E402

DRAW = (0x22E, 0xF, 0)
DRAW_INDEXED = (0x22F, 0xF, 0)


def W(reg, value, mask=0xF):
    return (reg, mask, value)


def build(path, lists, init=None, frames=None):
    """INIT with PICA registers `init`, then one command list (GX + PICA_REG) per entry of `lists`."""
    with gcap.Writer(str(path)) as w:
        w.init({gcap.K_PICA_REGS: gcap.pica_regs_block(init or {})})
        for i, writes in enumerate(lists):
            w.frame_marker((frames[i] if frames else 100 + i))
            w.submit_cmdlist(0x20000000, 0x100)
            w.pica_reg(writes)
    return str(path)


def analyse_one(path, label="T"):
    c = pc.LabelCensus(label)
    pc.census_capture(str(path), c)
    return pc.analyse(c)


def feat(a, name):
    return {v: n for v, (n, _f) in a.features.get(name, {}).items()}


# ---- draw triggers ----------------------------------------------------------------------------------------

def test_array_and_indexed_draws_and_the_byte_mask_does_not_matter(tmp_path):
    p = build(tmp_path / "a.mgpu", [[DRAW, (0x22E, 0x0, 5), DRAW_INDEXED], [W(0x228, 3)]])
    a = analyse_one(p)
    assert a.summary["draws"] == {"total": 3, "array": 2, "indexed": 1, "immediate": 0}
    assert a.summary["frames_with_draws"] == 1
    assert a.summary["frame_markers"] == 2
    assert feat(a, "draw_kind") == {"array": 2, "indexed": 1}


def test_no_draw_trigger_for_other_registers(tmp_path):
    a = analyse_one(build(tmp_path / "a.mgpu", [[W(0x228, 3), W(0x229, 2), W(0x25E, 0x200)]]))
    assert a.summary["draws"]["total"] == 0


def test_command_buffer_triggers_are_counted_but_are_no_draws(tmp_path):
    a = analyse_one(build(tmp_path / "a.mgpu", [[W(0x23C, 1), W(0x23D, 1), DRAW]]))
    assert a.summary["command_buffer_triggers"] == 2
    assert a.summary["draws"]["total"] == 1


def test_immediate_mode_one_draw_per_batch(tmp_path):
    # 0x232 = 0xF selects immediate mode; max_input_attrib_index (0x242) = 1: two attributes of three words per vertex.
    vertex = [W(0x233, 1), W(0x234, 2), W(0x235, 3)] * 2
    p = build(tmp_path / "a.mgpu", [[W(0x242, 1), W(0x232, 0xF)] + vertex * 3, [W(0x232, 0xF)] + vertex, [DRAW]])
    a = analyse_one(p)
    assert a.summary["draws"] == {"total": 3, "array": 1, "indexed": 0, "immediate": 2}
    assert a.summary["immediate_vertices"] == 4


def test_default_attribute_words_are_no_immediate_draw(tmp_path):
    # index 0 < 0xF: the words set default attribute 0, then 1 (the index advances), no vertex is formed
    words = [W(0x233, 1), W(0x234, 2), W(0x235, 3)]
    p = build(tmp_path / "a.mgpu", [[W(0x232, 0)] + words * 2 + [W(0x232, 0xF)]], init={0x242: 0})
    a = analyse_one(p)
    assert a.summary["draws"]["total"] == 0 and a.summary["immediate_vertices"] == 0
    # the index counts up through the attributes written, then immediate mode starts at 0xF
    p = build(tmp_path / "b.mgpu", [[W(0x232, 0xE)] + words + words], init={0x242: 0})
    assert analyse_one(p).summary["immediate_vertices"] == 1


def test_immediate_state_starts_from_the_init_record(tmp_path):
    # INIT says the queue already holds two words of an attribute and the register says immediate mode
    path = tmp_path / "a.mgpu"
    imm = bytearray(gcap.INIT_BLOCK_SIZES[gcap.K_IMMEDIATE])
    struct.pack_into("<I", imm, 256 + 8 + 16, 2)
    with gcap.Writer(str(path)) as w:
        w.init({gcap.K_PICA_REGS: gcap.pica_regs_block({0x232: 0xF}), gcap.K_IMMEDIATE: bytes(imm)})
        w.submit_cmdlist(0x20000000, 0x100)
        w.pica_reg([W(0x235, 9)])
    assert analyse_one(path).summary["draws"]["immediate"] == 1


# ---- topology, indices, attributes ------------------------------------------------------------------------

def test_topology_geometry_shader_and_index_size(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x25E, 0 << 8), DRAW],
        [W(0x25E, 1 << 8), DRAW],
        [W(0x25E, 2 << 8), DRAW, DRAW],
        [W(0x25E, 3 << 8), DRAW],
        [W(0x25E, 0), W(0x229, 2), W(0x252, 1), DRAW],
        [W(0x229, 0), W(0x227, 0), DRAW_INDEXED],
        [W(0x227, 1 << 31), DRAW_INDEXED, DRAW],
    ])
    a = analyse_one(p)
    assert feat(a, "topology") == {"List": 5, "Strip": 1, "Fan": 2, "Shader": 1}
    assert feat(a, "geometry_shader") == {"No": 8, "Yes": 1}
    assert feat(a, "gs_mode") == {"VariablePrimitive": 1}
    assert feat(a, "index_size") == {"none": 7, "8-bit": 1, "16-bit": 1}
    assert a.answers["fans"] == {"answer": "YES", "draws": 2, "frames": 1}
    assert a.answers["geometry_shaders"]["draws"] == 1          # use_gs only; topology Shader is listed apart
    assert a.answers["geometry_shaders"]["topology_shader_draws"] == 1
    assert a.answers["index8"]["draws"] == 1


def test_vertex_attribute_formats_and_strides(tmp_path):
    # 3 attributes: 0 = FLOAT x3 (format 3, size-1 2), 1 = UBYTE x4 (1, 3), 2 = fixed (mask bit 2)
    low = 3 | (2 << 2) | ((1 | (3 << 2)) << 4)
    high = (1 << (16 + 2)) | (2 << 28)
    loader = [W(0x203, 0), W(0x204, 0x10), W(0x205, (14 << 16) | (2 << 28))]    # loader 0: 2 components, 14 bytes
    p = build(tmp_path / "a.mgpu", [[W(0x201, low), W(0x202, high)] + loader + [DRAW]])
    a = analyse_one(p)
    assert feat(a, "attr_format") == {"FLOATx3": 1, "UBYTEx4": 1, "fixed": 1}
    assert feat(a, "attr_count") == {"3": 1}
    assert feat(a, "loader_stride") == {"14": 1}
    assert a.answers["stride_not_mult4"]["draws"] == 1
    p = build(tmp_path / "b.mgpu", [[W(0x201, low), W(0x202, high), W(0x205, (16 << 16) | (1 << 28)), DRAW]])
    assert analyse_one(p).answers["stride_not_mult4"] == {"answer": "NO", "draws": 0, "frames": 0}


def test_short_and_byte_formats(tmp_path):
    low = 2 | (1 << 2) | (0 << 4) | (3 << 6)                  # attr 0 SHORT x2, attr 1 BYTE x4
    p = build(tmp_path / "a.mgpu", [[W(0x201, low), W(0x202, 1 << 28), DRAW]])
    assert feat(analyse_one(p), "attr_format") == {"SHORTx2": 1, "BYTEx4": 1}


# ---- shaders and TEV --------------------------------------------------------------------------------------

def _upload(words, base=0x2B0, offset=0):
    return [W(base + 0x1B, offset)] + [W(base + 0x1C, x) for x in words]


def test_vertex_shader_hash_follows_the_uploaded_words(tmp_path):
    a1 = analyse_one(build(tmp_path / "1.mgpu", [_upload([0x11, 0x22]) + [DRAW]]))
    a2 = analyse_one(build(tmp_path / "2.mgpu", [_upload([0x11, 0x22]) + [DRAW]]))
    a3 = analyse_one(build(tmp_path / "3.mgpu", [_upload([0x11, 0x23]) + [DRAW]]))
    a4 = analyse_one(build(tmp_path / "4.mgpu", [_upload([0x11, 0x22]) + [DRAW] + _upload([0x11, 0x23]) + [DRAW]]))
    (h1,), (h2,), (h3,) = (list(feat(a, "vs_program")) for a in (a1, a2, a3))
    assert h1 == h2 != h3 and len(h1) == 12
    assert list(feat(a4, "vs_program")) == [h1, h3] or set(feat(a4, "vs_program")) == {h1, h3}
    assert a4.unique["vs_program"]["count"] == 2


def test_program_hash_ignores_the_leftover_of_a_longer_earlier_upload(tmp_path):
    # a 3-word program replaced by a 2-word one uploaded from offset 0: the stale third word is not hashed
    a = analyse_one(build(tmp_path / "a.mgpu", [_upload([1, 2, 3]) + _upload([4, 5]) + [DRAW]]))
    b = analyse_one(build(tmp_path / "b.mgpu", [_upload([4, 5]) + [DRAW]]))
    assert feat(a, "vs_program").keys() == feat(b, "vs_program").keys()


def test_entry_point_is_counted_apart_and_gs_program_only_with_gs(tmp_path):
    p = build(tmp_path / "a.mgpu", [[W(0x2BA, 7), DRAW], [W(0x229, 2)] + _upload([9], base=0x280) + [DRAW]])
    a = analyse_one(p)
    assert feat(a, "vs_entry") == {"7": 2}
    assert len(feat(a, "gs_program")) == 1 and sum(feat(a, "gs_program").values()) == 1


def test_swizzle_words_are_part_of_the_program_hash(tmp_path):
    base = _upload([1, 2])
    a = analyse_one(build(tmp_path / "a.mgpu", [base + [W(0x2D5, 0), W(0x2D6, 5), DRAW]]))
    b = analyse_one(build(tmp_path / "b.mgpu", [base + [W(0x2D5, 0), W(0x2D6, 6), DRAW]]))
    assert feat(a, "vs_program").keys() != feat(b, "vs_program").keys()


def test_tev_hash_covers_stage_words_but_not_constant_colours(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0xC0, 0x1234), DRAW],
        [W(0xC3, 0xFFFFFFFF), DRAW],              # stage 0 constant colour: not configuration
        [W(0xF8 + 2, 0x7), DRAW],                 # stage 5 operations
        [W(0xE0, 0x300), DRAW],                   # combiner buffer update mask (bits 8-15 of 0x0E0)
    ])
    a = analyse_one(p)
    counts = sorted(feat(a, "tev_config").values())
    assert counts == [1, 1, 2]


# ---- lighting, fog, proctex -------------------------------------------------------------------------------

def test_lighting_off_on_lights_and_luts(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x1C6, 1), DRAW],
        # on, 2 lights (max index 1), config Config4 (4 << 4), slots 3 and 5
        [W(0x1C6, 0), W(0x1C2, 1), W(0x1C3, 4 << 4), W(0x1D9, 3 | (5 << 4)), DRAW],
    ])
    a = analyse_one(p)
    assert feat(a, "lighting") == {"off": 1, "on": 1}
    assert feat(a, "light_count") == {"2": 1}
    assert feat(a, "light_config") == {"Config4": 1}
    # Config4: reflect R/G/B, distribution 0/1, spotlight; no fresnel. Spot and distance attenuation default to on.
    assert set(feat(a, "light_lut")) == {"D0", "D1", "RR", "RG", "RB", "SP", "DA"}
    assert a.answers["lighting"]["draws"] == 1


def test_lut_disable_bits_and_attenuation_disables(tmp_path):
    # Config0 (reflect R, D0, spotlight): D1/FR/RG/RB unsupported; disable D0 (bit 16) and spot atten of light 0 (bit 8)
    p = build(tmp_path / "a.mgpu", [[W(0x1C3, 0 << 4), W(0x1C4, (1 << 16) | (1 << 8) | (1 << 24)), W(0x1D9, 0), DRAW]])
    a = analyse_one(p)
    assert set(feat(a, "light_lut")) == {"RR"}
    p = build(tmp_path / "b.mgpu", [[W(0x1C3, 5 << 4), DRAW]])          # Config5: R/G/B, D0, fresnel, spot
    assert set(feat(analyse_one(p), "light_lut")) == {"D0", "FR", "RR", "RG", "RB", "SP", "DA"}


def test_lighting_register_08f_is_reported_apart(tmp_path):
    a = analyse_one(build(tmp_path / "a.mgpu", [[W(0x8F, 1), DRAW], [W(0x8F, 0), DRAW]]))
    assert feat(a, "lighting_reg_0x08F") == {"set": 1, "clear": 1}


def test_fog_gas_and_procedural_texture(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0xE0, 0), DRAW], [W(0xE0, 5), DRAW], [W(0xE0, 7), DRAW],
        [W(0x80, 1 << 10), DRAW],
    ])
    a = analyse_one(p)
    assert feat(a, "fog_mode") == {"None": 1, "Fog": 1, "Gas": 2}
    assert a.answers["fog"]["draws"] == 1 and a.answers["gas"]["draws"] == 2   # the fog mode persists
    assert feat(a, "procedural_texture") == {"off": 3, "on": 1}
    assert a.answers["procedural_textures"]["draws"] == 1


# ---- fragment operations ----------------------------------------------------------------------------------

def test_fragment_operation_mode_shadow_and_gas(tmp_path):
    p = build(tmp_path / "a.mgpu", [[W(0x100, 0), DRAW], [W(0x100, 3), DRAW], [W(0x100, 1), DRAW]])
    a = analyse_one(p)
    assert feat(a, "fragment_operation_mode") == {"Default": 1, "Shadow": 1, "Gas": 1}
    assert a.answers["shadow_passes"]["draws"] == 1
    assert a.answers["gas"]["draws"] == 1                       # fragment operation mode Gas counts too


def test_logic_op_without_blending_and_which(tmp_path):
    rgba = W(0x107, 0xF00)
    on = W(0x113, 0xF)
    p = build(tmp_path / "a.mgpu", [
        [W(0x11D, 1), rgba, on, W(0x100, 0), W(0x102, 3), DRAW],                  # Copy: the plain write
        [W(0x102, 11), DRAW],                                        # Xor
        [W(0x102, 11), DRAW],
        [W(0x102, 7), DRAW],                                         # Invert
        [W(0x100, 1 << 8), DRAW],                                    # blending on: the logic op is not used
        [W(0x100, 0), W(0x113, 0), DRAW],                            # colour writes off: not effective
        [W(0x113, 0xF), W(0x107, 0), DRAW],                          # no channel enabled
        [W(0x107, 0xF00), W(0x100, 3), DRAW],                        # shadow pass: not effective
    ])
    a = analyse_one(p)
    assert feat(a, "logic_op_class") == {pc.LOGIC_UNHANDLED: 3}
    assert feat(a, "logic_op_effective") == {"Xor": 2, "Invert": 1}
    assert a.answers["logic_ops"]["draws"] == 3
    om = feat(a, "output_merger")
    assert om["logic:Xor"] == 2 and om["blend"] == 1 and om["no colour write"] == 2
    assert om["logic:Copy"] == 1 and om["logic:Invert"] == 2


def test_logic_op_applies_in_gas_mode_and_needs_a_colour_buffer_and_classes(tmp_path):
    base = [W(0x113, 0xF), W(0x107, 0xF00), W(0x100, 0)]
    p = build(tmp_path / "a.mgpu", [
        base + [W(0x11D, 5), W(0x102, 0), DRAW],                    # Clear: emulated by the SPIR-V path
        [W(0x102, 4), DRAW],                                        # Set
        [W(0x102, 6), DRAW],                                        # NoOp
        [W(0x102, 5), DRAW],                                        # CopyInverted: drawn as Copy
        [W(0x100, 1), W(0x102, 11), DRAW],                          # Gas mode: the op is applied
        [W(0x11D, 0), DRAW],                                        # no colour buffer: nothing is drawn
    ])
    a = analyse_one(p)
    assert feat(a, "logic_op_effective") == {"Clear": 1, "Set": 1, "NoOp": 1, "CopyInverted": 1, "Xor": 1}
    assert feat(a, "logic_op_class") == {pc.LOGIC_EMULATED: 3, pc.LOGIC_UNHANDLED: 2}


def test_blend_equations_and_min_max(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x100, 1 << 8), W(0x101, 0), DRAW],
        [W(0x101, 3 | (4 << 8)), DRAW],
    ])
    a = analyse_one(p)
    assert feat(a, "blend_equation") == {"Add/Add": 1, "Min/Max": 1}
    assert a.answers["blend_minmax"]["draws"] == 1


def test_alpha_test_stencil_and_depth(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x104, 0), W(0x105, 0), W(0x107, 0), DRAW],
        [W(0x104, 1 | (6 << 4)), W(0x105, 1 | (2 << 4)), W(0x106, 2 | (1 << 4) | (3 << 8)),
         W(0x107, 1 | (4 << 4) | (1 << 12)), W(0x115, 3), W(0x11C, 1), DRAW],
    ])
    a = analyse_one(p)
    assert feat(a, "alpha_test") == {"off": 1, "GreaterThan": 1}
    assert feat(a, "stencil_test") == {"off": 1, "Equal": 1}
    assert feat(a, "stencil_ops") == {"Replace/Zero/Increment": 1}
    assert feat(a, "depth_test") == {"off": 1, "LessThan": 1}
    assert feat(a, "depth_write") == {"off": 1, "on": 1}
    assert feat(a, "depth_in_use") == {"no": 1, "yes": 1}


def test_depth_use_rule_differences(tmp_path):
    # Azahar's using_depth_fb: shadow rendering, plain D24 + stencil and a zero depth address do not use the depth buffer
    p = build(tmp_path / "a.mgpu", [
        [W(0x11C, 1), W(0x116, 2), W(0x107, 1), W(0x100, 3), DRAW],     # Shadow + depth test
        [W(0x100, 0), W(0x105, 1), W(0x107, 0), DRAW],                  # D24 + stencil only
        [W(0x107, 1), W(0x11C, 0), DRAW],                               # depth test, address 0
        [W(0x11C, 1), DRAW],                                            # depth test, D24, address set: used
    ])
    a = analyse_one(p)
    assert feat(a, "depth_in_use") == {"no": 3, "yes": 1}
    assert a.answers["depth_formats"]["draws"] == 1


def test_depth_write_needs_the_depth_stencil_write_permission(tmp_path):
    a = analyse_one(build(tmp_path / "a.mgpu", [[W(0x107, 1 << 12), W(0x115, 0), DRAW]]))
    assert feat(a, "depth_write") == {"off": 1}


# ---- textures and render targets --------------------------------------------------------------------------

def test_texture_units_types_formats_sizes(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x80, 0b011),                                             # units 0 and 1
         W(0x83, 2 << 28), W(0x82, (64 << 16) | 32), W(0x8E, 4),     # unit 0: Shadow2D 64x32 RGBA4
         W(0x92, (16 << 16) | 16), W(0x96, 12), DRAW],               # unit 1: 16x16 ETC1
        [W(0x80, 0b001), W(0x83, 1 << 28), W(0x8E, 0), DRAW],        # TextureCube RGBA8
        [W(0x80, 0b001), W(0x83, 3 << 28), DRAW],                    # Projection2D
        [W(0x80, 0b100), W(0x9E, 13), W(0x9A, (8 << 16) | 4), DRAW],  # unit 2 only
        [W(0x80, 0b000), DRAW],
    ])
    a = analyse_one(p)
    assert feat(a, "texture_type_unit") == {"unit0:Shadow2D": 1, "unit1:Texture2D": 1, "unit0:TextureCube": 1,
                                            "unit0:Projection2D": 1, "unit2:Texture2D": 1}
    assert feat(a, "texture_format_unit")["unit0:RGBA4"] == 1 and feat(a, "texture_format_unit")["unit1:ETC1"] == 1
    assert feat(a, "texture_format_unit")["unit2:ETC1A4"] == 1
    sizes = feat(a, "texture_size_unit")
    assert sizes["unit0:64x32"] == 3 and sizes["unit1:16x16"] == 1 and sizes["unit2:8x4"] == 1
    assert feat(a, "texture_units") == {"2": 1, "1": 3, "0": 1}
    assert a.answers["shadow_textures"]["draws"] == 1
    assert a.answers["cube_texture"]["draws"] == 1 and a.answers["projection_texture"]["draws"] == 1


def test_disabled_unit_is_not_counted_and_unit0_type_5_is_disabled(tmp_path):
    a = analyse_one(build(tmp_path / "a.mgpu", [[W(0x80, 1), W(0x83, 5 << 28), DRAW]]))
    assert feat(a, "texture_type") == {"Disabled": 1}


def test_border_colour_in_use(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x80, 1), W(0x81, 0x11223344), W(0x83, 0), DRAW],                    # ClampToEdge: not in use
        [W(0x83, 1 << 12), DRAW],                                               # wrap_s ClampToBorder
        [W(0x83, 5 << 8), DRAW],                                                # wrap_t ClampToBorder2
        [W(0x80, 0), DRAW],                                                     # unit disabled
    ])
    a = analyse_one(p)
    assert feat(a, "border_colour") == {"0x11223344": 2}
    assert a.answers["border_colour"]["draws"] == 2


def test_buffer_formats_and_24_bit_depth(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x116, 0), W(0x117, 2 << 16), DRAW],                                 # D16, RGB5A1
        [W(0x116, 2), W(0x117, 0), DRAW],                                       # D24 but depth not used
        [W(0x107, 1), DRAW],                                                    # D24, depth test on
        [W(0x116, 3), W(0x107, 0), W(0x105, 1), DRAW],                          # D24S8 with stencil
    ], init={0x11C: 1})
    a = analyse_one(p)
    assert feat(a, "depth_format") == {"D16": 1, "D24": 2, "D24S8": 1}
    assert feat(a, "color_format") == {"RGB5A1": 1, "RGBA8": 3}
    assert a.answers["depth_formats"]["draws"] == 2


# ---- labels, files, outputs -------------------------------------------------------------------------------

def test_several_captures_per_label_start_from_their_own_init(tmp_path):
    p1 = build(tmp_path / "gpu-2.mgpu", [[W(0x25E, 2 << 8), DRAW]])
    p2 = build(tmp_path / "gpu-10.mgpu", [[DRAW]])                  # a fresh INIT: topology is List again
    c = pc.LabelCensus("G1")
    for p in (p1, p2):
        pc.census_capture(p, c)
    a = pc.analyse(c)
    assert feat(a, "topology") == {"Fan": 1, "List": 1}
    assert len(a.summary["captures"]) == 2
    assert a.summary["frames_with_draws"] == 2                       # same frame number, two capture files


def test_parse_specs_expands_patterns_in_natural_order(tmp_path):
    for n in (10, 2, 1):
        build(tmp_path / f"gpu-{n}.mgpu", [[DRAW]])
    plan = pc.parse_specs([f"G1={tmp_path}/gpu-*.mgpu", f"G2={tmp_path}/gpu-1.mgpu", f"G1={tmp_path}/gpu-2.mgpu"])
    assert [l for l, _ in plan] == ["G1", "G2"]
    assert [os.path.basename(p) for p in plan[0][1]] == ["gpu-1.mgpu", "gpu-2.mgpu", "gpu-10.mgpu", "gpu-2.mgpu"]


def test_json_and_markdown_outputs(tmp_path):
    p = build(tmp_path / "gpu-0.mgpu", [
        [W(0x25E, 2 << 8), W(0xE0, 7), W(0x102, 11), W(0x107, 0xF00), W(0x113, 0xF), DRAW, DRAW],
        [W(0x100, 3), DRAW_INDEXED],
    ])
    q = build(tmp_path / "gpu-1.mgpu", [[DRAW]])
    js, md = tmp_path / "out" / "c.json", tmp_path / "out" / "c.md"
    out, err = io.StringIO(), io.StringIO()
    rc = pc.run([f"G1={p}", f"G2={q}"], str(js), str(md), False, out=out, err=err)
    assert rc == 0, err.getvalue()
    data = json.loads(js.read_text())
    assert data["tool"] == "pica_census" and data["labels"] == ["G1", "G2"]
    g1 = data["per_label"]["G1"]
    assert g1["summary"]["draws"]["total"] == 3
    assert g1["features"]["topology"]["Fan"] == {"draws": 3, "frames": 2}
    assert g1["answers"]["fans"]["answer"] == "YES" and data["per_label"]["G2"]["answers"]["fans"]["answer"] == "NO"
    assert g1["answers"]["shadow_passes"]["draws"] == 1 and g1["answers"]["gas"]["draws"] == 3
    assert g1["unique"]["full_state"]["count"] == 2
    text = md.read_text()
    for heading in ("## 1. Logic ops", "## 2. Triangle fans", "## 3. Geometry shaders", "## 4. Shadow passes", "## 5. Gas",
                    "## 6. Procedural textures", "## 7. Depth formats", "## 8. Lighting and LUTs", "## 9. Fog",
                    "## Answers at a glance", "## Tables per feature", "### Unique configurations"):
        assert heading in text, heading
    assert "YES (3)" in text and "| Triangle fans | YES (3) | NO |" in text
    assert "0x102" in text and "0x25E" in text                       # register-level descriptions
    # the output is a function of the captures only
    js2, md2 = tmp_path / "c2.json", tmp_path / "c2.md"
    assert pc.run([f"G1={p}", f"G2={q}"], str(js2), str(md2), False, out=io.StringIO(), err=io.StringIO()) == 0
    assert js2.read_text() == js.read_text() and md2.read_text() == text


def test_outputs_use_lf_line_endings_on_every_platform(tmp_path):
    p = build(tmp_path / "gpu-0.mgpu", [[DRAW, DRAW_INDEXED]])
    js, md = tmp_path / "a.json", tmp_path / "a.md"
    assert pc.run([f"G1={p}"], str(js), str(md), False, out=io.StringIO(), err=io.StringIO()) == 0
    for f in (js, md):
        raw = f.read_bytes()          # bytes: read_text() would hide a CRLF on Windows
        assert raw.count(b"\n") > 10 and b"\r" not in raw, f.name


def test_unique_gs_programs_do_not_count_draws_without_a_geometry_shader(tmp_path):
    gs = [W(0x229, 2)] + _upload([9], base=0x280)
    p = build(tmp_path / "a.mgpu", [[DRAW], gs + [DRAW], [W(0x229, 0), DRAW]])
    a = analyse_one(p)
    assert a.unique["gs_program"]["count"] == 1
    assert [t["draws"] for t in a.unique["gs_program"]["top"]] == [1]
    assert a.unique["vs_program"]["count"] >= 1
    # no geometry shader anywhere: no gs_program group at all, so the table prints 0
    b = analyse_one(build(tmp_path / "b.mgpu", [[DRAW, DRAW]]))
    assert "gs_program" not in b.unique


def test_stdout_has_counts_only(tmp_path):
    p = build(tmp_path / "gpu-0.mgpu", [[DRAW, DRAW_INDEXED]])
    out = io.StringIO()
    assert pc.run([f"G1={p}"], str(tmp_path / "a.json"), str(tmp_path / "a.md"), False, out=out, err=io.StringIO()) == 0
    lines = out.getvalue().splitlines()
    assert lines[0].startswith("pica_census: G1: gpu-0.mgpu: ") and "2 draw triggers" in lines[0]
    assert "G1: 1 capture(s), 2 draws (1 array, 1 indexed, 0 immediate) in 1 frames" in out.getvalue()
    assert "logic_ops=NO(0)" in out.getvalue()


def test_a_label_without_draws_answers_na_not_no(tmp_path):
    p = build(tmp_path / "a.mgpu", [[DRAW]])
    q = build(tmp_path / "b.mgpu", [[W(0x228, 3)]])
    md = tmp_path / "c.md"
    err = io.StringIO()
    assert pc.run([f"G1={p}", f"G2={q}"], str(tmp_path / "c.json"), str(md), False, out=io.StringIO(), err=err) == 0
    assert "warning: no draw trigger in label(s) G2" in err.getvalue()
    assert "**No draw trigger in: G2.**" in md.read_text() and "| Triangle fans | NO | n/a |" in md.read_text()
    assert pc.run([f"G1={p}", f"G2={q}"], str(tmp_path / "c.json"), str(md), True, out=io.StringIO(), err=io.StringIO()) == 3


# ---- exit codes and errors --------------------------------------------------------------------------------

def run_cli(args, capsys):
    rc = pc.main(args)
    cap = capsys.readouterr()
    return rc, cap.out, cap.err


def test_no_draws_anywhere_is_exit_3_and_writes_nothing(tmp_path, capsys):
    p = build(tmp_path / "a.mgpu", [[W(0x228, 3)]])
    rc, out, err = run_cli([f"G1={p}", "--json", str(tmp_path / "j.json"), "--md", str(tmp_path / "m.md")], capsys)
    assert rc == 3 and "nothing censused is not a pass" in err
    assert not (tmp_path / "j.json").exists() and not (tmp_path / "m.md").exists()


def test_truncated_capture_exits_1_with_a_message(tmp_path, capsys):
    p = build(tmp_path / "a.mgpu", [[DRAW]])
    data = open(p, "rb").read()
    open(p, "wb").write(data[:-5])
    rc, out, err = run_cli([f"G1={p}", "--json", str(tmp_path / "j.json"), "--md", str(tmp_path / "m.md")], capsys)
    assert rc == 1 and "truncated" in err and "Traceback" not in err
    assert not (tmp_path / "j.json").exists()


@pytest.mark.parametrize("damage", ["magic", "version", "empty"])
def test_bad_header_exits_1(tmp_path, capsys, damage):
    p = tmp_path / "a.mgpu"
    p.write_bytes({"magic": b"XXXX" + struct.pack("<I", 1), "version": b"MGPU" + struct.pack("<I", 9), "empty": b""}[damage])
    rc, out, err = run_cli([f"G1={p}"], capsys)
    assert rc == 1 and "Traceback" not in err and "pica_census: G1:" in err


def test_structural_problem_exits_1(tmp_path, capsys):
    # a PICA_REG that does not follow a command-list GX
    path = tmp_path / "a.mgpu"
    with gcap.Writer(str(path)) as w:
        w.init()
        w.pica_reg([DRAW])
    rc, out, err = run_cli([f"G1={path}"], capsys)
    assert rc == 1 and "PICA_REG" in err and "Traceback" not in err


def test_pica_reg_before_init_exits_1(tmp_path, capsys):
    path = tmp_path / "a.mgpu"
    with gcap.Writer(str(path)) as w:
        w.pica_reg([DRAW])
    rc, out, err = run_cli([f"G1={path}"], capsys)
    assert rc == 1 and "INIT" in err and "Traceback" not in err


def test_one_bad_capture_fails_the_run_after_the_others_were_read(tmp_path, capsys):
    good = build(tmp_path / "good.mgpu", [[DRAW]])
    bad = tmp_path / "bad.mgpu"
    bad.write_bytes(b"MGPU")
    rc, out, err = run_cli([f"G1={good}", f"G2={bad}", "--json", str(tmp_path / "j.json"), "--md", str(tmp_path / "m.md")],
                           capsys)
    assert rc == 1 and "good.mgpu" in out and "bad.mgpu" in err and "nothing written" in err
    assert not (tmp_path / "m.md").exists()


@pytest.mark.parametrize("arg", ["nolabel.mgpu", "=x.mgpu", "G 1=x.mgpu", "G1="])
def test_bad_arguments_exit_2(tmp_path, capsys, arg):
    rc, out, err = run_cli([arg], capsys)
    assert rc == 2 and "Traceback" not in err


def test_missing_file_and_unmatched_pattern_exit_2(tmp_path, capsys):
    assert run_cli([f"G1={tmp_path}/none.mgpu"], capsys)[0] == 2
    assert run_cli([f"G1={tmp_path}/none-*.mgpu"], capsys)[0] == 2


def test_no_arguments_is_a_usage_error(capsys):
    with pytest.raises(SystemExit) as e:
        pc.main([])
    assert e.value.code == 2


def test_help_shows_the_g1_g4_command(capsys):
    with pytest.raises(SystemExit) as e:
        pc.main(["--help"])
    assert e.value.code == 0
    text = capsys.readouterr().out
    for needle in ("G1=build/golden/G1/gpu-*.mgpu", "G4=build/golden/G4/gpu-*.mgpu", "--md docs/research/pica-census.md",
                   "--json build/pica-census.json", "exit codes"):
        assert needle in text


# ---- the tables --------------------------------------------------------------------------------------------

def test_tables_are_consistent():
    assert all(0 <= r.index < gcap.NUM_PICA_REGS for r in pc.REGS.values())
    assert all(0 <= f.reg < gcap.NUM_PICA_REGS and 0 < f.bits <= 32 and f.pos + f.bits <= 32 for f in pc.FIELDS.values())
    # the register trigger numbers the tests above write literally
    assert (pc.REGS["trigger_draw"].index, pc.REGS["trigger_draw_indexed"].index) == (0x22E, 0x22F)
    assert pc.fld([0] * 0x300, "fog_mode") == 0


# ---- the oracle: Azahar's headers, parsed textually --------------------------------------------------------

def _azahar_pica_dir():
    root = os.path.expanduser(os.environ.get("MH4N_AZAHAR_DIR", ""))
    d = os.path.join(root, "src", "video_core", "pica")
    return d if root and os.path.isfile(os.path.join(d, "regs_internal.h")) else None


def _need_azahar():
    d = _azahar_pica_dir()
    if d is None:
        message = "needs MH4N_AZAHAR_DIR (Azahar's sources at the pinned commit)"
        if os.environ.get("MH4N_REQUIRE_TEST_DATA") == "1":
            pytest.fail(message)
        pytest.skip(message)
    return d


class _Member:
    def __init__(self, name=None, typename=None, sub=None, count=1, bits=None, pad=0):
        self.name, self.typename, self.sub, self.count, self.bits, self.pad = name, typename, sub, count, bits, pad


class _Struct:
    def __init__(self, kind, members):
        self.kind, self.members = kind, members


class _Headers:
    """A textual reader for the register structures of regs_*.h: word offsets, bit fields, enumerations."""

    SCALARS = {"u32", "s32", "f32", "u16", "s16"}

    def __init__(self, directory):
        self.typedefs: dict[str, _Struct] = {}
        self.enum_names: set[str] = set()
        self.enums: dict[str, list[dict[int, str]]] = {}
        self.asserts: list[tuple[str, int]] = []
        for name in ("regs_rasterizer.h", "regs_texturing.h", "regs_framebuffer.h", "regs_lighting.h",
                     "regs_pipeline.h", "regs_shader.h", "regs_internal.h"):
            raw = open(os.path.join(directory, name), encoding="utf-8").read()
            text = self.clean(raw)
            self.asserts += [(m.group(1), int(m.group(2), 16)) for m in
                             re.finditer(r"ASSERT_REG_POSITION\(([\w.\[\]]+),\s*(0x[0-9a-fA-F]+)\);", text)]
            for m in re.finditer(r"\bstruct (\w+) \{", text):
                body, _ = self.block(text, m.end())
                self.typedefs[m.group(1)] = self.parse(body)

    @staticmethod
    def clean(text):
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))

    @staticmethod
    def block(text, start):
        """(the text between the braces, the index after the closing brace) for a '{' just before `start`."""
        depth, i = 1, start
        while depth:
            c = text[i]
            depth += (c == "{") - (c == "}")
            i += 1
        return text[start:i - 1], i

    def parse(self, body) -> _Struct:
        return self._members(body, "struct")

    def _members(self, body, kind) -> _Struct:
        members: list[_Member] = []
        i, cur = 0, ""
        while i < len(body):
            c = body[i]
            if c == "{":
                inner, j = self.block(body, i + 1)
                head = cur.strip()
                cur = ""
                if "(" in head:                                   # a function body
                    i = j
                    continue
                k = body.index(";", j)
                tail = body[j:k].strip()
                i = k + 1
                self._statement(members, head, inner, tail)
            elif c == ";":
                self._statement(members, cur.strip(), None, "")
                cur = ""
                i += 1
            else:
                cur += c
                i += 1
        return _Struct(kind, members)

    def _statement(self, members, head, block, tail):
        if not head and block is None:
            return
        words = head.split()
        if words and words[0] in ("static", "using", "typedef", "friend", "template", "static_assert"):
            return
        if head.startswith("static_assert"):
            return
        if words and words[0] == "enum":
            m = re.match(r"enum\s+(?:class\s+)?(\w+)", head)
            self.enum_names.add(m.group(1))
            values, nxt = {}, 0
            for entry in inner_entries(block):
                name, _, val = entry.partition("=")
                nxt = int(val.strip(), 0) if val.strip() else nxt
                values[nxt] = name.strip()
                nxt += 1
            self.enums.setdefault(m.group(1), []).append(values)
            return
        if block is None:
            if head.startswith("INSERT_PADDING_WORDS"):
                members.append(_Member(pad=int(re.search(r"\((.*)\)", head).group(1), 0)))
                return
            if "(" in head:
                return
            m = re.match(r"^(?P<type>.+?)\s+(?P<name>\w+)\s*(?:\[(?P<n>\w+)\])?$", head)
            assert m, f"cannot read the declaration {head!r}"
            typename = m.group("type").replace("const ", "").strip()
            if typename.startswith("std::array"):
                return
            bits = None
            b = re.match(r"BitField<\s*(\d+)\s*,\s*(\d+)", typename)
            if b:
                bits = (int(b.group(1)), int(b.group(2)))
            members.append(_Member(m.group("name"), typename, count=int(m.group("n"), 0) if m.group("n") else 1, bits=bits))
            return
        kind = words[0]
        assert kind in ("struct", "union"), head
        sub = self._members(block, kind)
        typename = words[1] if len(words) > 1 else None
        if typename:
            self.typedefs[typename] = sub
        if not tail:
            if not typename:
                members.append(_Member(sub=sub))                  # anonymous: its members belong to the parent
            return
        for decl in tail.split(","):
            m = re.match(r"^(\w+)\s*(?:\[(\w+)\])?$", decl.strip())
            members.append(_Member(m.group(1), sub=sub, count=int(m.group(2), 0) if m.group(2) else 1))

    # -- sizes and lookup -------------------------------------------------------------------------------------
    def elem_words(self, m: _Member) -> int:
        if m.pad:
            return m.pad
        if m.sub is not None:
            return self.size(m.sub)
        t = m.typename.split("::")[-1]
        if m.typename.startswith("BitField") or t in self.SCALARS or t in self.enum_names:
            return 1
        return self.size(self.typedefs[t])

    def size(self, st: _Struct) -> int:
        sizes = [self.elem_words(m) * (1 if m.pad else m.count) for m in st.members]
        return max(sizes, default=0) if st.kind == "union" else sum(sizes)

    def find(self, st, name, base=0):
        off = base
        for m in st.members:
            here = base if st.kind == "union" else off
            if m.pad == 0 and m.name == name:
                return here, m
            if m.pad == 0 and m.name is None:
                r = self.find(m.sub, name, here)
                if r:
                    return r
            off += self.elem_words(m) * (1 if m.pad else m.count)
        return None

    def resolve(self, path):
        """(word offset in RegsInternal, member) of 'pipeline.index_array.format' and the like."""
        st, off, member = self.typedefs["RegsInternal"], 0, None
        for seg in path.split("."):
            m = re.match(r"^(\w+)(?:\[(\d+)\])?$", seg)
            found = self.find(st, m.group(1))
            assert found, f"{path}: no member {m.group(1)}"
            o, member = found
            off += o + (int(m.group(2)) * self.elem_words(member) if m.group(2) else 0)
            if member.sub is not None:
                st = member.sub
            elif member.typename and member.typename.split("::")[-1] in self.typedefs:
                st = self.typedefs[member.typename.split("::")[-1]]
        return off, member


def inner_entries(block):
    return [e.strip() for e in block.split(",") if e.strip()]


@pytest.fixture(scope="module")
def headers():
    return _Headers(_need_azahar())


def test_oracle_the_parser_reproduces_azahars_own_position_asserts(headers):
    assert len(headers.asserts) > 50
    assert headers.size(headers.typedefs["RegsInternal"]) == 0x300
    for path, position in headers.asserts:
        assert headers.resolve(path)[0] == position, path


def test_oracle_register_indices_equal_azahars(headers):
    for name, reg in pc.REGS.items():
        assert headers.resolve(reg.path)[0] == reg.index, (name, reg.path)


def test_oracle_field_registers_and_bit_positions_equal_azahars(headers):
    for name, f in pc.FIELDS.items():
        offset, member = headers.resolve(f.path)
        assert offset == f.reg, (name, f.path, hex(offset), hex(f.reg))
        assert member.bits == (f.pos, f.bits), (name, f.path, member.bits, (f.pos, f.bits))


def test_oracle_enumerations_equal_azahars(headers):
    for name, mine in pc.ENUMS.items():
        assert name in headers.enums, name
        assert all(v == mine for v in headers.enums[name]), (name, headers.enums[name], mine)


def test_oracle_tables_cover_what_the_decoder_reads():
    """Every field name the decoder asks for exists (a typo would otherwise only show on a capture)."""
    src = open(pc.__file__, encoding="utf-8").read()
    for name in set(re.findall(r"\bf\(\"(\w+)\"\)", src)) | set(re.findall(r"fld\(regs, \"(\w+)\"\)", src)):
        assert name in pc.FIELDS, name


# ---- manifests, output files ------------------------------------------------------------------------------

def _manifest(path, text):
    open(str(path) + ".json", "w").write(text)


def test_manifest_unrecorded_register_writes(tmp_path):
    p = build(tmp_path / "a.mgpu", [[DRAW]])
    _manifest(p, json.dumps({"tool": "x", "unrecorded_register_writes": 7}))
    q = build(tmp_path / "b.mgpu", [[DRAW]])
    _manifest(q, json.dumps({"unrecorded_register_writes": 0}))
    out, err = io.StringIO(), io.StringIO()
    md = tmp_path / "c.md"
    assert pc.run([f"G1={p}", f"G2={q}"], str(tmp_path / "c.json"), str(md), False, out=out, err=err) == 0
    assert "warning: G1: a.mgpu: 7 register writes" in err.getvalue() and "b.mgpu" not in err.getvalue()
    data = json.loads((tmp_path / "c.json").read_text())["per_label"]
    assert data["G1"]["summary"]["unrecorded_register_writes"] == 7
    assert data["G2"]["summary"]["unrecorded_register_writes"] == 0
    text = md.read_text()
    assert "G1: 7 register writes of register-started command lists are not in the capture" in text
    assert "NO means 'not seen'" in text and "G2: 0 register" not in text


def test_manifest_missing_is_silent_and_garbled_is_noted(tmp_path):
    p = build(tmp_path / "a.mgpu", [[DRAW]])
    err = io.StringIO()
    assert pc.run([f"G1={p}"], str(tmp_path / "c.json"), str(tmp_path / "c.md"), False, out=io.StringIO(), err=err) == 0
    assert err.getvalue() == ""
    for text in ("{not json", "[]", '{"unrecorded_register_writes": "many"}', '{"unrecorded_register_writes": -1}', "{}"):
        _manifest(p, text)
        err = io.StringIO()
        assert pc.run([f"G1={p}"], str(tmp_path / "c.json"), str(tmp_path / "c.md"), False, out=io.StringIO(), err=err) == 0
        assert "manifest unreadable" in err.getvalue(), text
        assert json.loads((tmp_path / "c.json").read_text())["per_label"]["G1"]["summary"]["unrecorded_register_writes"] == 0


def test_unwritable_output_exits_1_without_traceback_and_leaves_nothing(tmp_path, capsys):
    p = build(tmp_path / "a.mgpu", [[DRAW]])
    blocker = tmp_path / "file"
    blocker.write_text("x")
    rc, out, err = run_cli([f"G1={p}", "--md", str(blocker / "x.md"), "--json", str(tmp_path / "j.json")], capsys)
    assert rc == 1 and f"pica_census: cannot write {blocker / 'x.md'}:" in err and "Traceback" not in err
    assert not (tmp_path / "j.json").exists() and not list(tmp_path.glob("*.tmp"))
    rc, out, err = run_cli([f"G1={p}", "--md", str(tmp_path / "m.md"), "--json", str(blocker / "j.json")], capsys)
    assert rc == 1 and "cannot write" in err
    assert not (tmp_path / "m.md").exists() and not list(tmp_path.glob("*.tmp"))


def test_vs_program_writes_at_512_or_more_are_ignored(tmp_path):
    a = analyse_one(build(tmp_path / "a.mgpu", [_upload([1, 2]) + _upload([9], offset=512) + [DRAW]]))
    b = analyse_one(build(tmp_path / "b.mgpu", [_upload([1, 2]) + [DRAW]]))
    assert feat(a, "vs_program").keys() == feat(b, "vs_program").keys()


def test_markdown_extras(tmp_path):
    p = build(tmp_path / "a.mgpu", [[W(0x25E, 3 << 8), W(0x229, 2), DRAW, DRAW]])
    md = tmp_path / "c.md"
    assert pc.run([f"G1={p}"], str(tmp_path / "c.json"), str(md), False, out=io.StringIO(), err=io.StringIO()) == 0
    text = md.read_text()
    assert "| draws per frame |" in text and "| 2.00 |" in text
    assert "| label | shadow textures (unit 0) |" in text
    assert "upper bound" in text and "using_depth_fb" in text
    assert "topology Shader (3), apart" in text


# ---- N0-40: the pipeline-key groups, the extra fields and the union (--union) -------------------------------

TEX_BASE0 = 0x81


def _golden_lists():
    """A capture that exercises most of what the default output reports: TEV, lighting, fog, proctex, a geometry shader,
    blending and a logic op, depth and stencil, textures with a border colour, indexed and immediate draws."""
    return [
        [W(0x80, 0b101 | (1 << 10)), W(0x83, 0), W(0x82, (64 << 16) | 32), W(0x8E, 12), W(0x99 + 2, (1 << 8) | (5 << 12)),
         W(0x9A, (8 << 16) | 8), W(0x9E, 3), W(0xE0, 5), W(0xC0, 0x1234), W(0xC2, 1), W(0x25E, 1 << 8),
         W(0x100, 1 << 8), W(0x101, 0 | (6 << 16) | (7 << 20)), W(0x107, 0xF01 | (5 << 4)), W(0x11C, 1), W(0x11D, 1),
         W(0x113, 0xF), W(0x115, 3), W(0x116, 3), W(0x117, 0)] + _upload([5, 6, 7]) + [DRAW],
        [W(0x1C6, 0), W(0x1C2, 1), W(0x1C3, 4 << 4), W(0x1D9, 3 | (5 << 4)), W(0x229, 2), W(0x252, 1)]
        + _upload([9], base=0x280) + [DRAW, DRAW_INDEXED],
        [W(0x100, 0), W(0x102, 11), W(0x229, 0), W(0x1C6, 1), W(0x105, 1 | (2 << 4)), DRAW],
        [W(0x232, 0xF), W(0x242, 0), W(0x233, 1), W(0x234, 2), W(0x235, 3)],
    ]


GOLDEN_SHA256 = {
    # sha256 of the JSON and the markdown that the tool at 9cdee2a (N0-13, before N0-40) writes for _golden_lists()
    "json": "dc5a96585418a6e156d7c0b94ca04c401f80b6ac15db44be4a434957d1f7dfd6",
    "md": "e0409a1e22940c668a996fe56467223692754651a2d5d733bf0bd29d65449a1d",
}


def _sha256(path):
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_default_output_is_byte_identical_to_the_tool_before_n0_40(tmp_path):
    p = build(tmp_path / "gpu-0.mgpu", _golden_lists())
    js, md = tmp_path / "g.json", tmp_path / "g.md"
    assert pc.run([f"G1={p}"], str(js), str(md), False, out=io.StringIO(), err=io.StringIO()) == 0
    assert _sha256(js) == GOLDEN_SHA256["json"]
    assert _sha256(md) == GOLDEN_SHA256["md"]
    text = md.read_text()
    assert "N0-40" not in text and "fs_key" not in text and "union" not in json.loads(js.read_text())


def ext_one(path, label="T"):
    c = pc.LabelCensus(label)
    pc.census_capture(str(path), c)
    return pc.ext_analyse(c)


def efeat(e, name):
    return {v: n for v, (n, _f) in e.features.get(name, {}).items()}


def egroup(e, name):
    return e.groups.get(name, {})


BLEND_ON = W(0x100, 1 << 8)


def blend(eq_rgb=0, eq_a=0, src_rgb=1, dst_rgb=0, src_a=1, dst_a=0):
    """output_merger.alpha_blending (0x101): equations bits 0-2 and 8-10, factors bits 16-19, 20-23, 24-27, 28-31."""
    return W(0x101, eq_rgb | (eq_a << 8) | (src_rgb << 16) | (dst_rgb << 20) | (src_a << 24) | (dst_a << 28))


def test_blend_factors_are_decoded_with_azahars_names(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [BLEND_ON, blend(src_rgb=6, dst_rgb=7, src_a=1, dst_a=0), DRAW],
        [blend(src_rgb=14, dst_rgb=10, src_a=8, dst_a=13), DRAW],
        [W(0x100, 0), DRAW],                                         # blending off: no factors
    ])
    e = ext_one(p)
    assert efeat(e, "blend_factors_rgb") == {"Add SourceAlpha/OneMinusSourceAlpha": 1, "Add SourceAlphaSaturate/ConstantColor": 1}
    assert efeat(e, "blend_factors_a") == {"Add One/Zero": 1, "Add DestAlpha/OneMinusConstantAlpha": 1}
    assert efeat(e, "blend_factor") == {"SourceAlpha": 1, "OneMinusSourceAlpha": 1, "SourceAlphaSaturate": 1,
                                        "ConstantColor": 1, "DestAlpha": 1, "OneMinusConstantAlpha": 1,
                                        "One": 1, "Zero": 1}


def test_blend_factor_registers_and_enumerations_match_azahar_names():
    assert [pc.FIELDS[f"blend_factor_{n}"][:3] for n in ("src_rgb", "dst_rgb", "src_a", "dst_a")] == [
        (0x101, 16, 4), (0x101, 20, 4), (0x101, 24, 4), (0x101, 28, 4)]
    assert pc.ENUMS["BlendFactor"][14] == "SourceAlphaSaturate" and len(pc.ENUMS["BlendFactor"]) == 15


def test_colour_write_mask_cull_front_face_scissor_and_depth_buffering(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x113, 0xF), W(0x107, 0xF00), W(0x40, 0), W(0x65, 0), W(0x6D, 0), DRAW],
        [W(0x107, 0x500), W(0x40, 1), W(0x65, 1), W(0x6D, 1), DRAW],
        [W(0x113, 0), W(0x40, 2), W(0x65, 3), DRAW],                 # allow_color_write 0: no channel written
        [W(0x40, 3), DRAW],
    ])
    e = ext_one(p)
    assert efeat(e, "colour_write_mask") == {"RGBA": 1, "RB": 1, "none": 2}
    assert efeat(e, "cull_mode") == {"KeepAll": 1, "KeepClockWise": 1, "KeepCounterClockWise": 1, "KeepAll2": 1}
    # PicaToVK::FrontFace: clockwise only for KeepCounterClockWise
    assert efeat(e, "front_face") == {"CounterClockwise": 3, "Clockwise": 1}
    assert efeat(e, "scissor_mode") == {"Disabled": 1, "Exclude": 1, "Include": 2}
    assert efeat(e, "depthmap_enable") == {"WBuffering": 1, "ZBuffering": 3}


def test_wrap_modes_per_unit_and_border_kinds(tmp_path):
    # wrap_t bits 8-10, wrap_s bits 12-14 of the unit's config register (0x83, 0x93, 0x9B)
    cfg = lambda s, t: W(0x83, (s << 12) | (t << 8))                  # noqa: E731
    p = build(tmp_path / "a.mgpu", [
        [W(0x80, 0b011), cfg(1, 2), W(0x93, (5 << 12) | (0 << 8)), DRAW],
        [W(0x80, 0b001), cfg(5, 5), DRAW],
    ])
    e = ext_one(p)
    assert efeat(e, "wrap_mode_unit") == {"unit0:s=ClampToBorder/t=Repeat": 1, "unit1:s=ClampToBorder2/t=ClampToEdge": 1,
                                          "unit0:s=ClampToBorder2/t=ClampToBorder2": 1}
    assert efeat(e, "border_wrap_kind") == {"ClampToBorder": 1, "ClampToBorder2": 2}


def test_texture_and_fog_flags(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0xE0, 0), W(0x80, 0), W(0x8B, 0), DRAW],
        [W(0xE0, 5 | (1 << 16)), W(0x80, 1 << 13), W(0x8B, 1), W(0x83, 2 << 28), DRAW],
    ])
    e = ext_one(p)
    assert efeat(e, "fog_flip") == {"False": 1, "True": 1}
    assert efeat(e, "texture2_use_coord1") == {"False": 1, "True": 1}
    assert efeat(e, "shadow_orthographic") == {"False": 1, "True": 1}
    assert efeat(e, "texture0_type_register") == {"Texture2D": 1, "Shadow2D": 1}      # read even with unit 0 disabled


PROCTEX = [W(0x80, 1 << 10 | 2 << 8), W(0xA8, 1 | (2 << 3) | (7 << 6) | (8 << 10) | (1 << 14) | (1 << 15) | (1 << 16) | (2 << 18)),
           W(0xAC, 5 | (2 << 3) | (9 << 7) | (64 << 11)), W(0xAD, 0x04030201)]


def test_proctex_fields_follow_proctexconfig(tmp_path):
    p = build(tmp_path / "a.mgpu", [PROCTEX + [DRAW], [W(0x80, 0), DRAW]])
    e = ext_one(p)
    assert efeat(e, "proctex_coord") == {"2": 1}
    assert efeat(e, "proctex_u_clamp") == {"ToEdge": 1} and efeat(e, "proctex_v_clamp") == {"SymmetricalRepeat": 1}
    assert efeat(e, "proctex_color_combiner") == {"Min": 1} and efeat(e, "proctex_alpha_combiner") == {"Max": 1}
    assert efeat(e, "proctex_separate_alpha") == {"1": 1} and efeat(e, "proctex_noise_enable") == {"1": 1}
    assert efeat(e, "proctex_u_shift") == {"Odd": 1} and efeat(e, "proctex_v_shift") == {"Even": 1}
    assert efeat(e, "proctex_lut_filter") == {"LinearMipmapLinear": 1}
    assert efeat(e, "proctex_lod_min") == {"2": 1} and efeat(e, "proctex_lod_max") == {"9": 1}
    assert efeat(e, "proctex_lut_width") == {"64": 1} and efeat(e, "proctex_lut_offsets") == {"1/2/3/4": 1}
    # the configuration group counts only draws that sample the procedural texture
    assert list(egroup(e, "proctex_config").values()) == [1]
    assert "level" not in "".join(egroup(e, "proctex_config"))


def test_proctex_config_counts_distinct_configurations(tmp_path):
    p = build(tmp_path / "a.mgpu", [PROCTEX + [DRAW, DRAW], [W(0xAD, 0x08070605), DRAW], [W(0x80, 0), DRAW]])
    e = ext_one(p)
    assert sorted(egroup(e, "proctex_config").values()) == [1, 2]


LIGHT_ON = [W(0x1C6, 0), W(0x1C2, 1), W(0x1D9, 6 | (2 << 4))]


def test_lighting_fields_follow_lightconfig(tmp_path):
    flags = (1 << 2) | (1 << 3) | (1 << 27) | (2 << 22) | (1 << 0) | (1 << 17)       # alphas, clamp, selector 2, shadow, secondary
    p = build(tmp_path / "a.mgpu", [
        LIGHT_ON + [W(0x1C3, (4 << 4) | flags), W(0x1C4, 0), W(0x149 + 0x10 * 6, 0b0101), W(0x149 + 0x10 * 2, 0b1010),
                    W(0x1D0, 1 << 5), W(0x1D1, 3 | (2 << 8)), W(0x1D2, 7 | (6 << 24)), DRAW],
    ])
    e = ext_one(p)
    assert efeat(e, "light_config0_flag") == {"enable_primary_alpha": 1, "enable_secondary_alpha": 1, "clamp_highlights": 1,
                                              "enable_shadow": 1}
    assert efeat(e, "light_bump_selector") == {"texture 2": 1}
    assert efeat(e, "light_shadow_flag") == {"shadow_secondary": 1}
    assert efeat(e, "light_flag") == {"directional": 1, "geometric_factor_0": 1, "two_sided_diffuse": 1,
                                      "geometric_factor_1": 1, "dist_atten_enable": 1, "spot_atten_enable": 1,
                                      "shadow_enable": 1}
    inputs = efeat(e, "light_lut_input")
    assert inputs["d0:LN"] == 1 and inputs["sp:NV"] == 1 and inputs["d1:NH"] == 1       # lut_input d0 bits 0-2, sp bits 8-10
    assert "sp:signed" not in efeat(e, "light_lut_abs_input")                         # sp: disable_sp (bit 9) clear = abs
    assert efeat(e, "light_lut_abs_input")["sp:abs"] == 1 and efeat(e, "light_lut_abs_input")["d1:signed"] == 1
    scales = efeat(e, "light_lut_scale")
    assert scales["d0:Scale1_2"] == 1 and scales["rr:Scale1_4"] == 1 and scales["d1:Scale1"] == 1


def test_lighting_fields_are_absent_when_lighting_is_off(tmp_path):
    e = ext_one(build(tmp_path / "a.mgpu", [[W(0x1C6, 1), DRAW]]))
    assert not [f for f in e.features if f.startswith("light_")]


def test_lit_draws_against_geometry_shader(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [W(0x1C6, 1), W(0x229, 0), DRAW],
        [W(0x1C6, 1), W(0x229, 2), DRAW, DRAW],
        [W(0x1C6, 0), W(0x229, 0), DRAW, DRAW, DRAW],
        [W(0x229, 2), DRAW],
    ])
    e = ext_one(p)
    assert efeat(e, "lighting_x_geometry_shader") == {
        "lighting off + geometry shader no": 1, "lighting off + geometry shader yes": 2,
        "lighting on + geometry shader no": 3, "lighting on + geometry shader yes": 1}


def test_fixup_is_lighting_on_with_the_geometry_shader_bit_clear():
    # design 4.3 and 5.5: the fixed geometry shader serves lit draws shaded by the translated (hardware) vertex shader;
    # the draws that set use_gs are CPU draws and take no fix-up
    for light_on, use_gs, expected in ((False, False, False), (False, True, False), (True, False, True), (True, True, False)):
        assert pc.needs_fixup({"light_on": light_on, "use_gs": use_gs}) is expected


def test_pipeline_groups_apply_the_fixup_to_the_lit_draws_without_the_geometry_shader(tmp_path, monkeypatch):
    seen = {}
    real = pc.needs_fixup

    def spy(s):
        r = real(s)
        seen[(s["light_on"], s["use_gs"])] = r
        return r

    monkeypatch.setattr(pc, "needs_fixup", spy)
    ext_one(build(tmp_path / "a.mgpu", [
        [W(0x1C6, 1), W(0x229, 0), DRAW], [W(0x1C6, 1), W(0x229, 2), DRAW],
        [W(0x1C6, 0), W(0x229, 0), DRAW], [W(0x1C6, 0), W(0x229, 2), DRAW]]))
    assert seen == {(False, False): False, (False, True): False, (True, False): True, (True, True): False}


def test_fs_key_ignores_what_the_canonical_form_zeroes(tmp_path):
    # blending on: a stale logic op and the factors of an Add half are not in the fragment key, but the factors are
    # in the fixed-function state
    p = build(tmp_path / "a.mgpu", [
        [BLEND_ON, blend(src_rgb=6, dst_rgb=7), W(0x102, 0), DRAW],
        [W(0x102, 11), DRAW],                                        # another logic op, ignored while blending
        [blend(src_rgb=1, dst_rgb=1), DRAW],                         # other factors, Add equation
    ])
    e = ext_one(p)
    assert len(egroup(e, "fs_key")) == 1
    assert len(egroup(e, "fixed_function")) == 2


def test_fs_key_keeps_min_max_factors_and_the_logic_op_without_blending(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        [BLEND_ON, blend(eq_rgb=3, src_rgb=1, dst_rgb=1), DRAW],
        [blend(eq_rgb=3, src_rgb=6, dst_rgb=7), DRAW],               # Min: the factors matter
        [blend(eq_rgb=4, src_rgb=6, dst_rgb=7), DRAW],               # Max
    ])
    e = ext_one(p)
    assert len(egroup(e, "fs_key")) == 3
    q = build(tmp_path / "b.mgpu", [[W(0x100, 0), W(0x102, 3), DRAW], [W(0x102, 11), DRAW], [W(0x102, 11), DRAW]])
    f = ext_one(q)
    assert sorted(egroup(f, "fs_key").values()) == [1, 2] and sorted(egroup(f, "fixed_function").values()) == [1, 2]


def test_fs_key_fields_each_split_the_key(tmp_path):
    base = [W(0x80, 0b001), W(0x1C6, 1)]                               # unit 0 only, lighting off
    changes = {
        "wrap of a disabled unit": [W(0x9B, 5 << 12)],
        "fog flip": [W(0xE0, 1 << 16)],
        "texture2_use_coord1": [W(0x80, 0b001 | (1 << 13))],
        "shadow orthographic": [W(0x8B, 1)],
        "scissor": [W(0x65, 3)],
        "depthmap": [W(0x6D, 1)],
        "alpha test": [W(0x104, 1 | (6 << 4))],
        "light flag": [W(0x1C6, 0), W(0x1D9, 0), W(0x149, 1)],
        "proctex": PROCTEX,
        "tex0 type register": [W(0x83, 1 << 28)],
        "shadow rendering": [W(0x100, 3)],                           # Shadow fragment operation mode
    }
    lists = [base + [DRAW]]
    for ws in changes.values():
        lists.append(base + ws + [DRAW])
        lists.append([W(0x80, 0b001), W(0x9B, 0), W(0xE0, 0), W(0x8B, 0), W(0x65, 0), W(0x6D, 0), W(0x104, 0), W(0x1C6, 1),
                      W(0x83, 0), W(0x149, 0), W(0x100, 0), DRAW])     # back to the base state
    e = ext_one(build(tmp_path / "a.mgpu", lists))
    # the base state and one new key per change: every field of `changes` splits the key
    assert len(egroup(e, "fs_key")) == 1 + len(changes)


def test_lighting_fields_each_split_the_key_only_while_lighting_is_on(tmp_path):
    changes = {
        "per-light flag": [W(0x149 + 0x10 * 6, 1)],
        "LUT input": [W(0x1D1, 1)],
        "LUT absolute flag": [W(0x1D0, 1 << 1)],
        "LUT scale": [W(0x1D2, 1)],
        "config0 flag": [W(0x1C3, 1 << 2)],
        "bump selector": [W(0x1C3, 2 << 22)],                        # config0.bump_selector, bits 22-23
        "bump mode": [W(0x1C3, 1 << 28)],                            # config0.bump_mode, bits 28-29
        "disable bump renorm": [W(0x1C3, 1 << 30)],
        "enable shadow": [W(0x1C3, 1)],                              # config0.enable_shadow, bit 0
    }
    # the shadow fields are read only while enable_shadow is set: changed against a base that has it set
    shadow_on = 1
    shadow_changes = {
        "shadow primary": 1 << 16, "shadow secondary": 1 << 17, "shadow invert": 1 << 18, "shadow alpha": 1 << 19,
        "shadow selector": 2 << 24,
    }
    lit = [W(0x1C6, 0), W(0x1C2, 1), W(0x1D9, 6 | (2 << 4)), W(0x1C3, 0), W(0x149 + 0x10 * 6, 0), W(0x1D0, 0),
           W(0x1D1, 0), W(0x1D2, 0)]
    for state, light_reg in (("on", W(0x1C6, 0)), ("off", W(0x1C6, 1))):
        lists = [lit + [light_reg, DRAW]]
        for ws in changes.values():
            lists.append(lit + ws + [light_reg, DRAW])
        for bits in shadow_changes.values():
            lists.append(lit + [W(0x1C3, shadow_on | bits), light_reg, DRAW])
        e = ext_one(build(tmp_path / f"{state}.mgpu", lists))
        distinct = len(egroup(e, "fs_key"))
        # one key for the base and one more for each change while lighting is on; none while it is off
        expected = 1 + len(changes) + len(shadow_changes)
        assert distinct == (expected if state == "on" else 1), (state, distinct)


def test_tev_masking_of_dot3_rgba_merges_keys_but_census_tev_hash_does_not(tmp_path):
    # Dot3_RGBA (ops bits 0-3 = 7): sources and modifiers above bit 11 and the ops above bit 3 are not in the key
    p = build(tmp_path / "a.mgpu", [
        [W(0xC2, 7), W(0xC0, 0x00000123), DRAW],
        [W(0xC0, 0x00F00123), W(0xC1, 0x0000F000), W(0xC2, 0x70007), DRAW],
        [W(0xC2, 1), DRAW],                                          # Modulate: nothing masked
        [W(0xC0, 0x00000123), DRAW],                                 # Modulate with other sources: another key
    ])
    c = pc.LabelCensus("T")
    pc.census_capture(str(p), c)
    assert len(egroup(pc.ext_analyse(c), "fs_key")) == 3
    assert pc.analyse(c).unique["tev_config"]["count"] == 4


def test_vertex_stage_is_program_layout_and_topology(tmp_path):
    p = build(tmp_path / "a.mgpu", [
        _upload([1, 2]) + [DRAW],
        [W(0x25E, 1 << 8), DRAW],                                    # topology
        _upload([3, 4]) + [DRAW],                                    # program
        [W(0x202, 1 << 28), DRAW],                                   # layout (max_attribute_index)
        [W(0x1C6, 1), DRAW],                                         # no change to the vertex stage
    ])
    e = ext_one(p)
    assert sorted(egroup(e, "vertex_stage").values()) == [1, 1, 1, 2]
    assert all(v.startswith("vs=") and "topology=" in v for v in egroup(e, "vertex_stage"))


def test_fixed_function_holds_depth_stencil_cull_and_mask(tmp_path):
    depth = 1 | (5 << 4) | (1 << 12)                                 # depth test LessThanOrEqual with write
    lists = [
        [W(0x113, 1), DRAW],                                         # allow_color_write: the mask is "none" without it
        [W(0x107, depth), DRAW],
        [W(0x105, 1), DRAW],                                         # stencil
        [W(0x40, 2), DRAW],                                          # cull
    ]
    e = ext_one(build(tmp_path / "a.mgpu", lists))
    assert len(egroup(e, "fixed_function")) == 4
    # only the colour write mask (bits 8-11 of 0x107, the depth bits kept) changes in the last draw
    f = ext_one(build(tmp_path / "b.mgpu", lists + [[W(0x107, depth | 0xB00), DRAW]]))
    values = egroup(f, "fixed_function")
    assert len(values) == 5
    assert sum("mask=RGA;" in v for v in values) == 1 and all("mask=none;" in v for v in egroup(e, "fixed_function"))


def test_joint_tuple_includes_pass_kind_and_formats(tmp_path):
    p = build(tmp_path / "a.mgpu", [[DRAW], [W(0x100, 3), DRAW], [W(0x100, 0), W(0x117, 2 << 16), DRAW],
                                    [W(0x116, 2), DRAW]])
    e = ext_one(p)
    # shadow pass (pass kind and fs_key), colour format, depth format: each changes the tuple
    assert len(egroup(e, "pipeline_tuple")) == 4


def test_union_counts_a_shared_configuration_once_and_reports_contributions(tmp_path):
    shared = [W(0x80, 0b001), W(0x65, 3)]
    a = build(tmp_path / "gpu-0.mgpu", [shared + [DRAW, DRAW], [W(0xE0, 5), DRAW]])
    b = build(tmp_path / "gpu-1.mgpu", [shared + [DRAW], [W(0x40, 2), W(0x8B, 1), DRAW]])
    ea, eb = ext_one(a, "A"), ext_one(b, "B")
    ug = pc.union_groups([ea, eb])
    fs = ug["fs_key"]
    assert (len(egroup(ea, "fs_key")), len(egroup(eb, "fs_key")), fs["union"]) == (2, 2, 3)
    assert fs["in_every_label"] == 1
    assert fs["per_label"] == {"A": {"distinct": 2, "only_here": 1, "draws": 3}, "B": {"distinct": 2, "only_here": 1, "draws": 2}}
    shared_value = [v for v, per in fs["values"].items() if len(per) == 2][0]
    assert fs["values"][shared_value] == {"A": 2, "B": 1}
    assert ug["pipeline_tuple"]["union"] == 3
    assert ug["fixed_function"]["union"] == 2                       # only the second draw of B changes the cull mode


def test_union_run_writes_the_extension_and_leaves_the_census_part_unchanged(tmp_path):
    p = build(tmp_path / "gpu-0.mgpu", _golden_lists())
    q = build(tmp_path / "gpu-1.mgpu", [[W(0x1C6, 0), W(0x229, 2), DRAW], [W(0x40, 1), DRAW]])
    plain_js, plain_md = tmp_path / "p.json", tmp_path / "p.md"
    js, md = tmp_path / "out" / "u.json", tmp_path / "out" / "u.md"
    assert pc.run([f"G1={p}", f"G2={q}"], str(plain_js), str(plain_md), False, out=io.StringIO(), err=io.StringIO()) == 0
    out = io.StringIO()
    assert pc.run([f"G1={p}", f"G2={q}"], str(js), str(md), False, out=out, err=io.StringIO(), union=True) == 0
    plain, with_union = json.loads(plain_js.read_text()), json.loads(js.read_text())
    union = with_union.pop("union")
    assert with_union == plain                                       # the existing JSON is a subset, unchanged
    assert md.read_text().startswith(plain_md.read_text())
    text = md.read_text()
    for needle in ("# Pipeline-key groups and the union across captures (N0-40)", "## Distinct values per group",
                   "## Lit draws against geometry shader", "| fs_key |", "| pipeline_tuple |", "proctex_config"):
        assert needle in text, needle
    assert union["labels"] == ["G1", "G2"]
    assert union["groups"]["fs_key"]["union"] == len(
        set(pc.union_groups([ext_one(p, "G1"), ext_one(q, "G2")])["fs_key"]["values"]))
    assert "G1" in union["features"] and "blend_factors_rgb" in union["features"]["G1"]
    assert all(c in "0123456789abcdef" for g in ("fs_key", "pipeline_tuple") for t in union["groups"][g]["top"]
               for c in t["value"])                                   # long keys are printed as hashes
    assert out.getvalue().count("wrote") == 2
    # the output is a function of the captures only
    js2, md2 = tmp_path / "u2.json", tmp_path / "u2.md"
    assert pc.run([f"G1={p}", f"G2={q}"], str(js2), str(md2), False, out=io.StringIO(), err=io.StringIO(), union=True) == 0
    assert js2.read_text() == js.read_text() and md2.read_text() == text


def test_union_option_on_the_command_line_and_help_warns_about_the_md_default(tmp_path, capsys):
    p = build(tmp_path / "gpu-0.mgpu", [[DRAW]])
    rc, out, err = run_cli([f"G1={p}", "--union", "--json", str(tmp_path / "u.json"), "--md", str(tmp_path / "u.md")], capsys)
    assert rc == 0 and "union" in json.loads((tmp_path / "u.json").read_text())
    rc, out, err = run_cli([f"G1={p}", "--json", str(tmp_path / "n.json"), "--md", str(tmp_path / "n.md")], capsys)
    assert rc == 0 and "union" not in json.loads((tmp_path / "n.json").read_text())
    assert "pipeline-key" not in (tmp_path / "n.md").read_text()
    with pytest.raises(SystemExit):
        pc.main(["--help"])
    text = capsys.readouterr().out
    assert "--union" in text and "OVERWRITES docs/research/pica-census.md" in text
    assert "OVERWRITES" in pc.__doc__ or "overwrites the document" in pc.__doc__
