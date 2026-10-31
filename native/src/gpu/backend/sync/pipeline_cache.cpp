// Derived from Azahar src/video_core/renderer_vulkan/vk_pipeline_cache.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later, as the file says: "Licensed under GPLv3 or any later version")
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
//
// The pipeline-cache blob (rhi-spike.md 9.4 item 8, G10). The check is the one of PipelineCache::IsCacheValid
// (vk_pipeline_cache.cpp:593-633): a blob is used only when it holds a full header, the header's length and version
// are the ones of VkPipelineCacheHeaderVersionOne, and its vendor, device and pipelineCacheUUID are the device's.
// A driver would also ignore a foreign blob, but silently, and it may trust a corrupted one: this check is
// ours, and it is what the Info line reports. The rest (the VkPipelineCache itself, which patch 06 owns and
// every pipeline creation of the vendored backend goes through) is plume's loadPipelineCache and
// savePipelineCache. The file names and the key list are N2-16's.
#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/sync/impl.h"
#include "mh4n/log.h"

namespace mh4n::gpu::rhi {

namespace sync {

namespace {

u32 ReadU32(std::span<const u8> blob, size_t offset) {
    u32 value = 0;
    std::memcpy(&value, blob.data() + offset, sizeof(value));  // the header is in the host's byte order
    return value;
}

}  // namespace

std::string CheckPipelineCacheBlob(std::span<const u8> blob, u32 vendor_id, u32 device_id, std::span<const u8, 16> uuid) {
    if (blob.size() < kPipelineCacheHeaderSize) {
        return fmt::format("{} bytes are fewer than the {} of a header", blob.size(), kPipelineCacheHeaderSize);
    }
    const u32 header_size = ReadU32(blob, 0);
    const u32 header_version = ReadU32(blob, 4);
    if (header_size < kPipelineCacheHeaderSize || header_size > blob.size()) {
        return fmt::format("the header length {} is not valid for a blob of {} bytes", header_size, blob.size());
    }
    if (header_version != static_cast<u32>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) {
        return fmt::format("the header version {} is not {}", header_version,
                           static_cast<u32>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE));
    }
    if (const u32 vendor = ReadU32(blob, 8); vendor != vendor_id) {
        return fmt::format("the vendor id {:#x} is not the device's {:#x}", vendor, vendor_id);
    }
    if (const u32 device = ReadU32(blob, 12); device != device_id) {
        return fmt::format("the device id {:#x} is not the device's {:#x}", device, device_id);
    }
    if (std::memcmp(blob.data() + 16, uuid.data(), 16) != 0) {
        return "the pipelineCacheUUID is not the device's";
    }
    return {};
}

}  // namespace sync

bool Backend::LoadPipelineCache(std::span<const u8> blob) {
    Backend::Impl& backend = *impl_;
    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(backend.physical_device, &properties);
    const std::string refusal = sync::CheckPipelineCacheBlob(
        blob, properties.vendorID, properties.deviceID, std::span<const u8, 16>(properties.pipelineCacheUUID, 16));
    if (!refusal.empty()) {
        // The line goes through the mh4n log at Info (rhi.h's DebugSeverity has no such level), category "RHI", like
        // the core's one-time mask line. The device keeps the cache it has: the empty one made with it, when this
        // is called at start-up as it should be.
        MH4N_LOG(Info, "RHI", "rhi: pipeline cache blob ignored ({}); the device keeps its current cache", refusal);
        return false;
    }
    SyncData& data = SyncState(backend);
    std::lock_guard lock(data.cache_mutex);
    if (!backend.device->loadPipelineCache(blob.data(), blob.size())) {
        MH4N_LOG(Warning, "RHI", "rhi: the driver refused a pipeline cache blob whose header matches; the device keeps its "
                                 "current cache");
        return false;
    }
    return true;
}

std::vector<u8> Backend::SavePipelineCache() {
    Backend::Impl& backend = *impl_;
    SyncData& data = SyncState(backend);
    std::lock_guard lock(data.cache_mutex);
    std::vector<u8> blob;
    if (!backend.device->savePipelineCache(blob)) {
        blob.clear();
    }
    return blob;
}

}  // namespace mh4n::gpu::rhi
