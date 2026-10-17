// The fallback interpreter's loop and its glue to the code host (N1-28). The structure follows
// ARM_Recomp::Interpret of the port's own Azahar backend (arm_recomp.cpp in the port's sources): the return
// address and the stack pointer that end an interpreted call. The instruction handlers are generated C.
#include "interp.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "../callbacks.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/scheduler.h"
#include "../recomp_c.h"  // last: it defines macros

// The handlers call these where the recompiled code would call rc_svc and rc_trap (the build renames
// them, see CMakeLists.txt, so this module defines no symbol a code library or another module could
// clash with).
extern "C" {
void ih_host_svc(RCtx* ctx, u32 imm, u32 next_pc) { mh4n::CodeHostCallbacks::Svc(ctx, imm, next_pc); }
void ih_host_trap(RCtx* ctx, u32 pc, u32 kind) { mh4n::CodeHostCallbacks::Trap(ctx, pc, kind); }
}

namespace mh4n {

namespace {

constexpr const char* kLog = "Cpu";
constexpr const char* kDefaultPath = "build/missing_entries.txt";

MH4N_OPTION("runner", "--missing-entries", "FILE",
            "append the entry of each function that had to be interpreted, in the format of "
            "config/extra_entries.txt (default build/missing_entries.txt; \"\" for none)");

}  // namespace

Interpreter::Interpreter(CodeHost& host, volatile u32* preempt_flag, std::string missing_entries_path)
    : host_(host), preempt_flag_(preempt_flag), path_(std::move(missing_entries_path)) {}

void Interpreter::Install() {
    host_.SetMissingFunctionHandler([this](RCtx& ctx, u32 target) { Run(ctx, target); });
}

void Interpreter::AppendEntry(u32 target) {
    if (path_.empty()) {
        return;
    }
    if (!path_loaded_) {
        path_loaded_ = true;
        std::ifstream in(path_);
        for (std::string line; std::getline(in, line);) {
            line = line.substr(0, line.find('#'));
            std::istringstream fields(line);
            std::string address;
            if (fields >> address) {
                try {
                    recorded_.insert(static_cast<u32>(std::stoul(address, nullptr, 16)));
                } catch (const std::exception&) {
                    // not an address: leave the line alone
                }
            }
        }
    }
    const u32 address = target & ~1u;
    if (!recorded_.insert(address).second) {
        return;
    }
    std::error_code ec;
    const std::filesystem::path path(path_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path_, std::ios::app);
    out << fmt::format("0x{:08x}\t{}\tinterpreted\n", address, (target & 1u) ? "thumb" : "arm");
    out.flush();
    if (!out) {
        MH4N_LOG(Warning, kLog, "cannot append to {}", path_);
        path_.clear();
    }
}

void Interpreter::NoteEntry(u32 target, u32 lr) {
    if (!entry_set_.insert(target).second) {
        return;
    }
    entries_.push_back(target);
    MH4N_LOG(Warning, kLog, "interpreting {:08X} ({}), which is not recompiled code (lr {:08X})", target & ~1u,
             (target & 1u) ? "thumb" : "arm", lr);
    AppendEntry(target);
}

void Interpreter::NoteUnpredictable(u32 pc, bool thumb, u32 word) {
    if (!unpredictable_set_.insert(pc).second) {
        return;
    }
    unpredictable_.push_back(pc);
    MH4N_LOG(Warning, kLog, "UNPREDICTABLE {} encoding {:0{}X} at {:08X}: ran as the nearest defined instruction",
             thumb ? "Thumb" : "ARM", word, thumb ? 4 : 8, pc);
}

void Interpreter::Run(RCtx& ctx, u32 target) {
    NoteEntry(target, ctx.r[14]);
    // A tail call says where the code should come back to (the caller's own return address, which LR may
    // no longer hold); a callee that also pops its caller's frame comes back with the stack pointer above
    // where it was.
    const u32 return_to = ctx.expect_next ? ctx.expect_next : ctx.r[14];
    ctx.expect_next = 0;
    const u32 return_sp = ctx.r[13];
    const bool tick_budget =
        charge_budget_.value_or(host_.Library() != nullptr && host_.Library()->Deterministic());

    ctx.r[15] = target;
    rc_apply_rounding(ctx.fpscr);  // the host FPU follows FPSCR while guest code runs
    for (;;) {
        const u32 pc = ctx.r[15];
        if (ih_step(&ctx) & kUnpredictable) {
            const bool thumb = (pc & 1u) != 0;
            const u32 address = pc & ~1u;
            u32 word = 0;
            std::memcpy(&word, ctx.mem + address, thumb ? 2 : 4);
            NoteUnpredictable(address, thumb, word);
        }
        ++instructions_;
        if (ctx.r[15] == return_to && ctx.r[13] >= return_sp) {
            break;
        }
        if (tick_budget) {
            if ((ctx.budget -= 1) < 0) {
                CodeHostCallbacks::Budget(&ctx);
            }
        } else if (__atomic_load_n(preempt_flag_, __ATOMIC_RELAXED) != 0) {
            CodeHostCallbacks::Budget(&ctx);
        }
    }
}

// ---- the boot step ------------------------------------------------------------------------------

MH4N_REGISTER(BootStep, "cpu.interp",
              (BootStep{Phase::Started, [](Runtime& rt) {
                   // Without a code library every function is missing; a program run from a C++ entry
                   // (tests) has no guest code at all.
                   if (rt.GetCodeHost().Library() == nullptr) {
                       return;
                   }
                   auto interpreter = std::make_shared<Interpreter>(
                       rt.GetCodeHost(), rt.Scheduler().PreemptFlag(),
                       rt.GetOptions().GetString("--missing-entries", kDefaultPath));
                   interpreter->Install();
                   rt.Own(interpreter);
               }}));

}  // namespace mh4n
