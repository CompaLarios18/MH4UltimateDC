// Derived from Azahar src/video_core/rasterizer_cache/sampler_params.h and rasterizer_cache.h (SamplerParams, GetSampler),
// src/video_core/renderer_vulkan/vk_texture_runtime.cpp (Sampler::Sampler), pica_to_vk.h (the wrap and filter tables),
// src/video_core/rasterizer_accelerated.cpp (the tex_lod_bias and tex_border_color uniforms),
// src/video_core/pica/regs_texturing.h (the register fields and the wrap-mode enumerators) and
// src/video_core/renderer_software/sw_texturing.cpp (GetWrappedTexCoord: what the wrap modes 4 to 7 do) and
// sw_rasterizer.cpp (TextureColor: the t flip that the row order of UploadRgba8 follows) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of those files says "Licensed under GPLv2 or any later
// version").
// Copyright 2023 Citra Emulator Project (sampler_params.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (rasterizer_cache.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_texture_runtime.cpp, pica_to_vk.h,
//   rasterizer_accelerated.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017 Citra Emulator Project (sw_texturing.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (sw_rasterizer.cpp)
//
// The host side of the `sampling/` feature (N2-23; docs/native/gpu-fragment-design.md 4.8, 5.6, 5.7):
//
//   SamplerParams / SamplerParamsOf    one texture unit's sampler state out of the PICA registers (Azahar's SamplerParams)
//   SamplerDescOf                      the backend's SamplerDesc for it, by the wrap and filter tables below
//   SamplerCache                       sampler objects made on first use, cached by value (never in a constructor: under the
//                                      capture player's `--queue` the GPU is built on another thread than the one that draws)
//   UploadRgba8                        writes decoded texels into a level and layer of a backend image
//
// WHICH ROW IS ROW 0. The PICA stores a texture bottom-up and the software renderer looks texel row `height - 1 - t` up
// (`t = height - 1 - wrapped(t)`, sw_rasterizer.cpp), so the sampled image holds the rows in the opposite order to the
// decoder's output (`texture::DecodeToRgba8` returns the memory's row 0 first). `UploadRgba8` takes texels in the DECODER's
// order ("memory order") and stores them flipped; texture coordinates are then used as they are (v = 0 is the image's row 0).
//
// THE WRAP MODES (what the software renderer does, `sw_texturing.cpp`'s GetWrappedTexCoord and the border test of
// `TextureColor`): 0 ClampToEdge, 1 ClampToBorder, 2 Repeat, 3 MirroredRepeat as their names say; 4 ClampToEdge2 is
// ClampToEdge for a positive coordinate and Repeat for a negative one, 5 ClampToBorder2 is the border colour at or beyond 1 and
// Repeat for a negative coordinate, 6 Repeat2 and 7 Repeat3 are Repeat. A sampler cannot say the mixed ones, so 4 and 5 are
// ClampToEdge samplers and the shader (sampling.cpp) repeats the negative coordinate and tests the border; mode 1 is the
// sampler's border colour where `custom_border_color` is on and the shader's test with a clamp-to-edge sampler where it is off.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <span>

#include "gpu/backend/rhi.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment::sampling {

// The wrap-mode numbers of TexturingRegs::TextureConfig::WrapMode.
inline constexpr u32 kClampToEdge = 0, kClampToBorder = 1, kRepeat = 2, kMirroredRepeat = 3;
inline constexpr u32 kClampToEdge2 = 4, kClampToBorder2 = 5, kRepeat2 = 6, kRepeat3 = 7;

// One unit's sampler state, as the registers hold it (Azahar's SamplerParams). Filters: 0 Nearest, 1 Linear.
struct SamplerParams {
    u32 mag_filter = 0;
    u32 min_filter = 0;
    u32 mip_filter = 0;
    u32 wrap_s = 0;
    u32 wrap_t = 0;
    u32 border_color = 0;  // the register word: r in bits 0-7, g 8-15, b 16-23, a 24-31
    u32 lod_min = 0;       // the PICA's min_level
    u32 lod_max = 0;       // the PICA's max_level
    friend bool operator==(const SamplerParams&, const SamplerParams&) = default;
    friend auto operator<=>(const SamplerParams&, const SamplerParams&) = default;
};

// Unit 0, 1 or 2 out of the 0x300 internal registers (`PicaState::regs`).
SamplerParams SamplerParamsOf(const u32* regs, u32 unit);
inline SamplerParams SamplerParamsOf(const pica::PicaState& state, u32 unit) { return SamplerParamsOf(state.regs.data(), unit); }

// The LOD bias of a unit (lod word bits 0-12, signed fixed 1.4.8) in levels: `tex_lod_bias[unit]`.
float LodBiasOf(u32 lod_word);

// The backend's sampler for these parameters. The border colour is taken only when `caps.custom_border_color` is on and a
// wrap is ClampToBorder (mode 1); with the bit off a ClampToBorder axis is clamp-to-edge and the shader tests the border.
// The PICA has no anisotropy and no compare sampler. `min_lod` and `max_lod` are the PICA's levels, `max_lod` raised to
// `min_lod` when the registers say less (Vulkan requires min <= max).
rhi::SamplerDesc SamplerDescOf(const SamplerParams& params, const rhi::Capabilities& caps);

// Sampler objects, cached by value. `Get` makes the sampler on first use, on the calling thread.
class SamplerCache {
public:
    explicit SamplerCache(std::shared_ptr<rhi::Backend> backend) : backend_(std::move(backend)) {}

    rhi::Result<std::shared_ptr<rhi::Sampler>> Get(const SamplerParams& params);

    size_t size() const { return samplers_.size(); }
    u64 creates() const { return creates_; }
    u64 hits() const { return hits_; }

private:
    std::shared_ptr<rhi::Backend> backend_;
    std::map<SamplerParams, std::shared_ptr<rhi::Sampler>> samplers_;
    u64 creates_ = 0;
    u64 hits_ = 0;
};

// Writes `rgba` (width * height * 4 bytes, the decoder's row order) into one mip level and layer of `image` through `staging`
// (a ring with BufferUsage::Transfer), flipping the rows as the note above says. Fails, with the ring's message, when the staging
// ring cannot hold it until the recorder is submitted: submit and call again. Records on `rec`, which must be the calling thread's
// open recorder.
rhi::Result<void> UploadRgba8(rhi::Recorder& rec, rhi::Ring& staging, rhi::Image& image, u32 mip, u32 layer, u32 width,
                              u32 height, std::span<const u8> rgba);

}  // namespace mh4n::gpu::fragment::sampling
