// The private state of the core (N2-26): what rhi.h's pimpl classes hold. The other modules of the
// backend (targets/, sync/, readback/, state/) include this header and never edit it; each keeps its
// per-backend state in its own slot of Backend::Impl (a pointer to a type it defines, made on first use
// with its own deleter), so the core links without them. Nothing outside native/src/gpu/backend/ includes it.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/backend/core/policy.h"
#include "gpu/backend/rhi.h"
#include "gpu/backend/vulkan_access.h"
#include "plume_vulkan.h"

#ifndef MH4N_RHI_CHECKS
#error "MH4N_RHI_CHECKS is a public compile definition of mh4n_gpu_backend_core: link that module"
#endif

namespace mh4n::gpu::rhi {

// ---- Instance ------------------------------------------------------------------------------------

struct Instance::Impl {
    ~Impl();
    std::unique_ptr<::plume::RenderInterface> interface; // a ::plume::VulkanInterface
    std::vector<std::string> enabled_extensions;
    std::unique_ptr<DebugMessageHandler> debug_handler; // the messenger's user data
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    ::plume::VulkanInterface* vulkan() const { return static_cast<::plume::VulkanInterface*>(interface.get()); }
};

// ---- The per-backend state of the other modules --------------------------------------------------

// Types the owner module defines (targets/, sync/, readback/, state/); only declared here. A slot is made
// by its owner on first use, with its own deleter, and is destroyed before the device.
struct TargetsData;
struct SyncData;
struct ReadbackData;
struct StateData;
// The same for the per-recorder state of the other modules (Recorder::Impl::targets and ::state).
struct RecorderTargetsData;
struct RecorderStateData;
template <typename T> using Slot = std::unique_ptr<T, void (*)(T*)>;

// ---- Backend -------------------------------------------------------------------------------------

// The owner rule's bookkeeping for one thread.
struct ThreadState {
    Recorder::Impl* open =
        nullptr;           // the thread's open recorder, if any (the last one begun when a breach let two be open)
    u32 open_count = 0;    // recorders begun on the thread and neither submitted nor dropped
    u64 next_sequence = 0; // the sequence number of the next recorder the thread begins
    u64 last_submitted_sequence = 0; // the highest sequence number of a submitted recorder
};

struct PendingFence {
    TimelineValue value = 0;
    std::unique_ptr<::plume::RenderCommandFence> fence;
};
struct InFlightList {
    TimelineValue value = 0;
    std::unique_ptr<::plume::RenderCommandList> list;
};

struct Backend::Impl : std::enable_shared_from_this<Backend::Impl> {
    ~Impl();

    std::shared_ptr<Instance> instance;
    Capabilities caps;
    std::vector<std::string> enabled_extensions;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool present_wait_available = false;
    bool display_timing_available = false;
    bool present_wait_measures_display = false;
    VkPhysicalDeviceLimits limits = {};
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;

    // The device, the one queue and its submit lock (the vendored backend's per-queue mutex).
    std::unique_ptr<::plume::RenderDevice> device_owner;
    ::plume::VulkanDevice* device = nullptr;
    std::unique_ptr<::plume::RenderCommandQueue> queue_owner;
    ::plume::VulkanCommandQueue* queue = nullptr;
    std::mutex* queue_mutex = nullptr;
    u32 queue_family = 0;
    VkQueue vk_queue = VK_NULL_HANDLE;

    // Submission. Values are assigned and the work submitted under `submit_mutex` (taken before the queue
    // mutex, never the other way), so signalled values rise in submission order. With the `timeline` bit
    // the completed value is the timeline semaphore's counter; without it each submission has a fence and
    // `fence_completed` follows them in order.
    std::unique_ptr<::plume::RenderCommandSemaphore> timeline_owner;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::mutex submit_mutex;
    std::atomic<TimelineValue> last_submitted{0};
    std::mutex fence_mutex;
    std::deque<PendingFence> pending_fences;
    std::vector<std::unique_ptr<::plume::RenderCommandFence>> free_fences;
    std::atomic<TimelineValue> fence_completed{0};

    // Command lists: reused once the value they were submitted with has passed.
    std::mutex list_mutex;
    std::vector<std::unique_ptr<::plume::RenderCommandList>> free_lists;
    std::deque<InFlightList> in_flight_lists;

    // The owner rule.
    std::mutex thread_mutex;
    std::unordered_map<std::thread::id, ThreadState> threads;

    // The framebuffers of passes with no attachments, by size (kept for the backend's life).
    std::mutex framebuffer_mutex;
    std::map<std::pair<u32, u32>, std::unique_ptr<::plume::RenderFramebuffer>> framebuffers;

    // The other modules' slots. Declared last: destroyed first.
    Slot<TargetsData> targets{nullptr, nullptr};
    Slot<SyncData> sync{nullptr, nullptr};
    Slot<ReadbackData> readback{nullptr, nullptr};
    Slot<StateData> state{nullptr, nullptr};

    TimelineValue Completed();
    void WaitFor(TimelineValue value);
};

// ---- Recorder ------------------------------------------------------------------------------------

struct Recorder::Impl {
    ~Impl();

    std::shared_ptr<Backend::Impl> backend;
    std::unique_ptr<::plume::RenderCommandList> list; // a ::plume::VulkanCommandList
    std::thread::id thread;
    u64 sequence = 0;      // among the recorders of `thread`, in the order they were begun
    bool counted = false;  // counted in the thread's open_count
    bool internal = false; // made by the backend itself (ReadBuffer): outside the per-thread rules

    // The waits the submission must cover (clause 2 of the owner rule): the targets module appends the
    // wait of a hand-over when the thread first records on or samples the image. Value and stage.
    std::vector<Wait> required_waits;

    // Called with the submission's value when the recorder is submitted, or with 0 when it is dropped.
    // Rings and binding arenas stamp the space and the sets they gave this recorder with it.
    std::vector<std::function<void(TimelineValue)>> on_submit;

    // The other modules' per-recorder state, in the pattern of Backend::Impl's slots: a pointer to a type the
    // owner defines (targets/: the open pass, its attachments, the feedback copies of the recorder; state/:
    // what N2-13 tracks), made by the owner on first use with its own deleter and released where `on_submit`
    // runs: when the recorder is submitted (after the hooks) or dropped. They are destroyed before the
    // command list is returned to the pool.
    Slot<RecorderTargetsData> targets{nullptr, nullptr};
    Slot<RecorderStateData> state{nullptr, nullptr};

    // The pass state.
    bool in_pass = false;                     // a pass is open (BeginPass or the targets module's BeginTargetPass)
    const Pipeline::Impl* pipeline = nullptr; // the pipeline last set

    ::plume::VulkanCommandList* vulkan_list() const { return static_cast<::plume::VulkanCommandList*>(list.get()); }
};

// ---- Resources -----------------------------------------------------------------------------------

struct Buffer::Impl {
    ~Impl();
    std::shared_ptr<Backend::Impl> backend;
    std::unique_ptr<::plume::RenderBuffer> buffer; // a ::plume::VulkanBuffer
    BufferDesc desc;
    u8* mapped = nullptr; // the whole buffer, for the Upload and Readback heaps

    ::plume::VulkanBuffer* vulkan() const { return static_cast<::plume::VulkanBuffer*>(buffer.get()); }
};

struct BindingSet::Impl {
    std::shared_ptr<Backend::Impl> backend;
    std::unique_ptr<::plume::RenderDescriptorSet> set;
    BindingSetDesc desc;
};

struct Pipeline::Impl {
    std::shared_ptr<Backend::Impl> backend;
    std::unique_ptr<::plume::RenderPipelineLayout> layout;
    std::unique_ptr<::plume::RenderPipeline> pipeline; // destroyed before the layout
    bool compute = false;
    std::vector<BindingSetDesc> sets;
};

// ---- Functions the other modules use -------------------------------------------------------------

// The owner-rule checks exist only with MH4N_RHI_CHECKS.
#if MH4N_RHI_CHECKS
inline constexpr bool kRhiChecks = true;
#else
inline constexpr bool kRhiChecks = false;
#endif

namespace core {

// Submits a recorder's command list with `waits` (each keeps its own stage) and returns the value its
// completion signals. The one path every Submit takes, through the submit lock; checks the order and the
// required waits when MH4N_RHI_CHECKS is on. Without the `timeline` bit a wait is not submitted: the
// caller has already waited for its value on the CPU (N2-28).
TimelineValue SubmitRecorder(Backend::Impl& backend, Recorder&& recorder, std::span<const Wait> waits);

// A new recorder on the calling thread (what BeginRecording does).
Recorder MakeRecorder(const std::shared_ptr<Backend::Impl>& backend, bool internal);

// The framebuffer of a pass with no attachments of that size.
::plume::RenderFramebuffer* ZeroAttachmentFramebuffer(Backend::Impl& backend, u32 width, u32 height);

// A binding set of the given shape. `buffers_only` is what Backend::CreateBindingSet passes: a shape with
// images, samplers or texel buffers is refused there and made through here by the targets module.
Result<std::shared_ptr<BindingSet>> MakeBindingSet(const std::shared_ptr<Backend::Impl>& backend,
                                                   const BindingSetDesc& desc, bool buffers_only);

// A pipeline layout of the vendored backend for these set shapes (set index = position).
std::unique_ptr<::plume::RenderPipelineLayout> MakePipelineLayout(Backend::Impl& backend,
                                                                  const std::vector<BindingSetDesc>& sets);

// The stage group of the vendored backend for a Stage.
::plume::RenderBarrierStages ToPlumeStages(Stage stage);

} // namespace core
} // namespace mh4n::gpu::rhi
