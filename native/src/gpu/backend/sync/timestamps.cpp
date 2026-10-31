// GPU timestamps per frame (N2-28): Recorder::MarkFrame writes the two marks of a frame and
// Backend::FrameGpuTimeNs reports their difference.
//
// A frame owns a query pool of two queries (plume's queryResults reads a whole pool without waiting and fails,
// printing a message and keeping nothing, when any query of it is unavailable: one pool per frame keeps every read
// on a pool whose two queries have been written). Begin resets the pair and writes query 0 at the bottom of the
// pipe; End writes query 1; a frame is read only once the submission that carried its End has been passed by the
// GPU (Backend::Impl::Completed()). plume converts the ticks to nanoseconds with the device's timestamp period.
#include <fmt/format.h>

#include "gpu/backend/sync/impl.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

namespace {

constexpr size_t kSparePools = 8;

bool TimestampsUsable(Backend::Impl& backend, SyncData& data) {
    if (!data.timestamps_checked) {
        data.timestamps_checked = true;
        u32 count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(backend.physical_device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(backend.physical_device, &count, families.data());
        const bool bits = backend.queue_family < count && families[backend.queue_family].timestampValidBits > 0;
        data.timestamps_usable = bits && backend.caps.timestamp_period > 0.0f;
        if (!data.timestamps_usable) {
            MH4N_LOG(Warning, "RHI", "rhi: the queue family has no timestamps: FrameGpuTimeNs() will be 0");
        }
    }
    return data.timestamps_usable;
}

FrameQuery* Find(SyncData& data, u64 id) {
    for (FrameQuery& frame : data.frames) {
        if (frame.id == id) {
            return &frame;
        }
    }
    return nullptr;
}

}  // namespace

namespace sync {

void HarvestFrames(Backend::Impl& backend, SyncData& data) {
    const TimelineValue completed = backend.Completed();
    for (auto it = data.frames.begin(); it != data.frames.end();) {
        FrameQuery& frame = *it;
        bool done = false;
        if (frame.dropped) {
            done = true;  // its reset and its write never ran
        } else if (frame.end_marked && frame.end_value != 0 && frame.end_value <= completed) {
            // Both queries have been written: the End's submission has passed, and the Begin was recorded before it.
            frame.pool->queryResults();
            const u64* ticks = frame.pool->getResults();
            data.last_begin_ns = ticks[0];
            data.last_end_ns = ticks[1];
            done = true;
        } else if (frame.abandoned && frame.begin_value != 0 && frame.begin_value <= completed) {
            done = true;  // never read: its pool is free once the Begin has run
        }
        if (done) {
            if (data.spare_pools.size() < kSparePools) {
                data.spare_pools.push_back(std::move(frame.pool));
            }
            it = data.frames.erase(it);
        } else {
            ++it;
        }
    }
}

FrameTimestamps LastFrameTimestamps(Backend& backend) {
    SyncData& data = SyncState(backend.impl());
    std::lock_guard lock(data.frame_mutex);
    if (TimestampsUsable(backend.impl(), data)) {
        HarvestFrames(backend.impl(), data);
    }
    return FrameTimestamps{data.last_begin_ns, data.last_end_ns};
}

size_t FramesInFlight(Backend& backend) {
    SyncData& data = SyncState(backend.impl());
    std::lock_guard lock(data.frame_mutex);
    if (TimestampsUsable(backend.impl(), data)) {
        HarvestFrames(backend.impl(), data);
    }
    return data.frames.size();
}

}  // namespace sync

void Recorder::MarkFrame(FrameMark mark) {
    Recorder::Impl& r = *impl_;
    Backend::Impl* backend = r.backend.get();
    SyncData& data = SyncState(*backend);
    if (mark == FrameMark::Begin) {
        sync::DrainRetired(*backend);  // a frame boundary is a point where the GPU has usually passed something
    }
    std::lock_guard lock(data.frame_mutex);
    if (!TimestampsUsable(*backend, data)) {
        return;
    }
    sync::HarvestFrames(*backend, data);

    if (mark == FrameMark::Begin) {
        if (r.in_pass) {
            // vkCmdResetQueryPool is not allowed inside a render pass.
            MH4N_LOG(Error, "RHI", "rhi: MarkFrame(Begin) inside a pass is ignored: mark the frame before its first pass");
            return;
        }
        if (FrameQuery* open = Find(data, data.current_frame)) {
            open->abandoned = true;  // a Begin with no End: that frame is not read
        }
        FrameQuery frame;
        frame.id = data.next_frame_id++;
        if (!data.spare_pools.empty()) {
            frame.pool = std::move(data.spare_pools.back());
            data.spare_pools.pop_back();
        } else {
            frame.pool = backend->device_owner->createQueryPool(2);
        }
        if (frame.pool == nullptr || static_cast<::plume::VulkanQueryPool*>(frame.pool.get())->vk == VK_NULL_HANDLE) {
            MH4N_LOG(Error, "RHI", "rhi: MarkFrame: the query pool could not be made");
            return;
        }
        r.list->resetQueryPool(frame.pool.get(), 0, 2);
        r.list->writeTimestamp(frame.pool.get(), 0);
        const u64 id = frame.id;
        data.current_frame = id;
        data.frames.push_back(std::move(frame));
        r.on_submit.push_back([backend, id](TimelineValue value) {
            SyncData& d = SyncState(*backend);
            std::lock_guard guard(d.frame_mutex);
            FrameQuery* f = Find(d, id);
            if (f == nullptr) {
                return;
            }
            if (value == 0) {
                f->dropped = true;  // the recorder was dropped: neither the reset nor the write will run
                if (d.current_frame == id) {
                    d.current_frame = 0;
                }
            } else {
                f->begin_value = value;
            }
        });
        return;
    }

    FrameQuery* frame = Find(data, data.current_frame);
    if (frame == nullptr) {
        MH4N_LOG(Warning, "RHI", "rhi: MarkFrame(End) with no frame begun: ignored");
        return;
    }
    r.list->writeTimestamp(frame->pool.get(), 1);
    frame->end_marked = true;
    const u64 id = frame->id;
    data.current_frame = 0;
    r.on_submit.push_back([backend, id](TimelineValue value) {
        SyncData& d = SyncState(*backend);
        std::lock_guard guard(d.frame_mutex);
        FrameQuery* f = Find(d, id);
        if (f == nullptr) {
            return;
        }
        if (value == 0) {
            f->end_marked = false;  // the recorder was dropped: this frame will never have its End
            f->abandoned = true;
        } else {
            f->end_value = value;
        }
    });
}

u64 Backend::FrameGpuTimeNs() const {
    Backend::Impl& backend = *impl_;
    SyncData& data = SyncState(backend);
    std::lock_guard lock(data.frame_mutex);
    if (!TimestampsUsable(backend, data)) {
        return 0;
    }
    sync::HarvestFrames(backend, data);
    return data.last_end_ns > data.last_begin_ns ? data.last_end_ns - data.last_begin_ns : 0;
}

}  // namespace mh4n::gpu::rhi
