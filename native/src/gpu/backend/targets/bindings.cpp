// Image, sampler and texel-buffer bindings, and the per-draw binding arena (N2-27, X5 and X5b).
//
// The vendored backend binds a descriptor with a plain vkUpdateDescriptorSets on a set that no submission in
// flight uses (no dynamic offsets; rhi.md "Binding a buffer at an offset"), and a set object makes its own layout
// and pool (plume_vulkan.cpp:1885-1944). So a draw gets a set from a free list of the arena, one list per set
// shape, and the set goes back to its list once the GPU has passed the value of the submission that used it:
// after the first frames no set is made.
#include <algorithm>

#include <fmt/format.h>

#include "gpu/backend/targets/impl.h"

namespace mh4n::gpu::rhi {

// The texel-buffer views of one arena set (the only sets that carry texel buffers). The vendored backend's own
// view type needs its buffer alive when it is destroyed; a VkBufferView of ours does not.
struct SetExtras {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<VkBufferView> texel_views;  // by slot index
    ~SetExtras() {
        for (VkBufferView view : texel_views) {
            if (view != VK_NULL_HANDLE) vkDestroyBufferView(device, view, nullptr);
        }
    }
};

namespace {

using namespace targets;

int FindSlot(const BindingSet::Impl& set, u32 binding) {
    for (size_t i = 0; i < set.desc.slots.size(); ++i) {
        if (set.desc.slots[i].binding == binding) return static_cast<int>(i);
    }
    return -1;
}

Result<int> CheckSlot(const BindingSet::Impl& set, u32 binding, BindingType type, const char* what) {
    const int slot = FindSlot(set, binding);
    if (slot < 0) return Error{fmt::format("the set has no binding {}", binding)};
    if (set.desc.slots[slot].type != type) return Error{fmt::format("binding {} is not a {}", binding, what)};
    return slot;
}

// What recording a use of `image` by a binding of the calling thread owes (see rhi.h: HandOver). The owner's
// recorder gets the barrier that brings the image to GENERAL with its writes visible; a thread that does not own
// the image only samples it (after a wait on the value that produced it, which is the caller's).
Result<void> PrepareUse(Image::Impl& image) {
    Backend::Impl& backend = *image.backend;
    const std::thread::id thread = std::this_thread::get_id();
    Recorder::Impl* recorder = OpenRecorder(backend, thread);
    if (image.open_pass_in != nullptr && image.open_pass_in == recorder) {
        return Error{"the image is an attachment of the open pass: sample the twin FeedbackCopy returns"};
    }
    bool mine = false;
    {
        std::lock_guard lock(image.owner_mutex);
        mine = image.owner == thread || (image.handover_pending && image.handover_thread == thread);
    }
    if (mine) {
        if (recorder != nullptr) {
            Acquire(*recorder, image);  // takes over a handed-over image and adds its wait to the recorder
            Normalize(*recorder, image);
        }
        return Result<void>{};
    }
#if MH4N_RHI_CHECKS
    if (image.open_pass_in != nullptr) {
        ReportOwnerRuleBreach(OwnerRule::ForeignThread, "a thread sampled an image that is an attachment of an open pass");
    } else if (image.vulkan()->textureLayout != ::plume::RenderTextureLayout::GENERAL) {
        ReportOwnerRuleBreach(OwnerRule::ForeignThread,
                              "a thread sampled an image that its owner has not settled in GENERAL (a pass left open at "
                              "Submit, a depth target): touch it on the owning thread first");
    }
#endif
    return Result<void>{};
}

::plume::VulkanDescriptorSet* Plume(BindingSet::Impl& set) {
    return static_cast<::plume::VulkanDescriptorSet*>(set.set.get());
}

}  // namespace

Result<void> BindingSet::SetSampledImage(u32 binding, const Image& image, ImageView view) {
    Result<int> slot = CheckSlot(*impl_, binding, BindingType::SampledImage, "sampled image");
    if (!slot.ok()) return Error{slot.error()};
    Image::Impl& img = image.impl();
    const VkImageView vk_view = GetView(img, view);
    if (vk_view == VK_NULL_HANDLE) return Error{fmt::format("binding {}: the image has no such view", binding)};
    if (Result<void> use = PrepareUse(img); !use.ok()) return use;
    VkDescriptorImageInfo info = {};
    info.imageView = vk_view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // every image lives in GENERAL outside a pass
    Plume(*impl_)->setDescriptor(static_cast<u32>(*slot), nullptr, &info, nullptr, nullptr);
    return Result<void>{};
}

Result<void> BindingSet::SetStorageImage(u32 binding, const Image& image, ImageView view) {
    Result<int> slot = CheckSlot(*impl_, binding, BindingType::StorageImage, "storage image");
    if (!slot.ok()) return Error{slot.error()};
    Image::Impl& img = image.impl();
    if (view != ImageView::ShadowStorage || !img.HasStorageUsage()) {
        return Error{fmt::format("binding {}: only a shadow source's ShadowStorage view is a storage image", binding)};
    }
    const VkImageView vk_view = GetView(img, view);
    if (vk_view == VK_NULL_HANDLE) return Error{fmt::format("binding {}: the storage view could not be made", binding)};
    if (Result<void> use = PrepareUse(img); !use.ok()) return use;
    VkDescriptorImageInfo info = {};
    info.imageView = vk_view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    Plume(*impl_)->setDescriptor(static_cast<u32>(*slot), nullptr, &info, nullptr, nullptr);
    return Result<void>{};
}

Result<void> BindingSet::SetSampler(u32 binding, const Sampler& sampler) {
    Result<int> slot = CheckSlot(*impl_, binding, BindingType::Sampler, "sampler");
    if (!slot.ok()) return Error{slot.error()};
    Plume(*impl_)->setSampler(static_cast<u32>(*slot), sampler.impl().sampler.get());
    return Result<void>{};
}

Result<void> BindingSet::SetTexelBuffer(u32 binding, const Buffer& buffer, Format format) {
    Result<int> slot = CheckSlot(*impl_, binding, BindingType::TexelBuffer, "texel buffer");
    if (!slot.ok()) return Error{slot.error()};
    if (!HasUsage(buffer.Usage(), BufferUsage::Texel)) {
        return Error{fmt::format("binding {}: the buffer was made without Texel usage", binding)};
    }
    const u32 texel = TexelBytes(format);
    if (texel == 0) return Error{fmt::format("binding {}: {} is not a texel-buffer format", binding, static_cast<int>(format))};
    if (buffer.Size() / texel > impl_->backend->caps.max_texel_buffer_elements) {
        return Error{fmt::format("binding {}: {} elements are above the device's limit {}", binding, buffer.Size() / texel,
                                 impl_->backend->caps.max_texel_buffer_elements)};
    }
    SetExtras* extras = nullptr;
    {
        TargetsData& data = Data(*impl_->backend);
        std::lock_guard lock(data.registry_mutex);
        const auto it = data.set_extras.find(impl_.get());
        if (it != data.set_extras.end()) extras = it->second;
    }
    if (extras == nullptr) {
        return Error{"SetTexelBuffer: a texel-buffer set comes from a BindingArena"};
    }
    VkBufferViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    info.buffer = buffer.impl().vulkan()->vk;
    info.format = ToVkFormat(format);
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;
    VkBufferView view = VK_NULL_HANDLE;
    if (vkCreateBufferView(extras->device, &info, nullptr, &view) != VK_SUCCESS) {
        return Error{fmt::format("binding {}: the texel-buffer view could not be made", binding)};
    }
    if (extras->texel_views.size() <= static_cast<size_t>(*slot)) extras->texel_views.resize(*slot + 1, VK_NULL_HANDLE);
    if (extras->texel_views[*slot] != VK_NULL_HANDLE) {
        // The set is acquired for this recorder (no submission in flight uses it), so the old view is free.
        vkDestroyBufferView(extras->device, extras->texel_views[*slot], nullptr);
    }
    extras->texel_views[*slot] = view;
    Plume(*impl_)->setDescriptor(static_cast<u32>(*slot), nullptr, nullptr, &view, nullptr);
    return Result<void>{};
}

// ---- The arena ------------------------------------------------------------------------------------

namespace {

struct ArenaEntry {
    std::shared_ptr<BindingSet> set;
    std::unique_ptr<SetExtras> extras;
    bool pending = false;   // acquired for a recorder that has not been submitted
    bool stamped = false;   // submitted: `value` says when it is free again
    TimelineValue value = 0;
};

struct ArenaState {
    std::mutex mutex;
    std::shared_ptr<Backend> backend;
    std::map<std::vector<u32>, std::vector<std::unique_ptr<ArenaEntry>>> lists;  // one list per set shape
};

std::vector<u32> KeyOf(const BindingSetDesc& desc) {
    std::vector<u32> key;
    for (const BindingSlot& slot : desc.slots) {
        key.push_back(slot.binding);
        key.push_back(static_cast<u32>(slot.type));
    }
    return key;
}

} // namespace

struct BindingArena::Impl {
    std::shared_ptr<ArenaState> state;
};

BindingArena::BindingArena(std::shared_ptr<Backend> backend) : impl_(std::make_unique<Impl>()) {
    impl_->state = std::make_shared<ArenaState>();
    impl_->state->backend = std::move(backend);
}

BindingArena::~BindingArena() {
    if (impl_ == nullptr || impl_->state == nullptr) return;
    ArenaState& state = *impl_->state;
    TargetsData& data = Data(state.backend->impl());
    std::lock_guard lock(state.mutex);
    std::lock_guard registry(data.registry_mutex);
    for (auto& list : state.lists) {
        for (auto& entry : list.second) {
            data.set_extras.erase(&entry->set->impl());
        }
    }
}

Result<BindingSet*> BindingArena::Acquire(const BindingSetDesc& desc) {
    ArenaState& state = *impl_->state;
    Backend::Impl& backend = state.backend->impl();
    Recorder::Impl* recorder = OpenRecorder(backend, std::this_thread::get_id());
    if (recorder == nullptr) {
        return Error{"BindingArena::Acquire: the calling thread has no open recorder to attach the set to"};
    }
    const TimelineValue completed = backend.Completed();
    ArenaEntry* chosen = nullptr;
    {
        std::lock_guard lock(state.mutex);
        std::vector<std::unique_ptr<ArenaEntry>>& list = state.lists[KeyOf(desc)];
        for (std::unique_ptr<ArenaEntry>& entry : list) {
            if (!entry->pending && (!entry->stamped || entry->value <= completed)) {
                chosen = entry.get();
                break;
            }
        }
        if (chosen == nullptr) {
            Result<std::shared_ptr<BindingSet>> made = core::MakeBindingSet(backend.shared_from_this(), desc, false);
            if (!made.ok()) return Error{made.error()};
            auto entry = std::make_unique<ArenaEntry>();
            entry->set = *made;
            entry->extras = std::make_unique<SetExtras>();
            entry->extras->device = backend.device->vk;
            {
                TargetsData& data = Data(backend);
                std::lock_guard registry(data.registry_mutex);
                data.set_extras[&entry->set->impl()] = entry->extras.get();
            }
            chosen = entry.get();
            list.push_back(std::move(entry));
        }
        chosen->pending = true;
        chosen->stamped = false;
    }
    recorder->on_submit.push_back([weak = std::weak_ptr<ArenaState>(impl_->state), chosen](TimelineValue value) {
        if (std::shared_ptr<ArenaState> state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            chosen->pending = false;
            chosen->stamped = value != 0;  // dropped (0): free at once
            chosen->value = value;
        }
    });
    return chosen->set.get();
}

} // namespace mh4n::gpu::rhi
