#include "arm_recomp.h"

#include <array>
#include <cstdlib>
#include <vector>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/memory.h"
#include "fiber.h"

// The C runtime contract; included last because it defines short macros (R, C_EQ, ...).
extern "C" {
#include "recomp.h"
// Emitted by tools/recompile.py into table.c, sorted by address.
extern const RFuncEntry rc_func_table[];
extern const u32 rc_func_count;
extern const u32 rc_text_base, rc_text_size, rc_text_crc32;
}

namespace Port {

namespace {

constexpr u32 SVC_EXIT_PROCESS = 0x03;
constexpr u32 SVC_EXIT_THREAD = 0x09;
constexpr u32 CPSR_T = 1u << 5;
// Bits that recomp.h keeps outside of the separate flag words (mode, masks, E, ...).
constexpr u32 CPSR_MISC_MASK = 0x07F0FFFFu & ~CPSR_T;

/// Recompiled functions by entry address; Thumb entries have bit 0 set.
const std::unordered_map<u32, RFunc>& Functions() {
    static const std::unordered_map<u32, RFunc> table = [] {
        std::unordered_map<u32, RFunc> t;
        t.reserve(rc_func_count);
        for (u32 i = 0; i < rc_func_count; ++i) {
            t.emplace(rc_func_table[i].addr | rc_func_table[i].mode, rc_func_table[i].fn);
        }
        return t;
    }();
    return table;
}

ARM_Recomp& Self(RCtx* ctx) {
    return *static_cast<ARM_Recomp*>(ctx->user);
}

u32 Crc32(const u8* p, std::size_t n) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            }
            t[i] = c;
        }
        return t;
    }();
    u32 crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

} // namespace

struct ARM_Recomp::GuestThread {
    std::unique_ptr<Fiber> fiber;
    bool finished = false;
};

ARM_Recomp::ARM_Recomp(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id,
                       std::shared_ptr<Core::Timing::Timer> timer_)
    : ARM_Interface(core_id, std::move(timer_)), system(system_), memory(memory_),
      svc_context(system_), ctx(std::make_unique<RCtx>()) {
    *ctx = RCtx{};
    ctx->user = this;
    ctx->cpsr_misc = 0x10; // user mode
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Recomp::~ARM_Recomp() = default;

std::size_t ARM_Recomp::FunctionCount() {
    return rc_func_count;
}

// ── execution ───────────────────────────────────────────────────────────────────────

void ARM_Recomp::Run() {
    Execute(GetTimer().GetDowncount());
}

void ARM_Recomp::Step() {
    Execute(1); // runs to the end of the current basic block
}

void ARM_Recomp::Execute(s64 budget) {
    if (break_flag || HasFailed() || !loaded) {
        return;
    }
    if (!host) {
        host = Fiber::FromCurrentThread(); // the emulation thread, created lazily
        // The recompiled code is only valid for exactly the code it came from.
        std::vector<u8> text(rc_text_size);
        memory.ReadBlock(rc_text_base, text.data(), text.size());
        const u32 crc = Crc32(text.data(), text.size());
        if (crc != rc_text_crc32) {
            failure = fmt::format("the running program's code ({:08X}+{:X}, crc32 {:08X}) is not the "
                                  "code that was recompiled (crc32 {:08X})",
                                  rc_text_base, rc_text_size, crc, rc_text_crc32);
            LOG_CRITICAL(Core_ARM11, "{}", failure);
            return;
        }
    }
    auto& slot = threads[loaded];
    if (!slot) {
        // first time this thread runs
        slot = std::make_unique<GuestThread>();
        slot->fiber = Fiber::Create([this] { GuestMain(); });
    }
    // Other cores may have written memory since this core last ran: a reservation taken
    // before then must not let a STREX succeed.
    ctx->excl_valid = 0;
    reschedule = false;
    slice_start = budget;
    ctx->budget = budget;
    running = slot.get();
    running_context = loaded;
    host->SwitchTo(*running->fiber);
    if (running->finished) {
        // The thread exited; its fiber is parked for good and nothing on its stack needs
        // unwinding, so the stack can go.
        threads.erase(running_context);
    }
    running = nullptr;
    running_context = nullptr;
    SyncTicks();
}

void ARM_Recomp::GuestMain() {
    const u32 entry = ctx->r[15] | (thumb ? 1u : 0u);
    Call(entry);
    Fail(fmt::format("thread entry {:08X} returned to {:08X}", entry, ctx->r[15]));
}

void ARM_Recomp::YieldToHost() {
    running->fiber->SwitchTo(*host);
}

void ARM_Recomp::SyncTicks() {
    const s64 used = slice_start - ctx->budget;
    if (used > 0) {
        GetTimer().AddTicks(static_cast<u64>(used));
    }
    slice_start = ctx->budget;
}

void ARM_Recomp::Call(u32 target) {
    const auto& functions = Functions();
    const auto it = functions.find(target);
    if (it == functions.end()) {
        Fail(fmt::format("jump to {:08X}, which is not recompiled code (lr {:08X})", target,
                         ctx->r[14]));
    }
    it->second(ctx.get());
}

void ARM_Recomp::Svc(u32 imm, u32 next_pc) {
    SyncTicks();
    ctx->r[15] = next_pc & ~1u;
    thumb = (next_pc & 1) != 0;
    svc_context.CallSVC(imm);
    if (imm == SVC_EXIT_THREAD || imm == SVC_EXIT_PROCESS) {
        running->finished = true;
        YieldToHost();
        UNREACHABLE_MSG("exited guest thread resumed");
    }
    if (reschedule || loaded != running_context) {
        // The kernel wants another thread (or another core) to run, or already switched
        // threads during the call: park this one until its context is loaded again.
        YieldToHost();
    }
    slice_start = ctx->budget = GetTimer().GetDowncount();
}

void ARM_Recomp::Budget() {
    SyncTicks();
    YieldToHost();
}

void ARM_Recomp::Fail(std::string message) {
    LOG_CRITICAL(Core_ARM11, "recompiled code: {}", message);
    failure = std::move(message);
    SetBreakFlag();
    running->finished = true;
    YieldToHost();
    std::abort(); // never resumed
}

void ARM_Recomp::PrepareReschedule() {
    reschedule = true;
}

// ── memory ──────────────────────────────────────────────────────────────────────────

void ARM_Recomp::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table_) {
    page_table = page_table_;
    ctx->pages = page_table ? page_table->GetPointerArray().data() : nullptr;
}

std::shared_ptr<Memory::PageTable> ARM_Recomp::GetPageTable() const {
    return page_table;
}

u32 ARM_Recomp::SlowRead(u32 addr, u32 size) {
    const bool crosses = (addr & Memory::CITRA_PAGE_MASK) > Memory::CITRA_PAGE_SIZE - size;
    if (size == 1) {
        return memory.Read8(addr);
    }
    if (crosses) {
        u32 v = 0;
        for (u32 i = 0; i < size; ++i) {
            v |= static_cast<u32>(memory.Read8(addr + i)) << (8 * i);
        }
        return v;
    }
    return size == 2 ? memory.Read16(addr) : memory.Read32(addr);
}

void ARM_Recomp::SlowWrite(u32 addr, u32 value, u32 size) {
    const bool crosses = (addr & Memory::CITRA_PAGE_MASK) > Memory::CITRA_PAGE_SIZE - size;
    if (size == 1) {
        memory.Write8(addr, static_cast<u8>(value));
    } else if (crosses) {
        for (u32 i = 0; i < size; ++i) {
            memory.Write8(addr + i, static_cast<u8>(value >> (8 * i)));
        }
    } else if (size == 2) {
        memory.Write16(addr, static_cast<u16>(value));
    } else {
        memory.Write32(addr, value);
    }
}

void ARM_Recomp::InvalidateCacheRange(u32 start_address, std::size_t length) {
    // Code is compiled ahead of time; nothing to invalidate. Code written at run time
    // (CROs, self-modifying code) would show up as jumps to non-recompiled addresses.
    LOG_TRACE(Core_ARM11, "ignoring code invalidation {:08X}+{:X}", start_address, length);
}

void ARM_Recomp::ClearExclusiveState() {
    ctx->excl_valid = 0;
}

// ── registers ───────────────────────────────────────────────────────────────────────

void ARM_Recomp::SetPC(u32 addr) {
    ctx->r[15] = addr;
}

u32 ARM_Recomp::GetPC() const {
    return ctx->r[15]; // exact at system calls, which is when the kernel asks
}

u32 ARM_Recomp::GetReg(int index) const {
    return ctx->r[index];
}

void ARM_Recomp::SetReg(int index, u32 value) {
    ctx->r[index] = value;
}

u32 ARM_Recomp::GetVFPReg(int index) const {
    return index < 32 ? ctx->f.sw[index] : 0;
}

void ARM_Recomp::SetVFPReg(int index, u32 value) {
    if (index < 32) {
        ctx->f.sw[index] = value;
    }
}

u32 ARM_Recomp::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return ctx->fpscr;
    case VFP_FPEXC:
        return fpexc;
    default:
        return 0;
    }
}

void ARM_Recomp::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        ctx->fpscr = value;
        break;
    case VFP_FPEXC:
        fpexc = value;
        break;
    default:
        break;
    }
}

u32 ARM_Recomp::GetCPSR() const {
    return rc_get_cpsr(ctx.get()) | (thumb ? CPSR_T : 0);
}

void ARM_Recomp::SetCPSR(u32 cpsr) {
    rc_set_cpsr(ctx.get(), cpsr, 8 | 4);
    ctx->cpsr_misc = cpsr & CPSR_MISC_MASK;
    thumb = (cpsr & CPSR_T) != 0;
}

u32 ARM_Recomp::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return ctx->tpidrurw;
    case CP15_THREAD_URO:
        return ctx->tpidruro;
    default:
        return 0;
    }
}

void ARM_Recomp::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        ctx->tpidrurw = value;
        break;
    case CP15_THREAD_URO:
        ctx->tpidruro = value;
        break;
    default:
        break;
    }
}

void ARM_Recomp::SaveContext(ThreadContext& tc) {
    for (int i = 0; i < 16; ++i) {
        tc.cpu_registers[i] = ctx->r[i];
    }
    tc.cpsr = GetCPSR();
    for (int i = 0; i < 64; ++i) {
        tc.fpu_registers[i] = i < 32 ? ctx->f.sw[i] : 0;
    }
    tc.fpscr = ctx->fpscr;
    tc.fpexc = fpexc;
}

void ARM_Recomp::LoadContext(const ThreadContext& tc) {
    for (int i = 0; i < 16; ++i) {
        ctx->r[i] = tc.cpu_registers[i];
    }
    SetCPSR(tc.cpsr);
    for (int i = 0; i < 32; ++i) {
        ctx->f.sw[i] = tc.fpu_registers[i];
    }
    ctx->fpscr = tc.fpscr;
    fpexc = tc.fpexc;
    loaded = &tc;
}

} // namespace Port

// ── runtime callbacks for the generated code ────────────────────────────────────────

extern "C" {

void rc_call(RCtx* ctx, u32 target) {
    Port::Self(ctx).Call(target);
}

void rc_svc(RCtx* ctx, u32 imm, u32 next_pc) {
    Port::Self(ctx).Svc(imm, next_pc);
}

void rc_trap(RCtx* ctx, u32 pc, u32 kind) {
    static const char* const kinds[] = {"?", "undefined instruction", "unsupported instruction",
                                        "breakpoint", "bad jump"};
    Port::Self(ctx).Fail(fmt::format("{} at {:08X}", kinds[kind < 5 ? kind : 0], pc));
}

void rc_bad_return(RCtx* ctx, u32 got, u32 expected) {
    Port::Self(ctx).Fail(fmt::format("returned to {:08X}, expected {:08X}", got, expected));
}

void rc_budget(RCtx* ctx) {
    Port::Self(ctx).Budget();
}

u32 rc_slow_read(RCtx* ctx, u32 a, u32 size) {
    return Port::Self(ctx).SlowRead(a, size);
}

void rc_slow_write(RCtx* ctx, u32 a, u32 v, u32 size) {
    Port::Self(ctx).SlowWrite(a, v, size);
}

} // extern "C"
