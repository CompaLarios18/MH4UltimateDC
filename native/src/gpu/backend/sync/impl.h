// The private state of the sync module (N2-28): the per-backend slot core/impl.h reserves (Backend::Impl::sync).
// Nothing outside native/src/gpu/backend/sync/ and the module's test includes it.
#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "gpu/backend/core/impl.h"
#include "gpu/backend/sync/sync.h"

namespace mh4n::gpu::rhi {

// An object kept alive until the GPU has passed `value`.
struct Retired {
    TimelineValue value = 0;
    std::shared_ptr<void> object;
};

// One frame's two timestamps: a query pool of two queries (plume reads a whole pool, and a read of a pool with an
// unavailable query fails, so a frame never shares its pool).
struct FrameQuery {
    u64 id = 0;
    std::unique_ptr<::plume::RenderQueryPool> pool;
    TimelineValue begin_value = 0;  // the submission that carried the Begin mark; 0 until it is submitted
    TimelineValue end_value = 0;    // the submission that carried the End mark; 0 until it is submitted
    bool end_marked = false;        // MarkFrame(End) was recorded
    bool abandoned = false;         // a newer Begin came before the End, or the End's recorder was dropped
    bool dropped = false;           // the Begin's recorder was dropped: the reset and the write never ran
};

struct SyncData {
    // ---- deferred destruction ----
    std::mutex retire_mutex;
    std::vector<Retired> retired;

    // ---- the pipeline cache ----
    std::mutex cache_mutex;  // Load against Save (plume's load replaces the VkPipelineCache)

    // ---- timestamps ----
    std::mutex frame_mutex;
    bool timestamps_checked = false;
    bool timestamps_usable = false;
    std::vector<FrameQuery> frames;
    std::vector<std::unique_ptr<::plume::RenderQueryPool>> spare_pools;  // pools of read frames, reset before reuse
    u64 next_frame_id = 1;
    u64 current_frame = 0;  // the id of the frame whose Begin was marked and whose End was not; 0 = none
    u64 last_begin_ns = 0;
    u64 last_end_ns = 0;
};

SyncData& SyncState(Backend::Impl& backend);

namespace sync {

// Reads the frames whose End has passed and recycles the pools of the finished ones (frame_mutex held by the caller).
void HarvestFrames(Backend::Impl& backend, SyncData& data);
// Releases the retired objects whose value has passed.
void DrainRetired(Backend::Impl& backend);

}  // namespace sync
}  // namespace mh4n::gpu::rhi
