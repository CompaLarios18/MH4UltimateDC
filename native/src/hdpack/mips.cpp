#include "hdpack/mips.h"

#include <algorithm>

#include "hdpack/image.h"

namespace mh4n::hdpack {

std::vector<std::vector<u8>> BuildMipChain(const u8* rgba, u32 width, u32 height,
                                           bool alpha_weighted) {
    std::vector<std::vector<u8>> chain;
    chain.emplace_back(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    u32 w = width;
    u32 h = height;
    while (w > 1 || h > 1) {
        const u32 nw = std::max(1u, w >> 1);
        const u32 nh = std::max(1u, h >> 1);
        const std::vector<u8>& src = chain.back();
        std::vector<u8> dst(static_cast<std::size_t>(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y) {
            const u32 y0 = std::min(2 * y, h - 1);
            const u32 y1 = std::min(2 * y + 1, h - 1);
            for (u32 x = 0; x < nw; ++x) {
                const u32 x0 = std::min(2 * x, w - 1);
                const u32 x1 = std::min(2 * x + 1, w - 1);
                const u8* p[4] = {&src[(static_cast<std::size_t>(y0) * w + x0) * 4],
                                  &src[(static_cast<std::size_t>(y0) * w + x1) * 4],
                                  &src[(static_cast<std::size_t>(y1) * w + x0) * 4],
                                  &src[(static_cast<std::size_t>(y1) * w + x1) * 4]};
                u8* out = &dst[(static_cast<std::size_t>(y) * nw + x) * 4];
                const u32 alpha_sum = p[0][3] + p[1][3] + p[2][3] + p[3][3];
                out[3] = static_cast<u8>((alpha_sum + 2) / 4);
                for (int c = 0; c < 3; ++c) {
                    if (alpha_weighted && alpha_sum > 0) {
                        u32 sum = 0;
                        for (const u8* q : p) {
                            sum += static_cast<u32>(q[c]) * q[3];
                        }
                        out[c] = static_cast<u8>((sum + alpha_sum / 2) / alpha_sum);
                    } else {
                        out[c] = static_cast<u8>((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                    }
                }
            }
        }
        chain.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return chain;
}

} // namespace mh4n::hdpack
