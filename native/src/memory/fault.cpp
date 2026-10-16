// Bad guest accesses (N1-02): the report, the table of reservations and the guest thread context.
//
// Everything reachable from HandleFault is async-signal-safe: loads from fixed arrays and
// thread-local PODs with constant initialisation, write(2) and _exit. No fmt, no allocation, no
// locks, no stdio.
#include <atomic>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#include "host.h"

namespace mh4n {
namespace {

struct FaultContext {
    const u32* regs; // an RCtx; r[14] is the LR
    u32 thread_id;
};
thread_local FaultContext t_context = {nullptr, 0};

class Writer {
public:
    Writer(char* out, std::size_t capacity) : out_(out), capacity_(capacity) {}
    void Str(const char* s) {
        while (*s != '\0') {
            Put(*s++);
        }
    }
    void Hex32(u32 value) {
        Str("0x");
        for (int shift = 28; shift >= 0; shift -= 4) {
            Put("0123456789abcdef"[(value >> shift) & 0xF]);
        }
    }
    void Dec(u32 value) {
        char digits[10];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (n > 0) {
            Put(digits[--n]);
        }
    }
    std::size_t size() const { return size_; }

private:
    void Put(char c) {
        if (size_ < capacity_) {
            out_[size_] = c;
        }
        ++size_;
    }
    char* out_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

} // namespace

void SetFaultContext(const void* rctx, u32 thread_id) {
    t_context = {static_cast<const u32*>(rctx), thread_id};
}

void ClearFaultContext() {
    t_context = {nullptr, 0};
}

std::size_t FormatFaultReport(char* out, std::size_t capacity, u32 guest_addr, FaultAccess access,
                              bool has_context, u32 thread_id, u32 lr) {
    Writer w(out, capacity);
    w.Str("mh4n: bad guest memory access: ");
    w.Str(access == FaultAccess::Read ? "read" : access == FaultAccess::Write ? "write" : "access");
    w.Str(" at ");
    w.Hex32(guest_addr);
    if (has_context) {
        w.Str(" (guest thread ");
        w.Dec(thread_id);
        w.Str(", lr ");
        w.Hex32(lr);
        w.Str(")\n");
    } else {
        w.Str(" (no guest thread)\n");
    }
    return w.size() < capacity ? w.size() : capacity;
}

} // namespace mh4n

namespace mh4n::fault {
namespace {

constexpr std::size_t kMaxReservations = 16;
struct Slot {
    std::atomic<std::uintptr_t> base{0};
    std::atomic<std::uintptr_t> end{0};
};
Slot g_slots[kMaxReservations];

} // namespace

bool RegisterReservation(std::uintptr_t base, std::size_t size) {
    for (Slot& slot : g_slots) {
        std::uintptr_t expected = 0;
        // a slot whose end is still 0 matches nothing
        if (slot.base.compare_exchange_strong(expected, base)) {
            slot.end.store(base + size);
            return true;
        }
    }
    return false;
}

void UnregisterReservation(std::uintptr_t base) {
    for (Slot& slot : g_slots) {
        if (slot.base.load() == base) {
            slot.end.store(0);
            slot.base.store(0);
            return;
        }
    }
}

bool HandleFault(std::uintptr_t host_addr, FaultAccess access) {
    for (const Slot& slot : g_slots) {
        const std::uintptr_t base = slot.base.load();
        if (base == 0 || host_addr < base || host_addr >= slot.end.load()) {
            continue;
        }
        // A fault in the guard after the 4 GiB is reported as the address it would have been
        // (wrapped to 32 bits).
        const u32 guest_addr = static_cast<u32>(host_addr - base);
        char line[160];
        const FaultContext& ctx = t_context;
        const std::size_t n =
            FormatFaultReport(line, sizeof line, guest_addr, access, ctx.regs != nullptr,
                              ctx.thread_id, ctx.regs != nullptr ? ctx.regs[14] : 0);
#if defined(_WIN32)
        (void)!_write(2, line, static_cast<unsigned>(n));
#else
        (void)!write(2, line, n);
#endif
        _exit(3);
    }
    return false;
}

} // namespace mh4n::fault
