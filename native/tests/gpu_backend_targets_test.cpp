// The backend's targets module (N2-27): the format query and chooser, images and their views, samplers, copies,
// clears, blits, passes with attachments, the one hidden layout policy with the owner rule, the feedback copy,
// per-draw bindings recycled by timeline value, WrapExternalImage and the swapchain image's way to presentation.
// Run on whatever Vulkan device the machine has (lavapipe in the cloud and in Linux CI); a case that needs a device
// skips with a message without one, and fails under MH4N_REQUIRE_VULKAN=1.
//
// Every expected pixel and byte below is worked out by hand from the formats (the comments show the arithmetic);
// none is computed with the code under test. Lavapipe has D24S8 and the PC does not: the runs with
// MH4N_RHI_MASK=d24s8 (or the `d24s8` bit in a case's own BackendOptions::mask) are the PC's path.
//
// Run it as the PC's paths would: MH4N_RHI_MASK=d24s8,interlock,bc7,direct_upload,custom_border_color,timeline
// ctest --test-dir build/native -R '^gpu_backend_targets_test$' (docs/native/rhi.md lists the masks).
//
// The swapchain frames are the one case that needs a surface (`swapchain/three_frames_over_a_headless_surface`); no
// other case does, so on a machine whose loader lacks VK_EXT_headless_surface (the RX 6700 XT's Windows loader) the
// rest of the file runs and that case fails under MH4N_REQUIRE_VULKAN=1 or skips.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/backend/targets/formats.h"
#include "gpu/backend/targets/impl.h"
#include "gpu/backend/targets/upload.h"
#include "support/vulkan_device.h"

#include "gpu_backend_targets_shaders.inc"

using mh4n::s32;
using mh4n::u16;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
namespace test = mh4n::test;
using namespace mh4n::gpu::rhi;

namespace {

// ---- helpers ---------------------------------------------------------------------------------------------------

template <size_t N>
ShaderCode Spv(const uint32_t (&words)[N]) {
    ShaderCode code;
    code.spirv = std::span<const u32>(words, N);
    return code;
}

struct BreachCapture {
    std::mutex mutex;
    std::vector<OwnerRule> rules;
    OwnerRuleHandler previous;
    BreachCapture() {
        previous = SetOwnerRuleHandler([this](const OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            rules.push_back(breach.rule);
        });
    }
    ~BreachCapture() { SetOwnerRuleHandler(previous); }
    int Count(OwnerRule rule) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count(rules.begin(), rules.end(), rule));
    }
    size_t Total() {
        std::lock_guard lock(mutex);
        return rules.size();
    }
};

std::shared_ptr<Backend> MakeBackend(std::initializer_list<Cap> masked = {}) {
    BackendOptions options;
    for (Cap cap : masked) options.mask = options.mask.With(cap);
    return test::RequireBackend(options);
}

std::shared_ptr<Image> MakeImage(Backend& backend, Format format, u32 width, u32 height, bool shadow = false, u32 mips = 1,
                                 u32 layers = 1, bool cube = false) {
    ImageDesc desc;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.mip_levels = mips;
    desc.layers = layers;
    desc.cube = cube;
    desc.shadow_source = shadow;
    desc.name = "test image";
    Result<std::shared_ptr<Image>> image = backend.CreateImage(desc);
    REQUIRE_MESSAGE(image.ok(), image.error());
    return *image;
}

std::shared_ptr<Buffer> MakeBuffer(Backend& backend, u64 size, Heap heap, BufferUsage usage) {
    BufferDesc desc;
    desc.size = size;
    desc.heap = heap;
    desc.usage = usage;
    desc.name = "test buffer";
    Result<std::shared_ptr<Buffer>> buffer = backend.CreateBuffer(desc);
    REQUIRE_MESSAGE(buffer.ok(), buffer.error());
    return *buffer;
}

std::shared_ptr<Pipeline> MakePipeline(Backend& backend, ShaderCode vertex, ShaderCode fragment, Format color, Format depth,
                                       const FixedFunctionState& state = {}, std::vector<BindingSetDesc> sets = {},
                                       const char* name = "test") {
    GraphicsPipelineDesc desc;
    desc.vertex = vertex;
    desc.fragment = fragment;
    desc.color_format = color;
    desc.depth_format = depth;
    desc.state = state;
    desc.sets = std::move(sets);
    desc.name = name;
    Result<std::shared_ptr<Pipeline>> pipeline = backend.CreateGraphicsPipeline(desc);
    REQUIRE_MESSAGE(pipeline.ok(), pipeline.error());
    return *pipeline;
}

// Reads one aspect of one level of an image back through CopyImageToBuffer (tightly packed rows) on a recorder of its
// own: call it with no recorder open on the calling thread.
std::vector<u8> ReadBack(Backend& backend, Image& image, u32 width, u32 height, u32 bytes_per_texel,
                         ImageAspect aspect = ImageAspect::Color, u32 mip = 0) {
    const u64 size = u64(width) * height * bytes_per_texel;
    std::shared_ptr<Buffer> buffer = MakeBuffer(backend, size, Heap::Readback, BufferUsage::Transfer);
    Recorder rec = backend.BeginRecording();
    BufferImageRegion region;
    region.mip_level = mip;
    region.image_rect = Rect{0, 0, width, height};
    region.aspect = aspect;
    rec.CopyImageToBuffer(image, *buffer, std::span<const BufferImageRegion>(&region, 1));
    const TimelineValue value = backend.Submit(std::move(rec));
    std::vector<u8> bytes(size);
    Result<void> read = backend.ReadBuffer(*buffer, 0, bytes, value);
    REQUIRE_MESSAGE(read.ok(), read.error());
    return bytes;
}

u32 Word(const std::vector<u8>& bytes, size_t index, u32 size) {
    u32 word = 0;
    for (u32 i = 0; i < size; ++i) word |= u32(bytes[index * size + i]) << (8 * i);
    return word;
}

// RGBA8 texel (x, y) of a tightly packed image `width` wide.
std::array<u8, 4> Texel(const std::vector<u8>& bytes, u32 width, u32 x, u32 y) {
    const size_t at = (size_t(y) * width + x) * 4;
    return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
}

ClearValue Color(float r, float g, float b, float a) {
    ClearValue value;
    value.color = {r, g, b, a};
    return value;
}

// Submits a recorder and waits for it.
void Run(Backend& backend, Recorder&& rec) {
    backend.WaitIdle(backend.Submit(std::move(rec)));
}

std::shared_ptr<Sampler> NearestSampler(Backend& backend) {
    SamplerDesc desc;
    desc.min_filter = desc.mag_filter = desc.mip_filter = Filter::Nearest;
    Result<std::shared_ptr<Sampler>> sampler = backend.CreateSampler(desc);
    REQUIRE_MESSAGE(sampler.ok(), sampler.error());
    return *sampler;
}

const BindingSetDesc kSampledAndSampler{{{0, BindingType::SampledImage}, {1, BindingType::Sampler}}};

// ---- the formats ------------------------------------------------------------------------------------------------

// The Vulkan format a Format is stored in, written out here and not taken from the module (PICA names: their default
// storage format, as core::ToPlumeFormat gives).
VkFormat ExpectedVk(Format format) {
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

bool IsDepthName(Format f) {
    return f == Format::D16Unorm || f == Format::D24S8 || f == Format::D32Float || f == Format::D32S8;
}

// A support table by hand, for the chooser's rules on devices this machine is not.
FormatSupport Usable() {
    FormatSupport s;
    s.attachment = s.sampled = s.blit_source = s.blit_destination = true;
    return s;
}

}  // namespace

TEST_CASE("formats/query_agrees_with_vkGetPhysicalDeviceFormatProperties_for_every_format") {
    for (const bool mask_bc7 : {false, true}) {
        std::shared_ptr<Backend> backend = mask_bc7 ? MakeBackend({Cap::Bc7}) : MakeBackend();
        if (!backend) return;
        const VulkanHandles handles = Handles(*backend);
        for (u32 i = 0; i <= static_cast<u32>(Format::Rgba16Sint); ++i) {
            const Format format = static_cast<Format>(i);
            const VkFormat vk = ExpectedVk(format);
            const FormatSupport got = backend->QueryFormat(format);
            if (vk == VK_FORMAT_UNDEFINED) {
                CHECK_FALSE((got.attachment || got.sampled || got.storage || got.blit_source || got.blit_destination));
                continue;
            }
            VkFormatProperties props = {};
            vkGetPhysicalDeviceFormatProperties(handles.physical_device, vk, &props);
            const VkFormatFeatureFlags f = props.optimalTilingFeatures;
            const VkFormatFeatureFlags attach = IsDepthName(format) ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                    : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            INFO("format " << i << " vk " << vk << " bc7 mask " << mask_bc7);
            CHECK(got.attachment == ((f & attach) != 0));
            CHECK(got.sampled == ((f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0));
            CHECK(got.storage == ((f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0));
            CHECK(got.blit_source == ((f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0));
            CHECK(got.blit_destination == ((f & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0));
        }
    }
}

TEST_CASE("formats/the_chooser_follows_the_support_table_on_devices_this_machine_is_not") {
    using targets::ChooseFormatWith;
    Capabilities caps;
    caps.d24s8 = true;
    caps.bc7 = true;
    // A table that lets exactly the formats in `usable` be targets.
    const auto table = [](std::initializer_list<Format> usable) {
        return [list = std::vector<Format>(usable)](Format f) {
            return std::find(list.begin(), list.end(), f) != list.end() ? Usable() : FormatSupport{};
        };
    };
    // MoltenVK (4, G9): only the R-first 16-bit orders are attachments.
    const auto r_first = table({Format::R5G6B5, Format::R5G5B5A1, Format::R4G4B4A4, Format::Rgba8Unorm});
    CHECK(ChooseFormatWith(Format::Rgb565, FormatUse::ColorTarget, caps, r_first).format == Format::R5G6B5);
    CHECK(ChooseFormatWith(Format::Rgb5A1, FormatUse::ColorTarget, caps, r_first).format == Format::R5G5B5A1);
    CHECK(ChooseFormatWith(Format::Rgba4, FormatUse::ColorTarget, caps, r_first).format == Format::R4G4B4A4);
    CHECK(ChooseFormatWith(Format::Rgba4, FormatUse::ColorTarget, caps, r_first).conversion == Conversion::None);
    // A device with only the swapped orders: the swapped one is chosen, the caller reads the order from the format.
    const auto swapped = table({Format::B5G6R5, Format::B5G5R5A1, Format::B4G4R4A4});
    CHECK(ChooseFormatWith(Format::Rgb565, FormatUse::ColorTarget, caps, swapped).format == Format::B5G6R5);
    CHECK(ChooseFormatWith(Format::Rgb5A1, FormatUse::ColorTarget, caps, swapped).format == Format::B5G5R5A1);
    CHECK(ChooseFormatWith(Format::Rgba4, FormatUse::ColorTarget, caps, swapped).format == Format::B4G4R4A4);
    // RGB5A1 has a third order; both earlier ones failing gives it.
    CHECK(ChooseFormatWith(Format::Rgb5A1, FormatUse::ColorTarget, caps, table({Format::A1R5G5B5})).format == Format::A1R5G5B5);
    // Both orders usable: the PICA's own bits win.
    CHECK(ChooseFormatWith(Format::Rgb565, FormatUse::ColorTarget, caps, table({Format::B5G6R5, Format::R5G6B5})).format == Format::R5G6B5);
    // Nothing usable: Rgba8 and the conversion the caller owes.
    for (Format f : {Format::Rgb565, Format::Rgb5A1, Format::Rgba4}) {
        const ChosenFormat none = ChooseFormatWith(f, FormatUse::ColorTarget, caps, table({}));
        CHECK(none.format == Format::Rgba8Unorm);
        CHECK(none.conversion == Conversion::UnpackToRgba8);
    }
    // A sampled-only format is a texture but not a target.
    FormatSupport sampled_only;
    sampled_only.sampled = true;
    const auto texture_only = [&](Format f) { return f == Format::R5G6B5 ? sampled_only : FormatSupport{}; };
    CHECK(ChooseFormatWith(Format::Rgb565, FormatUse::Sampled, caps, texture_only).format == Format::R5G6B5);
    CHECK(ChooseFormatWith(Format::Rgb565, FormatUse::ColorTarget, caps, texture_only).format == Format::Rgba8Unorm);
    // RGB8 is always RGBA8.
    const ChosenFormat rgb8 = ChooseFormatWith(Format::Rgb8, FormatUse::ColorTarget, caps, r_first);
    CHECK(rgb8.format == Format::Rgba8Unorm);
    CHECK(rgb8.conversion == Conversion::Rgb8ToRgba8);
    // D24S8 follows the d24s8 bit; D16 and the others are as they are.
    CHECK(ChooseFormatWith(Format::D24S8, FormatUse::DepthTarget, caps, r_first).format == Format::D24S8);
    caps.d24s8 = false;
    const ChosenFormat d32 = ChooseFormatWith(Format::D24S8, FormatUse::DepthTarget, caps, r_first);
    CHECK(d32.format == Format::D32S8);
    CHECK(d32.conversion == Conversion::DepthStencilTo32);
    CHECK(ChooseFormatWith(Format::D16Unorm, FormatUse::DepthTarget, caps, r_first).format == Format::D16Unorm);
    CHECK(ChooseFormatWith(Format::D16Unorm, FormatUse::DepthTarget, caps, r_first).conversion == Conversion::None);
    CHECK(ChooseFormatWith(Format::R5G6B5, FormatUse::ColorTarget, caps, table({})).format == Format::R5G6B5);
    // BC formats: themselves when the bit is on and the table says sampled, else decode on the CPU.
    for (Format f : {Format::Bc1Unorm, Format::Bc3Unorm, Format::Bc5Unorm, Format::Bc7Unorm}) {
        CHECK(ChooseFormatWith(f, FormatUse::Sampled, caps, [](Format) { return Usable(); }).format == f);
        const ChosenFormat decoded = ChooseFormatWith(f, FormatUse::Sampled, caps, table({}));
        CHECK(decoded.format == Format::Rgba8Unorm);
        CHECK(decoded.conversion == Conversion::DecodeBc);
    }
    caps.bc7 = false;
    CHECK(ChooseFormatWith(Format::Bc7Unorm, FormatUse::Sampled, caps, [](Format) { return Usable(); }).conversion == Conversion::DecodeBc);
}

TEST_CASE("formats/the_chooser_at_the_device_and_under_the_masks") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const Capabilities& caps = backend->Caps();
    // The rule applied to what the device says (the answer of each rule is checked above on tables).
    for (Format f : {Format::Rgb565, Format::Rgb5A1, Format::Rgba4}) {
        const ChosenFormat chosen = backend->ChooseFormat(f, FormatUse::ColorTarget);
        const FormatSupport s = backend->QueryFormat(chosen.format);
        if (chosen.format == Format::Rgba8Unorm) {
            CHECK(chosen.conversion == Conversion::UnpackToRgba8);
        } else {
            CHECK((s.attachment && s.sampled));  // whichever order the query says is an attachment
            CHECK(chosen.conversion == Conversion::None);
        }
    }
    CHECK(backend->ChooseFormat(Format::Rgb8, FormatUse::ColorTarget).format == Format::Rgba8Unorm);
    CHECK(backend->ChooseFormat(Format::Rgb8, FormatUse::ColorTarget).conversion == Conversion::Rgb8ToRgba8);
    CHECK(backend->ChooseFormat(Format::D16Unorm, FormatUse::DepthTarget).format == Format::D16Unorm);
    CHECK(backend->ChooseFormat(Format::D24S8, FormatUse::DepthTarget).format == (caps.d24s8 ? Format::D24S8 : Format::D32S8));
    CHECK(backend->ChooseFormat(Format::Bc7Unorm, FormatUse::Sampled).format ==
          (caps.bc7 && backend->QueryFormat(Format::Bc7Unorm).sampled ? Format::Bc7Unorm : Format::Rgba8Unorm));

    // MH4N_RHI_MASK=d24s8: D32S8 and the conversion the caller owes; the PC's path.
    if (std::shared_ptr<Backend> masked = MakeBackend({Cap::D24S8})) {
        const ChosenFormat d = masked->ChooseFormat(Format::D24S8, FormatUse::DepthTarget);
        CHECK(d.format == Format::D32S8);
        CHECK(d.conversion == Conversion::DepthStencilTo32);
        CHECK(masked->QueryFormat(Format::D32S8).attachment);
    }
    // MH4N_RHI_MASK=bc7: decode on the CPU, for every block-compressed format the bit gates.
    if (std::shared_ptr<Backend> masked = MakeBackend({Cap::Bc7})) {
        for (Format f : {Format::Bc1Unorm, Format::Bc3Unorm, Format::Bc5Unorm, Format::Bc7Unorm}) {
            const ChosenFormat c = masked->ChooseFormat(f, FormatUse::Sampled);
            CHECK(c.format == Format::Rgba8Unorm);
            CHECK(c.conversion == Conversion::DecodeBc);
        }
    }
}

// The PICA colour formats, cleared and copied to a buffer. `word` is the clear colour's bits in the format that was
// stored (worked out by hand: the first letter of the Vulkan name is in the highest bits).
TEST_CASE("images/each_colour_format_cleared_and_copied_to_a_buffer") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    struct Case {
        Format asked;
        ClearValue clear;
    };
    // 31 and 63 are the largest 5- and 6-bit values; the fractions below are k/max exactly (21/31, 42/63, 10/31 ...).
    const float f5 = 31.0f, f6 = 63.0f, f4 = 15.0f;
    const Case cases[] = {
        {Format::Rgba8Unorm, Color(1.0f, 51.0f / 255, 153.0f / 255, 102.0f / 255)},
        {Format::Rgb8, Color(1.0f, 51.0f / 255, 153.0f / 255, 1.0f)},
        {Format::Rgb565, Color(21 / f5, 42 / f6, 10 / f5, 1.0f)},
        {Format::Rgb5A1, Color(19 / f5, 10 / f5, 5 / f5, 1.0f)},
        {Format::Rgba4, Color(10 / f4, 5 / f4, 12 / f4, 1.0f)},
    };
    for (const Case& c : cases) {
        std::shared_ptr<Image> image = MakeImage(*backend, c.asked, 4, 4);
        INFO("asked " << static_cast<int>(c.asked) << " stored " << static_cast<int>(image->GetFormat()));
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*image, c.clear);
        Run(*backend, std::move(rec));
        const Format stored = image->GetFormat();
        const bool sixteen = stored != Format::Rgba8Unorm;
        const std::vector<u8> bytes = ReadBack(*backend, *image, 4, 4, sixteen ? 2 : 4);
        // Every texel of the 4x4 image is the clear colour.
        u32 expected = 0;
        switch (stored) {
        case Format::Rgba8Unorm:
            // little-endian bytes 255, 51, 153, then the alpha: 102 for Rgba8, 255 for Rgb8 stored as Rgba8
            expected = (c.asked == Format::Rgba8Unorm ? 102u : 255u) << 24 | 153u << 16 | 51u << 8 | 255u;
            break;
        case Format::R5G6B5: expected = 0xAD4A; break;    // (21 << 11) | (42 << 5) | 10
        case Format::B5G6R5: expected = 0x5555; break;    // (10 << 11) | (42 << 5) | 21
        case Format::R5G5B5A1: expected = 0x9A8B; break;  // (19 << 11) | (10 << 6) | (5 << 1) | 1
        case Format::B5G5R5A1: expected = 0x2AA7; break;  // (5 << 11) | (10 << 6) | (19 << 1) | 1
        case Format::A1R5G5B5: expected = 0xCD45; break;  // 1 << 15 | 19 << 10 | 10 << 5 | 5
        case Format::R4G4B4A4: expected = 0xA5CF; break;  // (10 << 12) | (5 << 8) | (12 << 4) | 15
        case Format::B4G4R4A4: expected = 0xC5AF; break;  // (12 << 12) | (5 << 8) | (10 << 4) | 15
        default: FAIL("an unexpected stored format " << static_cast<int>(stored));
        }
        if (stored == Format::Rgba8Unorm && (c.asked == Format::Rgb565 || c.asked == Format::Rgb5A1 || c.asked == Format::Rgba4)) {
            // UnpackToRgba8: the device stores Rgba8 and the caller owes the conversion; the clear is round(f * 255) per channel.
            const std::array<u8, 4> want = c.asked == Format::Rgb565   ? std::array<u8, 4>{173, 170, 82, 255}   // 21/31, 42/63, 10/31 of 255, rounded
                                           : c.asked == Format::Rgb5A1 ? std::array<u8, 4>{156, 82, 41, 255}    // 19/31, 10/31, 5/31
                                                                       : std::array<u8, 4>{170, 85, 204, 255};  // 10/15, 5/15, 12/15
            for (u32 i = 0; i < 16; ++i) CHECK(Texel(bytes, 4, i % 4, i / 4) == want);
            continue;
        }
        for (u32 i = 0; i < 16; ++i) {
            CHECK(Word(bytes, i, sixteen ? 2 : 4) == expected);
        }
    }
}

TEST_CASE("images/each_depth_format_cleared_and_copied_as_two_regions") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        for (const Format asked : {Format::D16Unorm, Format::D24S8}) {
            std::shared_ptr<Image> image = MakeImage(*backend, asked, 4, 4);
            const Format stored = image->GetFormat();
            INFO("asked " << static_cast<int>(asked) << " stored " << static_cast<int>(stored) << " d24s8 masked " << masked);
            if (asked == Format::D16Unorm) {
                CHECK(stored == Format::D16Unorm);
            } else {
                CHECK(stored == (backend->Caps().d24s8 ? Format::D24S8 : Format::D32S8));
                if (masked) CHECK(stored == Format::D32S8);
            }
            // [0] depth 1.0 stencil 0xAB, then [1] depth 0.0 stencil 0x3C
            const float depths[2] = {1.0f, 0.0f};
            const u32 stencils[2] = {0xAB, 0x3C};
            for (int round = 0; round < 2; ++round) {
                ClearValue clear;
                clear.depth = depths[round];
                clear.stencil = stencils[round];
                const u32 depth_bytes = stored == Format::D16Unorm ? 2 : 4;
                std::shared_ptr<Buffer> buffer = MakeBuffer(*backend, 16 * depth_bytes + 16, Heap::Readback, BufferUsage::Transfer);
                Recorder rec = backend->BeginRecording();
                rec.ClearImage(*image, clear);
                // Depth and stencil are two regions: the depth plane at offset 0, the stencil plane right after it.
                BufferImageRegion regions[2];
                regions[0].buffer_offset = 0;
                regions[0].image_rect = Rect{0, 0, 4, 4};
                regions[0].aspect = ImageAspect::Depth;
                regions[1].buffer_offset = 16 * depth_bytes;
                regions[1].image_rect = Rect{0, 0, 4, 4};
                regions[1].aspect = ImageAspect::Stencil;
                const bool has_stencil = stored != Format::D16Unorm;
                rec.CopyImageToBuffer(*image, *buffer, std::span<const BufferImageRegion>(regions, has_stencil ? 2 : 1));
                const TimelineValue value = backend->Submit(std::move(rec));
                std::vector<u8> bytes(16 * depth_bytes + 16);
                REQUIRE(backend->ReadBuffer(*buffer, 0, bytes, value).ok());
                for (u32 i = 0; i < 16; ++i) {
                    const u32 word = Word(bytes, i, depth_bytes);
                    if (stored == Format::D16Unorm) {
                        CHECK(word == (round == 0 ? 0xFFFFu : 0u));  // 1.0 * 65535, 0.0
                    } else if (stored == Format::D24S8) {
                        CHECK((word & 0xFFFFFFu) == (round == 0 ? 0xFFFFFFu : 0u));  // 24 bits in the low bits; the top byte is undefined
                    } else {
                        CHECK(word == (round == 0 ? 0x3F800000u : 0u));  // the float 1.0 and 0.0
                    }
                    if (has_stencil) CHECK(bytes[16 * depth_bytes + i] == stencils[round]);
                }
            }
        }
    }
}

TEST_CASE("images/an_upload_goes_through_a_staging_buffer_with_direct_upload_on_or_masked") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::DirectUpload}) : MakeBackend();
        if (!backend) return;
        CHECK(backend->Caps().direct_upload == (masked ? false : backend->Caps().direct_upload));
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 2, 2);
        const std::vector<u8> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const u64 before = StagingUploadCount(*backend);
        Recorder rec = backend->BeginRecording();
        BufferImageRegion region;
        region.image_rect = Rect{0, 0, 2, 2};
        REQUIRE(UploadToImage(*backend, rec, *image, region, data).ok());
        Run(*backend, std::move(rec));
        CHECK(StagingUploadCount(*backend) == before + 1);  // counted: one staging buffer
        CHECK(ReadBack(*backend, *image, 2, 2, 4) == data);
    }
}

TEST_CASE("images/storage_usage_only_on_shadow_sources") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> plain = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    std::shared_ptr<Image> shadow = MakeImage(*backend, Format::Rgba8Unorm, 4, 4, true);
    // from the creation flags the module records
    CHECK_FALSE(plain->impl().HasStorageUsage());
    CHECK_FALSE((plain->impl().usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
    CHECK(shadow->impl().HasStorageUsage());
    CHECK((shadow->impl().usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
    CHECK((plain->impl().plume_flags & ::plume::RenderTextureFlag::MUTABLE_FORMAT) == 0);
    CHECK((shadow->impl().plume_flags & ::plume::RenderTextureFlag::MUTABLE_FORMAT) != 0);
    // only the shadow source has the storage view, and only it may be bound as a storage image
    CHECK(targets::GetView(plain->impl(), ImageView::ShadowStorage) == VK_NULL_HANDLE);
    CHECK(targets::GetView(shadow->impl(), ImageView::ShadowStorage) != VK_NULL_HANDLE);
    BindingArena arena(backend);
    Recorder rec = backend->BeginRecording();
    Result<BindingSet*> set = arena.Acquire(BindingSetDesc{{{0, BindingType::StorageImage}}});
    REQUIRE_MESSAGE(set.ok(), set.error());
    CHECK_FALSE((*set)->SetStorageImage(0, *plain, ImageView::ShadowStorage).ok());
    CHECK((*set)->SetStorageImage(0, *shadow, ImageView::ShadowStorage).ok());
    CHECK_FALSE((*set)->SetStorageImage(0, *shadow, ImageView::Sampled).ok());
    backend->Submit(std::move(rec));
    // a shadow source is a 32-bit colour image
    ImageDesc bad;
    bad.format = Format::R8Unorm;
    bad.width = bad.height = 4;
    bad.shadow_source = true;
    CHECK_FALSE(backend->CreateImage(bad).ok());
    backend->WaitIdle();
}

TEST_CASE("images/copies_blits_and_mip_generation_within_one_image") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 8, 8, false, 4);  // levels 8, 4, 2, 1
    CHECK(image->MipLevels() == 4);
    Recorder rec = backend->BeginRecording();
    rec.ClearImage(*image, Color(1, 0, 0, 1), 0);  // red
    rec.ClearImage(*image, Color(0, 1, 0, 1), 1);  // green
    ImageBlitRegion top_left;                       // level 1 (4x4, green) over the top-left 4x4 of level 0
    top_left.src_mip = 1;
    top_left.dst_mip = 0;
    top_left.src = Rect{0, 0, 4, 4};
    top_left.dst = Rect{0, 0, 4, 4};
    rec.Blit(*image, *image, std::span<const ImageBlitRegion>(&top_left, 1), false);
    ImageCopyRegion copy;                           // within one image: the green block to the bottom right
    copy.width = copy.height = 4;
    copy.dst_x = copy.dst_y = 4;
    rec.CopyImage(*image, *image, std::span<const ImageCopyRegion>(&copy, 1));
    ImageBlitRegion down;                           // level 0 (8x8) to level 2 (2x2), nearest
    down.src_mip = 0;
    down.dst_mip = 2;
    down.src = Rect{0, 0, 8, 8};
    down.dst = Rect{0, 0, 2, 2};
    rec.Blit(*image, *image, std::span<const ImageBlitRegion>(&down, 1), false);
    ImageBlitRegion mip;                            // mip generation: level 1 (green) to level 3, linear
    mip.src_mip = 1;
    mip.dst_mip = 3;
    mip.src = Rect{0, 0, 4, 4};
    mip.dst = Rect{0, 0, 1, 1};
    rec.Blit(*image, *image, std::span<const ImageBlitRegion>(&mip, 1), true);
    Run(*backend, std::move(rec));

    const std::array<u8, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
    const std::vector<u8> level0 = ReadBack(*backend, *image, 8, 8, 4, ImageAspect::Color, 0);
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool green_block = (x < 4 && y < 4) || (x >= 4 && y >= 4);
            CHECK(Texel(level0, 8, x, y) == (green_block ? green : red));
        }
    }
    // level 2 samples the middle of each 4x4 quarter: green, red / red, green
    const std::vector<u8> level2 = ReadBack(*backend, *image, 2, 2, 4, ImageAspect::Color, 2);
    CHECK(Texel(level2, 2, 0, 0) == green);
    CHECK(Texel(level2, 2, 1, 0) == red);
    CHECK(Texel(level2, 2, 0, 1) == red);
    CHECK(Texel(level2, 2, 1, 1) == green);
    CHECK(Texel(ReadBack(*backend, *image, 1, 1, 4, ImageAspect::Color, 3), 1, 0, 0) == green);
}

// ---- passes ---------------------------------------------------------------------------------------------------

namespace {

// Binds a set of the shape and writes a texture and a sampler into it (the draw samples at its pixel).
BindingSet* SampleSet(BindingArena& arena, const Image& texture, const Sampler& sampler) {
    Result<BindingSet*> set = arena.Acquire(kSampledAndSampler);
    REQUIRE_MESSAGE(set.ok(), set.error());
    Result<void> a = (*set)->SetSampledImage(0, texture, ImageView::Sampled);
    REQUIRE_MESSAGE(a.ok(), a.error());
    REQUIRE((*set)->SetSampler(1, sampler).ok());
    return *set;
}

TargetPassDesc Pass(Image* color, Image* depth = nullptr, std::array<float, 4> clear = {0, 0, 0, 1}) {
    TargetPassDesc pass;
    pass.color = color;
    pass.depth = depth;
    pass.color_load = color != nullptr ? LoadOp::Clear : LoadOp::Load;
    pass.depth_load = depth != nullptr ? LoadOp::Clear : LoadOp::Load;
    pass.clear_color = clear;
    pass.clear_depth = 1.0f;
    pass.clear_stencil = 0;
    return pass;
}

}  // namespace

TEST_CASE("passes/render_into_an_image_and_sample_it_in_the_next_pass_in_one_recorder_and_in_two") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::shared_ptr<Pipeline> fetch = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_fetch_frag), Format::Rgba8Unorm,
                                                          Format::Undefined, {}, {kSampledAndSampler});
    const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
    BindingArena arena(backend);
    for (const bool two_recorders : {false, true}) {
        std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(Pass(x.get()));
        rec.SetPipeline(*red);
        rec.Draw(3);
        if (two_recorders) {
            backend->Submit(std::move(rec));  // 6.9 A2: the producer's list is submitted before the consumer's is begun
            rec = backend->BeginRecording();
        }
        rec.BeginTargetPass(Pass(y.get(), nullptr, {0, 0, 1, 1}));  // blue, so a missing sample shows
        BindingSet* set = SampleSet(arena, *x, *sampler);
        rec.SetPipeline(*fetch);
        rec.Bind(*set, 0);
        rec.Draw(3);
        backend->Submit(std::move(rec));
        const std::array<u8, 4> red_pixel{255, 0, 0, 255};
        for (Image* image : {x.get(), y.get()}) {
            const std::vector<u8> pixels = ReadBack(*backend, *image, 4, 4, 4);
            for (u32 i = 0; i < 16; ++i) CHECK(Texel(pixels, 4, i % 4, i / 4) == red_pixel);
        }
    }
}

namespace {

std::atomic<int> gRenderPassesBegun{0};
PFN_vkCmdBeginRenderPass gRealBeginRenderPass = nullptr;
void VKAPI_PTR CountBeginRenderPass(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    ++gRenderPassesBegun;
    gRealBeginRenderPass(commands, info, contents);
}
struct CountRenderPasses {
    CountRenderPasses() {
        gRealBeginRenderPass = vkCmdBeginRenderPass;
        vkCmdBeginRenderPass = CountBeginRenderPass;
        gRenderPassesBegun = 0;
    }
    ~CountRenderPasses() { vkCmdBeginRenderPass = gRealBeginRenderPass; }
    int Count() const { return gRenderPassesBegun.load(); }
};

}  // namespace

TEST_CASE("passes/a_pass_stays_open_for_the_same_attachments_and_area_and_a_copy_ends_it") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    CountRenderPasses counter;
    Recorder rec = backend->BeginRecording();
    rec.BeginTargetPass(Pass(x.get()));
    rec.SetPipeline(*red);
    rec.Draw(3);
    rec.BeginTargetPass(Pass(x.get()));  // same attachments, same area: kept open (the clear is applied inside it)
    rec.Draw(3);
    CHECK(counter.Count() == 1);
    TargetPassDesc small = Pass(x.get());
    small.color_load = LoadOp::Load;
    small.area = Rect{0, 0, 2, 2};       // another area: another pass
    rec.BeginTargetPass(small);
    rec.Draw(3);
    CHECK(counter.Count() == 2);
    rec.BeginTargetPass(Pass(y.get()));  // other attachments
    rec.Draw(3);
    CHECK(counter.Count() == 3);
    std::shared_ptr<Buffer> buffer = MakeBuffer(*backend, 64, Heap::Readback, BufferUsage::Transfer);
    BufferImageRegion region;
    region.image_rect = Rect{0, 0, 4, 4};
    rec.CopyImageToBuffer(*y, *buffer, std::span<const BufferImageRegion>(&region, 1));  // a copy ends the open pass
    CHECK(rec.impl().in_pass == false);
    rec.BeginTargetPass(Pass(y.get()));  // so this begins another
    rec.Draw(3);
    CHECK(counter.Count() == 4);
    backend->WaitIdle(backend->Submit(std::move(rec)));
    CHECK(Texel(ReadBack(*backend, *x, 4, 4, 4), 4, 3, 3) == std::array<u8, 4>{255, 0, 0, 255});
}

TEST_CASE("passes/the_render_area_limits_the_viewport_scissor_and_clear") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    Recorder rec = backend->BeginRecording();
    TargetPassDesc pass = Pass(x.get(), nullptr, {0, 0, 1, 1});  // blue everywhere would show a clear that ignored the area
    pass.area = Rect{0, 0, 4, 4};
    rec.BeginTargetPass(pass);
    TargetPassDesc small = Pass(x.get());
    small.color_load = LoadOp::Load;
    small.area = Rect{0, 0, 2, 2};
    rec.BeginTargetPass(small);
    rec.SetPipeline(*red);
    rec.Draw(3);
    Run(*backend, std::move(rec));
    const std::vector<u8> pixels = ReadBack(*backend, *x, 4, 4, 4);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 xx = 0; xx < 4; ++xx) {
            const bool inside = xx < 2 && y < 2;
            CHECK(Texel(pixels, 4, xx, y) == (inside ? std::array<u8, 4>{255, 0, 0, 255} : std::array<u8, 4>{0, 0, 255, 255}));
        }
    }
}

TEST_CASE("passes/a_draw_after_the_feedback_copy_samples_the_targets_previous_contents") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const std::shared_ptr<Pipeline> feedback = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_feedback_frag), Format::Rgba8Unorm,
                                                             Format::Undefined, {}, {kSampledAndSampler});
    const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
    BindingArena arena(backend);
    std::shared_ptr<Image> target = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    Recorder rec = backend->BeginRecording();
    rec.BeginTargetPass(Pass(target.get(), nullptr, {1, 0, 0, 1}));  // red
    // The target is an attachment of the open pass: sampling it directly is refused, its twin is the way.
    Result<BindingSet*> direct = arena.Acquire(kSampledAndSampler);
    REQUIRE(direct.ok());
    CHECK_FALSE((*direct)->SetSampledImage(0, *target, ImageView::Sampled).ok());
    Image& twin = rec.FeedbackCopy(*target);
    CHECK(&twin != target.get());
    CHECK(&rec.FeedbackCopy(*target) == &twin);  // made once per target and kept
    CHECK(twin.GetFormat() == target->GetFormat());
    CHECK(twin.MipLevels() == 1);
    BindingSet* set = SampleSet(arena, twin, *sampler);
    rec.SetPipeline(*feedback);
    rec.Bind(*set, 0);
    rec.Draw(3);  // the pass begins again with load: red + (0, 0.4, 0, 0)
    Run(*backend, std::move(rec));
    // green: 0.4 * 255 = 102
    const std::vector<u8> pixels = ReadBack(*backend, *target, 4, 4, 4);
    for (u32 i = 0; i < 16; ++i) CHECK(Texel(pixels, 4, i % 4, i / 4) == std::array<u8, 4>{255, 102, 0, 255});
    // the twin kept the contents of the moment of the (second) copy: red, not red + green
    CHECK(Texel(ReadBack(*backend, twin, 4, 4, 4), 4, 1, 1) == std::array<u8, 4>{255, 0, 0, 255});
}

TEST_CASE("passes/the_feedback_copy_copies_every_mip_level_and_the_twin_inherits_shadow_source") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 4, 4, true, 2);
    Recorder rec = backend->BeginRecording();
    rec.ClearImage(*image, Color(1, 0, 0, 1), 0);
    rec.ClearImage(*image, Color(0, 1, 0, 1), 1);
    Image& twin = rec.FeedbackCopy(*image);
    Run(*backend, std::move(rec));
    CHECK(twin.MipLevels() == 2);
    CHECK(twin.impl().shadow_source);
    CHECK(twin.impl().HasStorageUsage());
    CHECK(Texel(ReadBack(*backend, twin, 4, 4, 4, ImageAspect::Color, 0), 4, 3, 3) == std::array<u8, 4>{255, 0, 0, 255});
    CHECK(Texel(ReadBack(*backend, twin, 2, 2, 4, ImageAspect::Color, 1), 2, 1, 1) == std::array<u8, 4>{0, 255, 0, 255});
}

TEST_CASE("passes/blended_and_depth_tested_draws_on_d24s8_and_on_the_masked_d32s8") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::D24S8}) : MakeBackend();
        if (!backend) return;
        std::shared_ptr<Image> color = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        std::shared_ptr<Image> depth = MakeImage(*backend, Format::D24S8, 4, 4);
        INFO("masked " << masked << " depth stored " << static_cast<int>(depth->GetFormat()));
        CHECK(depth->GetFormat() == (masked || !backend->Caps().d24s8 ? Format::D32S8 : Format::D24S8));

        FixedFunctionState tested;
        tested.depth_test = true;
        tested.depth_write = true;
        tested.depth_compare = CompareOp::Less;
        const Format cf = color->GetFormat(), df = depth->GetFormat();
        const auto a = MakePipeline(*backend, Spv(spv_fs_vert_z25), Spv(spv_red_frag), cf, df, tested);    // red at depth 0.25
        const auto b = MakePipeline(*backend, Spv(spv_fs_vert_z75), Spv(spv_green_frag), cf, df, tested);  // green at 0.75
        const auto c = MakePipeline(*backend, Spv(spv_fs_vert_z10), Spv(spv_blue_frag), cf, df, tested);   // blue at 0.1
        // two draws: 0.25 passes against the cleared 1.0, 0.75 fails against the 0.25 it wrote: red
        {
            Recorder rec = backend->BeginRecording();
            rec.BeginTargetPass(Pass(color.get(), depth.get()));
            rec.SetPipeline(*a);
            rec.Draw(3);
            rec.SetPipeline(*b);
            rec.Draw(3);
            Run(*backend, std::move(rec));
            const std::vector<u8> pixels = ReadBack(*backend, *color, 4, 4, 4);
            for (u32 i = 0; i < 16; ++i) CHECK(Texel(pixels, 4, i % 4, i / 4) == std::array<u8, 4>{255, 0, 0, 255});
        }
        // a third at 0.1 passes against the 0.25 on a pass that loads, and then 0.75 fails again: blue
        {
            Recorder rec = backend->BeginRecording();
            TargetPassDesc load = Pass(color.get(), depth.get());
            load.color_load = load.depth_load = LoadOp::Load;
            rec.BeginTargetPass(load);
            rec.SetPipeline(*c);
            rec.Draw(3);
            rec.SetPipeline(*b);
            rec.Draw(3);
            Run(*backend, std::move(rec));
            const std::vector<u8> pixels = ReadBack(*backend, *color, 4, 4, 4);
            for (u32 i = 0; i < 16; ++i) CHECK(Texel(pixels, 4, i % 4, i / 4) == std::array<u8, 4>{0, 0, 255, 255});
        }
        // a blended draw: source (1, 0, 0, 0.2) over (0, 0, 1, 1) with src_alpha / one_minus_src_alpha on colour,
        // one / zero on alpha: r = 0.2 -> 51, g = 0, b = 0.8 -> 204, a = 0.2 -> 51 (k / 255 exactly)
        {
            FixedFunctionState blend;
            blend.blend_enabled = true;
            blend.src_color = BlendFactor::SrcAlpha;
            blend.dst_color = BlendFactor::OneMinusSrcAlpha;
            blend.src_alpha = BlendFactor::One;
            blend.dst_alpha = BlendFactor::Zero;
            blend.depth_test = true;
            blend.depth_compare = CompareOp::Always;
            const auto translucent = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_translucent_red_frag), cf, df, blend);
            Recorder rec = backend->BeginRecording();
            rec.BeginTargetPass(Pass(color.get(), depth.get(), {0, 0, 1, 1}));
            rec.SetPipeline(*translucent);
            rec.Draw(3);
            Run(*backend, std::move(rec));
            const std::vector<u8> pixels = ReadBack(*backend, *color, 4, 4, 4);
            for (u32 i = 0; i < 16; ++i) CHECK(Texel(pixels, 4, i % 4, i / 4) == std::array<u8, 4>{51, 0, 204, 51});
        }
    }
}

// ---- the layout policy ------------------------------------------------------------------------------------------

namespace {

struct BarrierRecord {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout from = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout to = VK_IMAGE_LAYOUT_UNDEFINED;
};
std::mutex gTraceMutex;
std::vector<BarrierRecord> gBarriers;
PFN_vkCmdPipelineBarrier gRealPipelineBarrier = nullptr;

void VKAPI_PTR TracePipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                    VkDependencyFlags flags, uint32_t memory_count, const VkMemoryBarrier* memory,
                                    uint32_t buffer_count, const VkBufferMemoryBarrier* buffer, uint32_t image_count,
                                    const VkImageMemoryBarrier* image) {
    {
        std::lock_guard lock(gTraceMutex);
        for (uint32_t i = 0; i < image_count; ++i) {
            gBarriers.push_back(BarrierRecord{image[i].image, image[i].oldLayout, image[i].newLayout});
        }
    }
    gRealPipelineBarrier(commands, src, dst, flags, memory_count, memory, buffer_count, buffer, image_count, image);
}

// The (old layout, new layout) of every image barrier recorded while it lives, through volk's function pointer.
struct TraceBarriers {
    TraceBarriers() {
        gRealPipelineBarrier = vkCmdPipelineBarrier;
        vkCmdPipelineBarrier = TracePipelineBarrier;
        std::lock_guard lock(gTraceMutex);
        gBarriers.clear();
    }
    ~TraceBarriers() { vkCmdPipelineBarrier = gRealPipelineBarrier; }
    std::vector<std::pair<VkImageLayout, VkImageLayout>> Of(VkImage image) {
        std::lock_guard lock(gTraceMutex);
        std::vector<std::pair<VkImageLayout, VkImageLayout>> list;
        for (const BarrierRecord& record : gBarriers) {
            if (record.image == image) list.emplace_back(record.from, record.to);
        }
        return list;
    }
};

using Layouts = std::vector<std::pair<VkImageLayout, VkImageLayout>>;
constexpr VkImageLayout kUndef = VK_IMAGE_LAYOUT_UNDEFINED, kGeneral = VK_IMAGE_LAYOUT_GENERAL,
                        kSrc = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kDst = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        kDepth = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kPresent = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

bool Chained(const Layouts& list) {
    for (size_t i = 1; i < list.size(); ++i) {
        if (list[i].first != list[i - 1].second) return false;  // each old layout is the one the barrier before left
    }
    return list.empty() || list.front().first == kUndef;
}

}  // namespace

TEST_CASE("layouts/an_operation_moves_the_image_to_a_transfer_layout_and_back_through_plumes_barrier") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::D16Unorm);
    std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    std::shared_ptr<Image> color = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    std::shared_ptr<Image> depth = MakeImage(*backend, Format::D16Unorm, 4, 4);
    std::shared_ptr<Buffer> buffer = MakeBuffer(*backend, 64, Heap::Readback, BufferUsage::Transfer);
    BufferImageRegion region;
    region.image_rect = Rect{0, 0, 4, 4};
    TraceBarriers trace;
    {
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*x, Color(1, 0, 0, 1));
        rec.CopyImageToBuffer(*x, *buffer, std::span<const BufferImageRegion>(&region, 1));
        backend->WaitIdle(backend->Submit(std::move(rec)));
    }
    // a new image: UNDEFINED to the transfer layout (contents not kept), back to GENERAL, to the source layout, back
    CHECK(trace.Of(NativeImage(*x)) == Layouts{{kUndef, kDst}, {kDst, kGeneral}, {kGeneral, kSrc}, {kSrc, kGeneral}});
    CHECK(x->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::GENERAL);  // the layout plume holds is the true one
    {
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(Pass(color.get(), depth.get()));
        rec.SetPipeline(*red);
        rec.Draw(3);
        rec.CopyImageToBuffer(*color, *buffer, std::span<const BufferImageRegion>(&region, 1));  // ends the pass
        backend->WaitIdle(backend->Submit(std::move(rec)));
    }
    // colour: GENERAL for the pass (patch 04), a visibility barrier when the pass ends, then the copy's pair;
    // depth: plume's attachment layout for the pass and GENERAL when it ends
    CHECK(trace.Of(NativeImage(*color)) == Layouts{{kUndef, kGeneral}, {kGeneral, kGeneral}, {kGeneral, kSrc}, {kSrc, kGeneral}});
    CHECK(trace.Of(NativeImage(*depth)) == Layouts{{kUndef, kDepth}, {kDepth, kGeneral}});
    CHECK(depth->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::GENERAL);
}

TEST_CASE("layouts/a_pass_left_open_at_submit_is_settled_by_the_next_operation_on_its_images") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::D16Unorm);
    std::shared_ptr<Image> color = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    std::shared_ptr<Image> depth = MakeImage(*backend, Format::D16Unorm, 4, 4);
    TraceBarriers trace;
    Recorder rec = backend->BeginRecording();
    rec.BeginTargetPass(Pass(color.get(), depth.get()));
    rec.SetPipeline(*red);
    rec.Draw(3);
    backend->WaitIdle(backend->Submit(std::move(rec)));  // the submit ends the pass; no barrier follows it
    // the depth image is still in the attachment layout, and the layout plume holds says so
    CHECK(depth->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::DEPTH_WRITE);
    CHECK(color->impl().open_pass_in == nullptr);  // no longer an attachment of an open pass
    Recorder next = backend->BeginRecording();
    next.ClearImage(*depth, ClearValue{});
    backend->WaitIdle(backend->Submit(std::move(next)));
    const Layouts chain = trace.Of(NativeImage(*depth));
    CHECK(Chained(chain));
    CHECK(chain == Layouts{{kUndef, kDepth}, {kDepth, kDst}, {kDst, kGeneral}});
    CHECK(depth->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::GENERAL);
}

TEST_CASE("layouts/a_recorder_dropped_without_a_submission_gives_the_images_back_their_layout") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    {
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*x, Color(1, 0, 0, 1));
        CHECK(x->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::GENERAL);
    }  // dropped: the barriers were never submitted
    CHECK(x->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::UNKNOWN);
    TraceBarriers trace;
    Recorder rec = backend->BeginRecording();
    rec.ClearImage(*x, Color(0, 1, 0, 1));
    backend->WaitIdle(backend->Submit(std::move(rec)));
    CHECK(trace.Of(NativeImage(*x)) == Layouts{{kUndef, kDst}, {kDst, kGeneral}});
    CHECK(Texel(ReadBack(*backend, *x, 4, 4, 4), 4, 2, 2) == std::array<u8, 4>{0, 255, 0, 255});
}

// ---- shadow sources, samplers, texel buffers --------------------------------------------------------------------

TEST_CASE("shadow/imageStore_into_the_r32_uint_view_with_the_interlock_shader_and_without_it") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::Interlock}) : MakeBackend();
        if (!backend) return;
        if (masked) CHECK_FALSE(backend->Caps().interlock);
        // the shader the capability picks: with the interlock, else the compare-and-swap loop
        const BindingSetDesc shape{{BindingSlot{0, BindingType::StorageImage}}};
        const std::shared_ptr<Pipeline> pipeline = MakePipeline(
            *backend, Spv(spv_fs_vert), backend->Caps().interlock ? Spv(spv_shadow_interlock_frag) : Spv(spv_shadow_cas_frag),
            Format::Undefined, Format::Undefined, {}, {shape});
        std::shared_ptr<Image> shadow = MakeImage(*backend, Format::Rgba8Unorm, 4, 4, true);
        BindingArena arena(backend);
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*shadow, Color(0, 0, 0, 0));
        Result<BindingSet*> set = arena.Acquire(shape);
        REQUIRE_MESSAGE(set.ok(), set.error());
        REQUIRE((*set)->SetStorageImage(0, *shadow, ImageView::ShadowStorage).ok());
        rec.BeginPass(PassDesc{4, 4});  // a pass with no attachments
        rec.SetPipeline(*pipeline);
        rec.Bind(**set, 0);
        for (int i = 0; i < 3; ++i) rec.Draw(3);  // three overlapping full-screen draws add 1 each
        rec.EndPass();
        rec.Barrier(Stage::FragmentShader, Stage::Transfer);
        Run(*backend, std::move(rec));
        // every R32_UINT texel is 3: the RGBA8 bytes 03 00 00 00 (little endian). The layer cannot see the ordering
        // between the draws; the texels prove the result, not the ordering (rhi-spike.md 6.9).
        const std::vector<u8> bytes = ReadBack(*backend, *shadow, 4, 4, 4);
        for (u32 i = 0; i < 16; ++i) CHECK(Texel(bytes, 4, i % 4, i / 4) == std::array<u8, 4>{3, 0, 0, 0});
    }
}

TEST_CASE("samplers/a_custom_border_colour_is_returned_out_of_range_and_refused_when_the_bit_is_off") {
    for (const bool masked : {false, true}) {
        std::shared_ptr<Backend> backend = masked ? MakeBackend({Cap::CustomBorderColor}) : MakeBackend();
        if (!backend) return;
        SamplerDesc desc;
        desc.min_filter = desc.mag_filter = desc.mip_filter = Filter::Nearest;
        desc.address_u = desc.address_v = AddressMode::ClampToBorder;
        desc.custom_border_color = std::array<float, 4>{0.2f, 0.4f, 0.6f, 0.8f};
        Result<std::shared_ptr<Sampler>> sampler = backend->CreateSampler(desc);
        if (!backend->Caps().custom_border_color) {
            // the bit is off (masked, or the device lacks it): creating the sampler fails with a message, and the
            // caller emulates the colour in the shader and asks for clamp-to-edge
            REQUIRE_FALSE(sampler.ok());
            CHECK(sampler.error().find("custom border colour") != std::string::npos);
            desc.custom_border_color.reset();
            desc.address_u = desc.address_v = AddressMode::ClampToEdge;
            CHECK(backend->CreateSampler(desc).ok());
            continue;
        }
        REQUIRE_MESSAGE(sampler.ok(), sampler.error());
        const std::shared_ptr<Pipeline> border = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_border_frag), Format::Rgba8Unorm,
                                                              Format::Undefined, {}, {kSampledAndSampler});
        std::shared_ptr<Image> source = MakeImage(*backend, Format::Rgba8Unorm, 1, 1);
        std::shared_ptr<Image> target = MakeImage(*backend, Format::Rgba8Unorm, 16, 16);
        BindingArena arena(backend);
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*source, Color(0, 1, 0, 1));  // green
        rec.BeginTargetPass(Pass(target.get()));
        BindingSet* set = SampleSet(arena, *source, **sampler);
        rec.SetPipeline(*border);
        rec.Bind(*set, 0);
        rec.Draw(3);
        Run(*backend, std::move(rec));
        // uv = (x + 0.5) / 16 * 3 - 1 is inside [0, 1] for x + 0.5 in [5.33, 10.67]: x = 5 to 10, likewise y.
        // Outside it the sampler returns (0.2, 0.4, 0.6, 0.8) = (51, 102, 153, 204) of 255.
        const std::vector<u8> pixels = ReadBack(*backend, *target, 16, 16, 4);
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const bool inside = x >= 5 && x <= 10 && y >= 5 && y <= 10;
                CHECK(Texel(pixels, 16, x, y) == (inside ? std::array<u8, 4>{0, 255, 0, 255} : std::array<u8, 4>{51, 102, 153, 204}));
            }
        }
    }
}

TEST_CASE("bindings/a_texel_buffer_is_read_by_the_shader") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const BindingSetDesc shape{{BindingSlot{0, BindingType::TexelBuffer}}};
    const std::shared_ptr<Pipeline> pipeline = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_texel_frag), Format::Rgba8Unorm,
                                                            Format::Undefined, {}, {shape});
    std::shared_ptr<Buffer> texels = MakeBuffer(*backend, 16, Heap::Upload, BufferUsage::Texel);
    const u8 data[16] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160};
    std::memcpy(texels->Mapped().data(), data, 16);
    texels->Flush(0, 16);
    std::shared_ptr<Image> target = MakeImage(*backend, Format::Rgba8Unorm, 4, 1);
    BindingArena arena(backend);
    Recorder rec = backend->BeginRecording();
    rec.BeginTargetPass(Pass(target.get()));
    Result<BindingSet*> set = arena.Acquire(shape);
    REQUIRE_MESSAGE(set.ok(), set.error());
    CHECK_FALSE((*set)->SetTexelBuffer(0, *MakeBuffer(*backend, 16, Heap::Upload, BufferUsage::Uniform), Format::Rgba8Unorm).ok());
    Result<void> bound = (*set)->SetTexelBuffer(0, *texels, Format::Rgba8Unorm);
    REQUIRE_MESSAGE(bound.ok(), bound.error());
    rec.SetPipeline(*pipeline);
    rec.Bind(**set, 0);
    rec.Draw(3);
    Run(*backend, std::move(rec));
    const std::vector<u8> pixels = ReadBack(*backend, *target, 4, 1, 4);
    for (u32 i = 0; i < 16; ++i) CHECK(pixels[i] == data[i]);  // pixel x is texel x
}

// ---- per-draw bindings ------------------------------------------------------------------------------------------

namespace {

std::atomic<int> gSetsAllocated{0};
PFN_vkAllocateDescriptorSets gRealAllocateSets = nullptr;
VkResult VKAPI_PTR CountAllocateSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
    gSetsAllocated += static_cast<int>(info->descriptorSetCount);
    return gRealAllocateSets(device, info, sets);
}
struct CountSetAllocations {
    CountSetAllocations() {
        gRealAllocateSets = vkAllocateDescriptorSets;
        vkAllocateDescriptorSets = CountAllocateSets;
        gSetsAllocated = 0;
    }
    ~CountSetAllocations() { vkAllocateDescriptorSets = gRealAllocateSets; }
};

}  // namespace

TEST_CASE("bindings/a_thousand_frames_with_a_fresh_texture_binding_each_make_no_new_set_after_the_first_ones") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const BindingSetDesc shape{{BindingSlot{0, BindingType::UniformBuffer}, BindingSlot{1, BindingType::SampledImage},
                                BindingSlot{2, BindingType::Sampler}}};
    const std::shared_ptr<Pipeline> pipeline = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_bind_frag), Format::Rgba8Unorm,
                                                            Format::Undefined, {}, {shape});
    const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
    // eight textures with a red of 0, 32, ... 224 of 255, bound in turn
    std::vector<std::shared_ptr<Image>> textures;
    {
        Recorder rec = backend->BeginRecording();
        for (int k = 0; k < 8; ++k) {
            textures.push_back(MakeImage(*backend, Format::Rgba8Unorm, 1, 1));
            rec.ClearImage(*textures.back(), Color(float(k * 32) / 255.0f, 0, 0, 1));
        }
        Run(*backend, std::move(rec));
    }
    constexpr int kFrames = 1000;
    std::shared_ptr<Image> target = MakeImage(*backend, Format::Rgba8Unorm, 1, 1);
    std::shared_ptr<Buffer> out = MakeBuffer(*backend, kFrames * 4, Heap::Readback, BufferUsage::Transfer);
    Result<std::unique_ptr<Ring>> ring = backend->CreateRing(64 * 1024, BufferUsage::Uniform, "uniform ring");
    REQUIRE(ring.ok());
    BindingArena arena(backend);
    CountSetAllocations count;
    std::vector<TimelineValue> values;
    int after_10 = -1;
    for (int i = 0; i < kFrames; ++i) {
        if (i >= 2) backend->WaitIdle(values[i - 2]);  // two frames in flight
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(Pass(target.get()));
        Result<RingAllocation> block = (*ring)->Map(16, backend->Caps().uniform_buffer_offset_alignment);
        REQUIRE_MESSAGE(block.ok(), block.error());
        const u32 value = (u32(i) * 37 + 11) % 256;
        const u32 words[4] = {value, 0, 0, 0};
        std::memcpy(block->data.data(), words, 16);
        REQUIRE((*ring)->Commit(16).ok());
        Result<BindingSet*> set = arena.Acquire(shape);
        REQUIRE_MESSAGE(set.ok(), set.error());
        REQUIRE((*set)->SetUniformBuffer(0, *(*ring)->GetBuffer(), block->offset, 16).ok());  // the offset changes per draw
        REQUIRE((*set)->SetSampledImage(1, *textures[i % 8], ImageView::Sampled).ok());       // a texture binding each frame
        REQUIRE((*set)->SetSampler(2, *sampler).ok());
        rec.SetPipeline(*pipeline);
        rec.Bind(**set, 0);
        rec.Draw(3);
        BufferImageRegion region;
        region.buffer_offset = u64(i) * 4;
        region.image_rect = Rect{0, 0, 1, 1};
        rec.CopyImageToBuffer(*target, *out, std::span<const BufferImageRegion>(&region, 1));
        values.push_back(backend->Submit(std::move(rec)));
        if (i == 9) after_10 = gSetsAllocated.load();
    }
    backend->WaitIdle();
    const int after_1000 = gSetsAllocated.load();
    CHECK(after_10 > 0);
    CHECK(after_1000 == after_10);  // as many plume descriptor-set objects after frame 1,000 as after frame 10
    CHECK(after_1000 <= 5);         // two frames in flight, the one being recorded
    std::vector<u8> bytes(kFrames * 4);
    REQUIRE(backend->ReadBuffer(*out, 0, bytes).ok());
    int wrong = 0;
    for (int i = 0; i < kFrames; ++i) {
        // red: the bound texture's red; green: the value the draw read at its uniform offset; B 0, A 255
        const std::array<u8, 4> want{u8((i % 8) * 32), u8((u32(i) * 37 + 11) % 256), 0, 255};
        wrong += (Texel(bytes, kFrames, i, 0) == want) ? 0 : 1;
    }
    CHECK(wrong == 0);
}

// ---- the owner rule ---------------------------------------------------------------------------------------------

#define REQUIRE_CHECKS_COMPILED_IN()                                                                                   \
    do {                                                                                                               \
        if (!kRhiChecks) {                                                                                             \
            WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");              \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

TEST_CASE("owner_rule/a_copy_recorded_by_a_thread_that_does_not_own_the_image_calls_the_handler") {
    REQUIRE_CHECKS_COMPILED_IN();
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    BreachCapture breaches;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);  // owned by this thread
    {
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*image, Color(1, 0, 0, 1));
        backend->WaitIdle(backend->Submit(std::move(rec)));
    }
    CHECK(breaches.Total() == 0);  // the owner records without a call
    std::shared_ptr<Buffer> buffer = MakeBuffer(*backend, 64, Heap::Readback, BufferUsage::Transfer);
    BufferImageRegion region;
    region.image_rect = Rect{0, 0, 4, 4};
    std::thread other([&] {
        Recorder rec = backend->BeginRecording();
        rec.CopyImageToBuffer(*image, *buffer, std::span<const BufferImageRegion>(&region, 1));
        backend->Submit(std::move(rec));
    });
    other.join();
    CHECK(breaches.Count(OwnerRule::ForeignThread) == 1);
    CHECK(breaches.Total() == 1);
    backend->WaitIdle();
}

TEST_CASE("owner_rule/a_hand_over_of_an_attachment_of_an_open_pass_or_of_a_depth_target_calls_the_handler") {
    REQUIRE_CHECKS_COMPILED_IN();
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    BreachCapture breaches;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::D16Unorm);
    std::shared_ptr<Image> color = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    std::shared_ptr<Image> depth = MakeImage(*backend, Format::D16Unorm, 4, 4);
    const std::thread::id someone;  // any other thread's id: a refused hand-over does not use it
    Recorder rec = backend->BeginRecording();
    rec.BeginTargetPass(Pass(color.get(), depth.get()));
    rec.SetPipeline(*red);
    rec.Draw(3);
    HandOver(*color, someone, Wait{1, Stage::Transfer});  // an attachment of the open pass
    CHECK(breaches.Count(OwnerRule::HandOver) == 1);
    HandOver(*depth, someone, Wait{1, Stage::Transfer});  // a depth-stencil target
    CHECK(breaches.Count(OwnerRule::HandOver) == 2);
    backend->WaitIdle(backend->Submit(std::move(rec)));
    // after the submission the colour image is no longer an attachment of an open pass: it can be handed over;
    // the depth image never can
    HandOver(*color, someone, Wait{1, Stage::Transfer});
    CHECK(breaches.Count(OwnerRule::HandOver) == 2);
    HandOver(*depth, someone, Wait{1, Stage::Transfer});
    CHECK(breaches.Count(OwnerRule::HandOver) == 3);
    CHECK(breaches.Total() == 3);
}

namespace {

// What the receiving thread of a hand-over does with the image `x` (made and rendered by the calling thread).
enum class Receiver { RecordsOn, SamplesWithWait, SamplesWithoutWait };

}  // namespace

TEST_CASE("owner_rule/after_a_hand_over_the_receiver_records_when_its_submission_carries_the_wait") {
    REQUIRE_CHECKS_COMPILED_IN();
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    BreachCapture breaches;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::shared_ptr<Pipeline> fetch = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_fetch_frag), Format::Rgba8Unorm,
                                                          Format::Undefined, {}, {kSampledAndSampler});
    const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
    for (const Receiver kind : {Receiver::SamplesWithWait, Receiver::SamplesWithoutWait, Receiver::RecordsOn}) {
        const size_t base = breaches.Total();
        const int base_required = breaches.Count(OwnerRule::RequiredWait);
        std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);  // the receiver's own
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(Pass(x.get(), nullptr, {1, 0, 0, 1}));
        const TimelineValue produced = [&] {
            rec.SetPipeline(*red);
            rec.Draw(3);
            return backend->Submit(std::move(rec));
        }();
        std::promise<std::thread::id> id;
        std::promise<void> handed;
        std::thread receiver([&] {
            // y belongs to this thread: it makes it here
            std::shared_ptr<Image> mine_y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
            id.set_value(std::this_thread::get_id());
            handed.get_future().wait();
            BindingArena arena(backend);
            Recorder mine = backend->BeginRecording();
            if (kind == Receiver::RecordsOn) {
                mine.ClearImage(*x, Color(0, 1, 0, 1));  // the first record on the image takes it over
            } else {
                mine.BeginTargetPass(Pass(mine_y.get(), nullptr, {0, 0, 1, 1}));
                BindingSet* set = SampleSet(arena, *x, *sampler);  // the first sample takes it over
                mine.SetPipeline(*fetch);
                mine.Bind(*set, 0);
                mine.Draw(3);
            }
            // the receiver's first use added the hand-over's wait (the value and the stage it was given) to the recorder
            const bool one_wait = mine.impl().required_waits.size() == 1;
            CHECK(one_wait);  // (no REQUIRE on a thread of its own: it would end the process)
            if (!one_wait) return;
            CHECK(mine.impl().required_waits[0].value == produced);
            CHECK(mine.impl().required_waits[0].stage == Stage::FragmentShader);
            if (kind == Receiver::RecordsOn) {
                return;  // dropped: nothing to submit, the bookkeeping is what is being checked
            }
            TimelineValue value = 0;
            if (kind == Receiver::SamplesWithWait) {
                const Wait wait{produced, Stage::FragmentShader};
                value = core::SubmitRecorder(backend->impl(), std::move(mine), std::span<const Wait>(&wait, 1));
            } else {
                value = backend->Submit(std::move(mine));  // no wait to cover it
            }
            backend->WaitIdle(value);
            CHECK(Texel(ReadBack(*backend, *mine_y, 4, 4, 4), 4, 3, 3) == std::array<u8, 4>{255, 0, 0, 255});
        });
        // the sender hands the image over, on the value its submission returned and the stage that will read it
        HandOver(*x, id.get_future().get(), Wait{produced, Stage::FragmentShader});
        handed.set_value();
        receiver.join();
        CHECK(breaches.Count(OwnerRule::RequiredWait) - base_required == (kind == Receiver::SamplesWithoutWait ? 1 : 0));
        CHECK(breaches.Total() - base == (kind == Receiver::SamplesWithoutWait ? 1u : 0u));
        backend->WaitIdle();
        // the image now belongs to the receiver: this thread recording on it is a breach
        const size_t before = breaches.Total();
        Recorder again = backend->BeginRecording();
        again.ClearImage(*x, Color(0, 1, 0, 1));
        backend->Submit(std::move(again));
        CHECK(breaches.Total() == before + 1);
        backend->WaitIdle();
    }
    CHECK(breaches.Count(OwnerRule::ForeignThread) == 3);
}

TEST_CASE("threads/two_threads_each_own_and_render_into_their_images") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    BreachCapture breaches;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t] {
            std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
            std::shared_ptr<Buffer> buffer = MakeBuffer(*backend, 64, Heap::Readback, BufferUsage::Transfer);
            for (int round = 0; round < 25; ++round) {
                Recorder rec = backend->BeginRecording();
                rec.BeginTargetPass(Pass(image.get(), nullptr, {0, float(t), 1, 1}));  // blue or cyan, then red over the left
                TargetPassDesc small = Pass(image.get());
                small.color_load = LoadOp::Load;
                small.area = Rect{0, 0, 2, 4};
                rec.BeginTargetPass(small);
                rec.SetPipeline(*red);
                rec.Draw(3);
                BufferImageRegion region;
                region.image_rect = Rect{0, 0, 4, 4};
                rec.CopyImageToBuffer(*image, *buffer, std::span<const BufferImageRegion>(&region, 1));
                const TimelineValue value = backend->Submit(std::move(rec));
                std::vector<u8> bytes(64);
                if (!backend->ReadBuffer(*buffer, 0, bytes, value).ok()) {
                    ++wrong;
                    continue;
                }
                wrong += Texel(bytes, 4, 0, 0) == std::array<u8, 4>{255, 0, 0, 255} ? 0 : 1;
                wrong += Texel(bytes, 4, 3, 3) == std::array<u8, 4>{0, u8(t * 255), 255, 255} ? 0 : 1;
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(wrong.load() == 0);
    CHECK(breaches.Total() == 0);
}

// ---- external images --------------------------------------------------------------------------------------------

TEST_CASE("external/a_wrapped_image_is_cleared_copied_and_not_destroyed_with_its_wrapper") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    const VulkanHandles handles = Handles(*backend);
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {8, 8, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage vk_image = VK_NULL_HANDLE;
    REQUIRE(vkCreateImage(handles.device, &info, nullptr, &vk_image) == VK_SUCCESS);
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(handles.device, vk_image, &requirements);
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(handles.physical_device, &memory);
    u32 type = 0;
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) != 0) {
            type = i;
            if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) break;
        }
    }
    VkMemoryAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    VkDeviceMemory device_memory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(handles.device, &allocate, nullptr, &device_memory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(handles.device, vk_image, device_memory, 0) == VK_SUCCESS);

    TraceBarriers trace;
    for (int round = 0; round < 2; ++round) {  // a second wrapper of the same VkImage: the first did not destroy it
        Result<std::shared_ptr<Image>> wrapped = WrapExternalImage(*backend, vk_image, Format::Rgba8Unorm, 8, 8);
        REQUIRE_MESSAGE(wrapped.ok(), wrapped.error());
        CHECK(NativeImage(**wrapped) == vk_image);
        CHECK((*wrapped)->Width() == 8);
        CHECK((*wrapped)->GetFormat() == Format::Rgba8Unorm);
        CHECK((*wrapped)->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::UNKNOWN);  // first operation: out of UNDEFINED
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(**wrapped, Color(0.2f, 0.4f, 0.6f, 0.8f));  // 51, 102, 153, 204 of 255
        backend->WaitIdle(backend->Submit(std::move(rec)));
        CHECK((*wrapped)->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::GENERAL);
        // a new wrapper holds UNKNOWN: its first operation leaves UNDEFINED (no contents kept), then GENERAL
        const Layouts chain = trace.Of(vk_image);
        CHECK(Chained(Layouts(chain.end() - 2, chain.end())));
        CHECK(Layouts(chain.end() - 2, chain.end()) == Layouts{{kUndef, kDst}, {kDst, kGeneral}});
        // read it back through a second recorder
        Recorder again = backend->BeginRecording();
        again.ClearImage(**wrapped, Color(0.2f, 0.4f, 0.6f, 0.8f));
        std::shared_ptr<Buffer> buffer = MakeBuffer(*backend, 256, Heap::Readback, BufferUsage::Transfer);
        BufferImageRegion region;
        region.image_rect = Rect{0, 0, 8, 8};
        again.CopyImageToBuffer(**wrapped, *buffer, std::span<const BufferImageRegion>(&region, 1));
        const TimelineValue value = backend->Submit(std::move(again));
        std::vector<u8> bytes(256);
        REQUIRE(backend->ReadBuffer(*buffer, 0, bytes, value).ok());
        for (u32 i = 0; i < 64; ++i) CHECK(Texel(bytes, 8, i % 8, i / 8) == std::array<u8, 4>{51, 102, 153, 204});
    }
    // the VkImage is still ours to destroy, before the device goes
    backend->WaitIdle();
    vkDestroyImage(handles.device, vk_image, nullptr);
    vkFreeMemory(handles.device, device_memory, nullptr);
}

// ---- the swapchain ----------------------------------------------------------------------------------------------

// The case that needs a surface. On the RX 6700 XT the Windows loader lists no VK_EXT_headless_surface, so the
// surface there comes from a real window (docs/native/rhi.md "A surface from the caller"): this case cannot run
// there, and every other case of this file does not need a surface.
TEST_CASE("swapchain/three_frames_over_a_headless_surface") {
    InstanceOptions options;
    options.headless_surface = true;
    std::shared_ptr<Instance> instance = test::RequireInstance(options);
    if (!instance) return;
    std::unique_ptr<test::HeadlessSurface> surface = test::MakeHeadlessSurface(instance);
    if (!surface) return;
    BackendOptions backend_options;
    backend_options.surface = surface->Handle();
    std::shared_ptr<Backend> backend = test::RequireBackend(backend_options, instance);
    if (!backend) return;
    const VulkanHandles handles = Handles(*backend);
    const VkSurfaceKHR vk_surface = surface->surface;

    VkSurfaceCapabilitiesKHR capabilities = {};
    REQUIRE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(handles.physical_device, vk_surface, &capabilities) == VK_SUCCESS);
    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, vk_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(handles.physical_device, vk_surface, &format_count, formats.data());
    VkSurfaceFormatKHR chosen = {};
    Format wrapped_format = Format::Undefined;
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = candidate; wrapped_format = Format::Bgra8Unorm; break; }
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM && wrapped_format == Format::Undefined) {
            chosen = candidate;
            wrapped_format = Format::Rgba8Unorm;
        }
    }
    if (wrapped_format == Format::Undefined) {
        WARN_MESSAGE(false, "skipped: the surface offers neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM");
        return;
    }
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu) extent = VkExtent2D{64, 48};

    VkSwapchainCreateInfoKHR swapchain_info = {};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = vk_surface;
    swapchain_info.minImageCount = std::max(capabilities.minImageCount, 2u);
    swapchain_info.imageFormat = chosen.format;
    swapchain_info.imageColorSpace = chosen.colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                        : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    REQUIRE(vkCreateSwapchainKHR(handles.device, &swapchain_info, nullptr, &swapchain) == VK_SUCCESS);
    u32 image_count = 0;
    vkGetSwapchainImagesKHR(handles.device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(handles.device, swapchain, &image_count, images.data());

    // The images are the caller's swapchain's: wrapped once each and kept for as long as the swapchain lives.
    std::vector<std::shared_ptr<Image>> wrapped;
    for (VkImage image : images) {
        Result<std::shared_ptr<Image>> w = WrapExternalImage(*backend, image, wrapped_format, extent.width, extent.height);
        REQUIRE_MESSAGE(w.ok(), w.error());
        wrapped.push_back(*w);
    }
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence acquired = VK_NULL_HANDLE;
    REQUIRE(vkCreateFence(handles.device, &fence_info, nullptr, &acquired) == VK_SUCCESS);

    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore rendered = VK_NULL_HANDLE;  // binary: the render's end to the present
    REQUIRE(vkCreateSemaphore(handles.device, &semaphore_info, nullptr, &rendered) == VK_SUCCESS);
    TraceBarriers trace;
    std::vector<u32> frames_of_image(image_count, 0);
    for (int frame = 0; frame < 3; ++frame) {
        u32 index = 0;
        REQUIRE(vkAcquireNextImageKHR(handles.device, swapchain, UINT64_MAX, VK_NULL_HANDLE, acquired, &index) == VK_SUCCESS);
        REQUIRE(vkWaitForFences(handles.device, 1, &acquired, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        vkResetFences(handles.device, 1, &acquired);
        Recorder rec = backend->BeginRecording();
        rec.ClearImage(*wrapped[index], Color(float(frame) / 2.0f, 0, 1, 1));
        rec.PrepareForPresent(*wrapped[index]);  // before every present
        backend->Submit(std::move(rec));
        CHECK(wrapped[index]->impl().vulkan()->textureLayout == ::plume::RenderTextureLayout::PRESENT);
        ++frames_of_image[index];
        // The test's stand-in for what N2b-06 supplies (docs/native/rhi.md): the frame is rendered by a submission
        // that has no binary semaphore, so an empty batch submitted after it on the queue signals one (its first
        // scope is everything submitted before it) and the present waits for that. Both go under the queue lock.
        VkResult result = VK_SUCCESS;
        {
            QueueLock lock(*backend);
            VkSubmitInfo signal = {};
            signal.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            signal.signalSemaphoreCount = 1;
            signal.pSignalSemaphores = &rendered;
            REQUIRE(vkQueueSubmit(handles.queue, 1, &signal, VK_NULL_HANDLE) == VK_SUCCESS);
            VkPresentInfoKHR present = {};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &rendered;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &index;
            result = vkQueuePresentKHR(handles.queue, &present);
        }
        CHECK((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR));
        backend->WaitIdle();  // the semaphore is reused by the next frame: this one is done with it
    }
    // The held layout after a present is PRESENT_SRC_KHR; the first operation after the next acquire leaves it as
    // UNDEFINED (contents not kept), so no barrier ever names PRESENT_SRC_KHR as its old layout.
    for (u32 i = 0; i < image_count; ++i) {
        const Layouts chain = trace.Of(images[i]);
        if (frames_of_image[i] == 0) continue;
        CHECK(chain.size() == 3 * frames_of_image[i]);  // per frame: UNDEFINED to the clear's layout, back to GENERAL, to PRESENT
        for (size_t k = 0; k < chain.size(); ++k) {
            CHECK(chain[k].first != kPresent);
            if (k % 3 == 0) CHECK(chain[k] == std::make_pair(kUndef, kDst));
            if (k % 3 == 1) CHECK(chain[k] == std::make_pair(kDst, kGeneral));
            if (k % 3 == 2) CHECK(chain[k] == std::make_pair(kGeneral, kPresent));
        }
    }
    backend->WaitIdle();
    wrapped.clear();  // dropped before the swapchain goes: the backend never destroys a swapchain image
    vkDeviceWaitIdle(handles.device);
    vkDestroySemaphore(handles.device, rendered, nullptr);
    vkDestroyFence(handles.device, acquired, nullptr);
    vkDestroySwapchainKHR(handles.device, swapchain, nullptr);
}
