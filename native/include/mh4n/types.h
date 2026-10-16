// Fixed-width integer aliases and the 3DS result code (N1-01).
#pragma once

#include <cstdint>

namespace mh4n {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// A 3DS result code. Bit layout (3dbrew "Error codes"; Azahar's core/hle/result.h):
//   description bits 0-9, module bits 10-17, summary bits 21-26, level bits 27-31.
// The top bit of the level marks an error.
using Result = u32;

inline constexpr u32 kResultDescriptionBits = 10;
inline constexpr u32 kResultModuleBits = 8;
inline constexpr u32 kResultSummaryBits = 6;
inline constexpr u32 kResultLevelBits = 5;

// Fields wider than their bits are masked, like Azahar's BitField::FormatValue.
constexpr Result MakeResult(u32 description, u32 module, u32 summary, u32 level) {
    return (description & ((1u << kResultDescriptionBits) - 1u)) |
           ((module & ((1u << kResultModuleBits) - 1u)) << 10) |
           ((summary & ((1u << kResultSummaryBits) - 1u)) << 21) |
           ((level & ((1u << kResultLevelBits) - 1u)) << 27);
}

constexpr bool IsError(Result result) {
    return (result >> 31) != 0;
}

constexpr bool IsSuccess(Result result) {
    return !IsError(result);
}

constexpr u32 ResultDescription(Result result) {
    return result & ((1u << kResultDescriptionBits) - 1u);
}
constexpr u32 ResultModule(Result result) {
    return (result >> 10) & ((1u << kResultModuleBits) - 1u);
}
constexpr u32 ResultSummary(Result result) {
    return (result >> 21) & ((1u << kResultSummaryBits) - 1u);
}
constexpr u32 ResultLevel(Result result) {
    return result >> 27;
}

} // namespace mh4n
