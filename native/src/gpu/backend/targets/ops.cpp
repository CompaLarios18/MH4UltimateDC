// The image operations of Recorder (N2-27): copies, clears, blits, buffer<->image copies, passes with
// attachments, the feedback copy and the swapchain image's way to presentation. Every operation ends the open
// pass (the module's, through targets::Settle), takes the images it touches through the owner rule
// (targets::Acquire), moves them with plume's barrier call and puts them back to GENERAL.
#include <algorithm>

#include <fmt/format.h>

#include "gpu/backend/targets/impl.h"
#include "gpu/backend/targets/upload.h"

namespace mh4n::gpu::rhi {

namespace {

using ::plume::RenderTextureLayout;
using namespace targets;

VkImageAspectFlags AspectFor(ImageAspect aspect) {
    switch (aspect) {
    case ImageAspect::Color: return VK_IMAGE_ASPECT_COLOR_BIT;
    case ImageAspect::Depth: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case ImageAspect::Stencil: return VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

bool IsIntegerFormat(Format f) {
    switch (f) {
    case Format::R32Uint: case Format::R8Uint: case Format::R8Sint: case Format::Rg8Uint: case Format::Rg8Sint:
    case Format::Rgba8Uint: case Format::Rgba8Sint: case Format::R16Uint: case Format::R16Sint: case Format::Rg16Sint:
    case Format::Rgba16Sint: return true;
    default: return false;
    }
}

void CheckMip(const Image::Impl& image, u32 mip, u32 layer) {
    RHI_TARGETS_REQUIRE(mip < image.mip_levels && layer < image.layers, "mip {} / layer {} outside the image", mip, layer);
    (void)image; (void)mip; (void)layer;
}

} // namespace

// ---- Copies ---------------------------------------------------------------------------------------

void Recorder::CopyImage(const Image& src, Image& dst, std::span<const ImageCopyRegion> regions) {
    Impl& r = *impl_;
    Image::Impl& s = src.impl();
    Image::Impl& d = dst.impl();
    Settle(r);
    Acquire(r, s);
    if (&s != &d) Acquire(r, d);
    RHI_TARGETS_REQUIRE(s.format == d.format, "CopyImage needs two images of one format");
    const bool same = &s == &d;
    if (same) {
        Transition(r, s, RenderTextureLayout::GENERAL);
    } else {
        Transition(r, s, RenderTextureLayout::COPY_SOURCE);
        Transition(r, d, RenderTextureLayout::COPY_DEST);
    }
    std::vector<VkImageCopy> copies;
    for (const ImageCopyRegion& region : regions) {
        CheckMip(s, region.src_mip, region.src_layer);
        CheckMip(d, region.dst_mip, region.dst_layer);
        VkImageCopy copy = {};
        copy.srcSubresource = {AspectOf(s), region.src_mip, region.src_layer, 1};
        copy.dstSubresource = {AspectOf(d), region.dst_mip, region.dst_layer, 1};
        copy.srcOffset = {static_cast<s32>(region.src_x), static_cast<s32>(region.src_y), 0};
        copy.dstOffset = {static_cast<s32>(region.dst_x), static_cast<s32>(region.dst_y), 0};
        copy.extent = {region.width, region.height, 1};
        copies.push_back(copy);
    }
    if (!copies.empty()) {
        vkCmdCopyImage(r.vulkan_list()->vk, s.vulkan()->vk, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       d.vulkan()->vk, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       static_cast<u32>(copies.size()), copies.data());
    }
    Transition(r, s, RenderTextureLayout::GENERAL);
    if (!same) Transition(r, d, RenderTextureLayout::GENERAL);
}

void Recorder::Blit(const Image& src, Image& dst, std::span<const ImageBlitRegion> regions, bool linear) {
    Impl& r = *impl_;
    Image::Impl& s = src.impl();
    Image::Impl& d = dst.impl();
    Settle(r);
    Acquire(r, s);
    if (&s != &d) Acquire(r, d);
    const bool same = &s == &d;
    RHI_TARGETS_REQUIRE(IsDepth(s) == IsDepth(d), "Blit between a colour and a depth image");
    if (same) {
        Transition(r, s, RenderTextureLayout::GENERAL);
    } else {
        Transition(r, s, RenderTextureLayout::COPY_SOURCE);
        Transition(r, d, RenderTextureLayout::COPY_DEST);
    }
    std::vector<VkImageBlit> blits;
    for (const ImageBlitRegion& region : regions) {
        CheckMip(s, region.src_mip, region.layer);
        CheckMip(d, region.dst_mip, region.layer);
        VkImageBlit blit = {};
        blit.srcSubresource = {AspectOf(s), region.src_mip, region.layer, 1};
        blit.dstSubresource = {AspectOf(d), region.dst_mip, region.layer, 1};
        blit.srcOffsets[0] = {region.src.x, region.src.y, 0};
        blit.srcOffsets[1] = {region.src.x + static_cast<s32>(region.src.width), region.src.y + static_cast<s32>(region.src.height), 1};
        blit.dstOffsets[0] = {region.dst.x, region.dst.y, 0};
        blit.dstOffsets[1] = {region.dst.x + static_cast<s32>(region.dst.width), region.dst.y + static_cast<s32>(region.dst.height), 1};
        blits.push_back(blit);
    }
    if (!blits.empty()) {
        // A depth-stencil blit is nearest only.
        const VkFilter filter = (linear && !IsDepth(s)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        vkCmdBlitImage(r.vulkan_list()->vk, s.vulkan()->vk, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       d.vulkan()->vk, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       static_cast<u32>(blits.size()), blits.data(), filter);
    }
    Transition(r, s, RenderTextureLayout::GENERAL);
    if (!same) Transition(r, d, RenderTextureLayout::GENERAL);
}

void Recorder::ClearImage(Image& image, const ClearValue& value, u32 mip_level, u32 layer) {
    Impl& r = *impl_;
    Image::Impl& img = image.impl();
    Settle(r);
    Acquire(r, img);
    CheckMip(img, mip_level, layer);
    Transition(r, img, RenderTextureLayout::COPY_DEST);
    VkImageSubresourceRange range = {AspectOf(img), mip_level, 1, layer, 1};
    VkCommandBuffer commands = r.vulkan_list()->vk;
    if (IsDepth(img)) {
        VkClearDepthStencilValue clear = {value.depth, value.stencil};
        vkCmdClearDepthStencilImage(commands, img.vulkan()->vk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    } else {
        VkClearColorValue clear = {};
        if (IsIntegerFormat(img.format)) {
            for (int i = 0; i < 4; ++i) clear.uint32[i] = static_cast<u32>(value.color[i]);
        } else {
            for (int i = 0; i < 4; ++i) clear.float32[i] = value.color[i];
        }
        vkCmdClearColorImage(commands, img.vulkan()->vk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    Transition(r, img, RenderTextureLayout::GENERAL);
}

void Recorder::CopyBufferToImage(const Buffer& src, Image& dst, std::span<const BufferImageRegion> regions) {
    Impl& r = *impl_;
    Image::Impl& img = dst.impl();
    Settle(r);
    Acquire(r, img);
    RHI_TARGETS_REQUIRE(HasUsage(src.Usage(), BufferUsage::Transfer), "CopyBufferToImage needs a buffer with Transfer usage");
    Transition(r, img, RenderTextureLayout::COPY_DEST);
    std::vector<VkBufferImageCopy> copies;
    for (const BufferImageRegion& region : regions) {
        CheckMip(img, region.mip_level, region.layer);
        VkBufferImageCopy copy = {};
        copy.bufferOffset = region.buffer_offset;  // rows and slices tightly packed
        copy.imageSubresource = {AspectFor(region.aspect), region.mip_level, region.layer, 1};
        copy.imageOffset = {region.image_rect.x, region.image_rect.y, 0};
        copy.imageExtent = {region.image_rect.width, region.image_rect.height, 1};
        copies.push_back(copy);
    }
    if (!copies.empty()) {
        vkCmdCopyBufferToImage(r.vulkan_list()->vk, src.impl().vulkan()->vk, img.vulkan()->vk,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(copies.size()), copies.data());
    }
    Transition(r, img, RenderTextureLayout::GENERAL);
}

void Recorder::CopyImageToBuffer(const Image& src, Buffer& dst, std::span<const BufferImageRegion> regions) {
    Impl& r = *impl_;
    Image::Impl& img = src.impl();
    Settle(r);
    Acquire(r, img);
    RHI_TARGETS_REQUIRE(HasUsage(dst.Usage(), BufferUsage::Transfer) && dst.GetHeap() != Heap::Upload,
                        "CopyImageToBuffer needs a Transfer buffer that is not in the Upload heap");
    Transition(r, img, RenderTextureLayout::COPY_SOURCE);
    std::vector<VkBufferImageCopy> copies;
    for (const BufferImageRegion& region : regions) {
        CheckMip(img, region.mip_level, region.layer);
        VkBufferImageCopy copy = {};
        copy.bufferOffset = region.buffer_offset;
        copy.imageSubresource = {AspectFor(region.aspect), region.mip_level, region.layer, 1};
        copy.imageOffset = {region.image_rect.x, region.image_rect.y, 0};
        copy.imageExtent = {region.image_rect.width, region.image_rect.height, 1};
        copies.push_back(copy);
    }
    VkCommandBuffer commands = r.vulkan_list()->vk;
    if (!copies.empty()) {
        // The buffer's memory may be a reused one (a destroyed readback buffer's): what earlier submissions did to
        // it is ordered before this copy's writes (core's ReadBuffer does the same for its staging buffer).
        VkMemoryBarrier reuse = {};
        reuse.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        reuse.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        reuse.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reuse, 0,
                             nullptr, 0, nullptr);
        vkCmdCopyImageToBuffer(commands, img.vulkan()->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.impl().vulkan()->vk,
                               static_cast<u32>(copies.size()), copies.data());
        // The copy's writes are visible to the host once the submission has completed (as ReadBuffer does it).
        VkMemoryBarrier host = {};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                             nullptr);
    }
    Transition(r, img, RenderTextureLayout::GENERAL);
}

// ---- Passes ---------------------------------------------------------------------------------------

namespace {

void ClearAttachments(Recorder::Impl& r, const TargetPass& pass, const TargetPassDesc& desc) {
    const ::plume::RenderRect rect(pass.area.x, pass.area.y, pass.area.x + static_cast<s32>(pass.area.width),
                                   pass.area.y + static_cast<s32>(pass.area.height));
    if (pass.color != nullptr && desc.color_load == LoadOp::Clear) {
        ::plume::RenderColor color;
        for (int i = 0; i < 4; ++i) color.rgba[i] = desc.clear_color[i];
        r.vulkan_list()->clearColor(0, color, &rect, 1);
    }
    if (pass.depth != nullptr && desc.depth_load == LoadOp::Clear) {
        r.vulkan_list()->clearDepthStencil(true, HasStencil(*pass.depth), desc.clear_depth, desc.clear_stencil, &rect, 1);
    }
}

}  // namespace

void Recorder::BeginTargetPass(const TargetPassDesc& desc) {
    Impl& r = *impl_;
    RecorderTargetsData& data = Data(r);
    Image::Impl* color = desc.color != nullptr ? &desc.color->impl() : nullptr;
    Image::Impl* depth = desc.depth != nullptr ? &desc.depth->impl() : nullptr;
    RHI_TARGETS_REQUIRE(color != nullptr || depth != nullptr, "BeginTargetPass needs an attachment (BeginPass is the pass with none)");
    RHI_TARGETS_REQUIRE(color == nullptr || (!IsDepth(*color) && (color->plume_flags & ::plume::RenderTextureFlag::RENDER_TARGET)),
                        "the colour attachment is not a colour target");
    RHI_TARGETS_REQUIRE(depth == nullptr || IsDepth(*depth), "the depth attachment is not a depth image");
    RHI_TARGETS_REQUIRE((color == nullptr || (color->mip_levels == 1 && color->layers == 1)) &&
                            (depth == nullptr || (depth->mip_levels == 1 && depth->layers == 1)),
                        "an attachment has one level and one layer (a framebuffer view has a single level)");
    const Image::Impl& first = color != nullptr ? *color : *depth;
    TargetPass pass;
    pass.color = color;
    pass.depth = depth;
    pass.area = desc.area;
    if (pass.area.width == 0 || pass.area.height == 0) {
        pass.area = Rect{0, 0, first.width, first.height};
    }
    RHI_TARGETS_REQUIRE(pass.area.x >= 0 && pass.area.y >= 0 && pass.area.x + pass.area.width <= first.width &&
                            pass.area.y + pass.area.height <= first.height,
                        "the render area is outside the attachments");
    RHI_TARGETS_REQUIRE(color == nullptr || depth == nullptr || (color->width == depth->width && color->height == depth->height),
                        "the attachments differ in size");

    // The same attachments and area: the pass stays open and only the new clears are done (a pass break costs a
    // tile round trip on a tile-based GPU).
    if (data.pass_open && r.in_pass && data.pass.color == color && data.pass.depth == depth &&
        data.pass.area.x == pass.area.x && data.pass.area.y == pass.area.y && data.pass.area.width == pass.area.width &&
        data.pass.area.height == pass.area.height) {
        ClearAttachments(r, pass, desc);
        return;
    }
    Settle(r);
    if (color != nullptr) Acquire(r, *color);
    if (depth != nullptr) Acquire(r, *depth);
    pass.framebuffer = GetFramebuffer(*r.backend, color, depth);
    MH4N_ASSERT(pass.framebuffer != nullptr, "the framebuffer of a pass could not be made");
    // The attachments' layouts for the pass: colour stays in GENERAL (patch 04), depth is in plume's attachment layout.
    if (color != nullptr) Transition(r, *color, RenderTextureLayout::GENERAL);
    if (depth != nullptr) Transition(r, *depth, RenderTextureLayout::DEPTH_WRITE);
    r.vulkan_list()->setFramebuffer(pass.framebuffer);
    const ::plume::RenderViewport viewport(static_cast<float>(pass.area.x), static_cast<float>(pass.area.y),
                                           static_cast<float>(pass.area.width), static_cast<float>(pass.area.height));
    const ::plume::RenderRect scissor(pass.area.x, pass.area.y, pass.area.x + static_cast<s32>(pass.area.width),
                                      pass.area.y + static_cast<s32>(pass.area.height));
    r.vulkan_list()->setViewports(&viewport, 1);
    r.vulkan_list()->setScissors(&scissor, 1);
    r.in_pass = true;
    data.pass_open = true;
    data.pass = pass;
    if (color != nullptr) { color->open_pass_in = &r; color->dirty = true; }
    if (depth != nullptr) { depth->open_pass_in = &r; depth->dirty = true; }
    ClearAttachments(r, pass, desc);
}

// ---- The feedback copy ----------------------------------------------------------------------------

Image& Recorder::FeedbackCopy(Image& target) {
    Impl& r = *impl_;
    RecorderTargetsData& data = Data(r);
    Image::Impl& t = target.impl();
    // Remember the pass to resume: the next draw starts a new one with load.
    const bool resume = data.pass_open && (data.pass.color == &t || data.pass.depth == &t);
    const TargetPass resumed = data.pass;
    Settle(r);
    Acquire(r, t);
    if (t.feedback_twin == nullptr) {
        ImageDesc twin;
        twin.format = t.format;
        twin.width = t.width;
        twin.height = t.height;
        twin.mip_levels = t.mip_levels;
        twin.layers = t.layers;
        twin.cube = t.cube;
        twin.shadow_source = t.shadow_source;  // the twin inherits shadow_source
        twin.name = t.requested.name + " (feedback)";
        Result<std::shared_ptr<Image>> made = MakeImage(r.backend, twin);
        MH4N_ASSERT(made.ok(), "the feedback twin could not be made: {}", made.error());
        t.feedback_twin = *made;
    }
    Image::Impl& w = t.feedback_twin->impl();
    Acquire(r, w);
    Transition(r, t, RenderTextureLayout::COPY_SOURCE);
    Transition(r, w, RenderTextureLayout::COPY_DEST);
    std::vector<VkImageCopy> copies;
    for (u32 mip = 0; mip < t.mip_levels; ++mip) {  // every mip level of the whole image
        VkImageCopy copy = {};
        copy.srcSubresource = {AspectOf(t), mip, 0, t.layers};
        copy.dstSubresource = {AspectOf(w), mip, 0, w.layers};
        copy.extent = {std::max(t.width >> mip, 1u), std::max(t.height >> mip, 1u), 1};
        copies.push_back(copy);
    }
    vkCmdCopyImage(r.vulkan_list()->vk, t.vulkan()->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, w.vulkan()->vk,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(copies.size()), copies.data());
    Transition(r, t, RenderTextureLayout::GENERAL);
    Transition(r, w, RenderTextureLayout::GENERAL);
    if (resume) {
        // Back into the pass the copy interrupted: same attachments, loaded (never cleared again).
        if (resumed.depth != nullptr) Transition(r, *resumed.depth, RenderTextureLayout::DEPTH_WRITE);
        r.vulkan_list()->setFramebuffer(resumed.framebuffer);
        r.in_pass = true;
        data.pass_open = true;
        data.pass = resumed;
        if (resumed.color != nullptr) { resumed.color->open_pass_in = &r; resumed.color->dirty = true; }
        if (resumed.depth != nullptr) { resumed.depth->open_pass_in = &r; resumed.depth->dirty = true; }
    }
    return *t.feedback_twin;
}

// ---- Presentation ---------------------------------------------------------------------------------

void Recorder::PrepareForPresent(Image& image) {
    Impl& r = *impl_;
    Image::Impl& img = image.impl();
    RHI_TARGETS_REQUIRE(img.wrapped, "PrepareForPresent is for a wrapped swapchain image only");
    Settle(r);
    Acquire(r, img);
    Transition(r, img, RenderTextureLayout::PRESENT);
}

}  // namespace mh4n::gpu::rhi
