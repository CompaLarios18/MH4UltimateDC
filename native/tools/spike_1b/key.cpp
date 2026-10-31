// The 24 synthetic keys of N2-35 (docs/native/wp/N2a.md, "The keys"). No game data: the TEV stages are textbook
// combiners and the lighting sets are the five LUT sets of the census, in the mix the spec lists.
#include "key.h"

#include <cstdio>

namespace spike {

namespace {

struct Stage {
    uint32_t cs[3] = {kSrcPrevious, kSrcPrevious, kSrcPrevious};
    uint32_t cm[3] = {kModSourceColor, kModSourceColor, kModSourceColor};
    uint32_t as[3] = {kSrcPrevious, kSrcPrevious, kSrcPrevious};
    uint32_t am[3] = {kAModSourceAlpha, kAModSourceAlpha, kAModSourceAlpha};
    uint32_t cop = kOpReplace;
    uint32_t aop = kOpReplace;
    uint32_t cscale = 0;
    uint32_t ascale = 0;
};

void Put(Key& k, uint32_t index, const Stage& s) {
    uint32_t src = 0;
    uint32_t mod = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        src |= s.cs[i] << (4 * i);
        src |= s.as[i] << (12 + 4 * i);
        mod |= s.cm[i] << (4 * i);
        mod |= s.am[i] << (12 + 4 * i);
    }
    k.w[f_tev_src_0 + index] = src;
    k.w[f_tev_mod_0 + index] = mod;
    k.w[f_tev_op_0 + index] = s.cop | (s.aop << 4);
    k.w[f_tev_scale_0 + index] = s.cscale | (s.ascale << 2);
}

// Both halves use the same two sources, as most combiners do.
Stage Two(uint32_t op, uint32_t src0, uint32_t src1, uint32_t cscale = 0) {
    Stage s;
    s.cs[0] = s.as[0] = src0;
    s.cs[1] = s.as[1] = src1;
    s.cop = s.aop = op;
    s.cscale = s.ascale = cscale;
    return s;
}

// The six textbook combiners of keys 0 to 5: replace, modulate, add, add-signed, interpolate, dot3.
Stage Textbook(uint32_t index) {
    switch (index) {
    case 0:
        return Two(kOpReplace, kSrcTexture0, kSrcPrevious);
    case 1:
        return Two(kOpModulate, kSrcPrevious, kSrcTexture1, 1);
    case 2: {
        Stage s = Two(kOpAdd, kSrcPrevious, kSrcPrimaryColor);
        s.cm[1] = kModOneMinusSourceColor;
        return s;
    }
    case 3: {
        Stage s = Two(kOpAddSigned, kSrcPrevious, kSrcPreviousBuffer);
        s.as[1] = kSrcConstant;
        return s;
    }
    case 4: {
        Stage s = Two(kOpLerp, kSrcPrevious, kSrcTexture2);
        s.cs[2] = s.as[2] = kSrcConstant;
        s.cm[2] = kModSourceAlpha;
        s.am[2] = kAModSourceAlpha;
        return s;
    }
    default: {
        Stage s = Two(kOpDot3Rgb, kSrcTexture0, kSrcConstant);
        s.aop = kOpReplace;
        s.as[0] = kSrcPrevious;
        return s;
    }
    }
}

// The four stages of a lit draw, then (keys 21 to 23) two more.
Stage Lit(uint32_t index) {
    switch (index) {
    case 0:
        return Two(kOpModulate, kSrcTexture0, kSrcPrimaryFragmentColor);
    case 1: {
        Stage s = Two(kOpAdd, kSrcPrevious, kSrcSecondaryFragmentColor);
        s.aop = kOpReplace;
        return s;
    }
    case 2:
        return Two(kOpModulate, kSrcPrevious, kSrcConstant, 1);
    case 3:
        return Two(kOpAddSigned, kSrcPrevious, kSrcTexture1);
    case 4: {
        Stage s = Two(kOpLerp, kSrcPrevious, kSrcTexture2);
        s.cs[2] = s.as[2] = kSrcConstant;
        s.cm[2] = kModSourceAlpha;
        s.am[2] = kAModSourceAlpha;
        return s;
    }
    default:
        return Two(kOpModulate, kSrcPrevious, kSrcPrimaryColor, 1);
    }
}

constexpr uint32_t LutField(LutId id) { return static_cast<uint32_t>(f_lut_cfg_0) + static_cast<uint32_t>(id); }

constexpr uint32_t Lut(bool enable, bool absolute, uint32_t input, uint32_t scale) {
    return (enable ? 1u : 0u) | (absolute ? 2u : 0u) | (input << 2) | (scale << 5);
}

// The identity stage, which option 1 does not emit: Previous, Replace, scale 1 in both halves.
void FillIdentity(Key& k, uint32_t from) {
    for (uint32_t i = from; i < 6; ++i) {
        Put(k, i, Stage{});
    }
}

enum LutSet { kNone, kFrRr, kDa, kD0Rr, kDaFrRr };

const char* SetName(LutSet s) {
    switch (s) {
    case kNone: return "no LUT";
    case kFrRr: return "FR+RR";
    case kDa: return "DA";
    case kD0Rr: return "D0+RR";
    default: return "DA+FR+RR";
    }
}

void Lighting(Key& k, uint32_t lights, LutSet set, uint32_t bump) {
    k.w[f_lit] = 1;
    k.w[f_light_count] = lights;
    k.w[f_light_bump] = bump;
    k.w[f_light_clamp] = 1;
    const bool fr = set == kFrRr || set == kDaFrRr;
    const bool rr = set == kFrRr || set == kD0Rr || set == kDaFrRr;
    const bool da = set == kDa || set == kDaFrRr;
    const bool d0 = set == kD0Rr;
    k.w[f_light_config] = fr ? 1u : 0u; // Config1 for the sets with FR+RR, Config0 for the others
    k.w[LutField(kD0)] = d0 ? Lut(true, true, kInNH, 0) : 0u;
    k.w[LutField(kFR)] = fr ? Lut(true, true, kInNV, 0) : 0u;
    k.w[LutField(kRR)] = rr ? Lut(true, false, kInVH, 1) : 0u;
    k.w[LutField(kDA)] = da ? Lut(true, false, kInNH, 0) : 0u;
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t flags = 0;
        if (i == 1) {
            flags |= 1u; // two-sided diffuse on the second light
        }
        if (fr) {
            flags |= 2u; // geometric factor
        }
        if (da) {
            flags |= 8u; // distance attenuation
        }
        k.w[f_light_flags_0 + i] = flags;
    }
    k.lights = lights;
}

} // namespace

std::vector<Key> MakeKeys() {
    std::vector<Key> keys;
    char text[160];

    // Keys 0 to 5: unlit, 1 to 6 active TEV stages.
    for (uint32_t n = 1; n <= 6; ++n) {
        Key k;
        for (uint32_t i = 0; i < n; ++i) {
            Put(k, i, Textbook(i));
        }
        FillIdentity(k, n);
        k.w[f_tev_upd] = (n >= 3) ? ((1u << 1) | (1u << 9)) : 0u; // stage 1 feeds the combiner buffer
        k.w[f_alpha_test_func] = 6;                               // GreaterThan
        k.w[f_depth_mode] = 0;
        k.tev_stages = n;
        std::snprintf(text, sizeof text, "unlit, %u TEV stage%s", n, n == 1 ? "" : "s");
        k.description = text;
        keys.push_back(std::move(k));
    }

    // Keys 6 to 20: lit, 4 TEV stages, 1 to 3 lights times the five LUT sets.
    const LutSet sets[] = {kNone, kFrRr, kDa, kD0Rr, kDaFrRr};
    for (uint32_t lights = 1; lights <= 3; ++lights) {
        for (LutSet set : sets) {
            Key k;
            for (uint32_t i = 0; i < 4; ++i) {
                Put(k, i, Lit(i));
            }
            FillIdentity(k, 4);
            k.w[f_tev_upd] = (1u << 1) | (1u << 9);
            k.w[f_alpha_test_func] = 6;
            k.w[f_depth_mode] = 0;
            Lighting(k, lights, set, 0);
            k.tev_stages = 4;
            std::snprintf(text, sizeof text, "lit, %u light%s, %s, Config%u, 4 TEV stages", lights,
                          lights == 1 ? "" : "s", SetName(set), k.w[f_light_config]);
            k.description = text;
            keys.push_back(std::move(k));
        }
    }

    // Keys 21 to 23: 1, 2 and 3 lights, DA+FR+RR, bump NormalMap, 6 TEV stages.
    for (uint32_t lights = 1; lights <= 3; ++lights) {
        Key k;
        for (uint32_t i = 0; i < 6; ++i) {
            Put(k, i, Lit(i));
        }
        k.w[f_tev_upd] = (1u << 1) | (1u << 9);
        k.w[f_alpha_test_func] = 6;
        k.w[f_depth_mode] = 1; // W buffering on the bump keys, so both depth modes occur
        Lighting(k, lights, kDaFrRr, 1);
        k.tev_stages = 6;
        std::snprintf(text, sizeof text, "lit, %u light%s, DA+FR+RR, Config1, bump NormalMap, 6 TEV stages", lights,
                      lights == 1 ? "" : "s");
        k.description = text;
        keys.push_back(std::move(k));
    }

    for (size_t i = 0; i < keys.size(); ++i) {
        std::snprintf(text, sizeof text, "k%02zu", i);
        keys[i].name = text;
    }
    return keys;
}

} // namespace spike
