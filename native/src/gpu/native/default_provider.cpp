// The default pipeline provider (N2-33). Written for this package: nothing is copied from Azahar.
#include "gpu/native/default_provider.h"

namespace mh4n::gpu::native {

DefaultPipelineProvider::DefaultPipelineProvider(BuildPipelineFn build, DefaultProviderOptions options)
    : build_(std::move(build)), force_ubershader_(options.force_ubershader) {}

void DefaultPipelineProvider::SetForceUbershader(bool force) {
    std::lock_guard lock(mutex_);
    force_ubershader_ = force;
}

size_t DefaultPipelineProvider::size() const {
    std::lock_guard lock(mutex_);
    return pipelines_.size();
}

rhi::Result<std::shared_ptr<rhi::Pipeline>> DefaultPipelineProvider::GetOrBuild(const PipelineKey& key, bool& built) {
    built = false;
    const auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        return it->second;
    }
    if (!build_) {
        return rhi::Error{"DefaultPipelineProvider: no build callback"};
    }
    rhi::Result<std::shared_ptr<rhi::Pipeline>> made = build_(key);
    if (!made.ok()) {
        ++total_.build_failures;
        ++frame_.build_failures;
        return rhi::Error{made.error()};
    }
    if (*made == nullptr) {
        ++total_.build_failures;
        ++frame_.build_failures;
        return rhi::Error{"DefaultPipelineProvider: the build callback returned no pipeline"};
    }
    built = true;
    ++total_.sync_builds;
    ++frame_.sync_builds;
    pipelines_.emplace(key, *made);
    return *made;
}

rhi::Result<PipelineHandle> DefaultPipelineProvider::Lookup(const PipelineKey& key) {
    std::lock_guard lock(mutex_);
    ++total_.lookups;
    ++frame_.lookups;

    PipelineHandle handle;
    bool built = false;
    if (force_ubershader_) {
        ++total_.forced_uber;
        ++frame_.forced_uber;
        rhi::Result<std::shared_ptr<rhi::Pipeline>> fallback = GetOrBuild(key.AsFallback(), built);
        if (!fallback.ok()) {
            return rhi::Error{fallback.error()};
        }
        handle.pipeline = *fallback;
        handle.fallback = !key.IsFallback();
    } else {
        rhi::Result<std::shared_ptr<rhi::Pipeline>> own = GetOrBuild(key, built);
        if (!own.ok()) {
            return rhi::Error{own.error()};
        }
        handle.pipeline = *own;
        handle.fallback = false;
    }
    if (!built) {
        ++total_.hits;
        ++frame_.hits;
    }
    return handle;
}

void DefaultPipelineProvider::ReportMiss(const PipelineKey&, std::string_view) {
    std::lock_guard lock(mutex_);
    ++total_.reported_misses;
    ++frame_.reported_misses;
}

ProviderCounters DefaultPipelineProvider::Counters() const {
    std::lock_guard lock(mutex_);
    return total_;
}

std::vector<FrameCounter> DefaultPipelineProvider::TakeFrameCounters() {
    std::lock_guard lock(mutex_);
    std::vector<FrameCounter> out;
    out.push_back({"pipeline_sync_builds", frame_.sync_builds});
    out.push_back({"pipeline_forced_uber", frame_.forced_uber});
    frame_ = ProviderCounters{};
    return out;
}

} // namespace mh4n::gpu::native
