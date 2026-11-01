// Written for this package from the design (docs/native/gpu-fragment-design.md 4.4.1, option 1: a two-mode emitter);
// nothing is copied from Azahar here (Azahar's `FragmentModule` was read for the idea of a generator that emits GLSL).
//
// The emitter (N2-08). Every decision that Azahar's generator makes with `switch (config.x)` goes through one of the
// helpers below, and the helper has two modes:
//
//   Specialised  the key's field is known when the shader is generated: the helper evaluates it and emits only the chosen
//                branch, with the numbers as literals (Azahar's output);
//   Uber         the field is read from the configuration block (`cfg`, the FragmentKey itself, bound with every draw) at
//                run time: the helper emits a GLSL `if` or `switch` on it, and a loop where the key holds a count.
//
// A `Value` is an unsigned key field as the current mode sees it (a literal, or a `cfg` read); a `Cond` is a boolean made
// of Values. Code that builds a `Value` or a `Cond` and passes it to `If` or `Switch` is written once and works in both
// modes: that is what keeps the two modes from drifting apart, together with the equality test of gpu_tev_test (the same
// draw in both modes must give the same pixels).
//
// A feature reaches the emitter through `EmitContext::emitter`. In the Uber mode `EmitContext::key` is all zero: read the
// key through `Field` and the conditions, never through `ctx.key`.
#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "gpu/fragment/feature.h"
#include "gpu/fragment/key.h"

namespace mh4n::gpu::fragment {

// The pass the shader is for (the fragment half of native::PassKind, which this module does not see).
enum class ShaderPass : u8 {
    Normal,       // a pass with colour and depth attachments
    ShadowWrite,  // a pass with no attachments that stores into the shadow source's R32_UINT view
};

// ---- where each part of the key is, in 32-bit words of FragmentKey (the ubershader reads cfg[word / 4][word % 4]) --------
namespace keyword {
inline constexpr size_t kFramebuffer = offsetof(FragmentKey, framebuffer) / sizeof(u32);
inline constexpr size_t kTexture = offsetof(FragmentKey, texture) / sizeof(u32);
inline constexpr size_t kLighting = offsetof(FragmentKey, lighting) / sizeof(u32);
inline constexpr size_t kProcTex = offsetof(FragmentKey, proctex) / sizeof(u32);
inline constexpr size_t kUser = offsetof(FragmentKey, user) / sizeof(u32);
inline constexpr size_t kGas = offsetof(FragmentKey, gas) / sizeof(u32);

// FramebufferConfig
inline constexpr size_t kFbRaw = kFramebuffer + offsetof(FramebufferConfig, raw) / sizeof(u32);
inline constexpr size_t kFbRgbBlend = kFramebuffer + offsetof(FramebufferConfig, requested_rgb_blend) / sizeof(u32);
inline constexpr size_t kFbAlphaBlend = kFramebuffer + offsetof(FramebufferConfig, requested_alpha_blend) / sizeof(u32);
inline constexpr size_t kFbLogicOp = kFramebuffer + offsetof(FramebufferConfig, requested_logic_op) / sizeof(u32);
// BlendConfig: eq +0, src_factor +1, dst_factor +2

// TextureConfig
inline constexpr size_t kTexRaw = kTexture + offsetof(TextureConfig, raw) / sizeof(u32);
inline constexpr size_t kTexBorder = kTexture + offsetof(TextureConfig, texture_border_color) / sizeof(u32);
inline constexpr size_t kTexTevStages = kTexture + offsetof(TextureConfig, tev_stages) / sizeof(u32);
inline constexpr size_t kTexWrap = kTexture + offsetof(TextureConfig, requested_wrap) / sizeof(u32);
// TevStageConfigRaw: sources +0, modifiers +1, ops +2, scales +3
inline constexpr size_t kTevStageWords = sizeof(TevStageConfigRaw) / sizeof(u32);

// LightConfig and ProcTexConfig: their first (raw) word; the rest by `offsetof` from there.
inline constexpr size_t kLightRaw = kLighting;
inline constexpr size_t kProcTexRaw = kProcTex;

static_assert(kFbRaw == 0 && kFbRgbBlend == 1 && kFbAlphaBlend == 4 && kFbLogicOp == 7);
static_assert(kTexRaw == 8 && kTexTevStages == 12 && kTexWrap == 36);
static_assert(kLighting == 42 && kProcTex == 61 && kUser == 68 && kGas == 69);
}  // namespace keyword

// A key field as the mode sees it: a literal in the specialised mode, a read of `cfg` in the ubershader.
struct Value {
    bool known = false;
    u32 literal = 0;     // when known
    std::string glsl;    // a GLSL `uint` expression (the literal, or the cfg read)

    static Value Known(u32 v) { return Value{true, v, fmt::format("{}u", v)}; }
};

// A boolean of Values, with the same duality.
struct Cond {
    bool known = false;
    bool value = false;  // when known
    std::string glsl;    // a GLSL `bool` expression

    static Cond Known(bool v) { return Cond{true, v, v ? "true" : "false"}; }
};

class Emitter {
public:
    using Body = std::function<void()>;

    Emitter(Mode mode, const FragmentKey& applied_key, const DeviceProfile& profile, ShaderPass pass);

    Mode mode() const { return mode_; }
    bool specialised() const { return mode_ == Mode::Specialised; }
    const DeviceProfile& profile() const { return profile_; }
    ShaderPass pass() const { return pass_; }

    // ---- text ----
    void Raw(std::string_view text) { out_ += text; }
    template <typename... Args>
    void Line(fmt::format_string<Args...> format, Args&&... args) {
        out_ += fmt::format(format, std::forward<Args>(args)...);
        out_ += '\n';
    }
    const std::string& text() const { return out_; }
    std::string& text() { return out_; }

    // ---- the key ----
    // Bits [first, first + count) of word `word` of the FragmentKey (`keyword::` has the words of the parts).
    Value Field(size_t word, u32 first, u32 count) const;
    Value Word(size_t word) const { return Field(word, 0, 32); }
    static Value Const(u32 v) { return Value::Known(v); }
    // `a ? b : c`.
    Value Select(const Cond& c, const Value& b, const Value& a) const;
    // `value >> shift & mask` of an arbitrary Value.
    Value Slice(const Value& v, u32 first, u32 count) const;

    // ---- conditions ----
    static Cond True() { return Cond::Known(true); }
    static Cond False() { return Cond::Known(false); }
    Cond Eq(const Value& v, u32 n) const;
    Cond Ne(const Value& v, u32 n) const;
    Cond Eq(const Value& a, const Value& b) const;
    Cond Not(const Cond& c) const;
    Cond And(const Cond& a, const Cond& b) const;
    Cond Or(const Cond& a, const Cond& b) const;

    // ---- control flow ----
    // Specialised: runs the chosen body only. Uber: `if (c) { then } else { otherwise }`.
    void If(const Cond& c, const Body& then, const Body& otherwise = {});
    // Specialised: the body of the matching case (or `otherwise`). Uber: `switch (v) { case n: { body } break; ... }`.
    void Switch(const Value& v, std::initializer_list<std::pair<u32, Body>> cases, const Body& otherwise = {});

    // A loop over a count the key holds (the lights of the lighting loop). Specialised: the body is emitted once per index,
    // `index` being the literal ("0u", "1u", ...), in a block of its own each. Uber: `for (uint <variable> = 0u; <variable> <
    // count; ++<variable>) { body }` with `index` the variable's name (`variable` must be a valid GLSL name that nothing else
    // in main uses). A count above `max` is cut to it in both modes.
    void ForEach(const Value& count, u32 max, std::string_view variable, const std::function<void(const std::string& index)>& body);

    // The bytes of the key the specialised mode reads (the profile-applied key).
    u32 KnownWord(size_t word) const { return words_[word]; }

private:
    Mode mode_;
    DeviceProfile profile_;
    ShaderPass pass_;
    u32 words_[kFragmentKeyWords] = {};
    std::string out_;
};

}  // namespace mh4n::gpu::fragment
