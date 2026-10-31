// The composition's slots (N2-33; docs/native/gpu-fragment-design.md 4.1, 5.1, 5.5, 5.6, 5.10).
//
// Written for this package from the design: nothing is copied from Azahar here.
//
// A `DrawComposer` records one draw on an `rhi::Recorder` from its `PipelineKey` pieces, its `Needs`, its uniform
// allocation and its bindings, through the slots that the packages after N2-33 only fill:
//   1. the PASS KIND: `Normal` in a target pass (`BeginTargetPass`, kept open from draw to draw); `ShadowWrite` in a pass
//      with no attachments (`BeginPass` with the shadow source's width and height) with the shadow source's R32_UINT view
//      bound as the storage image (set 2);
//   2. the FIX-UP STAGE: when `needs.fixup_gs` holds, the vertex stage is a translated one and
//      `Capabilities::geometry_shader` is on, the key's fix-up field is `FixedGeometry` if a fix-up generator is
//      registered (lighting/, N2-09) and the pipeline's geometry stage takes the SPIR-V it returns for the vertex stage's
//      output map; until one is registered the field stays `None` and the draw is counted `stubbed:fixup`;
//   3. the DESTINATION COPY: when `needs.dst_copy` holds, `FeedbackCopy` of the colour target before the draw and the
//      twin bound as `tex_color` (a pass break and a full copy per draw: design 5.6);
//   4. the BINDINGS: one layout for every module (bindings.h), one set per draw from the backend's `BindingArena`. A set
//      whose contents did not change since the previous draw of the same recording is bound again, not rewritten. A
//      stub feature leaves its slot neutral: the slots a draw does not use hold one-texel dummies.
//
// Final names (the design called them proposals): `DrawComposer`, `DrawDesc`, `DrawResult`, `ComposerCounters`,
// `FixupGenerator`, `FixupGeneratorRegistry`, `VertexStageOutputs`, `BaseDescOf`.
//
// What the caller does (the renderer thread, which also made the images):
//   - `BeginRecording(rec)` right after `backend.BeginRecording()` (and `FragmentUniformRing::BeginRecording()`);
//   - passes go through the composer (`BeginTargetPass`, the draw's `target`), never through the recorder directly: it
//     keeps track of the open pass. A target pass is begun with the caller's load operations by `BeginTargetPass`;
//     the draws continue it with load;
//   - the dynamic state that N2-13 owns (`SetBlendConstants`, `SetStencilReference`, `SetStencilMasks`) is set by the
//     caller before the first draw whose pipeline needs it (rhi.h: dynamic state that nothing set is a validation
//     error); the viewport and scissor are set by the composer when the draw gives them;
//   - `FixedFunctionState` is the requested form; the device profile on it (hardware logic op or not, the blend of a
//     min/max emulation) is applied by N2-13 when the pipeline is built (`BaseDescOf` copies it as it is).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/state.h"
#include "gpu/fragment/tables.h"
#include "gpu/native/bindings.h"
#include "gpu/native/pipeline_key.h"

namespace mh4n::gpu::native {

// ---- the fix-up generator -------------------------------------------------------------------------------

// The output map of a translated vertex stage (N2-06's `TranslatedVertexShader::{num_outputs, output_map,
// semantic_map}`), which the fixed geometry shader is generated from.
struct VertexStageOutputs {
    u64 program = 0;
    u32 num_outputs = 0;
    std::array<u8, 16> output_map{};  // output register -> attribute, 16 = none
    struct Semantic {
        u8 attribute = 16;  // 16: no output attribute maps this semantic
        u8 component = 0;
    };
    std::vector<Semantic> semantic_map;
};

// lighting/ (N2-09) registers the generator: SPIR-V of the fixed geometry shader (triangles in, triangle strip out)
// for a vertex stage's outputs, empty when it cannot be made.
using FixupGenerator = std::function<std::vector<u32>(const VertexStageOutputs&)>;

class FixupGeneratorRegistry {
public:
    static FixupGeneratorRegistry& Global();
    // Replaces the generator (an empty function removes it).
    void Set(FixupGenerator generator) { generator_ = std::move(generator); }
    bool Has() const { return static_cast<bool>(generator_); }
    std::vector<u32> Generate(const VertexStageOutputs& outputs) const { return generator_ ? generator_(outputs) : std::vector<u32>{}; }

private:
    FixupGenerator generator_;
};

// ---- what the pipeline builder gets ready-made ---------------------------------------------------------------

// Everything of a graphics pipeline description that does not depend on how shaders are generated, from the key: the
// vertex layout, the topology, the host formats (`ChooseFormat` of the logical ones; none for the shadow-write pass),
// the fixed-function state as given, and the three set shapes of the one layout. The builder (N2-08's generator, the
// tests' hand-written shaders) fills `vertex`, `fragment` and, when `key.fixup` is `FixedGeometry`, `geometry`.
rhi::GraphicsPipelineDesc BaseDescOf(const PipelineKey& key, const rhi::Backend& backend);

// The fix-up decision as a pure rule (design 5.5): the stage is wanted when lighting needs it (`needs.fixup_gs`) and the
// vertex stage is the translated one; it is possible when the device has geometry shaders; it is generated when a
// generator is registered. `Stubbed`: wanted and possible, but nobody generates it yet (counted `stubbed:fixup`).
enum class FixupDecision : u8 { None, Applied, Stubbed };
constexpr FixupDecision DecideFixup(bool needs_fixup, VertexStageKind vertex, bool geometry_shader, bool generator_registered) {
    if (!needs_fixup || vertex != VertexStageKind::Translated || !geometry_shader) {
        return FixupDecision::None;
    }
    return generator_registered ? FixupDecision::Applied : FixupDecision::Stubbed;
}

// ---- a draw ---------------------------------------------------------------------------------------------------------

struct UniformRange {
    const rhi::Buffer* buffer = nullptr;
    u64 offset = 0;
    u64 size = 0;
};

struct TextureBinding {
    const rhi::Image* image = nullptr;  // null: the dummy
    rhi::ImageView view = rhi::ImageView::Sampled;
    const rhi::Sampler* sampler = nullptr;  // null: the composer's nearest, clamped sampler
};

struct DrawDesc {
    // ---- the pipeline key's pieces (the pass kind and the fix-up stage are the composer's) ----
    VertexStageKey vertex;
    const fragment::FragmentKey* fragment = nullptr;
    fragment::Needs needs;
    rhi::FixedFunctionState state;
    rhi::Topology topology = rhi::Topology::TriangleList;
    rhi::Format color_format = rhi::Format::Undefined;  // logical PICA formats
    rhi::Format depth_format = rhi::Format::Undefined;
    std::optional<VertexStageOutputs> vertex_outputs;  // for the fix-up generator; the builder reads it from N2-06's cache

    // ---- the pass ----
    rhi::TargetPassDesc target;          // Normal: the attachments and area (the draw continues with load)
    rhi::Image* shadow_target = nullptr;  // ShadowWrite: the shadow source whose R32_UINT view is written; sets the pass size

    // ---- set 0 ----
    UniformRange vs_pica_uniforms;  // binding 0 (the vertex stage's, N2-06), skipped when the buffer is null
    UniformRange vs_uniforms;       // binding 1
    fragment::FragmentUniformRing::Allocation fragment_uniforms;  // binding 2
    const fragment::TableRings* tables = nullptr;                  // bindings 3 to 5 (skipped when null)

    // ---- set 1 ----
    std::array<TextureBinding, 3> units;  // unit 0 goes to the class its key's texture0_type names

    // ---- the geometry ----
    std::span<const rhi::VertexBufferBinding> vertex_buffers;  // from binding 0
    const rhi::Buffer* index_buffer = nullptr;
    u64 index_offset = 0;
    rhi::IndexType index_type = rhi::IndexType::U16;
    u32 count = 0;  // vertices, or indices when index_buffer is set
    u32 first = 0;  // first vertex or first index
    s32 base_vertex = 0;

    std::optional<rhi::Viewport> viewport;
    std::optional<rhi::Rect> scissor;
};

struct DrawResult {
    bool fallback = false;       // the provider answered with the ubershader fallback
    bool fixup = false;          // the draw ran with the fixed geometry stage
    bool fixup_stubbed = false;  // lighting needed the fix-up, the device could, and no generator is registered
    bool dst_copied = false;
    bool set0_reused = false, set1_reused = false, set2_reused = false;
};

struct ComposerCounters {
    u64 draws = 0;
    u64 fallback_draws = 0;
    u64 stubbed_fixup = 0;    // `stubbed:fixup`
    u64 dst_copies = 0;
    u64 sets_acquired = 0;
    u64 sets_reused = 0;
    u64 pass_begins = 0;      // BeginTargetPass or BeginPass calls that opened a pass (not the continuations)
};

class DrawComposer {
public:
    // Makes the dummies (one-texel images and a sampler) on the calling thread, which must be the thread that records.
    static rhi::Result<std::unique_ptr<DrawComposer>> Create(std::shared_ptr<rhi::Backend> backend,
                                                             PipelineProvider& provider);
    ~DrawComposer();
    DrawComposer(const DrawComposer&) = delete;
    DrawComposer& operator=(const DrawComposer&) = delete;

    // The fix-up generator the composer asks (the global registry by default); a test injects its own.
    void SetFixupRegistry(const FixupGeneratorRegistry* registry) { fixup_ = registry; }

    // Call right after the thread's `backend.BeginRecording()`: clears the dummies in this recorder and forgets the
    // previous recording's sets and pass.
    void BeginRecording(rhi::Recorder& rec);

    // Begins (or continues) a target pass with the caller's load operations: how a pass is cleared. A draw's `target`
    // continues it with load.
    void BeginTargetPass(rhi::Recorder& rec, const rhi::TargetPassDesc& pass);
    // Ends the pass the composer holds open (before a copy or a blit the caller records itself, which would end it
    // anyway; call this so the composer does not think it is still open).
    void EndPass(rhi::Recorder& rec);

    // The key the draw would use (pass kind and fix-up stage decided), without recording anything. The tests and
    // N2-16 use it.
    PipelineKey KeyOf(const DrawDesc& draw) const;

    rhi::Result<DrawResult> Draw(rhi::Recorder& rec, const DrawDesc& draw);

    const ComposerCounters& counters() const { return counters_; }
    // `stubbed:fixup`, `dst_copies`, ... since the last call; reading resets them.
    std::vector<FrameCounter> TakeFrameCounters();

private:
    DrawComposer(std::shared_ptr<rhi::Backend> backend, PipelineProvider& provider);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PipelineProvider& provider_;
    const FixupGeneratorRegistry* fixup_;
    ComposerCounters counters_;
    ComposerCounters frame_;
};

}  // namespace mh4n::gpu::native
