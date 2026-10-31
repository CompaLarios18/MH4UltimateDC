// The layout policy and the owner rule of the targets module (N2-27). See impl.h for the policy.
#include <algorithm>

#include <fmt/format.h>

#include "gpu/backend/targets/impl.h"

namespace mh4n::gpu::rhi::targets {

using ::plume::RenderTextureLayout;

void Transition(Recorder::Impl& recorder, Image::Impl& image, RenderTextureLayout layout) {
    ::plume::VulkanTexture* texture = image.vulkan();
    if (texture->textureLayout == RenderTextureLayout::PRESENT) {
        // After an acquire the contents of a presented image are not defined: leave PRESENT_SRC_KHR as UNDEFINED.
        texture->textureLayout = RenderTextureLayout::UNKNOWN;
    }
    ::plume::RenderTextureBarrier barrier(texture, layout);
    // Every stage on both sides: plume's barrier takes its source from the stages of the image's previous
    // barrier, so a full mask here keeps every later barrier a full dependency too.
    recorder.vulkan_list()->barriers(::plume::RenderBarrierStage::ALL, nullptr, 0, &barrier, 1);
    image.dirty = false;
}

void Normalize(Recorder::Impl& recorder, Image::Impl& image) {
    if (image.vulkan()->textureLayout != RenderTextureLayout::GENERAL || image.dirty) {
        Transition(recorder, image, RenderTextureLayout::GENERAL);
    }
}

void Acquire(Recorder::Impl& recorder, Image::Impl& image) {
    RecorderTargetsData& data = Data(recorder);
    if (!data.hook_registered) {
        data.hook_registered = true;
        RecorderTargetsData* raw = &data;
        Recorder::Impl* self = &recorder;
        // Runs when the recorder is submitted (value > 0, after the command buffer has ended) or dropped (0).
        recorder.on_submit.push_back([raw, self](TimelineValue value) {
            if (value == 0) {
                // A recorder that was never submitted left no barrier on the GPU: the state the images were in
                // when the recorder first touched them is the true one again.
                for (const TouchedImage& t : raw->touched) {
                    ::plume::VulkanTexture* texture = t.image->vulkan();
                    texture->textureLayout = t.layout;
                    texture->barrierStages = t.stages;
                    t.image->dirty = t.dirty;
                }
            }
            for (const TouchedImage& t : raw->touched) {
                if (t.image->open_pass_in == self) t.image->open_pass_in = nullptr;
            }
            if (value > 0 && !raw->staging.empty()) {
                TargetsData& targets = Data(*self->backend);
                std::lock_guard lock(targets.staging_mutex);
                for (std::shared_ptr<Buffer>& buffer : raw->staging) {
                    targets.staging_pending.emplace_back(value, std::move(buffer));
                }
                raw->staging.clear();
            }
        });
    }
    const bool known = std::any_of(data.touched.begin(), data.touched.end(),
                                   [&](const TouchedImage& t) { return t.image == &image; });
    if (!known) {
        ::plume::VulkanTexture* texture = image.vulkan();
        data.touched.push_back(TouchedImage{&image, texture->textureLayout, texture->barrierStages, image.dirty});
    }

    // The owner rule, clause 1 and the receiving half of clause 2.
    std::lock_guard lock(image.owner_mutex);
    if (image.handover_pending) {
        if (recorder.thread == image.handover_thread) {
            image.owner = recorder.thread;
            image.handover_pending = false;
            const Wait wait = image.handover_wait;
            const bool listed = std::any_of(recorder.required_waits.begin(), recorder.required_waits.end(),
                                            [&](const Wait& w) { return w.value == wait.value && w.stage == wait.stage; });
            if (!listed) recorder.required_waits.push_back(wait);
            return;
        }
#if MH4N_RHI_CHECKS
        ReportOwnerRuleBreach(OwnerRule::ForeignThread, "a thread recorded on an image that was handed over to another thread");
#endif
        return;
    }
#if MH4N_RHI_CHECKS
    if (recorder.thread != image.owner) {
        ReportOwnerRuleBreach(OwnerRule::ForeignThread, "a thread recorded on an image it does not own");
    }
#endif
}

void Settle(Recorder::Impl& recorder) {
    RecorderTargetsData& data = Data(recorder);
    if (!data.pass_open) return;
    data.pass_open = false;
    if (recorder.in_pass) {
        recorder.vulkan_list()->setFramebuffer(nullptr);  // ends the render pass
        recorder.in_pass = false;
    }
    const TargetPass pass = data.pass;
    data.pass = TargetPass{};
    // What the pass wrote becomes visible to everything after it; the depth attachment leaves the attachment layout.
    if (pass.color != nullptr) {
        Transition(recorder, *pass.color, RenderTextureLayout::GENERAL);
        pass.color->open_pass_in = nullptr;
    }
    if (pass.depth != nullptr) {
        Transition(recorder, *pass.depth, RenderTextureLayout::GENERAL);
        pass.depth->open_pass_in = nullptr;
    }
}

} // namespace mh4n::gpu::rhi::targets
