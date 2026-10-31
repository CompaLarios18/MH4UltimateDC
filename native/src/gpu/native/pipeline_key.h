// The pipeline key and the provider interface (N2-33; docs/native/gpu-fragment-design.md 4.3, 4.5, 4.7).
//
// Written for this package from the design: nothing is copied from Azahar here (`StaticPipelineInfo` of
// renderer_vulkan/vk_graphics_pipeline.h was read for the idea of what a pipeline is keyed on).
//
// A header-only CMake INTERFACE target, `mh4n_gpu_pipeline_key`, that depends only on the fragment key's header and
// on rhi.h: N2-16's `pipeline/` links it, implements `PipelineProvider`, serialises the key and owns the files; the
// composition receives the implementation by injection and never links `pipeline/`.
//
// `PipelineKey` is the REQUESTED form (what the registers ask for, no device profile): the fragment key as the
// registers give it, `rhi::FixedFunctionState` whole, the logical PICA formats. The device profile is applied when
// code is generated (the emulation bits) and when the pipeline is built (hardware logic op or none, the blend of a
// min/max emulation, the host formats from ChooseFormat). So one list moves between the PC and the Mac.
//
// Final names (the design called them proposals): `PipelineKey`, `PassKind`, `VertexStageKey`, `VertexInputKey`,
// `FixupStage`, `FragmentStageKey`, `UberModule`, `PipelineProvider`, `PipelineHandle`, `ProviderCounters`,
// `kPipelineKeyVersion`, `PipelineKey::Hash`, `PipelineKey::AsFallback`.
//
// Where it differs from the spec's wording: the fragment part holds the `FragmentKey` itself and not only its hash,
// because the build callback and N2-16's lists both need the key (a hash cannot be turned back into the
// configuration); `Hash()` uses `fragment::HashKey`, equality compares the key whole.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/key.h"
#include "gpu/fragment/struct_hash.h"
#include "mh4n/types.h"

namespace mh4n::gpu::native {

using fragment::HashCombine;

enum class PassKind : u8 {
    Normal,       // a pass with the colour and depth attachments (BeginTargetPass)
    ShadowWrite,  // a pass with no attachments storing into the shadow source's R32_UINT view (BeginPass)
};

// The fix-up stage: the fixed geometry shader that corrects the quaternion of lit draws shaded by the translated
// vertex stage (design 5.5).
enum class FixupStage : u8 { None, FixedGeometry };

enum class VertexStageKind : u8 {
    Translated,  // N2-06's translated vertex program; `program` is the u64 of its cache key
    Trivial,     // the CPU path's batches (N2-36, N2-37): the vertex stage that passes the vertices through
};

enum class FragmentKind : u8 { Specialised, Uber };
enum class UberModule : u8 { Color, ShadowWrite };

// The vertex-input description (N2-05's, `vertex::LoadedDraw`) reduced to what the pipeline bakes: the bindings'
// strides and the attributes' formats and offsets. The shader-side conversion (integer to float, the aliased w) is part
// of the vertex program's key, not of this. Unused entries are zero.
struct VertexInputKey {
    struct Binding {
        u32 stride = 0;
        u8 per_instance = 0;
        bool operator==(const Binding&) const = default;
    };
    struct Attribute {
        u8 location = 0;
        u8 binding = 0;
        rhi::Format format = rhi::Format::Undefined;
        u32 offset = 0;
        bool operator==(const Attribute&) const = default;
    };
    u8 binding_count = 0;
    u8 attribute_count = 0;
    std::array<Binding, rhi::kMaxVertexBindings> bindings{};
    std::array<Attribute, rhi::kMaxVertexAttributes> attributes{};
    bool operator==(const VertexInputKey&) const = default;

    // The layout the pipeline is made with (binding i is `bindings[i]`).
    rhi::VertexLayout ToLayout() const {
        rhi::VertexLayout layout;
        for (u32 i = 0; i < binding_count; ++i) {
            layout.bindings.push_back(rhi::VertexBinding{i, bindings[i].stride, bindings[i].per_instance != 0});
        }
        for (u32 i = 0; i < attribute_count; ++i) {
            layout.attributes.push_back(
                rhi::VertexAttribute{attributes[i].location, attributes[i].binding, attributes[i].format, attributes[i].offset});
        }
        return layout;
    }
};

struct VertexStageKey {
    VertexStageKind kind = VertexStageKind::Translated;
    u64 program = 0;  // Translated: N2-06's cache key; Trivial: 0
    VertexInputKey input;
    bool operator==(const VertexStageKey&) const = default;
};

struct FragmentStageKey {
    FragmentKind kind = FragmentKind::Specialised;
    UberModule uber = UberModule::Color;  // when kind is Uber
    fragment::FragmentKey key{};          // when kind is Specialised (zero otherwise)

    bool operator==(const FragmentStageKey& other) const {
        return kind == other.kind && uber == other.uber && key == other.key;
    }
};

namespace detail {

constexpr u64 Bits(float f) { return std::bit_cast<u32>(f); }

inline u64 HashStencilFace(u64 h, const rhi::StencilFaceState& f) {
    return HashCombine(h, static_cast<u64>(f.fail), static_cast<u64>(f.pass), static_cast<u64>(f.depth_fail),
                       static_cast<u64>(f.compare));
}

inline bool Same(const rhi::StencilFaceState& a, const rhi::StencilFaceState& b) {
    return a.fail == b.fail && a.pass == b.pass && a.depth_fail == b.depth_fail && a.compare == b.compare;
}

}  // namespace detail

// Field by field (the structure has padding), floats by their bits.
inline bool SameFixedFunctionState(const rhi::FixedFunctionState& a, const rhi::FixedFunctionState& b) {
    return a.blend_enabled == b.blend_enabled && a.src_color == b.src_color && a.dst_color == b.dst_color &&
           a.color_op == b.color_op && a.src_alpha == b.src_alpha && a.dst_alpha == b.dst_alpha &&
           a.alpha_op == b.alpha_op && a.color_write_mask == b.color_write_mask &&
           a.logic_op_enabled == b.logic_op_enabled && a.logic_op == b.logic_op && a.depth_test == b.depth_test &&
           a.depth_write == b.depth_write && a.depth_compare == b.depth_compare && a.stencil_test == b.stencil_test &&
           detail::Same(a.stencil_front, b.stencil_front) && detail::Same(a.stencil_back, b.stencil_back) &&
           a.cull == b.cull && a.front_face == b.front_face && a.depth_bias_enabled == b.depth_bias_enabled &&
           detail::Bits(a.depth_bias_constant) == detail::Bits(b.depth_bias_constant) &&
           detail::Bits(a.depth_bias_slope) == detail::Bits(b.depth_bias_slope);
}

inline u64 HashFixedFunctionState(u64 h, const rhi::FixedFunctionState& s) {
    h = HashCombine(h, s.blend_enabled, static_cast<u64>(s.src_color), static_cast<u64>(s.dst_color),
                    static_cast<u64>(s.color_op), static_cast<u64>(s.src_alpha), static_cast<u64>(s.dst_alpha),
                    static_cast<u64>(s.alpha_op), s.color_write_mask);
    h = HashCombine(h, s.logic_op_enabled, static_cast<u64>(s.logic_op), s.depth_test, s.depth_write,
                    static_cast<u64>(s.depth_compare), s.stencil_test);
    h = detail::HashStencilFace(h, s.stencil_front);
    h = detail::HashStencilFace(h, s.stencil_back);
    return HashCombine(h, static_cast<u64>(s.cull), static_cast<u64>(s.front_face), s.depth_bias_enabled,
                       detail::Bits(s.depth_bias_constant), detail::Bits(s.depth_bias_slope));
}

// The pipeline key. Equal keys hash equal; the hash is the same on every run and build (N2-16's lists store keys
// and compare their hashes across runs).
struct PipelineKey {
    PassKind pass = PassKind::Normal;
    VertexStageKey vertex;
    FixupStage fixup = FixupStage::None;
    FragmentStageKey fragment;
    rhi::FixedFunctionState state;  // as the registers request it (N2-13 fills it)
    rhi::Topology topology = rhi::Topology::TriangleList;
    // The logical PICA formats (rhi::Format::Rgba8Unorm, Rgb8, Rgb565, Rgb5A1, Rgba4; D16Unorm, D24S8). The host formats
    // come from ChooseFormat when the pipeline is built. Undefined: none.
    rhi::Format color_format = rhi::Format::Undefined;
    rhi::Format depth_format = rhi::Format::Undefined;

    u64 Hash() const {
        u64 h = HashCombine(0x504B4559ull, static_cast<u64>(pass), static_cast<u64>(vertex.kind), vertex.program);
        h = HashCombine(h, vertex.input.binding_count, vertex.input.attribute_count);
        for (u32 i = 0; i < vertex.input.binding_count; ++i) {
            h = HashCombine(h, vertex.input.bindings[i].stride, vertex.input.bindings[i].per_instance);
        }
        for (u32 i = 0; i < vertex.input.attribute_count; ++i) {
            const VertexInputKey::Attribute& a = vertex.input.attributes[i];
            h = HashCombine(h, a.location, a.binding, static_cast<u64>(a.format), a.offset);
        }
        h = HashCombine(h, static_cast<u64>(fixup), static_cast<u64>(fragment.kind), static_cast<u64>(fragment.uber));
        h = HashCombine(h, fragment.kind == FragmentKind::Specialised ? fragment::HashKey(fragment.key) : 0);
        h = HashFixedFunctionState(h, state);
        return HashCombine(h, static_cast<u64>(topology), static_cast<u64>(color_format), static_cast<u64>(depth_format));
    }

    bool operator==(const PipelineKey& o) const {
        return pass == o.pass && vertex == o.vertex && fixup == o.fixup && fragment == o.fragment &&
               SameFixedFunctionState(state, o.state) && topology == o.topology && color_format == o.color_format &&
               depth_format == o.depth_format;
    }

    // The pipeline that draws this key while its own is not ready (and every draw under force_ubershader): the
    // ubershader module of the pass kind with the same vertex stage, fix-up stage, fixed-function state, formats
    // and pass kind (design 4.5).
    PipelineKey AsFallback() const {
        PipelineKey fallback = *this;
        fallback.fragment = FragmentStageKey{};
        fallback.fragment.kind = FragmentKind::Uber;
        fallback.fragment.uber = pass == PassKind::ShadowWrite ? UberModule::ShadowWrite : UberModule::Color;
        return fallback;
    }
    bool IsFallback() const { return fragment.kind == FragmentKind::Uber; }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const noexcept { return static_cast<size_t>(key.Hash()); }
};

// The version of the key's types: lists keep it in their header (N2-16). Combines the fragment key's struct hash with
// the layout of the vertex-input key and of `rhi::FixedFunctionState`, and a number bumped by hand when `Hash()` or
// the lists' serialisation changes what it feeds.
namespace detail {
constexpr u64 kPipelineKeyFormat = 1;
constexpr u64 PipelineKeyLayoutHash() {
    using T = rhi::FixedFunctionState;
    using V = VertexInputKey;
    return HashCombine(kPipelineKeyFormat, sizeof(rhi::FixedFunctionState), offsetof(T, blend_enabled),
                       offsetof(T, color_write_mask), offsetof(T, logic_op_enabled), offsetof(T, depth_test),
                       offsetof(T, stencil_test), offsetof(T, stencil_front), offsetof(T, stencil_back),
                       offsetof(T, cull), offsetof(T, depth_bias_enabled), offsetof(T, depth_bias_slope),
                       sizeof(rhi::StencilFaceState), sizeof(V), sizeof(V::Binding), sizeof(V::Attribute),
                       rhi::kMaxVertexBindings, rhi::kMaxVertexAttributes);
}
}  // namespace detail
inline constexpr u64 kPipelineKeyVersion = HashCombine(fragment::kFragmentKeyStructHash, detail::PipelineKeyLayoutHash());

// ---- the provider ----------------------------------------------------------------------------------------

struct PipelineHandle {
    std::shared_ptr<rhi::Pipeline> pipeline;
    // The pipeline is the ubershader fallback (`PipelineKey::AsFallback`) and not the key's own.
    bool fallback = false;
};

struct ProviderCounters {
    u64 lookups = 0;
    u64 hits = 0;              // an own pipeline or a fallback already built
    u64 sync_builds = 0;       // pipelines built on the calling thread at a miss (`pipeline_sync_builds`)
    u64 forced_uber = 0;       // draws answered with the fallback because force_ubershader is on (`pipeline_forced_uber`)
    u64 build_failures = 0;
    u64 reported_misses = 0;   // ReportMiss calls
};

struct FrameCounter {
    std::string name;
    u64 value = 0;
};

class PipelineProvider {
public:
    virtual ~PipelineProvider() = default;

    // A ready pipeline for `key`, or its fallback, or an error (the build callback failed). The key is the
    // requested form. May build on the calling thread (the default provider does); N2-16's provider queues the
    // build on a worker and answers with the fallback meanwhile.
    virtual rhi::Result<PipelineHandle> Lookup(const PipelineKey& key) = 0;

    // The composition tells the provider that a draw wanted `key`'s pipeline and went without it for a reason the
    // provider cannot see (for example the generator reported the configuration cannot be generated). Providers
    // that keep a list note the key; the default one counts it.
    virtual void ReportMiss(const PipelineKey& key, std::string_view reason) = 0;

    virtual ProviderCounters Counters() const = 0;
    // The frame counters (`pipeline_sync_builds`, `pipeline_forced_uber`, ...) since the last call; reading resets them.
    virtual std::vector<FrameCounter> TakeFrameCounters() = 0;
};

}  // namespace mh4n::gpu::native
