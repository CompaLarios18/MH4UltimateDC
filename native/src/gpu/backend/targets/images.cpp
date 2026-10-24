// Images, views, samplers, framebuffers, the Vulkan access (NativeImage, WrapExternalImage) and HandOver (N2-27).
#include <algorithm>
#include <bit>

#include <fmt/format.h>

#include "gpu/backend/targets/impl.h"

namespace mh4n::gpu::rhi {

// ---- The slots the core reserves ------------------------------------------------------------------

namespace {
std::mutex& SlotMutex() {
    static std::mutex mutex;
    return mutex;
}
} // namespace

TargetsData& Data(Backend::Impl& backend) {
    std::lock_guard lock(SlotMutex());
    if (backend.targets == nullptr) {
        backend.targets = Slot<TargetsData>(new TargetsData, [](TargetsData* p) { delete p; });
    }
    return *backend.targets;
}

RecorderTargetsData& Data(Recorder::Impl& recorder) {
    if (recorder.targets == nullptr) {
        recorder.targets = Slot<RecorderTargetsData>(new RecorderTargetsData, [](RecorderTargetsData* p) { delete p; });
    }
    return *recorder.targets;
}

namespace targets {

bool IsDepth(const Image::Impl& image) {
    return core::IsDepthFormat(image.format);
}
bool HasStencil(const Image::Impl& image) {
    return image.format == Format::D24S8 || image.format == Format::D32S8;
}
VkImageAspectFlags AspectOf(const Image::Impl& image) {
    if (!IsDepth(image)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(image) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

Recorder::Impl* OpenRecorder(Backend::Impl& backend, std::thread::id thread) {
    std::lock_guard lock(backend.thread_mutex);
    const auto it = backend.threads.find(thread);
    return it == backend.threads.end() ? nullptr : it->second.open;
}

// ---- Views ----------------------------------------------------------------------------------------

VkImageView GetView(Image::Impl& image, ImageView view) {
    const u32 index = static_cast<u32>(view);
    if (index >= 4) return VK_NULL_HANDLE;
    std::lock_guard lock(image.view_mutex);
    if (image.views[index] != VK_NULL_HANDLE) {
        return image.views[index];
    }
    const bool depth = IsDepth(image);
    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image.vulkan()->vk;
    info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY};
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = image.mip_levels;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = image.layers;
    info.viewType = image.cube ? VK_IMAGE_VIEW_TYPE_CUBE
                               : (image.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    info.format = image.vk_format;
    switch (view) {
    case ImageView::Sampled:
        // A depth-stencil image is sampled one aspect at a time: Sampled is its depth.
        info.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    case ImageView::Depth:
        if (!depth) return VK_NULL_HANDLE;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    case ImageView::Stencil:
        if (!HasStencil(image)) return VK_NULL_HANDLE;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    case ImageView::ShadowStorage:
        if (!image.shadow_source) return VK_NULL_HANDLE;
        info.format = VK_FORMAT_R32_UINT;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        break;
    }
    VkImageView created = VK_NULL_HANDLE;
    if (vkCreateImageView(image.backend->device->vk, &info, nullptr, &created) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    image.views[index] = created;
    return created;
}

// ---- Framebuffers ---------------------------------------------------------------------------------

::plume::RenderFramebuffer* GetFramebuffer(Backend::Impl& backend, Image::Impl* color, Image::Impl* depth) {
    TargetsData& data = Data(backend);
    const std::pair<u64, u64> key{color != nullptr ? color->id : 0, depth != nullptr ? depth->id : 0};
    std::lock_guard lock(data.framebuffer_mutex);
    auto it = data.framebuffers.find(key);
    if (it != data.framebuffers.end()) {
        return it->second.get();
    }
    const ::plume::RenderTexture* color_texture = color != nullptr ? color->texture.get() : nullptr;
    ::plume::RenderFramebufferDesc desc;
    if (color != nullptr) {
        desc.colorAttachments = &color_texture;
        desc.colorAttachmentsCount = 1;
    }
    desc.depthAttachment = depth != nullptr ? depth->texture.get() : nullptr;
    // Patch 04: a colour attachment stays in GENERAL inside the pass too.
    desc.colorAttachmentsGeneralLayout = true;
    std::unique_ptr<::plume::RenderFramebuffer> framebuffer = backend.device_owner->createFramebuffer(desc);
    if (framebuffer == nullptr || static_cast<::plume::VulkanFramebuffer*>(framebuffer.get())->vk == VK_NULL_HANDLE) {
        return nullptr;
    }
    return data.framebuffers.emplace(key, std::move(framebuffer)).first->second.get();
}

void EraseFramebuffers(Backend::Impl& backend, u64 image_id) {
    if (backend.targets == nullptr) return;
    TargetsData& data = *backend.targets;
    std::lock_guard lock(data.framebuffer_mutex);
    for (auto it = data.framebuffers.begin(); it != data.framebuffers.end();) {
        if (it->first.first == image_id || it->first.second == image_id) {
            it = data.framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- Images ---------------------------------------------------------------------------------------

Result<std::shared_ptr<Image>> MakeImage(const std::shared_ptr<Backend::Impl>& backend_ptr, const ImageDesc& desc) {
    Backend::Impl& backend = *backend_ptr;
    const std::string where = fmt::format("image \"{}\": ", desc.name);
    if (desc.format == Format::Undefined) return Error{where + "no format"};
    if (desc.width == 0 || desc.height == 0) return Error{where + "an image needs a size"};
    if (desc.layers == 0 || desc.mip_levels == 0) return Error{where + "an image needs a level and a layer"};
    if (desc.cube && desc.layers != 6) return Error{where + "a cube has 6 layers"};
    if (!desc.cube && desc.layers != 1 && desc.layers > 6) return Error{where + "1 or 6 layers"};
    const u32 max_dim = std::max(desc.width, desc.height);
    if (desc.mip_levels > static_cast<u32>(std::bit_width(max_dim))) {
        return Error{where + fmt::format("{} levels do not fit a {}x{} image", desc.mip_levels, desc.width, desc.height)};
    }

    Backend tmp(backend_ptr);  // shares the impl; a view of the backend for ChooseFormat
    const bool depth_request = core::IsDepthFormat(desc.format);
    const bool compressed = desc.format == Format::Bc1Unorm || desc.format == Format::Bc3Unorm ||
                            desc.format == Format::Bc5Unorm || desc.format == Format::Bc7Unorm;
    const ChosenFormat chosen = tmp.ChooseFormat(
        desc.format, depth_request ? FormatUse::DepthTarget : (compressed ? FormatUse::Sampled : FormatUse::ColorTarget));
    const Format stored = chosen.format;
    const FormatSupport support = tmp.QueryFormat(stored);
    if (!support.sampled) return Error{where + "the device cannot sample the format it would be stored in"};
    const bool depth = core::IsDepthFormat(stored);
    const bool target = (depth || !compressed) && support.attachment;
    if (depth && !support.attachment) return Error{where + "the device cannot draw to this depth format"};
    if (desc.shadow_source) {
        if (depth || compressed || TexelBytes(stored) != 4 || stored == Format::Rgba8Snorm) {
            return Error{where + "a shadow source is a 32-bit unsigned-normalised colour image (Rgba8)"};
        }
        if (!tmp.QueryFormat(Format::R32Uint).storage) return Error{where + "the device cannot store to R32_UINT"};
    }

    ::plume::RenderTextureDesc plume_desc = ::plume::RenderTextureDesc::Texture2D(
        desc.width, desc.height, desc.mip_levels, core::ToPlumeFormat(stored));
    plume_desc.arraySize = desc.layers;
    plume_desc.textureArrangement = ::plume::RenderTextureArrangement::UNKNOWN;
    if (depth) plume_desc.flags |= ::plume::RenderTextureFlag::DEPTH_TARGET;
    else if (target) plume_desc.flags |= ::plume::RenderTextureFlag::RENDER_TARGET;
    if (desc.cube) plume_desc.flags |= ::plume::RenderTextureFlag::CUBE;
    if (desc.shadow_source) {
        // Storage usage only on the images that become shadow sources (metal.md 3.2). The mutable-format flag is
        // what lets the R32_UINT view of an RGBA8 image exist; plume has no format list (VkImageFormatListCreateInfo).
        plume_desc.flags |= ::plume::RenderTextureFlag::STORAGE | ::plume::RenderTextureFlag::MUTABLE_FORMAT;
    }

    auto impl = std::make_unique<Image::Impl>();
    impl->backend = backend_ptr;
    impl->id = Data(backend).next_image_id.fetch_add(1);
    impl->requested = desc;
    impl->format = stored;
    impl->width = desc.width;
    impl->height = desc.height;
    impl->mip_levels = desc.mip_levels;
    impl->layers = desc.layers;
    impl->cube = desc.cube;
    impl->shadow_source = desc.shadow_source;
    impl->plume_flags = plume_desc.flags;
    impl->vk_format = ToVkFormat(stored);
    impl->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0) |
                  ((plume_desc.flags & ::plume::RenderTextureFlag::RENDER_TARGET) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
                  ((plume_desc.flags & ::plume::RenderTextureFlag::STORAGE) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    impl->owner = std::this_thread::get_id();
    impl->texture = backend.device_owner->createTexture(plume_desc);
    if (impl->texture == nullptr || impl->vulkan()->vk == VK_NULL_HANDLE || impl->vulkan()->imageView == VK_NULL_HANDLE) {
        return Error{where + fmt::format("the device could not make a {}x{} image of format {}", desc.width, desc.height,
                                         static_cast<int>(stored))};
    }
    if (!desc.name.empty()) impl->texture->setName(desc.name);
    return std::make_shared<Image>(std::move(impl));
}

} // namespace targets

Image::Impl::~Impl() {
    if (backend == nullptr) return;
    targets::EraseFramebuffers(*backend, id);
    for (VkImageView view : views) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(backend->device->vk, view, nullptr);
    }
}

Image::Image(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Image::~Image() = default;
Format Image::GetFormat() const { return impl_->format; }
u32 Image::Width() const { return impl_->width; }
u32 Image::Height() const { return impl_->height; }
u32 Image::MipLevels() const { return impl_->mip_levels; }
u32 Image::Layers() const { return impl_->layers; }

Sampler::Sampler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Sampler::~Sampler() = default;

Result<std::shared_ptr<Image>> Backend::CreateImage(const ImageDesc& desc) {
    return targets::MakeImage(impl_, desc);
}

// ---- Samplers -------------------------------------------------------------------------------------

namespace {

::plume::RenderFilter ToFilter(Filter f) {
    return f == Filter::Nearest ? ::plume::RenderFilter::NEAREST : ::plume::RenderFilter::LINEAR;
}

::plume::RenderTextureAddressMode ToAddress(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat: return ::plume::RenderTextureAddressMode::WRAP;
    case AddressMode::MirroredRepeat: return ::plume::RenderTextureAddressMode::MIRROR;
    case AddressMode::ClampToEdge: return ::plume::RenderTextureAddressMode::CLAMP;
    case AddressMode::ClampToBorder: return ::plume::RenderTextureAddressMode::BORDER;
    case AddressMode::MirrorClampToEdge: return ::plume::RenderTextureAddressMode::MIRROR_ONCE;
    }
    return ::plume::RenderTextureAddressMode::CLAMP;
}

} // namespace

Result<std::shared_ptr<Sampler>> Backend::CreateSampler(const SamplerDesc& desc) {
    if (desc.custom_border_color && !impl_->caps.custom_border_color) {
        // The core's bit is the device query ANDed with what plume enabled (patch 12): check it before asking
        // plume for a CUSTOM sampler, which would only print to stderr and return a null handle.
        return Error{"CreateSampler: a custom border colour needs Capabilities::custom_border_color (absent, or "
                     "masked, on this device): emulate the colour in the shader and use ClampToEdge"};
    }
    ::plume::RenderSamplerDesc plume_desc;
    plume_desc.minFilter = ToFilter(desc.min_filter);
    plume_desc.magFilter = ToFilter(desc.mag_filter);
    plume_desc.mipmapMode = desc.mip_filter == Filter::Nearest ? ::plume::RenderMipmapMode::NEAREST
                                                                : ::plume::RenderMipmapMode::LINEAR;
    plume_desc.addressU = ToAddress(desc.address_u);
    plume_desc.addressV = ToAddress(desc.address_v);
    plume_desc.addressW = ToAddress(desc.address_w);
    plume_desc.mipLODBias = desc.mip_lod_bias;
    plume_desc.anisotropyEnabled = desc.max_anisotropy > 1.0f;
    plume_desc.maxAnisotropy = static_cast<uint32_t>(std::max(desc.max_anisotropy, 1.0f));
    plume_desc.comparisonEnabled = false;  // never a compare sampler: shadow lookups compare in the shader (5.3)
    plume_desc.minLOD = desc.min_lod;
    plume_desc.maxLOD = desc.max_lod;
    plume_desc.borderColor = ::plume::RenderBorderColor::TRANSPARENT_BLACK;
    if (desc.custom_border_color) {
        plume_desc.borderColor = ::plume::RenderBorderColor::CUSTOM;
        std::copy(desc.custom_border_color->begin(), desc.custom_border_color->end(), plume_desc.customBorderColor);
    }
    auto impl = std::make_unique<Sampler::Impl>();
    impl->backend = impl_;
    impl->sampler = impl_->device_owner->createSampler(plume_desc);
    if (impl->sampler == nullptr || impl->vulkan()->vk == VK_NULL_HANDLE) {
        return Error{"CreateSampler: the device refused the sampler"};
    }
    return std::make_shared<Sampler>(std::move(impl));
}

// ---- Vulkan access --------------------------------------------------------------------------------

VkImage NativeImage(const Image& image) {
    return image.impl().vulkan()->vk;
}

Result<std::shared_ptr<Image>> WrapExternalImage(Backend& backend, VkImage image, Format format, u32 width, u32 height) {
    if (image == VK_NULL_HANDLE) return Error{"WrapExternalImage: no image"};
    const VkFormat vk_format = targets::ToVkFormat(format);
    if (vk_format == VK_FORMAT_UNDEFINED) return Error{"WrapExternalImage: the format has no device format"};
    if (width == 0 || height == 0) return Error{"WrapExternalImage: an image needs a size"};
    std::shared_ptr<Backend::Impl> backend_ptr = backend.impl().shared_from_this();
    const bool depth = core::IsDepthFormat(format);

    auto impl = std::make_unique<Image::Impl>();
    impl->backend = backend_ptr;
    impl->id = Data(*backend_ptr).next_image_id.fetch_add(1);
    impl->requested.format = format;
    impl->requested.width = width;
    impl->requested.height = height;
    impl->requested.name = "external";
    impl->format = format;
    impl->width = width;
    impl->height = height;
    impl->wrapped = true;
    impl->vk_format = vk_format;
    impl->owner = std::this_thread::get_id();
    // Plume's public constructor for a VkImage it does not own: ownership false (never destroyed), the held layout
    // UNKNOWN (so the first operation moves it to GENERAL without keeping contents). Its description is empty, and
    // plume reads it for barriers, copies and the framebuffer: fill it as the image's.
    auto texture = std::make_unique<::plume::VulkanTexture>(backend_ptr->device, image);
    texture->desc = ::plume::RenderTextureDesc::Texture2D(width, height, 1, core::ToPlumeFormat(format),
                                                           depth ? ::plume::RenderTextureFlag::DEPTH_TARGET
                                                                 : ::plume::RenderTextureFlag::RENDER_TARGET);
    texture->imageFormat = vk_format;
    texture->ownership = false;
    texture->fillSubresourceRange();
    texture->createImageView(vk_format);  // the view a framebuffer over it needs
    impl->plume_flags = texture->desc.flags;
    impl->texture = std::move(texture);
    if (impl->vulkan()->imageView == VK_NULL_HANDLE) {
        return Error{"WrapExternalImage: the device could not make a view of the image"};
    }
    return std::make_shared<Image>(std::move(impl));
}

// ---- Hand-over ------------------------------------------------------------------------------------

void HandOver(Image& image, std::thread::id thread, Wait wait) {
    Image::Impl& impl = image.impl();
    std::lock_guard lock(impl.owner_mutex);
#if MH4N_RHI_CHECKS
    if (impl.owner != std::this_thread::get_id() && !impl.handover_pending) {
        ReportOwnerRuleBreach(OwnerRule::ForeignThread, "HandOver of an image by a thread that does not own it");
        return;
    }
    if (impl.open_pass_in != nullptr) {
        ReportOwnerRuleBreach(OwnerRule::HandOver, "HandOver of an image that is an attachment of an open pass");
        return;
    }
    if (targets::IsDepth(impl)) {
        ReportOwnerRuleBreach(OwnerRule::HandOver, "HandOver of a depth-stencil target (its attachment layout is not "
                                                   "settled across threads)");
        return;
    }
#endif
    impl.handover_pending = true;
    impl.handover_thread = thread;
    impl.handover_wait = wait;
}

} // namespace mh4n::gpu::rhi
