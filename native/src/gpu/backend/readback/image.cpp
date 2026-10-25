// ReadImage (N2-28): a region of any backend image to the host in the image's own format, for the compositor's tests
// and screenshots. The copy is the targets module's own CopyImageToBuffer (so the owner rule and the layout policy
// apply as they do to any copy), recorded on an internal recorder, followed by a CPU wait.
#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/readback/impl.h"

namespace mh4n::gpu::rhi {

namespace {

// Bytes per texel of the colour plane (or of the depth plane) of a format an image can have; 0 for the ones a
// region cannot be read in (the block-compressed formats, whose rows are rows of blocks, and the vertex formats).
u32 TexelBytes(Format format) {
    switch (format) {
    case Format::R8Unorm: return 1;
    case Format::Rg8Unorm:
    case Format::R5G6B5:
    case Format::B5G6R5:
    case Format::R5G5B5A1:
    case Format::B5G5R5A1:
    case Format::A1R5G5B5:
    case Format::R4G4B4A4:
    case Format::B4G4R4A4:
    case Format::D16Unorm: return 2;
    case Format::Rgba8Unorm:
    case Format::Rgba8Snorm:
    case Format::Bgra8Unorm:
    case Format::R32Uint:
    case Format::D24S8:
    case Format::D32Float:
    case Format::D32S8: return 4;
    case Format::Rgba16Float: return 8;
    default: return 0;
    }
}

}  // namespace

Result<std::vector<u8>> Backend::ReadImage(const Image& image, u32 x, u32 y, u32 width, u32 height) {
    Image::Impl& img = image.impl();
    const u32 texel = TexelBytes(img.format);
    if (texel == 0) {
        return Error{fmt::format("ReadImage: format {} cannot be read back as rows of texels", static_cast<int>(img.format))};
    }
    if (width == 0 || height == 0 || static_cast<u64>(x) + width > img.width || static_cast<u64>(y) + height > img.height) {
        return Error{fmt::format("ReadImage: the region ({}, {}) {}x{} is outside the {}x{} image", x, y, width, height, img.width,
                                 img.height)};
    }
    if (Result<void> submitted = readback::CheckSubmitted(*impl_, img, "ReadImage"); !submitted.ok()) {
        return Error{submitted.error()};
    }
    const bool stencil = img.format == Format::D24S8 || img.format == Format::D32S8;
    const u64 plane = static_cast<u64>(width) * height;
    const u64 total = plane * texel + (stencil ? plane : 0);

    BufferDesc desc;
    desc.size = total;
    desc.heap = Heap::Readback;
    desc.usage = BufferUsage::Transfer;
    desc.name = "ReadImage";
    Result<std::shared_ptr<Buffer>> buffer = CreateBuffer(desc);
    if (!buffer.ok()) return Error{buffer.error()};

    BufferImageRegion regions[2];
    regions[0].image_rect = Rect{static_cast<s32>(x), static_cast<s32>(y), width, height};
    regions[0].aspect = targets::IsDepth(img) ? ImageAspect::Depth : ImageAspect::Color;
    size_t count = 1;
    if (stencil) {
        regions[1] = regions[0];
        regions[1].buffer_offset = plane * texel;  // the stencil plane follows the depth plane
        regions[1].aspect = ImageAspect::Stencil;
        count = 2;
    }
    Recorder recorder = core::MakeRecorder(impl_, true);
    recorder.CopyImageToBuffer(image, **buffer, std::span<const BufferImageRegion>(regions, count));
    readback::SubmitAndWait(*this, std::move(recorder));

    (*buffer)->Invalidate(0, total);
    std::vector<u8> bytes(total);
    std::memcpy(bytes.data(), (*buffer)->Mapped().data(), total);
    return bytes;
}

}  // namespace mh4n::gpu::rhi
