// Written for this package from the design: nothing is copied from Azahar here.
//
// The pipeline builder of `--gpu native` (N2-08): turns a `PipelineKey` into a graphics pipeline. It is the build callback of
// N2-33's default provider (and what N2-16's provider calls on a worker thread):
//   - the base description (vertex layout, topology, host formats, the three set shapes) is `BaseDescOf`;
//   - the fixed-function state is the requested one with the device profile applied (`rhi::state::PlanPipeline`: the
//     hardware logic op or none, the blend of an emulated min/max half);
//   - the vertex stage is N2-06's SPIR-V for a translated program (registered by the draw with `AddVertexStage`: the key
//     holds only its id), or N2-37's trivial stage for the CPU path's batches;
//   - the fragment stage is the specialised shader of the key's `FragmentKey` (glslang, on the calling thread) or the
//     ubershader of its module (generated once per device profile, kept);
//   - the fix-up geometry stage (key.fixup) from the generator that lighting/ registers, for the vertex stage's output map.
//
// Thread: the builder's tables are guarded by a mutex; `Build` may be called from several threads.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/generator.h"
#include "gpu/native/compose.h"
#include "gpu/native/pipeline_key.h"
#include "gpu/shader/vertex_shader.h"

namespace mh4n::gpu::native {

struct BuilderStats {
    u64 specialised_builds = 0;
    u64 uber_generated = 0;     // ubershader modules generated (once per module and profile)
    u64 uber_builds = 0;        // pipelines built from an ubershader
    u64 fragment_failures = 0;  // the generated GLSL did not compile
    double glslang_ms = 0.0;    // spent on the fragment shaders
};

class PipelineBuilder {
public:
    PipelineBuilder(std::shared_ptr<rhi::Backend> backend, const fragment::FeatureRegistry& features);

    // Registers a translated vertex stage under the id the key's `vertex.program` holds.
    void AddVertexStage(u64 program, std::shared_ptr<const shader::TranslatedVertexShader> stage);

    // The default provider's build callback.
    rhi::Result<std::shared_ptr<rhi::Pipeline>> Build(const PipelineKey& key);

    // The specialised shader of a key, as Build makes it (for the tests and the logs).
    fragment::Generated GenerateFor(const PipelineKey& key) const;

    BuilderStats stats() const;

private:
    struct UberKey {
        UberModule module;
        bool operator<(const UberKey& o) const { return module < o.module; }
    };

    std::shared_ptr<rhi::Backend> backend_;
    const fragment::FeatureRegistry& features_;
    fragment::DeviceProfile profile_;
    mutable std::mutex mutex_;
    std::unordered_map<u64, std::shared_ptr<const shader::TranslatedVertexShader>> vertex_stages_;
    std::map<UberKey, std::shared_ptr<const fragment::Generated>> ubers_;
    BuilderStats stats_;
};

}  // namespace mh4n::gpu::native
