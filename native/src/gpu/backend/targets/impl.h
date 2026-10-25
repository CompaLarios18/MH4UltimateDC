// The private state of the targets module (N2-27): images, samplers, the per-backend and per-recorder slots
// the core reserves (core/impl.h), and the functions the module's files share. Nothing outside
// native/src/gpu/backend/targets/ and the module's test includes it.
//
// The layout policy (rhi-spike.md 9.4 item 5; docs/native/rhi.md "Targets, layouts and bindings"):
//   * every image is in GENERAL outside a pass; a colour attachment is in GENERAL inside one too (patch 04,
//     every framebuffer made here sets colorAttachmentsGeneralLayout); a depth-stencil attachment is in
//     plume's depth-attachment layout during a pass and goes back to GENERAL when the module ends the pass;
//   * a transfer moves the image GENERAL -> transfer layout -> GENERAL, always through plume's barrier call
//     (VulkanCommandList::barriers) so that VulkanTexture::textureLayout, which plume reads while recording
//     copies, stays true;
//   * only the owning thread records on an image (the owner rule), checked with MH4N_RHI_CHECKS.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/backend/core/impl.h"
#include "gpu/backend/core/to_plume.h"
#include "gpu/backend/targets/formats.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"

// A precondition of a recording call (the image is in the right state, the format fits): checked with the
// backend's other checks, MH4N_RHI_CHECKS.
#if MH4N_RHI_CHECKS
#define RHI_TARGETS_REQUIRE(cond, ...) MH4N_ASSERT(cond, __VA_ARGS__)
#else
#define RHI_TARGETS_REQUIRE(cond, ...)                                                                                 \
    do {                                                                                                               \
    } while (0)
#endif

namespace mh4n::gpu::rhi {

struct Image::Impl {
    ~Impl();

    std::shared_ptr<Backend::Impl> backend;
    u64 id = 0;                    // unique in the backend: keys the framebuffer cache
    ImageDesc requested;           // as the caller asked
    Format format = Format::Undefined;  // what the device stores
    u32 width = 0, height = 0, mip_levels = 1, layers = 1;
    bool cube = false;
    bool shadow_source = false;
    bool wrapped = false;          // over a VkImage the backend does not own (never destroyed here)

    // The vendored backend's texture: owned (created by the device) or wrapped (VulkanTexture(device, VkImage)).
    std::unique_ptr<::plume::RenderTexture> texture;
    ::plume::VulkanTexture* vulkan() const { return static_cast<::plume::VulkanTexture*>(texture.get()); }
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    // What the module recorded when it made the image (an image made without shadow_source has no storage usage).
    u32 plume_flags = 0;           // ::plume::RenderTextureFlag bits
    VkImageUsageFlags usage = 0;   // the Vulkan usage of an owned image (plume's: sampled, both transfers, + the flags)
    bool HasStorageUsage() const { return (plume_flags & ::plume::RenderTextureFlag::STORAGE) != 0; }

    // Views, made on first use (indexed by ImageView).
    std::mutex view_mutex;
    VkImageView views[4] = {};

    // The owner rule. `owner` is the thread that records on the image; after HandOver `pending` is set until
    // the receiving thread first records on it.
    std::mutex owner_mutex;
    std::thread::id owner;
    bool handover_pending = false;
    std::thread::id handover_thread;
    Wait handover_wait;

    // Record-time state, touched only by the owning thread.
    bool dirty = false;                       // the GPU wrote it since the last visibility barrier
    Recorder::Impl* open_pass_in = nullptr;   // the recorder whose open pass has this image as an attachment

    std::shared_ptr<Image> feedback_twin;     // made once by FeedbackCopy and kept
};

struct Sampler::Impl {
    std::shared_ptr<Backend::Impl> backend;
    std::unique_ptr<::plume::RenderSampler> sampler;
    ::plume::VulkanSampler* vulkan() const { return static_cast<::plume::VulkanSampler*>(sampler.get()); }
};

// The open pass of a recorder.
struct TargetPass {
    Image::Impl* color = nullptr;
    Image::Impl* depth = nullptr;
    Rect area;
    ::plume::RenderFramebuffer* framebuffer = nullptr;
};

// One image's state at the first touch in a recorder: restored when the recorder is dropped without a submission.
struct TouchedImage {
    Image::Impl* image = nullptr;
    ::plume::RenderTextureLayout layout = ::plume::RenderTextureLayout::UNKNOWN;
    ::plume::RenderBarrierStages stages = 0;
    bool dirty = false;
};

struct RecorderTargetsData {
    bool hook_registered = false;
    bool pass_open = false;
    TargetPass pass;
    std::vector<TouchedImage> touched;
    std::vector<std::shared_ptr<Buffer>> staging;  // kept until the recorder's submission has passed
};

struct SetExtras;

struct TargetsData {
    std::atomic<u64> next_image_id{1};

    std::mutex framebuffer_mutex;
    std::map<std::pair<u64, u64>, std::unique_ptr<::plume::RenderFramebuffer>> framebuffers;  // (colour id, depth id)

    std::mutex staging_mutex;
    std::vector<std::pair<TimelineValue, std::shared_ptr<Buffer>>> staging_pending;
    std::atomic<u64> staging_uploads{0};

    std::mutex registry_mutex;
    std::unordered_map<const BindingSet::Impl*, SetExtras*> set_extras;
};

TargetsData& Data(Backend::Impl& backend);
RecorderTargetsData& Data(Recorder::Impl& recorder);

namespace targets {

// Makes an image (what Backend::CreateImage does).
Result<std::shared_ptr<Image>> MakeImage(const std::shared_ptr<Backend::Impl>& backend, const ImageDesc& desc);
// The view of an image, made on first use; VK_NULL_HANDLE when the view cannot be made for this image.
VkImageView GetView(Image::Impl& image, ImageView view);
// The framebuffer of an attachment pair, made on first use and kept until either image dies.
::plume::RenderFramebuffer* GetFramebuffer(Backend::Impl& backend, Image::Impl* color, Image::Impl* depth);
void EraseFramebuffers(Backend::Impl& backend, u64 image_id);

bool IsDepth(const Image::Impl& image);
bool HasStencil(const Image::Impl& image);
VkImageAspectFlags AspectOf(const Image::Impl& image);  // every aspect the format has

// ---- The layout policy and the owner rule (layouts.cpp) ----

// Records a barrier through plume's call that moves `image` to `layout` with a full dependency (all stages and
// accesses), whatever its layout was: the same layout gives a memory barrier. A presented image is reinterpreted
// as UNDEFINED first (its contents are not kept after the next acquire).
void Transition(Recorder::Impl& recorder, Image::Impl& image, ::plume::RenderTextureLayout layout);
// The owner rule's check for a recording call on `image` by `recorder`'s thread: takes the image over when it was
// handed over to this thread (appending the hand-over's wait to the recorder's required waits), calls the
// handler when the thread does not own the image. Also registers the image with the recorder (so a dropped
// recorder can restore its state).
void Acquire(Recorder::Impl& recorder, Image::Impl& image);
// Ends the open pass of the recorder (if any) and records what a finished pass owes: the colour attachment's
// visibility barrier, the depth attachment's move back to GENERAL.
void Settle(Recorder::Impl& recorder);
// Brings an image to GENERAL with its writes visible (a no-op when it already is and nothing wrote it).
void Normalize(Recorder::Impl& recorder, Image::Impl& image);

// The thread's open recorder (null when there is none).
Recorder::Impl* OpenRecorder(Backend::Impl& backend, std::thread::id thread);

} // namespace targets
} // namespace mh4n::gpu::rhi
