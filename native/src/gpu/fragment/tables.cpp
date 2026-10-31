// Derived from Azahar src/video_core/renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTsLF and
// SyncAndUploadLUTs: one Map of the worst case per ring, the "invalidate" rule after a wrap, the commit of
// the bytes used) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: the file says "Licensed under
// GPLv3 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_rasterizer.cpp)
#include "gpu/fragment/tables.h"

#include <array>

#include "gpu/fragment/feature.h"
#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {

struct TableRings::Impl {
    struct Side {
        std::unique_ptr<rhi::Ring> ring;
        std::span<u8> mapped;
        u64 offset = 0;      // of the open session in the buffer
        u64 used = 0;        // bytes taken in the session
        bool open = false;
        u64 wraps = 0;
    };
    std::array<Side, 2> sides;
};

TableRings::TableRings() : impl_(std::make_unique<Impl>()) {}
TableRings::~TableRings() = default;

rhi::Result<std::unique_ptr<TableRings>> TableRings::Create(rhi::Backend& backend, const TableRingsDesc& desc) {
    std::unique_ptr<TableRings> rings(new TableRings());
    const u64 sizes[2] = {desc.lf_bytes, desc.general_bytes};
    const char* names[2] = {"fragment tables lf", "fragment tables general"};
    for (int i = 0; i < 2; ++i) {
        if (sizes[i] < (i == 0 ? kLfRingMaxUpload : kGeneralRingMaxUpload)) {
            return rhi::Error{"TableRings: a ring is smaller than one worst-case upload"};
        }
        rhi::Result<std::unique_ptr<rhi::Ring>> ring = backend.CreateRing(sizes[i], rhi::BufferUsage::Texel, names[i]);
        if (!ring.ok()) {
            return rhi::Error{ring.error()};
        }
        rings->impl_->sides[i].ring = std::move(*ring);
    }
    return rings;
}

const rhi::Buffer& TableRings::Buffer(TableRing ring) const {
    return *impl_->sides[static_cast<size_t>(ring)].ring->GetBuffer();
}

rhi::Result<bool> TableRings::Begin(TableRing which, u64 max_bytes) {
    Impl::Side& side = impl_->sides[static_cast<size_t>(which)];
    if (side.open) {
        return rhi::Error{"TableRings::Begin: a session is already open on this ring"};
    }
    rhi::Result<rhi::RingAllocation> alloc = side.ring->Map(max_bytes, kTableAlignment);
    if (!alloc.ok()) {
        return rhi::Error{alloc.error()};
    }
    side.mapped = alloc->data;
    side.offset = alloc->offset;
    side.used = 0;
    side.open = true;
    if (alloc->wrapped) {
        ++side.wraps;
    }
    return alloc->wrapped;
}

rhi::Result<TableSlice> TableRings::Take(TableRing which, u64 bytes) {
    Impl::Side& side = impl_->sides[static_cast<size_t>(which)];
    if (!side.open) {
        return rhi::Error{"TableRings::Take: no session is open on this ring"};
    }
    if (bytes % kTableAlignment != 0 || side.used + bytes > side.mapped.size()) {
        return rhi::Error{"TableRings::Take: the slice is not a multiple of 16 bytes or does not fit the session"};
    }
    TableSlice slice;
    slice.data = side.mapped.subspan(side.used, bytes);
    slice.byte_offset = side.offset + side.used;
    side.used += bytes;
    return slice;
}

rhi::Result<void> TableRings::End(TableRing which) {
    Impl::Side& side = impl_->sides[static_cast<size_t>(which)];
    if (!side.open) {
        return rhi::Error{"TableRings::End: no session is open on this ring"};
    }
    side.open = false;
    side.mapped = {};
    return side.ring->Commit(side.used);
}

u64 TableRings::Wraps(TableRing ring) const {
    return impl_->sides[static_cast<size_t>(ring)].wraps;
}

rhi::Result<UploadSummary> UploadTables(const pica::PicaState& state, TableRings& rings, FragmentBlock& block,
                                        UniformDirty& dirty) {
    return UploadTables(state, rings, block, dirty, FeatureRegistry::Global());
}

rhi::Result<UploadSummary> UploadTables(const pica::PicaState& state, TableRings& rings, FragmentBlock& block,
                                        UniformDirty& dirty, const FeatureRegistry& features) {
    UploadSummary summary;
    TableDirty need;
    need.lighting = state.lighting.lut_dirty;
    need.fog = state.fog.lut_dirty;
    need.proctex = state.proctex.table_dirty;

    const bool lf = need.lighting != 0 || need.fog;
    const bool general = need.proctex != 0;
    if (!lf && !general) {
        return summary;
    }

    if (lf) {
        rhi::Result<bool> wrapped = rings.Begin(TableRing::Lf, kLfRingMaxUpload);
        if (!wrapped.ok()) {
            return rhi::Error{wrapped.error()};
        }
        if (*wrapped) {
            need.lighting = pica::Lighting::kAllDirty;
            need.fog = true;
            summary.lf_wrapped = true;
        }
    }
    if (general) {
        rhi::Result<bool> wrapped = rings.Begin(TableRing::General, kGeneralRingMaxUpload);
        if (!wrapped.ok()) {
            if (lf) {
                (void)rings.End(TableRing::Lf);
            }
            return rhi::Error{wrapped.error()};
        }
        if (*wrapped) {
            need.proctex = pica::ProcTex::kAllDirty;
            summary.general_wrapped = true;
        }
    }
    // A ring that was not opened has nothing for the features to take; they only look at what `need` asks for,
    // and `need` is zero for it.
    TableUpload upload{state, rings, block, dirty, need};
    for (const FeatureModule* feature : features.All()) {
        feature->UploadTables(upload);
    }

    rhi::Result<void> result;
    if (lf) {
        result = rings.End(TableRing::Lf);
    }
    if (general) {
        rhi::Result<void> second = rings.End(TableRing::General);
        if (result.ok()) {
            result = std::move(second);
        }
    }
    if (!result.ok()) {
        return rhi::Error{result.error()};
    }
    summary.uploaded = true;
    return summary;
}

} // namespace mh4n::gpu::fragment
