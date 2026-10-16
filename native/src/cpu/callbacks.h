// The callbacks of RHost (tools/recomp/runtime/recomp.h) as the code host implements them (N1-04).
// Internal to the module; cpu_test.cpp calls them directly to reach the failure paths that need a
// malformed program.
#pragma once

#include "mh4n/cpu.h"

namespace mh4n {

// Each one finds its thread through ctx->user, which GuestThread sets to itself.
struct CodeHostCallbacks {
    static void Call(RCtx* ctx, u32 target);
    static void Svc(RCtx* ctx, u32 imm, u32 next_pc);
    static void Trap(RCtx* ctx, u32 pc, u32 kind);
    static void BadReturn(RCtx* ctx, u32 got, u32 expected);
    static void Budget(RCtx* ctx);
    static u32 SlowRead(RCtx* ctx, u32 address, u32 size);
    static void SlowWrite(RCtx* ctx, u32 address, u32 value, u32 size);
};

}  // namespace mh4n
