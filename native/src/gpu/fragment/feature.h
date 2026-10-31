// The feature interface of the fragment pipeline (N2-33; docs/native/gpu-fragment-design.md 4.8).
//
// Written for this package from the design: nothing is copied from Azahar here.
//
// Five features own a directory of `native/src/gpu/fragment/` each, filled by their packages:
// `sampling/` (N2-23), `lighting/` (N2-09), `fog/` (N2-10), `proctex/` (N2-11) and `shadow/` (N2-12). Each
// defines one `FeatureModule` and registers it at start-up with `MH4N_REGISTER_FRAGMENT_FEATURE`. The
// generator (N2-08) calls the registered features at fixed points, in Azahar's order, and the composition
// (N2-33's `native/`) asks them for the draw's needs, uniforms and tables. A feature with nothing to say
// about a hook leaves the default (a no-op).
//
// Final names (the design called them proposals): `FeatureModule`, `FeatureRegistry`, `Point`, `Mode`,
// `EmitContext`, `ReservedWords`, `TableUpload` (tables.h), `MH4N_REGISTER_FRAGMENT_FEATURE`.
//
// THE RULE (design 4.8, adapted to the whole port of the key): the key is complete before any feature
// lands (`MakeKey` ports Azahar's `FSConfig` whole). A feature
//   - READS the key's fields (every sub-struct), never another feature's internals: it calls the
//     functions of the prototype block that core declares before any definition;
//   - WRITES only its own uniform members (named below), its own table offsets, and the reserved words
//     (`ReservedWords`: the gas word, owned by `fog/`; the user word, owned by the HD pack);
//   - reports `needs-decision` if it needs a key field the port does not hold, or a uniform outside its
//     members. Core's key and uniform files (key.h, uniforms.h) are N2-33's.
//
// The members each feature owns (FragmentUniforms, uniforms.h):
//   sampling  tex_lod_bias, tex_border_color
//   lighting  lighting_lut_offset, lighting_global_ambient, light_src
//   fog       fog_color, fog_lut_offset, and the gas region
//   proctex   proctex_noise_lut_offset, proctex_color_map_offset, proctex_alpha_map_offset, proctex_lut_offset,
//             proctex_diff_lut_offset, proctex_bias, proctex_noise_f, proctex_noise_a, proctex_noise_p
//   shadow    shadow_bias_constant, shadow_bias_linear, shadow_texture_bias
// Core owns the rest: framebuffer_scale, alphatest_ref, depth_scale, depth_offset, the scissor rectangle,
// const_color, tev_combiner_buffer_color and blend_color.
#pragma once

#include <array>
#include <span>
#include <string_view>

#include "gpu/fragment/key.h"
#include "gpu/fragment/uniforms.h"

namespace mh4n::gpu::fragment {

struct TableUpload;  // tables.h

// The points at which the generator calls `Emit` (the order of Azahar's FragmentModule::Generate).
enum class Point : u8 {
    Declarations,  // the feature's own declarations (core declares the varyings, bindings and prototype block)
    Prologue,
    ScissorDepth,
    Lighting,
    Tev,
    AlphaTest,
    FogGas,
    Output,
};

// How the generator is writing the shader: the specialised mode evaluates the key's fields and emits the chosen
// branch with literals; the ubershader mode reads them from `cfg` (design 4.4.1).
enum class Mode : u8 { Specialised, Uber };

class Emitter;  // N2-08 defines the emitter helpers; this package only passes a pointer

struct EmitContext {
    const FragmentKey& key;
    const DeviceProfile& profile;
    Mode mode = Mode::Specialised;
    Emitter* emitter = nullptr;  // N2-08's helpers (null in this package's tests)
};

// The words a feature's `Key` hook may write.
struct ReservedWords {
    u32& user;  // the HD pack's word (N2-17/N2-18); no feature of the five writes it
    u32& gas;   // `fog/`'s
};

class FeatureModule {
public:
    virtual ~FeatureModule() = default;

    // "sampling", "lighting", "fog", "proctex" or "shadow".
    virtual const char* Name() const = 0;

    // Fills the reserved words. The key has been made from the registers already.
    virtual void Key(const pica::PicaState&, const FragmentKey&, ReservedWords) const {}

    // Generated code at one point (both modes).
    virtual void Emit(Point, EmitContext&) const {}

    // Writes the feature's uniform members of `block.uniforms` (and, for `fog/`, the gas region `block.gas`; and
    // only those) when a dirty bit of `PicaState` they depend on is set or `dirty.all` is (the first block), and marks
    // `UniformDirty` when it writes.
    virtual void FillUniforms(const pica::PicaState&, FragmentBlock&, UniformDirty&) const {}

    // Uploads the feature's tables when `PicaState`'s `lighting.lut_dirty`, `fog.lut_dirty` or
    // `proctex.table_dirty` says so (the request is `upload.need`), and writes the offsets into the uniforms.
    virtual void UploadTables(TableUpload&) const {}

    // The feature's part of the draw's needs, ORed into core's.
    virtual Needs NeedsOf(const FragmentKey&, const DeviceProfile&) const { return {}; }

    // Non-null when the feature is a stub and this draw needed the feature (the generator counts
    // `stubbed:<name>` for it): the text after "stubbed:" is returned.
    virtual const char* Stubbed(const FragmentKey&) const { return nullptr; }
};

inline constexpr std::array<std::string_view, 5> kFeatureNames = {"sampling", "lighting", "fog", "proctex", "shadow"};

// The features by name. The global registry is filled at start-up by the five directories; a test may
// make its own.
class FeatureRegistry {
public:
    static FeatureRegistry& Global();

    // False, and nothing added, if the name is not one of the five or is taken.
    bool Add(const FeatureModule& feature);
    // Removes a feature (for tests that replace one in a private registry).
    void Clear() {
        features_.fill(nullptr);
        Rebuild();
    }

    const FeatureModule* Find(std::string_view name) const;
    // The registered features in the order of `kFeatureNames`.
    std::span<const FeatureModule* const> All() const { return std::span<const FeatureModule* const>(ordered_.data(), count_); }

private:
    std::array<const FeatureModule*, 5> features_{};
    std::array<const FeatureModule*, 5> ordered_{};
    size_t count_ = 0;

    void Rebuild();
};

// The `stubbed:<feature>` names of the registered features whose `Stubbed` is non-null for this key.
size_t StubbedFeatures(const FragmentKey& key, const FeatureRegistry& features, std::span<const char*> out);

namespace detail {
struct FeatureRegistrar {
    explicit FeatureRegistrar(const FeatureModule& feature);
};
} // namespace detail

} // namespace mh4n::gpu::fragment

// In a .cpp of a feature directory, at namespace scope: MH4N_REGISTER_FRAGMENT_FEATURE(MyFeatureClass);
#define MH4N_FF_CONCAT_IMPL(a, b) a##b
#define MH4N_FF_CONCAT(a, b) MH4N_FF_CONCAT_IMPL(a, b)
#define MH4N_REGISTER_FRAGMENT_FEATURE(Type)                                                    \
    namespace {                                                                                  \
    const Type MH4N_FF_CONCAT(mh4n_fragment_feature_, __LINE__){};                               \
    const ::mh4n::gpu::fragment::detail::FeatureRegistrar MH4N_FF_CONCAT(mh4n_fragment_registrar_, __LINE__)( \
        MH4N_FF_CONCAT(mh4n_fragment_feature_, __LINE__));                                       \
    }
