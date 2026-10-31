// The per-draw driver of the fragment state (N2-33; docs/native/gpu-fragment-design.md 4.1 and 5.10).
//
// Written for this package from the design: nothing is copied from Azahar here.
//
// `FragmentState` is what the thread that owns the command processor calls at every `Backend::Draw`:
// it re-extracts the key only when a dirty register of the fragment blocks is set (`IsFragmentKeyRegister`),
// fills the uniforms only for the dirty bits, and uploads the tables the dirty flags name. A draw whose fragment
// state did not change leaves `block_changed` false, and `FragmentUniformRing::Write` then reuses the previous
// ring allocation (and the composition the binding set) within the same recording.
//
// Final names: `FragmentState`, `FragmentUniformRing`.
#pragma once

#include <memory>

#include "gpu/backend/rhi.h"
#include "gpu/fragment/feature.h"
#include "gpu/fragment/tables.h"
#include "gpu/fragment/uniforms.h"

namespace mh4n::gpu::fragment {

class FragmentState {
public:
    explicit FragmentState(const DeviceProfile& profile);
    FragmentState(const DeviceProfile& profile, const FeatureRegistry& features);

    struct Prepared {
        bool key_changed = false;    // the key differs from the previous draw's
        bool block_changed = false;  // some member of the block (uniforms or cfg) was written
        bool tables_uploaded = false;
    };

    // One call per draw, on the thread that owns `state`. `rings` may be null (no table is uploaded; for tests of the
    // key and the uniforms alone); with rings the calling thread's recorder must be open. The first call writes
    // everything.
    rhi::Result<Prepared> Prepare(const pica::PicaState& state, const UniformContext& context, TableRings* rings);

    const FragmentKey& key() const { return block_.cfg; }
    const Needs& needs() const { return needs_; }
    const FragmentBlock& block() const { return block_; }
    FragmentBlock& mutable_block() { return block_; }
    const DeviceProfile& profile() const { return profile_; }

    // Keys extracted and draws prepared, for the tests ("a draw with no dirty fragment state does not extract").
    u64 key_extractions() const { return key_extractions_; }
    u64 draws() const { return draws_; }

private:
    DeviceProfile profile_;
    const FeatureRegistry& features_;
    FragmentBlock block_{};
    Needs needs_{};
    bool first_ = true;
    u64 key_extractions_ = 0;
    u64 draws_ = 0;
};

// The fragment uniform block in a ring of its own (uniform usage), one allocation per change, aligned to 256 (or the
// device's own alignment when larger).
class FragmentUniformRing {
public:
    static rhi::Result<std::unique_ptr<FragmentUniformRing>> Create(rhi::Backend& backend, u64 ring_bytes = 1ull << 20);
    ~FragmentUniformRing();

    struct Allocation {
        const rhi::Buffer* buffer = nullptr;
        u64 offset = 0;
        u32 size = kFragmentBlockBytes;  // the range a binding covers
        bool reused = false;             // the previous allocation of this recording, not a new one
    };

    // Starts a recording: the previous allocation is forgotten (space committed in an earlier recorder can be
    // reused by the ring once that submission has passed, so it is not offered again). Call it when the caller
    // begins a recorder.
    void BeginRecording() { have_last_ = false; }

    // The block's allocation: the previous one when `changed` is false and this recording has one; otherwise a new
    // allocation with the block's bytes, committed to the calling thread's open recorder.
    rhi::Result<Allocation> Write(const FragmentBlock& block, bool changed);

    u64 allocations() const { return allocations_; }
    u64 reuses() const { return reuses_; }

private:
    FragmentUniformRing() = default;
    std::unique_ptr<rhi::Ring> ring_;
    u64 alignment_ = kFragmentBlockAlignment;
    Allocation last_{};
    bool have_last_ = false;
    u64 allocations_ = 0;
    u64 reuses_ = 0;
};

} // namespace mh4n::gpu::fragment
