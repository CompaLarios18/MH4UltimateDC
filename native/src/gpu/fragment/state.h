// The per-draw driver of the fragment state (N2-33; docs/native/gpu-fragment-design.md 4.1 and 5.10).
//
// Written for this package from the design: nothing is copied from Azahar here.
//
// `FragmentState` is what the thread that owns the command processor calls at every `Backend::Draw`:
// it re-extracts the key only when a dirty register of the fragment blocks is set (`IsFragmentKeyRegister`),
// fills the uniforms only for the dirty bits, and collects the tables the dirty flags name into its pending tables,
// which a table step uploads. A draw whose fragment state did not change leaves `block_changed` false, and
// `FragmentUniformRing::Write` then reuses the previous ring allocation (and the composition the binding set) within
// the same recording.
//
// THE TABLES (N2-42; tables.h, THE RULE): a table's offset is valid only in the recording that committed it. The
// pending tables start at "all", gain every table flag `Prepare` sees (the command processor clears the flags after
// each draw, drawn or dropped, so they are kept only here), go back to "all" at `BeginRecording`, and are cleared only
// by an upload that committed them. `--gpu native` calls `Prepare` without rings and `UploadPendingTables` after every
// step of the draw that can submit the recording.
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

    // One call per draw, on the thread that owns `state`. The state's table flags are added to the pending tables. With
    // `rings` null nothing is uploaded (`--gpu native`, which uploads in its table step, and tests of the key and the
    // uniforms alone); with rings the calling thread's recorder must be open and the pending tables are uploaded as
    // `UploadPendingTables` does (its change is in `block_changed`; a failed upload leaves them pending). The first call
    // writes everything (and, with rings, uploads every table).
    rhi::Result<Prepared> Prepare(const pica::PicaState& state, const UniformContext& context, TableRings* rings);

    // Starts a recording: every table is pending again. Call it whenever the caller begins a recorder (next to
    // `FragmentUniformRing::BeginRecording`); without it a caller that keeps this state across recordings keeps offsets
    // into space an earlier recording committed.
    void BeginRecording();

    struct TableStep {
        bool block_changed = false;  // an offset in the block was written (the upload's `UniformDirty::changed`)
        bool opened = false;         // a session was opened and committed (something was pending)
        UploadSummary summary;
    };
    // Uploads the pending tables (`need` = the pending tables, widened by a wrap), reading their contents from `state`
    // now; opens no session when nothing is pending. The pending tables are cleared only when both rings' sessions were
    // committed; on an error they stay pending. On the thread that owns `state`, with its recorder open.
    rhi::Result<TableStep> UploadPendingTables(const pica::PicaState& state, TableRings& rings);

    const FragmentKey& key() const { return block_.cfg; }
    const Needs& needs() const { return needs_; }
    const FragmentBlock& block() const { return block_; }
    const DeviceProfile& profile() const { return profile_; }

    // Keys extracted and draws prepared, for the tests ("a draw with no dirty fragment state does not extract").
    u64 key_extractions() const { return key_extractions_; }
    u64 draws() const { return draws_; }

private:
    DeviceProfile profile_;
    const FeatureRegistry& features_;
    FragmentBlock block_{};
    Needs needs_{};
    TableDirty pending_{};  // "all" from the constructor
    bool first_ = true;
    u64 key_extractions_ = 0;
    u64 draws_ = 0;
};

// The fragment uniform block in a ring of its own (uniform usage), one allocation per change, aligned to 256 (or the
// device's own alignment when larger).
//
// LIMIT: inside ONE recording every change of the fragment state takes a new 1,792-byte block, and space committed to a
// recorder that has not been submitted cannot be waited for. A 1 MiB ring (the default) therefore holds about 585 changes
// per recording (1,048,576 / 1,792); the next `Write` that needs a new block fails with the ring's error ("the ring is
// full of space committed in a recorder that has not been submitted ... submit it first"), it does not wait. A caller
// that can have more changes than that in one recording submits and begins another (BeginRecording), or creates a
// bigger ring.
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
