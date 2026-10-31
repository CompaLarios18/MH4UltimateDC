// UploadToImage (N2-27).
#include "gpu/backend/targets/upload.h"

#include <cstring>

#include <fmt/format.h>

#include "gpu/backend/targets/impl.h"

namespace mh4n::gpu::rhi {

Result<void> UploadToImage(Backend& backend, Recorder& recorder, Image& image, const BufferImageRegion& region,
                           std::span<const u8> data) {
    if (data.empty()) {
        return Error{"UploadToImage: no data"};
    }
    TargetsData& targets = Data(backend.impl());
    {
        // Staging buffers of earlier submissions whose value has passed are released here.
        const TimelineValue completed = backend.CompletedValue();
        std::lock_guard lock(targets.staging_mutex);
        std::erase_if(targets.staging_pending, [&](const auto& entry) { return entry.first <= completed; });
    }
    BufferDesc desc;
    desc.size = data.size();
    desc.heap = Heap::Upload;
    desc.usage = BufferUsage::Transfer;
    desc.name = fmt::format("staging for {}", image.impl().requested.name);
    Result<std::shared_ptr<Buffer>> staging = backend.CreateBuffer(desc);
    if (!staging.ok()) {
        return Error{staging.error()};
    }
    std::memcpy((*staging)->Mapped().data(), data.data(), data.size());
    (*staging)->Flush(0, data.size());
    ++targets.staging_uploads;
    BufferImageRegion copy = region;
    copy.buffer_offset = 0;
    recorder.CopyBufferToImage(**staging, image, std::span<const BufferImageRegion>(&copy, 1));
    Data(recorder.impl()).staging.push_back(*staging);
    return Result<void>{};
}

u64 StagingUploadCount(Backend& backend) {
    return Data(backend.impl()).staging_uploads.load();
}

} // namespace mh4n::gpu::rhi
