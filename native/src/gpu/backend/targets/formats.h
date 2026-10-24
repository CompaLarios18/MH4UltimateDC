// The format rules of the targets module (N2-27), kept apart from the device so that tests can run them on
// synthetic format-support tables: the chooser, and the table from every Format to the Vulkan format.
#pragma once

#include <functional>

#include "gpu/backend/rhi.h"
#include <volk.h>

namespace mh4n::gpu::rhi::targets {

// What the device says about one format (Backend::QueryFormat).
using SupportFn = std::function<FormatSupport(Format)>;

// Backend::ChooseFormat's rules over any support table (docs/native/rhi.md):
//   Rgb8 -> Rgba8Unorm (Rgb8ToRgba8); D24S8 -> D24S8 when caps.d24s8, else D32S8 (DepthStencilTo32);
//   Rgb565, Rgb5A1, Rgba4 -> the first of their component orders (the PICA's bits first, then the swapped ones) that
//   the support table says is a usable image (an attachment and sampled for a target, sampled for a texture),
//   else Rgba8Unorm (UnpackToRgba8); BC1, BC3, BC5, BC7 -> themselves when caps.bc7 and the table says sampled,
//   else Rgba8Unorm (DecodeBc); every other format as it is. A swapped component order has Conversion::None:
//   the caller reads the order from `format` (rhi.h's comments on Format).
ChosenFormat ChooseFormatWith(Format format, FormatUse use, const Capabilities& caps, const SupportFn& support);

// The Vulkan format of a Format (the PICA names map to their default storage format, as core::ToPlumeFormat
// does). VK_FORMAT_UNDEFINED for Undefined.
VkFormat ToVkFormat(Format format);

// Texel size in bytes of a colour format (one aspect), 0 for the block-compressed and the depth-stencil formats.
u32 TexelBytes(Format format);

} // namespace mh4n::gpu::rhi::targets
