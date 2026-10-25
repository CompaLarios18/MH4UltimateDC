// The GPU wrapper `queue` and the capture player's option `--queue` (N2-22).
//
// The player applies a registered wrapper whose flag is on its command line, around the Gpu `--gpu`
// built with the player's host (docs/native/capture-player.md). The player judges guest memory, the
// interrupts it collected and the registers right after each call, so the wrapper it gets runs in
// lockstep: the renderer thread makes every call to the wrapped Gpu, and each call returns when it
// is done. The queueing, the frame limit and InvalidateRegion's wait are what the runtime uses
// (QueueGpu with the default QueueConfig, N2b) and what gpu_queue_test checks.
#include <memory>

#include "gpu/queue/queue_gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

namespace mh4n::gpu::queue {

MH4N_OPTION("capture-player", "--queue", "",
            "run the Gpu on the renderer thread of the queue wrapper (every call completes before "
            "the player looks at its result)");

MH4N_REGISTER(GpuWrapper, "queue", [](std::unique_ptr<Gpu> inner, Host&, const Options&) {
    QueueConfig config;
    config.lockstep = true;
    return std::unique_ptr<Gpu>(std::make_unique<QueueGpu>(std::move(inner), config));
});

} // namespace mh4n::gpu::queue
