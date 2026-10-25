// The spike's synthetic fragment key (N2-35): the fields the skeleton of uber_color reads, as 32-bit words.
// Nothing here is game data. The word layouts follow the PICA register fields the design calls "raw words"
// (docs/native/gpu-fragment-design.md 4.2: the six TEV stages as raw words); the values are hardware enumerators
// (3dbrew's GPU registers), not text from Azahar.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace spike {

// X(name): the order is the field's index in Key::w, its word in the configuration block and its specialisation
// constant id minus one (id 0 is spec_uber, which is not a key field).
//
// tev_src_k   bits 0-3, 4-7, 8-11: colour source 0..2; bits 12-15, 16-19, 20-23: alpha source 0..2
// tev_mod_k   the same layout for the colour and alpha modifiers (4 bits each)
// tev_op_k    bits 0-3 colour operation, bits 4-7 alpha operation
// tev_scale_k bits 0-1 colour scale, bits 2-3 alpha scale (raw 0 = x1, 1 = x2, 2 = x4)
// tev_upd     bit k: stage k updates the combiner buffer colour; bit 8+k: its alpha
// lut_cfg_n   bit 0 enable, bit 1 absolute input, bits 2-4 input (NH VH NV LN SP CP), bits 5-6 scale (x1 x2 x4 x8);
//             n = D0 D1 SP FR RR RG RB DA
// light_flags_n  bit 0 two-sided diffuse, bit 1 geometric factor, bit 2 spot attenuation, bit 3 distance attenuation
#define SPIKE_KEY_FIELDS(X)                                                                                      \
    X(tev_src_0) X(tev_src_1) X(tev_src_2) X(tev_src_3) X(tev_src_4) X(tev_src_5)                               \
    X(tev_mod_0) X(tev_mod_1) X(tev_mod_2) X(tev_mod_3) X(tev_mod_4) X(tev_mod_5)                               \
    X(tev_op_0) X(tev_op_1) X(tev_op_2) X(tev_op_3) X(tev_op_4) X(tev_op_5)                                     \
    X(tev_scale_0) X(tev_scale_1) X(tev_scale_2) X(tev_scale_3) X(tev_scale_4) X(tev_scale_5)                   \
    X(tev_upd) X(alpha_test_func) X(depth_mode)                                                                 \
    X(lit) X(light_count) X(light_config) X(light_bump) X(light_clamp)                                          \
    X(lut_cfg_0) X(lut_cfg_1) X(lut_cfg_2) X(lut_cfg_3) X(lut_cfg_4) X(lut_cfg_5) X(lut_cfg_6) X(lut_cfg_7)     \
    X(light_flags_0) X(light_flags_1) X(light_flags_2)

enum Field : uint32_t {
#define X(name) f_##name,
    SPIKE_KEY_FIELDS(X)
#undef X
    kFieldCount
};

inline constexpr uint32_t kBlockVec4s = (kFieldCount + 3) / 4; // the configuration block is uvec4[kBlockVec4s]

inline const char* FieldName(uint32_t f) {
    static const char* const names[] = {
#define X(name) #name,
        SPIKE_KEY_FIELDS(X)
#undef X
    };
    return names[f];
}

// PICA enumerators the keys use (TevStageConfig and the lighting configuration).
enum TevSource : uint32_t {
    kSrcPrimaryColor = 0,
    kSrcPrimaryFragmentColor = 1,
    kSrcSecondaryFragmentColor = 2,
    kSrcTexture0 = 3,
    kSrcTexture1 = 4,
    kSrcTexture2 = 5,
    kSrcTexture3 = 6,
    kSrcPreviousBuffer = 13,
    kSrcConstant = 14,
    kSrcPrevious = 15,
};
enum TevColorModifier : uint32_t {
    kModSourceColor = 0,
    kModOneMinusSourceColor = 1,
    kModSourceAlpha = 2,
    kModOneMinusSourceAlpha = 3,
    kModSourceRed = 4,
    kModOneMinusSourceRed = 5,
    kModSourceGreen = 8,
    kModOneMinusSourceGreen = 9,
    kModSourceBlue = 12,
    kModOneMinusSourceBlue = 13,
};
enum TevAlphaModifier : uint32_t {
    kAModSourceAlpha = 0,
    kAModOneMinusSourceAlpha = 1,
    kAModSourceRed = 2,
    kAModOneMinusSourceRed = 3,
    kAModSourceGreen = 4,
    kAModOneMinusSourceGreen = 5,
    kAModSourceBlue = 6,
    kAModOneMinusSourceBlue = 7,
};
enum TevOp : uint32_t {
    kOpReplace = 0,
    kOpModulate = 1,
    kOpAdd = 2,
    kOpAddSigned = 3,
    kOpLerp = 4,
    kOpSubtract = 5,
    kOpDot3Rgb = 6,
    kOpDot3Rgba = 7,
    kOpMultiplyThenAdd = 8,
    kOpAddThenMultiply = 9,
};
enum LutId : uint32_t { kD0, kD1, kSP, kFR, kRR, kRG, kRB, kDA, kLutCount };
enum LutInput : uint32_t { kInNH, kInVH, kInNV, kInLN, kInSP, kInCP };

struct Key {
    std::array<uint32_t, kFieldCount> w{};
    std::string name;        // "k07"
    std::string description; // for the report's table
    uint32_t tev_stages = 0; // active (non-identity) TEV stages
    uint32_t lights = 0;
};

// The 24 keys of the spec (docs/native/wp/N2a.md, N2-35, "The keys").
std::vector<Key> MakeKeys();

inline uint32_t Get(const Key& k, Field f) { return k.w[f]; }

} // namespace spike
