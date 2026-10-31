// A translation unit that includes only pipeline_key.h and is linked against nothing but the INTERFACE target
// `mh4n_gpu_pipeline_key` (N2-33): it proves that target is header-only in practice. N2-16's pipeline/ links only that
// target, so everything pipeline_key.h calls (PipelineKey::Hash, operator==, AsFallback, the key's hash) must be inline.
// Built with the tree (a link failure breaks the build) and run as the ctest test `gpu_pipeline_key_link_check`.
// Written for this package: nothing is copied from Azahar here.
#include <cstdio>

#include "gpu/native/pipeline_key.h"

int main() {
    namespace native = mh4n::gpu::native;
    native::PipelineKey a;
    a.vertex.program = 7;
    a.state.depth_test = true;
    a.fragment.key.framebuffer.raw = 193;
    native::PipelineKey b = a;
    const bool equal = a == b && a.Hash() == b.Hash();
    const native::PipelineKey fallback = a.AsFallback();
    b.vertex.program = 8;
    const bool differs = !(a == b) && a.Hash() != b.Hash();
    const bool ok = equal && differs && fallback.IsFallback() && native::kPipelineKeyVersion != 0 &&
                    mh4n::gpu::fragment::HashKey(a.fragment.key) != 0;
    std::printf("pipeline_key link check: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
