// Submission, the owner-rule checks and ReadBuffer (N2-26).
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/core/impl.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

// ---- The owner-rule handler ----------------------------------------------------------------------

namespace {

struct HandlerSlot {
    std::mutex mutex;
    OwnerRuleHandler handler;
};

HandlerSlot& TheHandler() {
    static HandlerSlot slot;
    return slot;
}

const char* RuleName(OwnerRule rule) {
    switch (rule) {
    case OwnerRule::OneOpenRecorder: return "one open recorder per thread";
    case OwnerRule::SubmissionOrder: return "recorders are submitted in the order they were begun";
    case OwnerRule::RequiredWait: return "a hand-over is covered by a wait";
    case OwnerRule::ForeignThread: return "an image is recorded on by its owning thread";
    case OwnerRule::HandOver: return "a hand-over of an image that can be handed over";
    case OwnerRule::WrongThread: return "a call runs on its own thread";
    }
    return "?";
}

} // namespace

OwnerRuleHandler SetOwnerRuleHandler(OwnerRuleHandler handler) {
    HandlerSlot& slot = TheHandler();
    std::lock_guard lock(slot.mutex);
    std::swap(slot.handler, handler);
    return handler;
}

void ReportOwnerRuleBreach(OwnerRule rule, std::string message) {
    OwnerRuleHandler handler;
    {
        HandlerSlot& slot = TheHandler();
        std::lock_guard lock(slot.mutex);
        handler = slot.handler;
    }
    if (handler) {
        OwnerRuleBreach breach;
        breach.rule = rule;
        breach.message = std::move(message);
        handler(breach);
        return;
    }
    MH4N_LOG(Critical, "RHI", "owner rule breached ({}): {}", RuleName(rule), message);
    std::abort();
}

// ---- Completed value and waiting -----------------------------------------------------------------

TimelineValue Backend::Impl::Completed() {
    if (timeline != VK_NULL_HANDLE) {
        u64 value = 0;
        if (vkGetSemaphoreCounterValue(device->vk, timeline, &value) != VK_SUCCESS) {
            return fence_completed.load();
        }
        return value;
    }
    std::lock_guard lock(fence_mutex);
    while (!pending_fences.empty()) {
        PendingFence& front = pending_fences.front();
        if (vkGetFenceStatus(device->vk, static_cast<::plume::VulkanCommandFence*>(front.fence.get())->vk) !=
            VK_SUCCESS) {
            break;
        }
        fence_completed = front.value;
        vkResetFences(device->vk, 1, &static_cast<::plume::VulkanCommandFence*>(front.fence.get())->vk);
        free_fences.push_back(std::move(front.fence));
        pending_fences.pop_front();
    }
    return fence_completed.load();
}

void Backend::Impl::WaitFor(TimelineValue value) {
    if (value == 0) {
        return;
    }
    if (value > last_submitted.load()) {
        MH4N_LOG(Error, "RHI", "rhi: waiting for value {}, but only {} has been issued", value, last_submitted.load());
        return;
    }
    if (timeline != VK_NULL_HANDLE) {
        VkSemaphoreWaitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores = &timeline;
        info.pValues = &value;
        const VkResult result = vkWaitSemaphores(device->vk, &info, UINT64_MAX);
        if (result != VK_SUCCESS) {
            MH4N_LOG(Error, "RHI", "rhi: vkWaitSemaphores failed with {}", static_cast<int>(result));
        }
        return;
    }
    // Fences, in submission order. The wait runs in short slices under the fence mutex so that no
    // other thread resets a fence being waited for.
    while (Completed() < value) {
        std::lock_guard lock(fence_mutex);
        if (pending_fences.empty()) {
            continue;
        }
        VkFence fence = static_cast<::plume::VulkanCommandFence*>(pending_fences.front().fence.get())->vk;
        vkWaitForFences(device->vk, 1, &fence, VK_TRUE, 2'000'000);
    }
}

TimelineValue Backend::CompletedValue() const {
    return impl_->Completed();
}
TimelineValue Backend::LastSubmittedValue() const {
    return impl_->last_submitted.load();
}
void Backend::WaitIdle(TimelineValue value) {
    impl_->WaitFor(value);
}
void Backend::WaitIdle() {
    impl_->WaitFor(impl_->last_submitted.load());
}

// ---- Recorders -----------------------------------------------------------------------------------

namespace core {

Recorder MakeRecorder(const std::shared_ptr<Backend::Impl>& backend, bool internal) {
    auto impl = std::make_unique<Recorder::Impl>();
    impl->backend = backend;
    impl->thread = std::this_thread::get_id();
    impl->internal = internal;

    const TimelineValue completed = backend->Completed();
    {
        std::lock_guard lock(backend->list_mutex);
        while (!backend->in_flight_lists.empty() && backend->in_flight_lists.front().value <= completed) {
            backend->free_lists.push_back(std::move(backend->in_flight_lists.front().list));
            backend->in_flight_lists.pop_front();
        }
        if (!backend->free_lists.empty()) {
            impl->list = std::move(backend->free_lists.back());
            backend->free_lists.pop_back();
        }
    }
    if (impl->list == nullptr) {
        impl->list = backend->queue_owner->createCommandList();
        MH4N_ASSERT(impl->list != nullptr && impl->vulkan_list()->vk != VK_NULL_HANDLE,
                    "the command list could not be made");
    }
    impl->vulkan_list()->begin();

    bool second_open = false;
    if (!internal) {
        std::lock_guard lock(backend->thread_mutex);
        ThreadState& state = backend->threads[impl->thread];
        second_open = state.open_count > 0;
        ++state.open_count;
        impl->sequence = ++state.next_sequence;
        state.open = impl.get();
        impl->counted = true;
    }
#if MH4N_RHI_CHECKS
    if (second_open) {
        ReportOwnerRuleBreach(OwnerRule::OneOpenRecorder,
                              "a thread began a recorder while it had another one open: submit or drop the first");
    }
#else
    (void)second_open;
#endif
    return Recorder(std::move(impl));
}

TimelineValue SubmitRecorder(Backend::Impl& backend, Recorder&& recorder, std::span<const Wait> waits) {
    Recorder::Impl& r = recorder.impl();
    MH4N_ASSERT(r.list != nullptr, "a recorder that was already submitted (or moved from) was submitted");

    // The owner rule: the order of the thread's recorders, and the waits the recorder requires.
    bool out_of_order = false;
    if (r.counted) {
        std::lock_guard lock(backend.thread_mutex);
        ThreadState& state = backend.threads[r.thread];
        out_of_order = r.sequence < state.last_submitted_sequence;
        state.last_submitted_sequence = std::max(state.last_submitted_sequence, r.sequence);
        if (state.open_count > 0) {
            --state.open_count;
        }
        if (state.open == &r) {
            state.open = nullptr;
        }
        r.counted = false;
    }
#if MH4N_RHI_CHECKS
    if (out_of_order) {
        ReportOwnerRuleBreach(OwnerRule::SubmissionOrder,
                              fmt::format("recorder {} of a thread was submitted after a later one", r.sequence));
    }
    for (const Wait& required : r.required_waits) {
        const bool covered = std::any_of(waits.begin(), waits.end(), [&](const Wait& wait) {
            return wait.stage == required.stage && wait.value >= required.value;
        });
        if (!covered) {
            ReportOwnerRuleBreach(OwnerRule::RequiredWait,
                                  fmt::format("a submission lacks the wait for value {} at stage {} that covers an "
                                              "image handed over to its thread",
                                              required.value, static_cast<int>(required.stage)));
        }
    }
#else
    (void)out_of_order;
#endif

    r.vulkan_list()->end();
    const ::plume::RenderCommandList* list = r.list.get();

    TimelineValue value = 0;
    {
        std::lock_guard submit_lock(backend.submit_mutex);
        value = ++backend.last_submitted;
        std::vector<::plume::RenderCommandSemaphore*> wait_semaphores;
        std::vector<::plume::RenderBarrierStages> wait_stages;
        std::vector<uint64_t> wait_values;
        if (backend.timeline != VK_NULL_HANDLE) {
            for (const Wait& wait : waits) {
                wait_semaphores.push_back(backend.timeline_owner.get());
                wait_stages.push_back(core::ToPlumeStages(wait.stage));
                wait_values.push_back(wait.value);
            }
            ::plume::RenderCommandSemaphore* signal = backend.timeline_owner.get();
            backend.queue->executeCommandListsEx(&list, 1, wait_semaphores.data(),
                                                 static_cast<uint32_t>(wait_semaphores.size()), wait_stages.data(),
                                                 wait_values.data(), &signal, 1, &value, nullptr);
        } else {
            std::unique_ptr<::plume::RenderCommandFence> fence;
            {
                std::lock_guard lock(backend.fence_mutex);
                if (!backend.free_fences.empty()) {
                    fence = std::move(backend.free_fences.back());
                    backend.free_fences.pop_back();
                }
            }
            if (fence == nullptr) {
                fence = backend.device_owner->createCommandFence();
            }
            ::plume::RenderCommandFence* raw = fence.get();
            {
                std::lock_guard lock(backend.fence_mutex);
                backend.pending_fences.push_back(PendingFence{value, std::move(fence)});
            }
            backend.queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0, raw);
        }
    }

    {
        std::lock_guard lock(backend.list_mutex);
        backend.in_flight_lists.push_back(InFlightList{value, std::move(r.list)});
    }
    std::vector<std::function<void(TimelineValue)>> hooks = std::move(r.on_submit);
    r.on_submit.clear();
    for (auto& hook : hooks) {
        hook(value);
    }
    r.targets.reset(); // the per-recorder state of the other modules ends with the recorder's use
    r.state.reset();
    return value;
}

} // namespace core

Recorder Backend::BeginRecording() {
    return core::MakeRecorder(impl_, false);
}

TimelineValue Backend::Submit(Recorder&& recorder) {
    return core::SubmitRecorder(*impl_, std::move(recorder), {});
}

// ---- ReadBuffer ----------------------------------------------------------------------------------

Result<void> Backend::ReadBuffer(const Buffer& buffer, u64 offset, std::span<u8> out, TimelineValue after) {
    if (offset + out.size() > buffer.Size()) {
        return Error{fmt::format("ReadBuffer: [{}, {}) is outside the buffer of {} bytes", offset, offset + out.size(),
                                 buffer.Size())};
    }
    if (after > impl_->last_submitted.load()) {
        return Error{fmt::format("ReadBuffer: it was asked to wait for value {}, but only {} has been issued", after,
                                 impl_->last_submitted.load())};
    }
    if (out.empty()) {
        return Result<void>{};
    }
    impl_->WaitFor(after);

    if (buffer.GetHeap() != Heap::Device) {
        buffer.Invalidate(offset, out.size());
        std::memcpy(out.data(), buffer.Mapped().data() + offset, out.size());
        return Result<void>{};
    }

    BufferDesc staging_desc;
    staging_desc.size = out.size();
    staging_desc.heap = Heap::Readback;
    staging_desc.usage = BufferUsage::Transfer;
    staging_desc.name = "ReadBuffer staging";
    Result<std::shared_ptr<Buffer>> staging = CreateBuffer(staging_desc);
    if (!staging.ok()) {
        return Error{staging.error()};
    }
    if (!HasUsage(buffer.Usage(), BufferUsage::Transfer)) {
        return Error{"ReadBuffer: a buffer in the Device heap needs Transfer usage"};
    }

    Recorder recorder = core::MakeRecorder(impl_, true);
    VkCommandBuffer commands = recorder.impl().vulkan_list()->vk;
    // What earlier submissions wrote is visible to the copy ...
    VkMemoryBarrier before = {};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; // the staging memory may be reused
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0,
                         nullptr, 0, nullptr);
    recorder.CopyBuffer(**staging, 0, buffer, offset, out.size());
    // ... and the copy's writes are visible to the host once the submission has completed.
    VkMemoryBarrier after_copy = {};
    after_copy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after_copy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after_copy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after_copy, 0,
                         nullptr, 0, nullptr);
    const TimelineValue value = core::SubmitRecorder(*impl_, std::move(recorder), {});
    impl_->WaitFor(value);
    (*staging)->Invalidate(0, out.size());
    std::memcpy(out.data(), (*staging)->Mapped().data(), out.size());
    return Result<void>{};
}

} // namespace mh4n::gpu::rhi
