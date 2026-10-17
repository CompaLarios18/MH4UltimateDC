#include "gpu/texture/cache.h"

// XXH3 is vendored (native/third_party/xxhash, BSD-2-Clause) and used only here, as the cache's
// own content hash; the custom-texture hashes are CityHash64 (hash.h).
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/texture/decode.h"

namespace mh4n::gpu::texture {

const CachedTexture* TextureCache::Get(const CacheKey& key) {
    if (!IsValidSize(key.width, key.height) || !IsValidFormat(static_cast<u32>(key.format))) {
        ++stats_.failures;
        return nullptr;
    }
    const std::size_t bytes = EncodedBytes(key.format, key.width, key.height);

    auto it = entries_.find(key);
    if (it != entries_.end() && !it->second.maybe_changed) {
        ++stats_.hits;
        return &it->second;
    }

    const u8* memory = host_.PhysicalPointer(key.paddr, static_cast<u32>(bytes));
    if (memory == nullptr) {
        ++stats_.failures;
        if (it != entries_.end()) {
            entries_.erase(it); // its memory is gone; do not keep a copy nobody can validate
        }
        return nullptr;
    }
    const u64 hash = XXH3_64bits(memory, bytes);

    if (it == entries_.end()) {
        CachedTexture texture;
        texture.key = key;
        texture.content_hash = hash;
        texture.generation = 1;
        if (!DecodeToRgba8(memory, bytes, key.width, key.height, key.format, texture.rgba8)) {
            ++stats_.failures;
            return nullptr;
        }
        ++stats_.misses;
        return &entries_.emplace(key, std::move(texture)).first->second;
    }

    CachedTexture& texture = it->second;
    ++stats_.rehashes;
    texture.maybe_changed = false;
    if (hash == texture.content_hash) {
        ++stats_.unchanged;
        return &texture;
    }
    if (!DecodeToRgba8(memory, bytes, key.width, key.height, key.format, texture.rgba8)) {
        ++stats_.failures;
        entries_.erase(it);
        return nullptr;
    }
    texture.content_hash = hash;
    ++texture.generation;
    ++stats_.redecodes;
    return &texture;
}

void TextureCache::Notify(u32 paddr, u32 size) {
    const u64 begin = paddr;
    const u64 end = begin + size;
    for (auto& [key, texture] : entries_) {
        if (config_.check_all_on_notify) {
            texture.maybe_changed = true;
            continue;
        }
        const u64 texture_begin = key.paddr;
        const u64 texture_end = texture_begin + EncodedBytes(key.format, key.width, key.height);
        if (begin < texture_end && texture_begin < end) {
            texture.maybe_changed = true;
        }
    }
}

} // namespace mh4n::gpu::texture
