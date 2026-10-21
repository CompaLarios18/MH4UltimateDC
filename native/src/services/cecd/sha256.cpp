// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), for cecd:u's message HMAC (N1-19). Written for this
// project; Azahar uses Crypto++ for the same two primitives.
#include "sha256.h"

#include <cstring>
#include <vector>

namespace mh4n::cecd {

namespace {

constexpr std::array<u32, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr u32 Rotr(u32 value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void Compress(std::array<u32, 8>& state, const u8* block) {
    std::array<u32, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] = static_cast<u32>(block[i * 4]) << 24 |
                                         static_cast<u32>(block[i * 4 + 1]) << 16 |
                                         static_cast<u32>(block[i * 4 + 2]) << 8 |
                                         static_cast<u32>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const u32 s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    u32 e = state[4], f = state[5], g = state[6], h = state[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const u32 s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + s1 + ch + kRound[i] + w[i];
        const u32 s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = s0 + maj;
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    state[0] += a, state[1] += b, state[2] += c, state[3] += d;
    state[4] += e, state[5] += f, state[6] += g, state[7] += h;
}

}  // namespace

std::array<u8, 32> Sha256(std::span<const u8> data) {
    std::array<u32, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t offset = 0;
    for (; offset + 64 <= data.size(); offset += 64) {
        Compress(state, data.data() + offset);
    }
    std::vector<u8> tail(data.begin() + static_cast<std::ptrdiff_t>(offset), data.end());
    tail.push_back(0x80);
    while (tail.size() % 64 != 56) {
        tail.push_back(0);
    }
    const u64 bits = static_cast<u64>(data.size()) * 8;
    for (int i = 7; i >= 0; --i) {
        tail.push_back(static_cast<u8>(bits >> (i * 8)));
    }
    for (std::size_t at = 0; at < tail.size(); at += 64) {
        Compress(state, tail.data() + at);
    }
    std::array<u8, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<u8>(state[i] >> 24);
        out[i * 4 + 1] = static_cast<u8>(state[i] >> 16);
        out[i * 4 + 2] = static_cast<u8>(state[i] >> 8);
        out[i * 4 + 3] = static_cast<u8>(state[i]);
    }
    return out;
}

std::array<u8, 32> HmacSha256(std::span<const u8> key, std::span<const u8> message) {
    std::array<u8, 64> block{};
    if (key.size() > block.size()) {
        const std::array<u8, 32> hashed = Sha256(key);
        std::memcpy(block.data(), hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(block.data(), key.data(), key.size());
    }
    std::vector<u8> inner(64);
    std::vector<u8> outer(64);
    for (std::size_t i = 0; i < 64; ++i) {
        inner[i] = block[i] ^ 0x36;
        outer[i] = block[i] ^ 0x5c;
    }
    inner.insert(inner.end(), message.begin(), message.end());
    const std::array<u8, 32> inner_hash = Sha256(inner);
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return Sha256(outer);
}

}  // namespace mh4n::cecd
