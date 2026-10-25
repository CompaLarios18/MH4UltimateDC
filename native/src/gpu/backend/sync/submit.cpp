// Submit with waits and Retire (N2-28).
//
// Waits (rhi-spike.md 9.4 item 7, R2). Each Wait keeps its own stage; core::SubmitRecorder turns the list into one
// vkQueueSubmit with one stage mask per wait semaphore (patch 03) on the backend's timeline (patch 05). Without the
// `timeline` bit that function submits no wait: this file first waits on the CPU for the highest value asked,
// through core's own fence fallback (Backend::Impl::WaitFor and Completed: a fence per submission, the completed
// value advanced in order), and builds no second mechanism. Read for the design (never copied): Azahar's
// vk_master_semaphore.cpp (GPL-2.0-or-later, "Copyright 2020 yuzu Emulator Project"), whose form without timeline
// semaphores is fences plus a waiter thread; core's variant has no waiter thread.
//
// Deferred destruction. core has no hook that runs when a value completes, so the list is ours (SyncData::retired)
// and is drained at every Retire, every Submit with waits, every MarkFrame(Begin) and by DrainRetired.
#include <algorithm>

#include <fmt/format.h>

#include "gpu/backend/sync/impl.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

namespace {

std::mutex& SlotMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

SyncData& SyncState(Backend::Impl& backend) {
    std::lock_guard lock(SlotMutex());
    if (backend.sync == nullptr) {
        backend.sync = Slot<SyncData>(new SyncData, [](SyncData* p) { delete p; });
    }
    return *backend.sync;
}

namespace sync {

void DrainRetired(Backend::Impl& backend) {
    SyncData& data = SyncState(backend);
    const TimelineValue completed = backend.Completed();
    std::vector<Retired> due;
    {
        std::lock_guard lock(data.retire_mutex);
        const auto split = std::stable_partition(data.retired.begin(), data.retired.end(),
                                                 [&](const Retired& r) { return r.value > completed; });
        due.assign(std::make_move_iterator(split), std::make_move_iterator(data.retired.end()));
        data.retired.erase(split, data.retired.end());
    }
    // Released here, outside the lock: a destructor may take other locks of the backend.
    due.clear();
}

size_t RetiredCount(Backend& backend) {
    SyncData& data = SyncState(backend.impl());
    std::lock_guard lock(data.retire_mutex);
    return data.retired.size();
}

void DrainRetired(Backend& backend) {
    DrainRetired(backend.impl());
}

void ReleaseRetired(Backend& backend) {
    backend.WaitIdle();
    DrainRetired(backend.impl());
}

}  // namespace sync

TimelineValue Backend::Submit(Recorder&& recorder, std::span<const Wait> waits) {
    Backend::Impl& backend = *impl_;
    // A wait for a value no submission has issued would block the queue for ever (or the CPU, without timeline
    // semaphores): refused with a message and left out; a required wait it was meant to cover is then a breach.
    const TimelineValue issued = backend.last_submitted.load();
    std::vector<Wait> kept;
    kept.reserve(waits.size());
    TimelineValue highest = 0;
    for (const Wait& wait : waits) {
        if (wait.value > issued) {
            MH4N_LOG(Error, "RHI", "rhi: Submit: a wait for value {} was left out, only {} has been issued", wait.value,
                     issued);
            continue;
        }
        kept.push_back(wait);
        highest = std::max(highest, wait.value);
    }
    if (backend.timeline == VK_NULL_HANDLE && highest > 0) {
        // The CPU wait of the fence fallback: values complete in order, so the highest covers them all. It runs
        // before the submit lock is taken, so other threads keep submitting while this one waits.
        backend.WaitFor(highest);
    }
    const TimelineValue value = core::SubmitRecorder(backend, std::move(recorder), kept);
    sync::DrainRetired(backend);
    return value;
}

void Backend::Retire(std::shared_ptr<void> object, TimelineValue last_use) {
    if (object == nullptr) {
        return;
    }
    Backend::Impl& backend = *impl_;
    if (last_use <= backend.Completed()) {
        object.reset();  // the GPU has passed it already: released here
        return;
    }
    {
        SyncData& data = SyncState(backend);
        std::lock_guard lock(data.retire_mutex);
        data.retired.push_back(Retired{last_use, std::move(object)});
    }
    sync::DrainRetired(backend);
}

}  // namespace mh4n::gpu::rhi
