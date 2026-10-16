#include "mh4n/crc32.h"

#include <array>

namespace mh4n {
namespace {

constexpr std::array<u32, 256> MakeTable() {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<u32, 256> kTable = MakeTable();

} // namespace

u32 Crc32(const void* data, std::size_t size, u32 crc) {
    const u8* bytes = static_cast<const u8*>(data);
    u32 c = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        c = kTable[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

} // namespace mh4n
