// The default pipeline provider (N2-33; docs/native/gpu-fragment-design.md 4.7 and 4.5).
//
// Written for this package from the design: nothing is copied from Azahar here.
//
// On a miss it builds the pipeline synchronously on the calling thread through the build callback the composition
// gives it (N2-08 passes its generator; the tests pass hand-written shaders), counts it in `pipeline_sync_builds`
// (a frame counter), keeps it for the run and keeps no list. With `force_ubershader` it answers every draw with the
// fallback (the `UberModule` pipeline with the same vertex stage, fix-up stage, fixed-function state, formats and
// pass kind, `PipelineKey::AsFallback`) and counts `pipeline_forced_uber`; a fallback that does not exist is built
// synchronously, once, and counted in `pipeline_sync_builds` too (design 4.5: a fallback that does not exist is a
// compile on the render thread). N2-16 later registers its own provider through `mh4n/registry.h`.
//
// One thread at a time: the lookups run on the renderer thread. A mutex makes a second caller wait rather than race.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "gpu/native/pipeline_key.h"

namespace mh4n::gpu::native {

// Builds the pipeline of a key (specialised or ubershader: `key.fragment.kind` says which). Called on a miss.
using BuildPipelineFn = std::function<rhi::Result<std::shared_ptr<rhi::Pipeline>>(const PipelineKey&)>;

struct DefaultProviderOptions {
    bool force_ubershader = false;  // every draw takes the fallback (`--force-ubershader`)
};

class DefaultPipelineProvider final : public PipelineProvider {
public:
    DefaultPipelineProvider(BuildPipelineFn build, DefaultProviderOptions options = {});

    rhi::Result<PipelineHandle> Lookup(const PipelineKey& key) override;
    void ReportMiss(const PipelineKey& key, std::string_view reason) override;
    ProviderCounters Counters() const override;
    std::vector<FrameCounter> TakeFrameCounters() override;

    void SetForceUbershader(bool force);
    // Pipelines kept.
    size_t size() const;

private:
    rhi::Result<std::shared_ptr<rhi::Pipeline>> GetOrBuild(const PipelineKey& key, bool& built);

    BuildPipelineFn build_;
    mutable std::mutex mutex_;
    bool force_ubershader_;
    std::unordered_map<PipelineKey, std::shared_ptr<rhi::Pipeline>, PipelineKeyHash> pipelines_;
    ProviderCounters total_;
    ProviderCounters frame_;
};

}  // namespace mh4n::gpu::native
