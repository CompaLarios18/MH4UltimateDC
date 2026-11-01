// Written for this package from the design: nothing is copied from Azahar here.
#include "gpu/native/pipeline_builder.h"

#include <fmt/format.h>

#include "gpu/backend/state/state.h"
#include "gpu/cpuvertex/trivial_vertex.h"
#include "gpu/fragment/profile.h"

namespace mh4n::gpu::native {

PipelineBuilder::PipelineBuilder(std::shared_ptr<rhi::Backend> backend, const fragment::FeatureRegistry& features)
    : backend_(std::move(backend)), features_(features), profile_(fragment::ProfileOf(backend_->Caps())) {}

void PipelineBuilder::AddVertexStage(u64 program, std::shared_ptr<const shader::TranslatedVertexShader> stage) {
    std::lock_guard lock(mutex_);
    vertex_stages_[program] = std::move(stage);
}

BuilderStats PipelineBuilder::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

fragment::Generated PipelineBuilder::GenerateFor(const PipelineKey& key) const {
    const fragment::ShaderPass pass =
        key.pass == PassKind::ShadowWrite ? fragment::ShaderPass::ShadowWrite : fragment::ShaderPass::Normal;
    return fragment::GenerateSpecialised(key.fragment.key, profile_, pass, features_);
}

rhi::Result<std::shared_ptr<rhi::Pipeline>> PipelineBuilder::Build(const PipelineKey& key) {
    rhi::GraphicsPipelineDesc desc = BaseDescOf(key, *backend_);

    // The fixed-function state on this device.
    rhi::Result<rhi::state::PipelinePlan> plan = rhi::state::PlanPipeline(key.state, backend_->Caps());
    if (!plan.ok()) {
        return rhi::Error{fmt::format("pipeline plan: {}", plan.error())};
    }
    desc.state = plan->built;

    // The vertex stage.
    std::shared_ptr<const shader::TranslatedVertexShader> translated;
    std::span<const u32> vertex_spirv;
    if (key.vertex.kind == VertexStageKind::Translated) {
        std::lock_guard lock(mutex_);
        const auto it = vertex_stages_.find(key.vertex.program);
        if (it == vertex_stages_.end()) {
            return rhi::Error{"pipeline builder: the translated vertex stage of this key was not registered"};
        }
        translated = it->second;
        vertex_spirv = translated->spirv;
    } else {
        vertex_spirv = cpuvertex::TrivialVertexShader(true);
    }
    desc.vertex.spirv = vertex_spirv;

    // The fragment stage.
    std::shared_ptr<const fragment::Generated> uber;
    fragment::Generated specialised;
    if (key.IsFallback()) {
        std::lock_guard lock(mutex_);
        const UberKey uber_key{key.fragment.uber};
        auto it = ubers_.find(uber_key);
        if (it == ubers_.end()) {
            const fragment::ShaderPass pass = key.fragment.uber == UberModule::ShadowWrite ? fragment::ShaderPass::ShadowWrite
                                                                                          : fragment::ShaderPass::Normal;
            auto generated = std::make_shared<fragment::Generated>(fragment::GenerateUber(pass, profile_, features_));
            ++stats_.uber_generated;
            stats_.glslang_ms += generated->compile_ms;
            it = ubers_.emplace(uber_key, std::move(generated)).first;
        }
        uber = it->second;
        if (!uber->ok()) {
            ++stats_.fragment_failures;
            return rhi::Error{fmt::format("the ubershader does not compile: {}", uber->error)};
        }
        desc.fragment.spirv = uber->spirv;
        ++stats_.uber_builds;
        desc.name += " (uber)";
    } else {
        specialised = GenerateFor(key);
        {
            std::lock_guard lock(mutex_);
            ++stats_.specialised_builds;
            stats_.glslang_ms += specialised.compile_ms;
            if (!specialised.ok()) {
                ++stats_.fragment_failures;
            }
        }
        if (!specialised.ok()) {
            return rhi::Error{fmt::format("a fragment shader does not compile: {}", specialised.error)};
        }
        desc.fragment.spirv = specialised.spirv;
    }

    // The fix-up stage (design 5.5): lighting/ generates it for the vertex stage's output map.
    std::vector<u32> geometry;
    if (key.fixup == FixupStage::FixedGeometry) {
        if (translated == nullptr) {
            return rhi::Error{"pipeline builder: a fix-up stage needs a translated vertex stage"};
        }
        VertexStageOutputs outputs;
        outputs.program = key.vertex.program;
        outputs.num_outputs = translated->num_outputs;
        outputs.output_map = translated->output_map;
        for (const shader::SemanticSlot& slot : translated->semantic_map) {
            outputs.semantic_map.push_back({slot.attribute, slot.component});
        }
        geometry = FixupGeneratorRegistry::Global().Generate(outputs);
        if (geometry.empty()) {
            return rhi::Error{"pipeline builder: the fix-up generator made no geometry stage"};
        }
        desc.geometry.spirv = geometry;
    }

    return backend_->CreateGraphicsPipeline(desc);
}

}  // namespace mh4n::gpu::native
