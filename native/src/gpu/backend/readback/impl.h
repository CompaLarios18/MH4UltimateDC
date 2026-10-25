// The private state of the readback module (N2-28): the per-backend slot core/impl.h reserves
// (Backend::Impl::readback) and the helpers the two files share. Nothing outside native/src/gpu/backend/readback/
// and the module's test includes it.
#pragma once

#include <memory>
#include <mutex>

#include "gpu/backend/core/impl.h"
#include "gpu/backend/targets/impl.h"

namespace mh4n::gpu::rhi {

// The compute passes of ReadbackToGuest, made on first use and kept: a pipeline and a binding set per kind, the
// output buffer (grown when a readback needs more) and the parameter buffer. One readback at a time per backend
// (`mutex`): each waits for its submission, so nothing here is in use when the next one starts.
struct ReadbackData {
    struct Kind {
        std::shared_ptr<Pipeline> pipeline;
        std::shared_ptr<BindingSet> set;
    };
    std::mutex mutex;
    Kind kinds[3];  // 0 colour, 1 depth, 2 depth and stencil
    std::shared_ptr<Buffer> output;   // Readback heap, storage
    std::shared_ptr<Buffer> params;   // Upload heap, uniform
};

ReadbackData& ReadbackState(Backend::Impl& backend);

namespace readback {

// The error to return when the calling thread's open recorder has recorded on `image` (or it is an attachment of an
// open pass): the readback sees only what has been submitted.
Result<void> CheckSubmitted(Backend::Impl& backend, Image::Impl& image, const char* who);

// Submits an internal recorder with the waits its first use of an image added (the hand-over's waits, each at its own
// stage and at the compute stage) and waits for it on the CPU. Returns when the GPU has finished it.
void SubmitAndWait(Backend& backend, Recorder&& recorder);

}  // namespace readback
}  // namespace mh4n::gpu::rhi
