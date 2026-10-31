// Derived from Azahar src/video_core/renderer_vulkan/vk_instance.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later, as the file says: "Licensed under GPLv2 or any later version")
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
//
// The pattern taken from Instance::DetermineTraits and Instance::CreateFormatTable (vk_instance.cpp:212-312): a
// format is used as it is when the device can sample it and draw to it, and otherwise Rgba8 (colour) or D32(S8)
// (depth) takes its place and the caller owes a conversion. The rest is ours: the choice reads the format query
// of the vendored backend (patch 11) instead of vkGetPhysicalDeviceFormatProperties, orders the component
// variants of the 16-bit formats, and is a pure function over a support table so that it can be tested on
// synthetic ones.
#include "gpu/backend/targets/impl.h"

namespace mh4n::gpu::rhi {

namespace targets {

namespace {

// The component orders a PICA 16-bit format may be stored in, the PICA's own bits first (rhi.h's comments on Format).
struct Variants {
    Format order[3];
    int count;
};

Variants VariantsOf(Format format) {
    switch (format) {
    case Format::Rgb565: return {{Format::R5G6B5, Format::B5G6R5, Format::Undefined}, 2};
    case Format::Rgb5A1: return {{Format::R5G5B5A1, Format::B5G5R5A1, Format::A1R5G5B5}, 3};
    case Format::Rgba4: return {{Format::R4G4B4A4, Format::B4G4R4A4, Format::Undefined}, 2};
    default: return {{Format::Undefined, Format::Undefined, Format::Undefined}, 0};
    }
}

bool IsCompressed(Format format) {
    return format == Format::Bc1Unorm || format == Format::Bc3Unorm || format == Format::Bc5Unorm ||
           format == Format::Bc7Unorm;
}

} // namespace

ChosenFormat ChooseFormatWith(Format format, FormatUse use, const Capabilities& caps, const SupportFn& support) {
    ChosenFormat chosen;
    chosen.format = format;
    chosen.conversion = Conversion::None;
    if (format == Format::Rgb8) {
        chosen.format = Format::Rgba8Unorm;
        chosen.conversion = Conversion::Rgb8ToRgba8;
        return chosen;
    }
    if (format == Format::D24S8 && !caps.d24s8) {
        chosen.format = Format::D32S8;
        chosen.conversion = Conversion::DepthStencilTo32;
        return chosen;
    }
    if (const Variants variants = VariantsOf(format); variants.count > 0) {
        for (int i = 0; i < variants.count; ++i) {
            const FormatSupport s = support(variants.order[i]);
            const bool usable = use == FormatUse::Sampled ? s.sampled : (s.sampled && s.attachment);
            if (usable) {
                chosen.format = variants.order[i];
                return chosen;
            }
        }
        chosen.format = Format::Rgba8Unorm;
        chosen.conversion = Conversion::UnpackToRgba8;
        return chosen;
    }
    if (IsCompressed(format) && !(caps.bc7 && support(format).sampled)) {
        chosen.format = Format::Rgba8Unorm;
        chosen.conversion = Conversion::DecodeBc;
    }
    return chosen;
}

VkFormat ToVkFormat(Format format) {
    switch (format) {
    case Format::Undefined: return VK_FORMAT_UNDEFINED;
    case Format::Rgb8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgb565: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::Rgb5A1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case Format::Rgba4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::Rg8Unorm: return VK_FORMAT_R8G8_UNORM;
    case Format::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::Rgba8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
    case Format::Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R5G6B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::B5G6R5: return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case Format::R5G5B5A1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case Format::B5G5R5A1: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
    case Format::A1R5G5B5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case Format::R4G4B4A4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case Format::B4G4R4A4: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case Format::R32Uint: return VK_FORMAT_R32_UINT;
    case Format::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::Bc1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::Bc3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::Bc5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::Bc7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::D16Unorm: return VK_FORMAT_D16_UNORM;
    case Format::D24S8: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    case Format::D32S8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case Format::Rg32Float: return VK_FORMAT_R32G32_SFLOAT;
    case Format::Rgb32Float: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R8Uint: return VK_FORMAT_R8_UINT;
    case Format::R8Sint: return VK_FORMAT_R8_SINT;
    case Format::Rg8Uint: return VK_FORMAT_R8G8_UINT;
    case Format::Rg8Sint: return VK_FORMAT_R8G8_SINT;
    case Format::Rgba8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case Format::Rgba8Sint: return VK_FORMAT_R8G8B8A8_SINT;
    case Format::R16Uint: return VK_FORMAT_R16_UINT;
    case Format::R16Sint: return VK_FORMAT_R16_SINT;
    case Format::Rg16Sint: return VK_FORMAT_R16G16_SINT;
    case Format::Rgba16Sint: return VK_FORMAT_R16G16B16A16_SINT;
    }
    return VK_FORMAT_UNDEFINED;
}

u32 TexelBytes(Format format) {
    switch (format) {
    case Format::R8Unorm: case Format::R8Uint: case Format::R8Sint: return 1;
    case Format::Rg8Unorm: case Format::Rg8Uint: case Format::Rg8Sint: case Format::R16Uint: case Format::R16Sint:
    case Format::Rgb565: case Format::Rgb5A1: case Format::Rgba4: case Format::R5G6B5: case Format::B5G6R5:
    case Format::R5G5B5A1: case Format::B5G5R5A1: case Format::A1R5G5B5: case Format::R4G4B4A4: case Format::B4G4R4A4:
        return 2;
    case Format::Rgb8: case Format::Rgba8Unorm: case Format::Rgba8Snorm: case Format::Bgra8Unorm: case Format::R32Uint:
    case Format::R32Float: case Format::Rgba8Uint: case Format::Rgba8Sint: case Format::Rg16Sint: return 4;
    case Format::Rgba16Float: case Format::Rg32Float: case Format::Rgba16Sint: return 8;
    case Format::Rgb32Float: return 12;
    case Format::Rgba32Float: return 16;
    default: return 0;
    }
}

} // namespace targets

// ---- Backend: the format query and the chooser ----------------------------------------------------

FormatSupport Backend::QueryFormat(Format format) const {
    FormatSupport support;
    if (format == Format::Undefined) {
        return support;
    }
    const ::plume::RenderFormatSupport s = impl_->device->getFormatSupport(core::ToPlumeFormat(format));
    support.attachment = s.attachment;
    support.sampled = s.sampled;
    support.storage = s.storage;
    support.blit_source = s.blitSource;
    support.blit_destination = s.blitDestination;
    return support;
}

ChosenFormat Backend::ChooseFormat(Format format, FormatUse use) const {
    return targets::ChooseFormatWith(format, use, impl_->caps, [this](Format f) { return QueryFormat(f); });
}

} // namespace mh4n::gpu::rhi
