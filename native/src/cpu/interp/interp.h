// The fallback interpreter (N1-28, D13): code that was not recompiled runs here instead of stopping the
// game, and each entry it has to run is logged and appended to build/missing_entries.txt for the next
// recompilation.
//
// The instruction handlers are C generated at build time from the recompiler's own decoders
// (tools/recomp/interp_gen.py --all; the generated file is not in the repository). They work on the
// RCtx of recomp.h exactly as the code library's functions do, so interpreted and recompiled code can
// call each other through the code host.
//
// How it runs (the shape of the Azahar backend's Interpret): the code host calls Run with the address
// of a function the library has none for. Run executes one instruction at a time, from guest memory,
// until control reaches the address the recompiled caller expects (LR, or the caller's own return
// address when the function was entered by a tail call: RCtx::expect_next) with the stack pointer at or
// above where it was. Calls made by the interpreted code are interpreted too; a system call goes to the
// kernel through the code host, so it can block and switch threads like any other.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "mh4n/cpu.h"
#include "mh4n/types.h"

// The generated handlers and dispatcher (C linkage, build/<...>/ih_handlers.c).
extern "C" {
// Runs the instruction at ctx->r[15] (bit 0 set: Thumb) and leaves the next address there. Returns
// kUnpredictable when the word is one the architecture leaves UNPREDICTABLE and the nearest defined form ran.
mh4n::u32 ih_step(RCtx* ctx);
// The route the dispatcher takes for an ARM word, or for a Thumb halfword with the following one in the
// upper half (only a BL / BLX prefix looks at it): an index into ih_route_names, 0 or the last for a word
// that is unsupported.
mh4n::u32 ih_classify_arm(mh4n::u32 insn);
mh4n::u32 ih_classify_thumb(mh4n::u32 word);
mh4n::u32 ih_unpredictable_arm(mh4n::u32 insn);
mh4n::u32 ih_unpredictable_thumb(mh4n::u32 word);
void ih_exec_arm(RCtx* ctx, mh4n::u32 insn);
void ih_exec_thumb(RCtx* ctx, mh4n::u32 word);
extern const char* const ih_route_names[];
extern const mh4n::u32 ih_route_count;
}

namespace mh4n {

inline constexpr u32 kUnpredictable = 1;

class Interpreter {
public:
    // `preempt_flag` is the scheduler's flag (GuestScheduler::PreemptFlag()), tested after each
    // instruction in a library built without --deterministic; a deterministic library charges one tick
    // per instruction to RCtx::budget instead. `missing_entries_path` is the file the entries are
    // appended to ("" for none).
    Interpreter(CodeHost& host, volatile u32* preempt_flag, std::string missing_entries_path);

    // Installs Run as the host's missing-function handler.
    void Install();

    // Whether to charge one tick per instruction to RCtx::budget (what a deterministic library's own code
    // does) instead of looking at the preemption flag. The default follows the code host's library.
    void SetChargeBudget(bool charge) { charge_budget_ = charge; }

    // The missing-function handler: runs the code at `target` (Thumb bit set as in the function table)
    // until it returns. On the guest thread's fiber, in the host's FPU mode (the CodeHost sets that up).
    void Run(RCtx& ctx, u32 target);

    // Entries interpreted so far (with the Thumb bit), in the order they were first entered.
    const std::vector<u32>& Entries() const { return entries_; }
    u64 InstructionCount() const { return instructions_; }
    // Addresses (Thumb bit clear) of the UNPREDICTABLE encodings executed, once each.
    const std::vector<u32>& UnpredictableAddresses() const { return unpredictable_; }

private:
    void NoteEntry(u32 target, u32 lr);
    void NoteUnpredictable(u32 pc, bool thumb, u32 word);
    void AppendEntry(u32 target);

    CodeHost& host_;
    volatile u32* preempt_flag_;
    std::string path_;
    bool path_loaded_ = false;
    std::unordered_set<u32> recorded_;      // addresses already in the file
    std::unordered_set<u32> entry_set_;
    std::vector<u32> entries_;
    std::unordered_set<u32> unpredictable_set_;
    std::vector<u32> unpredictable_;
    u64 instructions_ = 0;
    std::optional<bool> charge_budget_;
};

}  // namespace mh4n
