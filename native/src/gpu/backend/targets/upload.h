// Filling an image from host bytes (N2-27). Not part of the frozen interface: the callers that upload (the surface
// cache, the capture player) include this header. An image of this backend is never host-writable (Capabilities::
// direct_upload is reported, no path acts on it), so every upload goes through a staging buffer: with the bit on
// or masked off alike.
#pragma once

#include <span>

#include "gpu/backend/rhi.h"

namespace mh4n::gpu::rhi {

// Copies `data` (rows tightly packed, as CopyBufferToImage reads them) into `region` of `image` with a new staging
// buffer in the Upload heap, recorded on `recorder`; `region.buffer_offset` is ignored (the staging buffer starts at
// the data). The staging buffer is kept until the GPU has passed the value the recorder's submission returns.
Result<void> UploadToImage(Backend& backend, Recorder& recorder, Image& image, const BufferImageRegion& region,
                           std::span<const u8> data);

// How many staging buffers UploadToImage has made on this backend.
u64 StagingUploadCount(Backend& backend);

} // namespace mh4n::gpu::rhi
