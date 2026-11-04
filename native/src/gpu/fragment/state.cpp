// The per-draw driver of the fragment state (N2-33). Written for this package: nothing is copied from Azahar.
#include "gpu/fragment/state.h"

#include <algorithm>
#include <cstring>

#include "gpu/pica/state.h"

namespace mh4n::gpu::fragment {

namespace {

// Every table of both rings.
TableDirty AllTables() {
    TableDirty all;
    all.lighting = pica::Lighting::kAllDirty;
    all.fog = true;
    all.proctex = pica::ProcTex::kAllDirty;
    return all;
}

}  // namespace

FragmentState::FragmentState(const DeviceProfile& profile) : FragmentState(profile, FeatureRegistry::Global()) {}

FragmentState::FragmentState(const DeviceProfile& profile, const FeatureRegistry& features)
    : profile_(profile), features_(features), pending_(AllTables()) {
    std::memset(&block_, 0, sizeof(block_));
}

void FragmentState::BeginRecording() {
    pending_ = AllTables();
}

rhi::Result<FragmentState::TableStep> FragmentState::UploadPendingTables(const pica::PicaState& state, TableRings& rings) {
    TableStep step;
    if (pending_ == TableDirty{}) {
        return step;
    }
    UniformDirty dirty;
    rhi::Result<UploadSummary> uploaded = UploadTables(state, rings, block_, dirty, pending_, features_);
    if (!uploaded.ok()) {
        return rhi::Error{uploaded.error()};  // the pending tables stay pending
    }
    pending_ = TableDirty{};
    step.block_changed = dirty.changed;
    step.opened = uploaded->uploaded;
    step.summary = *uploaded;
    return step;
}

rhi::Result<FragmentState::Prepared> FragmentState::Prepare(const pica::PicaState& state, const UniformContext& context,
                                                             TableRings* rings) {
    Prepared out;
    ++draws_;

    // The key: re-extracted only when a register of the fragment blocks is dirty (or on the first draw).
    bool extract = first_;
    if (!extract) {
        for (u32 id = 0x65; id < 0x1E0 && !extract; ++id) {
            if (IsFragmentKeyRegister(id) && state.dirty.Test(id)) {
                extract = true;
            }
        }
    }
    UniformDirty dirty;
    dirty.all = first_;  // the first block holds nothing: write every member whatever the dirty bits say
    if (extract) {
        ++key_extractions_;
        const FragmentKey key = MakeKey(state, features_);
        if (StoreKey(key, block_)) {
            out.key_changed = true;
            dirty.Mark();
            needs_ = NeedsOf(key, profile_, features_);
        } else if (first_) {
            needs_ = NeedsOf(key, profile_, features_);
        }
    }

    FillUniforms(state, block_, dirty, context, features_);

    // The table flags are cleared after this draw whatever happens to it: they are kept in the pending tables until an
    // upload commits them.
    pending_.lighting |= state.lighting.lut_dirty;
    pending_.fog = pending_.fog || state.fog.lut_dirty;
    pending_.proctex = static_cast<u8>(pending_.proctex | state.proctex.table_dirty);

    if (rings != nullptr) {
        rhi::Result<TableStep> step = UploadPendingTables(state, *rings);
        if (!step.ok()) {
            return rhi::Error{step.error()};
        }
        out.tables_uploaded = step->opened;
        if (step->block_changed) {
            dirty.Mark();
        }
    }

    out.block_changed = dirty.changed || first_;
    first_ = false;
    return out;
}

// ---- FragmentUniformRing -------------------------------------------------------------------------------

FragmentUniformRing::~FragmentUniformRing() = default;

rhi::Result<std::unique_ptr<FragmentUniformRing>> FragmentUniformRing::Create(rhi::Backend& backend, u64 ring_bytes) {
    std::unique_ptr<FragmentUniformRing> self(new FragmentUniformRing());
    rhi::Result<std::unique_ptr<rhi::Ring>> ring = backend.CreateRing(ring_bytes, rhi::BufferUsage::Uniform, "fragment uniforms");
    if (!ring.ok()) {
        return rhi::Error{ring.error()};
    }
    self->ring_ = std::move(*ring);
    self->alignment_ = std::max<u64>(kFragmentBlockAlignment, backend.Caps().uniform_buffer_offset_alignment);
    return self;
}

rhi::Result<FragmentUniformRing::Allocation> FragmentUniformRing::Write(const FragmentBlock& block, bool changed) {
    if (!changed && have_last_) {
        ++reuses_;
        Allocation again = last_;
        again.reused = true;
        return again;
    }
    const u64 size = (sizeof(FragmentBlock) + alignment_ - 1) / alignment_ * alignment_;
    rhi::Result<rhi::RingAllocation> alloc = ring_->Map(size, alignment_);
    if (!alloc.ok()) {
        return rhi::Error{alloc.error()};
    }
    std::memset(alloc->data.data(), 0, alloc->data.size());
    std::memcpy(alloc->data.data(), &block, sizeof(FragmentBlock));
    rhi::Result<void> committed = ring_->Commit(size);
    if (!committed.ok()) {
        return rhi::Error{committed.error()};
    }
    last_ = Allocation{ring_->GetBuffer().get(), alloc->offset, kFragmentBlockBytes, false};
    have_last_ = true;
    ++allocations_;
    return last_;
}

} // namespace mh4n::gpu::fragment
