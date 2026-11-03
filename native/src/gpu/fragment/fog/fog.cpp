// Derived from Azahar src/video_core/shader/generator/glsl_fs_shader_gen.cpp (FragmentModule::WriteFog and the fog_mode switch
// of FragmentModule::Generate), src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTsLF: the fog table's
// {value, difference} pairs in the R32G32 texel buffer and the offset kept in the uniforms), src/video_core/pica/pica_core.h
// (Fog::LutEntry: the entry's bit fields and the division by 2047), src/video_core/rasterizer_accelerated.cpp and
// src/video_core/pica/dirty_regs.h (SyncDrawUniforms and CheckFogColor: the fog colour uniform and the register that dirties
// it), src/video_core/renderer_software/sw_rasterizer.cpp (WriteFog and how the depth it takes is computed and clamped) and
// src/video_core/pica/regs_texturing.h (the fog mode, flip and colour registers) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: glsl_fs_shader_gen.cpp and vk_rasterizer.cpp say "Licensed under
// GPLv3 or any later version"; pica_core.h, rasterizer_accelerated.cpp, dirty_regs.h, sw_rasterizer.cpp and regs_texturing.h
// each say "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (glsl_fs_shader_gen.cpp, vk_rasterizer.cpp,
//   pica_core.h, rasterizer_accelerated.cpp)
// Copyright 2025-2026 Citra Emulator Project / Azahar Emulator Project (dirty_regs.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
//
// The `fog/` feature (N2-10): depth fog with its table, and the gas mode.
//
// FOG (fog mode Fog, 5) is Azahar's. It follows the software renderer's formula and the GLSL generator's text. One deliberate
// choice: the depth is clamped to [0, 1] before the table index is taken, as sw_rasterizer.cpp clamps it (the GLSL does not, and
// extrapolates the last table segment for a depth outside the range; inside the range the two are the same).
//
// GAS is Azahar's nowhere: its generators log "Unimplemented gas mode" and write transparent black. Everything gas does here
// comes from 3dbrew (GPU/Internal Registers, revision 23943: GPUREG_TEXENV_UPDATE_BUFFER, GPUREG_COLOR_OPERATION, GPUREG_GAS_*,
// "Gas Look-Up Table", the RGBA8/Gas colour format) and from the labelled choices of docs/research/gas.md, which also lists
// what is open. Three labels are used in this file and in gas.md:
//     [3dbrew]      stated there;
//     [inference]   read from the names and the fields of the registers;
//     [unverified]  a choice of ours, open until a real 3DS has been checked (LATER.md).
// 3dbrew gives registers and fields and no per-fragment formula, so the model below is the smallest one the descriptions suggest.
// No other source was used for gas (no SDK documentation, no other emulator).
//
// THE MODEL (a fragment's gas work happens at the `FogGas` point, after the alpha test, on `combiner_output`):
//   Density pass, fragment operation mode Gas (key.gas bit 0)   [3dbrew: "gas density rendering (fragment operation mode 1)"]
//     D1 [3dbrew]     the density lives in the blue and alpha bytes of the colour buffer, a float1.5.10 compared as an
//                     unsigned 16-bit integer (GPUREG_GAS_ACCMAX_FEEDBACK).
//     D2 [inference]  the 16 bits are blue as the high byte and alpha as the low byte (the colour is laid out "in reverse
//                     byte order in memory", so the low halfword of the 32-bit word is blue then alpha: GPUREG_COLORBUFFER_FORMAT).
//     D3 [inference]  the fragment's plain density is those two bytes of the TEV output (after the byte rounding).
//     D4 [inference]  "accumulation": the new density is the sum of the fragment's density and the destination's, read from the
//                     colour buffer as it was before the draw (`Needs::dst_copy`).
//     D5 [unverified] float1.5.10 is the layout of IEEE binary16 (bias 15); the sum is rounded as `packHalf2x16` rounds (the
//                     rounding mode is the device's; gas.md's draw uses sums that are exact); no clamp at GAS_ACCMAX in this pass.
//     D6 [unverified] red and green keep the TEV output's value.
//   Shading pass, fog mode Gas (key texture.fog_mode, 7)       [3dbrew: fog mode 7 is Gas]
//     S1 [inference]  the density is the destination's, as in D2 and D4 (the same `dst_copy` copy).
//     S2 [inference]  t = density / GPUREG_GAS_ACCMAX, clamped to [0, 1] (3dbrew: the maximum written "can be read back to compute
//                     GPUREG_GAS_ACCMAX"). What 3dbrew says about an unsigned 16-bit compare is only about the TRACKED MAXIMUM
//                     (GPUREG_GAS_ACCMAX_FEEDBACK: "compared as unsigned 16-bit integers, so negative values, infinities and NaN
//                     are greater than any positive value"); applying it to a shading-pass density against ACCMAX is [unverified]:
//                     under it a density at or above ACCMAX (a negative, infinite or NaN one included) is t = 1. Also [unverified]:
//                     a density of +0 or -0 (0x0000, 0x8000) is t = 0 (not t = 1 for -0 by the unsigned compare); an ACCMAX that is
//                     negative or NaN gives t = 0.
//     S3 [3dbrew]     the colour LUT input is the gas density (GPUREG_GAS_LIGHT_Z_COLOR bit 8 = 0) or the light factor (1).
//     S4 [unverified] the light factor has no source in the registers' descriptions (planar and view shading intensities and
//                     attenuations, with no per-fragment direction): with the light-factor input t = 1.
//     S5 [inference]  the 16-entry table is 8 signed differences (entries 0-7) and 8 unsigned colours (entries 8-15) of three
//                     bytes: [3dbrew] gives the signedness; that entries 8-15 are the values of eight segments and entries 0-7 the
//                     differences to the next value (as the fog table is) is [inference]; the 1/255 scale of a difference and
//                     that the last segment is extended to t = 1 (as the fog table is) are [unverified].
//                     position = 8 t, segment = min(floor(position), 7), colour = (value[segment] + difference[segment] *
//                     (position - segment)) / 255, clamped.
//     S6 [unverified] the output RGB is the table colour; alpha is the TEV output's; depth, blending and the logic op follow as for
//                     any fragment.
//   A fragment with both modes set runs the density pass only [unverified].
//   Not modelled (the registers are copied raw into the gas region, so a later model needs no new key field or uniform):
//   GPUREG_GAS_ATTENUATION, GPUREG_GAS_LIGHT_XY/Z, the low byte of GPUREG_GAS_LIGHT_Z_COLOR, GPUREG_GAS_DELTAZ_DEPTH, the shading
//   density source (GPUREG_TEXENV_UPDATE_BUFFER bit 3), and the maximum the hardware tracks (GPUREG_GAS_ACCMAX_FEEDBACK bits
//   16-31: reading it back needs a GPU-to-CPU path that does not exist, N2-04 does not render).
#include <cstring>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/emitter.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/fog/fog_layout.h"
#include "gpu/fragment/regs.h"
#include "gpu/fragment/tables.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {
namespace {

namespace re = regenum;
namespace kw = keyword;

constexpr u32 kFogModeFog = 5;
constexpr u32 kFogModeGas = 7;
constexpr u32 kOperationModeGas = 1;

// The helpers the gas code calls. The gas block of fs_data is `uvec4 gas_regs[4]` (the layout of fog_layout.h) and
// `uvec4 gas_table[4]`.
constexpr const char* kGasHelpers = R"(
uint mh4n_gas_reg(uint index) { return gas_regs[index >> 2][index & 3u]; }
uint mh4n_gas_table(uint index) { return gas_table[index >> 2][index & 3u]; }
// D2: blue is the high byte and alpha the low byte of the 16-bit density.
uint mh4n_gas_bits(vec4 pixel) { return (uint(round(pixel.b * 255.0)) << 8) | uint(round(pixel.a * 255.0)); }
// S5: the table colour at t in [0, 1].
vec3 mh4n_gas_color(float t) {
    float position = t * 8.0;
    uint segment = min(uint(position), 7u);
    float fraction = position - float(segment);
    uint difference = mh4n_gas_table(segment);
    uint value = mh4n_gas_table(segment + 8u);
    vec3 base = vec3(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u));
    vec3 slope = vec3(float(bitfieldExtract(int(difference), 0, 8)), float(bitfieldExtract(int(difference), 8, 8)),
                      float(bitfieldExtract(int(difference), 16, 8)));
    return clamp((base + slope * fraction) / 255.0, vec3(0.0), vec3(1.0));
}
)";

class FogFeature final : public FeatureModule {
public:
    const char* Name() const override { return "fog"; }

    // ---- the reserved gas key word ----------------------------------------------------------------------------------

    void Key(const pica::PicaState& state, const FragmentKey& key, ReservedWords words) const override {
        u32 gas = 0;
        const bool density_pass = Bits(state.regs[freg::kFragmentOperation], 0, 2) == kOperationModeGas;
        if (density_pass) {
            gas |= fog::kKeyDensityPass;
        } else if (key.texture.fog_mode() == re::FogMode::Gas && Bits(state.regs[pica::gas_reg::kLightZColor], 8, 1) != 0) {
            gas |= fog::kKeyLightFactorInput;
        }
        words.gas = gas;
    }

    // ---- the generated code ------------------------------------------------------------------------------------------

    void Emit(Point point, EmitContext& ctx) const override {
        Emitter& e = *ctx.emitter;
        if (point == Point::Declarations) {
            // Core's declarations come first; in the specialised mode the helpers are emitted only when the key uses gas.
            if (!e.specialised() || ctx.key.gas.raw != 0 || ctx.key.texture.fog_mode() == re::FogMode::Gas) {
                e.Raw(kGasHelpers);
            }
            return;
        }
        if (point != Point::FogGas) {
            return;
        }
        const Value fog_mode = e.Field(kw::kTexRaw, 12, 3);
        e.If(e.Eq(fog_mode, kFogModeFog), [&] { WriteFog(e); });
        e.If(
            e.Eq(e.Field(kw::kGas, 0, 1), 1), [&] { WriteDensityPass(e); },
            [&] { e.If(e.Eq(fog_mode, kFogModeGas), [&] { WriteShadingPass(e); }); });
    }

    // Azahar's WriteFog; `depth` is the local of main that WriteDepth made. The depth is clamped as sw_rasterizer.cpp clamps it.
    // The software renderer truncates the fogged colour to a u8 and this shader (the GLSL's mix) leaves a float that the colour
    // write rounds, so the two differ by at most one step of 8 bits: why gpu_fog_test compares fog pixels with a tolerance of 1.
    static void WriteFog(Emitter& e) {
        e.Raw("{\n"
              "float fog_depth = clamp(depth, 0.0, 1.0);\n"
              "float fog_index;\n");
        e.If(
            e.Eq(e.Field(kw::kTexRaw, 15, 1), 1), [&] { e.Raw("fog_index = (1.0 - fog_depth) * 128.0;\n"); },
            [&] { e.Raw("fog_index = fog_depth * 128.0;\n"); });
        e.Raw("float fog_i = clamp(floor(fog_index), 0.0, 127.0);\n"
              "float fog_f = fog_index - fog_i;\n"
              "vec2 fog_lut_entry = texelFetch(texture_buffer_lut_lf, int(fog_i) + fog_lut_offset).rg;\n"
              "float fog_factor = fog_lut_entry.r + fog_lut_entry.g * fog_f;\n"
              "fog_factor = clamp(fog_factor, 0.0, 1.0);\n"
              "combiner_output.rgb = mix(fog_color.rgb, combiner_output.rgb, fog_factor);\n"
              "}\n");
    }

    // D1 to D6.
    static void WriteDensityPass(Emitter& e) {
        e.Raw("{\n"
              "vec4 gas_dest = texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0);\n"
              "float gas_sum = unpackHalf2x16(mh4n_gas_bits(combiner_output)).x + unpackHalf2x16(mh4n_gas_bits(gas_dest)).x;\n"
              "uint gas_bits = packHalf2x16(vec2(gas_sum, 0.0)) & 0xFFFFu;\n"
              "combiner_output.b = float(gas_bits >> 8) / 255.0;\n"
              "combiner_output.a = float(gas_bits & 255u) / 255.0;\n"
              "}\n");
    }

    // S1 to S6.
    static void WriteShadingPass(Emitter& e) {
        e.Raw("{\n"
              "uint gas_density = mh4n_gas_bits(texelFetch(tex_color, ivec2(gl_FragCoord.xy), 0));\n"
              "uint gas_max = mh4n_gas_reg(9u) & 0xFFFFu;\n"
              "float gas_t;\n"
              "if ((gas_density & 0x7FFFu) == 0u) {\n"
              "    gas_t = 0.0;\n"
              "} else if (gas_density >= gas_max) {\n"
              "    gas_t = 1.0;\n"
              "} else if (gas_max > 0x7C00u) {\n"
              "    gas_t = 0.0;\n"
              "} else {\n"
              "    gas_t = clamp(unpackHalf2x16(gas_density).x / unpackHalf2x16(gas_max).x, 0.0, 1.0);\n"
              "}\n");
        e.If(e.Eq(e.Field(kw::kGas, 1, 1), 1), [&] { e.Raw("gas_t = 1.0;\n"); });
        e.Raw("combiner_output.rgb = mh4n_gas_color(gas_t);\n"
              "}\n");
    }

    // ---- the draw's needs --------------------------------------------------------------------------------------------

    // Both gas passes read the colour buffer as it was before the draw: the feedback copy (`dst_copy`). The ordered
    // read-modify-write (`ordered_rmw`) is not asked for: fragments of one draw that overlap do not see each other (gas.md, open).
    Needs NeedsOf(const FragmentKey& key, const DeviceProfile&) const override {
        Needs needs;
        const bool fog = key.texture.fog_mode() == re::FogMode::Fog;
        const bool gas = key.texture.fog_mode() == re::FogMode::Gas || (key.gas.raw & fog::kKeyDensityPass) != 0;
        needs.lut_lf = fog;
        needs.dst_copy = gas;
        return needs;
    }

    // ---- uniforms ----------------------------------------------------------------------------------------------------

    void FillUniforms(const pica::PicaState& state, FragmentBlock& block, UniformDirty& dirty) const override {
        const auto& regs = state.regs;
        if (dirty.all || state.dirty.Test(freg::kFogColor)) {
            const u32 word = regs[freg::kFogColor];
            const Vec3f color{static_cast<float>(Bits(word, 0, 8)) / 255.0f, static_cast<float>(Bits(word, 8, 8)) / 255.0f,
                              static_cast<float>(Bits(word, 16, 8)) / 255.0f};
            if (dirty.all || std::memcmp(&color, &block.uniforms.fog_color, sizeof(color)) != 0) {
                block.uniforms.fog_color = color;
                dirty.Mark();
            }
        }

        bool gas_dirty = dirty.all || state.gas.table_dirty;
        for (u32 id : {freg::kFogModeAndCombiner, pica::gas_reg::kAttenuation, pica::gas_reg::kAccMax}) {
            gas_dirty = gas_dirty || state.dirty.Test(id);
        }
        for (u32 id = freg::kGasFirst; id <= pica::gas_reg::kDeltaZDepth; ++id) {
            gas_dirty = gas_dirty || state.dirty.Test(id);
        }
        if (gas_dirty) {
            GasUniforms gas{};
            for (u32 i = 0; i <= pica::gas_reg::kDeltaZDepth - freg::kGasFirst; ++i) {
                gas.regs[i] = regs[freg::kGasFirst + i];
            }
            gas.regs[fog::kRegFogMode] = regs[freg::kFogModeAndCombiner];
            gas.regs[fog::kRegAttenuation] = regs[pica::gas_reg::kAttenuation];
            gas.regs[fog::kRegAccMax] = regs[pica::gas_reg::kAccMax];
            for (u32 i = 0; i < pica::Gas::kEntries; ++i) {
                gas.table[i] = state.gas.lut[i];
            }
            if (dirty.all || std::memcmp(&gas, &block.gas, sizeof(gas)) != 0) {
                block.gas = gas;
                dirty.Mark();
            }
        }
    }

    // ---- the fog table in lut_lf --------------------------------------------------------------------------------------

    // Azahar's SyncAndUploadLUTsLF for the fog part: 128 entries of {value, difference} as two floats, and the offset in elements
    // of the R32G32 view in `fog_lut_offset`. The session was opened for the worst case of the whole ring (lighting included), so
    // `Take` cannot run out of room; if it did, the offset would be left as it was.
    void UploadTables(TableUpload& upload) const override {
        if (!upload.need.fog) {
            return;
        }
        rhi::Result<TableSlice> slice = upload.rings.Take(TableRing::Lf, pica::Fog::kEntries * sizeof(Vec2f));
        if (!slice.ok()) {
            return;
        }
        for (u32 i = 0; i < pica::Fog::kEntries; ++i) {
            const u32 raw = upload.state.fog.lut[i];
            // Fog::LutEntry: difference is bits 0-12 (signed), value bits 13-23, both over 2047.
            const s32 difference = (static_cast<s32>(Bits(raw, 0, 13) << 19)) >> 19;
            const Vec2f entry{static_cast<float>(Bits(raw, 13, 11)) / 2047.0f, static_cast<float>(difference) / 2047.0f};
            std::memcpy(slice->data.data() + i * sizeof(Vec2f), &entry, sizeof(entry));
        }
        upload.block.uniforms.fog_lut_offset = slice->ElementOffset(sizeof(Vec2f));
        upload.dirty.Mark();
    }
};

}  // namespace

MH4N_REGISTER_FRAGMENT_FEATURE(FogFeature)
}  // namespace mh4n::gpu::fragment
