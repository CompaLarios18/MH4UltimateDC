#include "hdpack/bc7.h"

#include <algorithm>
#include <mutex>

#include <bc7enc.h>

namespace mh4n::hdpack {

bool ParseBc7Quality(std::string_view name, Bc7Quality& out) {
    if (name == "fast") {
        out = {0, 0};
    } else if (name == "normal") {
        out = {0, 16};
    } else if (name == "high") {
        out = {2, 64};
    } else {
        return false;
    }
    return true;
}

void EncodeBc7(const u8* rgba, u32 width, u32 height, const Bc7Quality& quality, u8* out) {
    static std::once_flag once;
    std::call_once(once, [] { bc7enc_compress_block_init(); });

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    bc7enc_compress_block_params_init_linear_weights(&params);
    // Only modes 1 and 6, so that the archive has two block layouts to support and to test;
    // modes 5 and 7 (alpha blocks with separate channels) are not used.
    params.m_mode_mask = (1u << 1) | (1u << 6);
    params.m_uber_level = std::min(quality.uber_level, static_cast<u32>(BC7ENC_MAX_UBER_LEVEL));
    params.m_max_partitions = std::min(quality.max_partitions, static_cast<u32>(BC7ENC_MAX_PARTITIONS));

    const u32 blocks_x = (width + 3) / 4;
    const u32 blocks_y = (height + 3) / 4;
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            u8 block_pixels[16 * 4];
            for (u32 y = 0; y < 4; ++y) {
                const u32 sy = std::min(by * 4 + y, height - 1);
                for (u32 x = 0; x < 4; ++x) {
                    const u32 sx = std::min(bx * 4 + x, width - 1);
                    const u8* src = rgba + (static_cast<std::size_t>(sy) * width + sx) * 4;
                    std::copy(src, src + 4, block_pixels + (y * 4 + x) * 4);
                }
            }
            bc7enc_compress_block(out + (static_cast<std::size_t>(by) * blocks_x + bx) * 16,
                                  block_pixels, &params);
        }
    }
}

} // namespace mh4n::hdpack
