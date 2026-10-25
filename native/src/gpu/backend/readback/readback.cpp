// Derived from Azahar src/video_core/renderer_vulkan/vk_blit_helper.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later, as the file says: "Licensed under GPLv2 or any later version")
// Copyright 2022-2026 Citra Emulator Project / Azahar Emulator Project
//
// ReadbackToGuest (N2-28, rhi-spike.md 9.4 item 15, R5). The pattern of BlitHelper::DepthToBuffer
// (vk_blit_helper.cpp:513-580): the depth and stencil views of the image are bound to a compute pass that writes a
// storage buffer, the pass is dispatched over the region and the buffer is read on the CPU. Read besides: Azahar's
// vulkan_depth_to_buffer.comp, rasterizer_cache/texture_codec.h (MortonCopy, the row direction) and common/color.h
// (the byte order of the PICA formats), which readback.comp (derived from the first) cites; texture_decode.cpp (the
// tiled layouts, which the framebuffer shares). What is ours: one pass for every format (a word of the output
// finds its pixel), the compatibility table, the first-use path through the targets module and the CPU wait.
//
// The framebuffer layout in guest memory is the texture layout of texture_codec.h: the PICA writes its colour and depth
// buffers with the same 8x8 Morton tiles as it reads textures (Azahar's rasterizer cache uses one MortonCopy for
// both), so there is a single layout to implement here.
#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/readback/impl.h"
#include "gpu/backend/sync/sync.h"
#include "mh4n/log.h"

#include "readback_shaders.inc"

namespace mh4n::gpu::rhi {

namespace {

std::mutex& SlotMutex() {
    static std::mutex mutex;
    return mutex;
}

u32 BytesPerPixel(PicaFormat format) {
    switch (format) {
    case PicaFormat::Rgba8: return 4;
    case PicaFormat::Rgb8: return 3;
    case PicaFormat::Rgb5A1:
    case PicaFormat::Rgb565:
    case PicaFormat::Rgba4:
    case PicaFormat::D16: return 2;
    case PicaFormat::D24: return 3;
    case PicaFormat::D24S8: return 4;
    }
    return 0;
}

// Bits of r, g, b, a a PICA colour format keeps.
std::array<u32, 4> PicaBits(PicaFormat format) {
    switch (format) {
    case PicaFormat::Rgba8: return {8, 8, 8, 8};
    case PicaFormat::Rgb8: return {8, 8, 8, 0};
    case PicaFormat::Rgb5A1: return {5, 5, 5, 1};
    case PicaFormat::Rgb565: return {5, 6, 5, 0};
    case PicaFormat::Rgba4: return {4, 4, 4, 4};
    default: return {0, 0, 0, 0};
    }
}

// What the image stores, for the pass: its kind (0 colour, 1 depth, 2 depth and stencil) and the bits of its components.
struct Source {
    u32 kind = 0;
    std::array<u32, 4> bits = {0, 0, 0, 0};
};

// The images each PICA format can be read from. A colour format needs every component it keeps in at least that many
// bits (an RGBA8 image fills any of them by cutting from the top, as Azahar's Convert8To5 and kin do; a 16-bit image only
// its own format); a depth format needs a depth image of enough bits. Anything else is an error, not a conversion.
Result<Source> Classify(const Image::Impl& image, PicaFormat format) {
    Source source;
    switch (image.format) {
    case Format::Rgba8Unorm:
    case Format::Bgra8Unorm: source.bits = {8, 8, 8, 8}; break;
    case Format::R5G6B5:
    case Format::B5G6R5: source.bits = {5, 6, 5, 0}; break;
    case Format::R5G5B5A1:
    case Format::B5G5R5A1:
    case Format::A1R5G5B5: source.bits = {5, 5, 5, 1}; break;
    case Format::R4G4B4A4:
    case Format::B4G4R4A4: source.bits = {4, 4, 4, 4}; break;
    case Format::D16Unorm:
        if (format != PicaFormat::D16) break;
        source.kind = 1;
        return source;
    case Format::D32Float:
        if (format != PicaFormat::D24) break;
        source.kind = 1;
        return source;
    case Format::D24S8:
    case Format::D32S8:
        if (format != PicaFormat::D24 && format != PicaFormat::D24S8) break;
        source.kind = format == PicaFormat::D24S8 ? 2 : 1;
        return source;
    default: break;
    }
    if (source.bits[0] != 0 && format <= PicaFormat::Rgba4) {
        const std::array<u32, 4> want = PicaBits(format);
        bool fits = true;
        for (int c = 0; c < 4; ++c) {
            fits = fits && (want[c] == 0 || source.bits[c] >= want[c]);
        }
        if (fits) return source;
    }
    return Error{fmt::format("ReadbackToGuest: a PICA format {} cannot be read from an image stored as format {}",
                             static_cast<int>(format), static_cast<int>(image.format))};
}

struct Params {
    s32 rect[4];
    u32 info[4];
    u32 src_bits[4];
};

BindingSetDesc SetShape(u32 kind) {
    BindingSetDesc desc;
    if (kind == 2) {
        desc.slots = {{0, BindingType::SampledImage}, {1, BindingType::SampledImage}, {2, BindingType::StorageBuffer},
                      {3, BindingType::UniformBuffer}};
    } else {
        desc.slots = {{0, BindingType::SampledImage}, {1, BindingType::StorageBuffer}, {2, BindingType::UniformBuffer}};
    }
    return desc;
}

Result<void> EnsureKind(Backend& backend, ReadbackData& data, u32 kind) {
    ReadbackData::Kind& k = data.kinds[kind];
    if (k.pipeline != nullptr) return Result<void>{};
    static const std::span<const u32> kShaders[3] = {readback_color_comp, readback_depth_comp,
                                                     readback_depth_stencil_comp};
    static const char* const kNames[3] = {"readback colour", "readback depth", "readback depth and stencil"};
    ComputePipelineDesc desc;
    desc.shader.spirv = kShaders[kind];
    desc.sets = {SetShape(kind)};
    desc.name = kNames[kind];
    Result<std::shared_ptr<Pipeline>> pipeline = backend.CreateComputePipeline(desc);
    if (!pipeline.ok()) return Error{pipeline.error()};
    Result<std::shared_ptr<BindingSet>> set = core::MakeBindingSet(backend.impl().shared_from_this(), SetShape(kind), false);
    if (!set.ok()) return Error{set.error()};
    k.pipeline = *pipeline;
    k.set = *set;
    return Result<void>{};
}

Result<std::shared_ptr<Buffer>> EnsureBuffer(Backend& backend, std::shared_ptr<Buffer>& slot, u64 size, Heap heap,
                                             BufferUsage usage, const char* name) {
    if (slot != nullptr && slot->Size() >= size) return slot;
    BufferDesc desc;
    desc.size = std::max<u64>(size, 4096);
    desc.heap = heap;
    desc.usage = usage;
    desc.name = name;
    Result<std::shared_ptr<Buffer>> buffer = backend.CreateBuffer(desc);
    if (!buffer.ok()) return Error{buffer.error()};
    slot = *buffer;  // the old one is idle: every readback waits for its submission
    return slot;
}

}  // namespace

ReadbackData& ReadbackState(Backend::Impl& backend) {
    std::lock_guard lock(SlotMutex());
    if (backend.readback == nullptr) {
        backend.readback = Slot<ReadbackData>(new ReadbackData, [](ReadbackData* p) { delete p; });
    }
    return *backend.readback;
}

namespace readback {

Result<void> CheckSubmitted(Backend::Impl& backend, Image::Impl& image, const char* who) {
    if (image.open_pass_in != nullptr) {
        return Error{fmt::format("{}: the image is an attachment of an open pass: submit that recorder first", who)};
    }
    Recorder::Impl* open = targets::OpenRecorder(backend, std::this_thread::get_id());
    if (open != nullptr && open->targets != nullptr) {
        for (const TouchedImage& touched : Data(*open).touched) {
            if (touched.image == &image) {
                return Error{fmt::format("{}: the calling thread's open recorder has recorded on the image: submit it "
                                         "first (a readback sees what has been submitted)",
                                         who)};
            }
        }
    }
    return Result<void>{};
}

void SubmitAndWait(Backend& backend, Recorder&& recorder) {
    std::vector<Wait> waits;
    for (const Wait& required : recorder.impl().required_waits) {
        waits.push_back(required);  // the stage the hand-over named: what the owner rule's check compares
        waits.push_back(Wait{required.value, Stage::ComputeShader});
    }
    // The hand-over's value has to have passed before the readback runs, whatever the stage; the wait is on the CPU
    // as well (Submit does it without the `timeline` bit), because the caller blocks on the result anyway.
    for (const Wait& wait : waits) {
        backend.WaitIdle(wait.value);
    }
    backend.WaitIdle(backend.Submit(std::move(recorder), waits));
}

}  // namespace readback

Result<void> Backend::ReadbackToGuest(const Image& image, Rect rect, PicaFormat format, std::span<u8> dest) {
    Image::Impl& img = image.impl();
    const u32 bpp = BytesPerPixel(format);
    if (bpp == 0) return Error{"ReadbackToGuest: unknown PICA format"};
    if (rect.width == 0 || rect.height == 0 || rect.width % 8 != 0 || rect.height % 8 != 0) {
        return Error{fmt::format("ReadbackToGuest: the rect {}x{} is not a whole number of 8x8 tiles", rect.width, rect.height)};
    }
    if (rect.x < 0 || rect.y < 0 || rect.x % 8 != 0 || rect.y % 8 != 0 ||
        static_cast<u64>(rect.x) + rect.width > img.width || static_cast<u64>(rect.y) + rect.height > img.height) {
        return Error{fmt::format("ReadbackToGuest: the rect ({}, {}) {}x{} is not tile-aligned inside the {}x{} image", rect.x,
                                 rect.y, rect.width, rect.height, img.width, img.height)};
    }
    const u64 bytes = static_cast<u64>(rect.width) * rect.height * bpp;
    if (dest.size() < bytes) {
        return Error{fmt::format("ReadbackToGuest: {} bytes of destination for {} needed", dest.size(), bytes)};
    }
    Result<Source> source = Classify(img, format);
    if (!source.ok()) return Error{source.error()};
    if (bytes > impl_->caps.max_storage_buffer_range) {
        return Error{fmt::format("ReadbackToGuest: {} bytes are above the device's storage buffer range {}", bytes,
                                 impl_->caps.max_storage_buffer_range)};
    }
    if (Result<void> submitted = readback::CheckSubmitted(*impl_, img, "ReadbackToGuest"); !submitted.ok()) return submitted;

    ReadbackData& data = ReadbackState(*impl_);
    std::lock_guard lock(data.mutex);
    if (Result<void> made = EnsureKind(*this, data, source->kind); !made.ok()) return made;
    ReadbackData::Kind& kind = data.kinds[source->kind];
    Result<std::shared_ptr<Buffer>> output =
        EnsureBuffer(*this, data.output, bytes, Heap::Readback, BufferUsage::Storage | BufferUsage::Transfer, "readback output");
    if (!output.ok()) return Error{output.error()};
    Result<std::shared_ptr<Buffer>> params_buffer =
        EnsureBuffer(*this, data.params, sizeof(Params), Heap::Upload, BufferUsage::Uniform, "readback parameters");
    if (!params_buffer.ok()) return Error{params_buffer.error()};

    const u64 words = bytes / 4;
    const u64 groups = (words + 63) / 64;
    const u32 groups_x = static_cast<u32>(std::min<u64>(groups, 65535));
    const u32 groups_y = static_cast<u32>((groups + groups_x - 1) / groups_x);
    Params params = {};
    params.rect[0] = rect.x;
    params.rect[1] = rect.y;
    params.rect[2] = static_cast<s32>(rect.width);
    params.rect[3] = static_cast<s32>(rect.height);
    params.info[0] = static_cast<u32>(format);
    params.info[1] = static_cast<u32>(words);
    params.info[2] = groups_x * 64;
    for (int c = 0; c < 4; ++c) params.src_bits[c] = source->bits[c];
    std::memcpy((*params_buffer)->Mapped().data(), &params, sizeof(params));
    (*params_buffer)->Flush(0, sizeof(params));

    // The views of the image the pass reads, and the buffers.
    const u32 out_binding = source->kind == 2 ? 2 : 1;
    const u32 param_binding = out_binding + 1;
    const VkImageView first_view = targets::GetView(img, source->kind == 0 ? ImageView::Sampled : ImageView::Depth);
    const VkImageView stencil_view = source->kind == 2 ? targets::GetView(img, ImageView::Stencil) : VK_NULL_HANDLE;
    if (first_view == VK_NULL_HANDLE || (source->kind == 2 && stencil_view == VK_NULL_HANDLE)) {
        return Error{"ReadbackToGuest: the image has no view the pass can read"};
    }
    if (Result<void> r = kind.set->SetStorageBuffer(out_binding, **output, 0, bytes); !r.ok()) return r;
    if (Result<void> r = kind.set->SetUniformBuffer(param_binding, **params_buffer, 0, sizeof(Params)); !r.ok()) return r;
    ::plume::VulkanDescriptorSet* vk_set = static_cast<::plume::VulkanDescriptorSet*>(kind.set->impl().set.get());
    VkDescriptorImageInfo image_info = {};
    image_info.imageView = first_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    vk_set->setDescriptor(0, nullptr, &image_info, nullptr, nullptr);
    if (stencil_view != VK_NULL_HANDLE) {
        VkDescriptorImageInfo stencil_info = {};
        stencil_info.imageView = stencil_view;
        stencil_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        vk_set->setDescriptor(1, nullptr, &stencil_info, nullptr, nullptr);
    }

    Recorder recorder = core::MakeRecorder(impl_, true);
    // The first-use path of the targets module, as an operation on a GENERAL image would take it: the owner rule
    // (a hand-over's wait is added to the recorder), then the move to GENERAL with the writes visible. A depth image
    // a pass left attached at Submit is in plume's attachment layout until this barrier.
    targets::Acquire(recorder.impl(), img);
    targets::Normalize(recorder.impl(), img);
    // The output buffer is reused: what an earlier readback's pass wrote to it is ordered before this pass's writes (the layer
    // sees the host read of the earlier one only as a barrier on that submission, not as a dependency for this one).
    VkMemoryBarrier reuse = {};
    reuse.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    reuse.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    reuse.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(recorder.impl().vulkan_list()->vk, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &reuse, 0, nullptr, 0, nullptr);
    recorder.SetPipeline(*kind.pipeline);
    recorder.Bind(*kind.set, 0);
    recorder.Dispatch(groups_x, groups_y, 1);
    VkMemoryBarrier host = {};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(recorder.impl().vulkan_list()->vk, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &host, 0, nullptr, 0, nullptr);
    readback::SubmitAndWait(*this, std::move(recorder));

    (*output)->Invalidate(0, bytes);
    std::memcpy(dest.data(), (*output)->Mapped().data(), bytes);
    return Result<void>{};
}

}  // namespace mh4n::gpu::rhi
