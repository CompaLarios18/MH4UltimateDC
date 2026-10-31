// The sync module's own interface (N2-28), beside rhi.h: what rhi.h does not declare because only the module's
// tests and the readback module use it. Namespace mh4n::gpu::rhi::sync.
//
// The members of Backend and Recorder this module defines (Submit with waits, Retire, LoadPipelineCache,
// SavePipelineCache, FrameGpuTimeNs, MarkFrame) are documented in rhi.h and docs/native/rhi.md.
#pragma once

#include <span>
#include <string>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::rhi::sync {

// Objects retired and not yet released (their value has not been seen to pass by the last drain).
size_t RetiredCount(Backend& backend);
// Releases every retired object whose value the GPU has passed. Retire, Submit with waits and MarkFrame(Begin)
// do it; this is for the code that wants it now and for the tests.
void DrainRetired(Backend& backend);
// WaitIdle, then releases everything retired. Call it before dropping the last reference to the Backend: a retired
// object that is a Buffer, Image or BindingSet holds the backend's state, so a list that still holds one when the
// last outside reference goes keeps the backend (and the device) alive.
void ReleaseRetired(Backend& backend);

// The raw ticks of the last frame that was read, converted to nanoseconds (the device's timestamp period):
// the Begin mark and the End mark. Both 0 before the first frame has been read. Reads finished frames first.
struct FrameTimestamps {
    u64 begin_ns = 0;
    u64 end_ns = 0;
};
FrameTimestamps LastFrameTimestamps(Backend& backend);
// Frames that hold a query pool at this moment (begun and not yet read, or being read).
size_t FramesInFlight(Backend& backend);

// The pipeline-cache blob check as a pure function: "" when the blob's header is a Vulkan pipeline-cache header
// of version one for this vendor, device and pipelineCacheUUID, else the reason it is refused.
inline constexpr size_t kPipelineCacheHeaderSize = 32;  // VkPipelineCacheHeaderVersionOne: 4 x u32 + 16 bytes
std::string CheckPipelineCacheBlob(std::span<const u8> blob, u32 vendor_id, u32 device_id, std::span<const u8, 16> uuid);

}  // namespace mh4n::gpu::rhi::sync
