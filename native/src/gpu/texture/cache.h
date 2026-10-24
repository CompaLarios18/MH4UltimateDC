// The texture cache (N2-14): decoded textures keyed by physical address, format and size, kept
// valid by a content hash.
//
// Principle 2 of docs/native/ROADMAP.md: the CPU writes memory the GPU reads, and the game tells
// the GPU so with a cache flush or invalidate. A cached texture whose memory such a notification
// touches may have changed, so the next lookup hashes the memory again (XXH3, 64 bits) and
// decodes it again only if the hash differs. A texture nobody touched is returned without
// reading its memory.
//
// Safety net (docs/research/flat-memory-check.md, N0-14, has not reported yet): until it does,
// `check_all_on_notify` is on, and every flush and invalidate, whatever range it names, marks
// every cached texture as possibly changed, so a game that forgets or mis-sizes a flush cannot
// leave stale pixels behind for more than one lookup.
//
// This is the CPU side. The GPU-side copy of a texture (an image on the backend) is N2-27's and
// N2-15's; they use `generation` to know when to upload again.
#pragma once

#include <map>
#include <tuple>
#include <vector>

#include "gpu/texture/formats.h"
#include "mh4n/gpu.h"

namespace mh4n::gpu::texture {

struct CacheKey {
    u32 paddr = 0;
    Format format = Format::RGBA8;
    u32 width = 0;
    u32 height = 0;

    friend bool operator==(const CacheKey&, const CacheKey&) = default;
    friend bool operator<(const CacheKey& a, const CacheKey& b) {
        return std::tie(a.paddr, a.format, a.width, a.height) <
               std::tie(b.paddr, b.format, b.width, b.height);
    }
};

struct CachedTexture {
    CacheKey key;
    u64 content_hash = 0;       // XXH3 of the texture's bytes in memory
    u32 generation = 0;         // 1 when first decoded, +1 each time the contents changed
    std::vector<u8> rgba8;      // width * height * 4, decoded by DecodeToRgba8
    bool maybe_changed = false; // a flush or invalidate touched it since the last lookup
};

struct CacheStats {
    u64 hits = 0;       // returned without reading memory
    u64 misses = 0;     // decoded for the first time
    u64 rehashes = 0;   // hashed again because of a notification
    u64 unchanged = 0;  // ... and the hash was the same
    u64 redecodes = 0;  // ... and it differed
    u64 failures = 0;   // memory unmapped, or a size that is not a whole number of tiles
};

class TextureCache {
public:
    struct Config {
        bool check_all_on_notify = true;
    };

    explicit TextureCache(gpu::Host& host) : host_(host) {}
    TextureCache(gpu::Host& host, Config config) : host_(host), config_(config) {}

    // The decoded texture, valid until the next call that changes the cache; nullptr when its
    // memory is not mapped or its size is not valid.
    const CachedTexture* Get(const CacheKey& key);

    // The CPU wrote [paddr, paddr + size) (FlushDataCache), the CPU is about to read it
    // (InvalidateDataCache), or the GPU wrote it (a fill, a transfer, a draw into it). Physical
    // addresses.
    void OnFlush(u32 paddr, u32 size) {
        Notify(paddr, size);
    }
    void OnInvalidate(u32 paddr, u32 size) {
        Notify(paddr, size);
    }
    void OnGpuWrite(u32 paddr, u32 size) {
        Notify(paddr, size);
    }

    void Clear() {
        entries_.clear();
    }
    size_t size() const {
        return entries_.size();
    }
    const CacheStats& stats() const {
        return stats_;
    }
    const Config& config() const {
        return config_;
    }

private:
    void Notify(u32 paddr, u32 size);

    gpu::Host& host_;
    Config config_;
    std::map<CacheKey, CachedTexture> entries_;
    CacheStats stats_;
};

} // namespace mh4n::gpu::texture
