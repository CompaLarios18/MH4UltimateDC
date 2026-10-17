// Derived from Azahar src/core/hle/kernel/thread.cpp, process.cpp, svc.cpp, resource_limit.cpp and
// kernel.cpp and src/core/hle/service/pm/pm_app.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-3.0-or-later: svc.cpp and kernel.cpp say so; thread.cpp, process.cpp, resource_limit.cpp and
// pm_app.cpp say GPL-2.0-or-later): KernelSystem::CreateThread, SetupMainThread and
// ResetThreadContext, Process::AllocateThreadLocalStorage and the thread part of Process::Run,
// SVC::CreateThread, ResourceLimit::ApplyAppMaxCPUSetting (the table of 1.0 titles included),
// PM_APP::UpdateResourceLimit / GetResourceLimit. The scheduler, the fibers and the flat memory
// they sit on are ours.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (thread.cpp, svc.cpp,
//   kernel.cpp, pm_app.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (process.cpp,
//   resource_limit.cpp)
//
// Threads (N1-27): creation, TLS and the core-1 time limit. See mh4n/threads.h.
#include "mh4n/threads.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "cpu_limiter.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"

// The registers of a guest thread (RCtx). Last: recomp.h defines short macros.
#include "../cpu/recomp_c.h"

namespace mh4n {

// ---- the CPU setting of an application ------------------------------------------------------------

Core1CpuSetting ComputeAppMaxCpuSetting(u64 program_id, u32 exh_mode, u32 exh_cpu_limit) {
    // List of 1.0 titles with max CPU time overrides. Azahar: "This is not a 'hack' list, these
    // values actually exist in the 3DS kernel." (Values above 100 and below 200 are the unused
    // multi-mode form: the percentage plus 100.)
    struct Override {
        u32 title;
        s32 time;
    };
    static constexpr Override kCpuTimeOverrides[] = {
        // 3DS sound (JPN, USA, EUR)
        {0x205, kCore1CpuTimeExempted},
        {0x215, kCore1CpuTimeExempted},
        {0x225, kCore1CpuTimeExempted},
        // Star Fox 64 3D (JPN)
        {0x304, kCore1CpuTimeExempted},
        // Super Monkey Ball 3D (JPN)
        {0x32E, kCore1CpuTimeExempted},
        // The Legend of Zelda: Ocarina of Time 3D (JPN, USA, EUR)
        {0x334, 30},
        {0x335, 30},
        {0x336, 30},
        // Doctor Lautrec to Boukyaku no Kishidan (JPN, USA, EUR)
        {0x348, kCore1CpuTimeExempted},
        {0x368, kCore1CpuTimeExempted},
        {0x562, kCore1CpuTimeExempted},
        // Pro Yakyuu Spirits 2011 (JPN)
        {0x349, kCore1CpuTimeExempted},
        // Super Monkey Ball 3D (USA, EUR)
        {0x370, kCore1CpuTimeExempted},
        {0x389, kCore1CpuTimeExempted},
        // Star Fox 64 3D (USA, EUR)
        {0x490, kCore1CpuTimeExempted},
        {0x491, kCore1CpuTimeExempted},
    };

    Core1CpuSetting setting{Core1ScheduleMode::Multi, 0, kCore1CpuTimeDisabled};
    if (exh_mode == 0 && exh_cpu_limit == 0) {
        // A 1.0 app with the CPU time value not set: the default or the hardcoded value.
        setting.mode = Core1ScheduleMode::Single;
        setting.limit_value = 80;
        setting.current_value = 25;

        const u32 title = static_cast<u32>((program_id >> 8) & 0xFFFFF);
        const auto it = std::find_if(std::begin(kCpuTimeOverrides), std::end(kCpuTimeOverrides),
                                     [title](const Override& o) { return o.title == title; });
        if (it != std::end(kCpuTimeOverrides)) {
            if (it->time > 100 && it->time < 200) {
                // Azahar: "This code path is actually unused"
                setting.mode = Core1ScheduleMode::Multi;
                setting.limit_value = setting.current_value = it->time - 100;
            } else {
                setting.mode = Core1ScheduleMode::Single;
                setting.limit_value = setting.current_value = it->time;
            }
        }
    } else {
        setting.mode = exh_mode == 0 ? Core1ScheduleMode::Single : Core1ScheduleMode::Multi;
        setting.limit_value = static_cast<s32>(exh_cpu_limit);
    }
    return setting;
}

// ---- the manager ---------------------------------------------------------------------------------

ThreadManager::ThreadManager(Kernel& kernel, GuestScheduler& scheduler)
    : kernel_(kernel),
      scheduler_(scheduler),
      limiter_(std::make_unique<Core1Limiter>(kernel, scheduler)) {
    // Azahar asks the limiter about every thread PopNextReadyThread selects on core 1.
    scheduler_.SetSelectionFilter(1, [this](Thread& candidate) {
        return limiter_->DoTimeLimit(candidate);
    });
}

ThreadManager::~ThreadManager() {
    scheduler_.SetSelectionFilter(1, {});
    limiter_.reset();
}

// ---- thread-local storage (Process::AllocateThreadLocalStorage) ------------------------------------

bool ThreadManager::TlsSlotInUse(u32 page, u32 slot) const {
    if (page >= tls_pages_.size() || slot >= kTlsSlotsPerPage) {
        return false;
    }
    const auto thread = tls_pages_[page].owner[slot].lock();
    return thread != nullptr && thread->status != ThreadStatus::Dead;
}

Result ThreadManager::AllocateThreadLocalStorage(const std::shared_ptr<Process>& process,
                                                 const std::shared_ptr<Thread>& thread) {
    std::size_t tls_page = 0;
    std::size_t tls_slot = 0;
    bool needs_allocation = true;

    // Iterate over all the allocated pages, and try to find one where not all slots are used; in
    // it, the first free slot.
    for (tls_page = 0; tls_page < tls_pages_.size(); ++tls_page) {
        for (tls_slot = 0; tls_slot < kTlsSlotsPerPage; ++tls_slot) {
            if (!TlsSlotInUse(static_cast<u32>(tls_page), static_cast<u32>(tls_slot))) {
                needs_allocation = false;
                break;
            }
        }
        if (!needs_allocation) {
            break;
        }
    }

    if (needs_allocation) {
        tls_page = tls_pages_.size();
        tls_slot = 0;
        MH4N_LOG(Debug, "Kernel", "Allocating new TLS page in slot {}", tls_page);

        // There are no already-allocated pages with free slots, lets allocate a new one. TLS pages
        // are allocated from the BASE region in the linear heap: the first free page from the
        // bottom of the region.
        const auto offset = kernel_.Regions().LinearAllocate(FcramRegion::Base, kPageSize);
        if (!offset) {
            MH4N_LOG(Error, "Kernel",
                     "Not enough space in BASE linear region to allocate a new TLS page");
            return kResultOutOfMemory;
        }
        const u32 page_addr = TlsSlotAddress(static_cast<u32>(tls_page), 0);
        // The page belongs to the FCRAM at `offset` (the backing token makes Query show Azahar's
        // extents: neighbouring pages are one block only when their FCRAM is contiguous).
        const Result mapped = kernel_.Memory().Map(page_addr, kPageSize, MemPerm::ReadWrite,
                                                   MemState::Locked, kFcramPaddr + *offset);
        if (IsError(mapped)) {
            kernel_.Regions().Free(FcramRegion::Base, *offset, kPageSize);
            return mapped;
        }
        process->memory_used += kPageSize;
        tls_pages_.emplace_back();
    } else {
        MH4N_LOG(Debug, "Kernel", "Allocating TLS in existing page slot {}", tls_page);
    }

    // Mark the slot as used.
    tls_pages_[tls_page].owner[tls_slot] = thread;

    const u32 tls_address = TlsSlotAddress(static_cast<u32>(tls_page), static_cast<u32>(tls_slot));
    static const std::array<u8, kTlsEntrySize> kZeroes{};
    kernel_.Memory().WriteBlock(tls_address, kZeroes.data(), kTlsEntrySize);
    thread->tls_address = tls_address;
    return kResultSuccess;
}

// ---- creating threads ----------------------------------------------------------------------------

// KernelSystem::CreateThread, with ResetThreadContext.
Result ThreadManager::MakeThread(std::string name, u32 entry_point, u32 priority, u32 arg,
                                 s32 processor_id, u32 stack_top, u32 fpscr,
                                 const std::shared_ptr<Process>& process,
                                 GuestThread::Entry host_entry, std::shared_ptr<Thread>* out) {
    // Check if the priority is in range. Lowest priority -> highest priority id.
    if (priority > kThreadPrioLowest) {
        MH4N_LOG(Error, "Kernel.SVC", "Invalid thread priority: {}", priority);
        return kResultOutOfRange;
    }

    if (processor_id > kThreadProcessorIdMax) {
        MH4N_LOG(Error, "Kernel.SVC", "Invalid processor id: {}", processor_id);
        return kResultOutOfRangeKernel;
    }
    if (processor_id < kThreadProcessorId0 || processor_id > kThreadProcessorId1) {
        // Only the two cores of an Old 3DS exist (see the header: Azahar indexes a core that is not
        // there for 2 and 3).
        MH4N_LOG(Error, "Kernel.SVC", "Invalid processor id: {}", processor_id);
        return kResultOutOfRange;
    }

    // Azahar: "TODO(yuriks): Other checks, returning 0xD9001BEA"
    if (!kernel_.Memory().IsMapped(entry_point, 1)) {
        MH4N_LOG(Error, "Kernel.SVC", "(name={}): invalid entry {:08x}", name, entry_point);
        return kResultInvalidEntryAddress;
    }

    // The id is taken before the TLS allocation, as in Azahar.
    auto thread = kernel_.NewThread(std::move(name), priority, processor_id, process);
    thread->entry_point = entry_point;
    thread->stack_top = stack_top;
    thread->last_running_ticks = kernel_.NowTicks();
    thread->wait_address = 0;
    if (const Result tls = AllocateThreadLocalStorage(process, thread); IsError(tls)) {
        return tls;
    }

    // The initial context: r0 the argument, sp the stack top, pc the entry, user mode, Thumb if bit
    // 0 of the entry is set. The CP15 thread register (TPIDRURO) holds the TLS slot.
    GuestThread& guest = scheduler_.Attach(thread, std::move(host_entry));
    RCtx& ctx = guest.Context();
    ctx.r[0] = arg;
    ctx.r[13] = stack_top;
    ctx.r[15] = entry_point;
    ctx.cpsr_misc = 0x10u | ((entry_point & 1u) << 5);
    ctx.fpscr = fpscr;
    ctx.tpidruro = thread->tls_address;

    scheduler_.Start(thread);  // Azahar: make_ready, the back of the ready queue
    *out = std::move(thread);
    return kResultSuccess;
}

Result ThreadManager::CreateMainThread(const ProcessImage& image, std::shared_ptr<Thread>* out,
                                       GuestThread::Entry host_entry) {
    std::shared_ptr<Process> process = kernel_.CurrentProcess();
    if (process == nullptr) {
        process = kernel_.NewProcess(image.name);
    }
    process->kernel_version = static_cast<u16>(image.kernel_version);
    process->ideal_processor = static_cast<u8>(image.ideal_processor);
    process->memory_region = image.memory_region;
    if (image.resource_limit_category !=
        static_cast<u32>(process->resource_limit->GetCategory())) {
        MH4N_LOG(Warning, "Kernel",
                 "resource limit category {} requested; only the Application limit exists",
                 image.resource_limit_category);
    }

    // Process::Run: "Ensure we can reserve a thread. Real kernel returns 0xC860180C if this fails."
    if (!process->resource_limit->Reserve(ResourceLimitType::Thread, 1)) {
        return kResultOutOfThreads;
    }
    process->status = ProcessStatus::Running;

    // The loaders call ApplyAppMaxCPUSetting for the application category (before Process::Run in
    // Azahar; nothing can tell the order apart).
    if (image.apply_app_max_cpu) {
        ApplyAppMaxCpuSetting(image.program_id, image.core1_schedule_mode, image.core1_max_cpu);
    }

    // SetupMainThread (without the 2.6 s sleep Azahar adds while LLE modules start, see the header).
    std::shared_ptr<Thread> thread;
    const Result result =
        MakeThread(process->Name() + "-main", image.entry,
                   static_cast<u32>(image.main_thread_priority), 0,
                   static_cast<s32>(process->ideal_processor), image.stack_top,
                   image.main_thread_fpscr, process, std::move(host_entry), &thread);
    if (IsError(result)) {
        return result;
    }
    if (out != nullptr) {
        *out = std::move(thread);
    }
    return kResultSuccess;
}

Result ThreadManager::CreateThread(const ThreadSpec& spec, Handle* out_handle,
                                   std::shared_ptr<Thread>* out_thread) {
    const std::shared_ptr<Process> process = kernel_.CurrentProcess();
    MH4N_ASSERT(process != nullptr, "CreateThread without a process");
    const std::shared_ptr<ResourceLimit>& resource_limit = process->resource_limit;

    // SVC::CreateThread
    if (spec.priority > kThreadPrioLowest) {
        return kResultOutOfRange;
    }
    const u32 max_priority =
        static_cast<u32>(resource_limit->GetLimitValue(ResourceLimitType::Priority));
    if (!(max_priority <= spec.priority || no_thread_restrictions_)) {
        return kResultNotAuthorized;
    }

    s32 processor_id = spec.processor_id;
    if (processor_id == kThreadProcessorIdDefault) {
        // Set the target CPU to the one specified in the process' exheader.
        processor_id = process->ideal_processor;
    }
    switch (processor_id) {
    case kThreadProcessorId0:
    case kThreadProcessorId1:
        break;
    case kThreadProcessorIdAll:
        MH4N_LOG(Info, "Kernel.SVC",
                 "Newly created thread is allowed to be run in any Core, for now run in core 0.");
        processor_id = kThreadProcessorId0;
        break;
    case kThreadProcessorId2:
    case kThreadProcessorId3:
        // Azahar lets these through to a core that an Old 3DS does not have.
    default:
        return kResultOutOfRange;
    }

    // Update thread count in resource limit. Azahar does not give it back if the creation fails.
    if (!resource_limit->Reserve(ResourceLimitType::Thread, 1)) {
        return kResultOutOfThreads;
    }

    // Create thread.
    char entry_text[16];
    std::snprintf(entry_text, sizeof(entry_text), "%08X", spec.entry_point);
    std::shared_ptr<Thread> thread;
    if (const Result result = MakeThread(process->Name() + "-" + entry_text, spec.entry_point,
                                         spec.priority, spec.arg, processor_id, spec.stack_top,
                                         kThreadFpscr, process, spec.host_entry, &thread);
        IsError(result)) {
        return result;
    }

    kernel_.GetScheduler().PrepareReschedule();

    MH4N_LOG(Trace, "Kernel.SVC",
             "called entrypoint=0x{:08X} ({}), arg=0x{:08X}, stacktop=0x{:08X}, "
             "threadpriority=0x{:08X}, processorid=0x{:08X}",
             spec.entry_point, thread->Name(), spec.arg, spec.stack_top, spec.priority,
             static_cast<u32>(processor_id));

    if (out_thread != nullptr) {
        *out_thread = thread;
    }
    return process->handle_table.Create(out_handle, std::move(thread));
}

// ---- the core-1 time limit -----------------------------------------------------------------------

void ThreadManager::ApplyAppMaxCpuSetting(u64 program_id, u32 exh_mode, u32 exh_cpu_limit) {
    const std::shared_ptr<Process> process = kernel_.CurrentProcess();
    MH4N_ASSERT(process != nullptr, "ApplyAppMaxCpuSetting without a process");
    const Core1CpuSetting setting = ComputeAppMaxCpuSetting(program_id, exh_mode, exh_cpu_limit);

    // Normally done by PM through svcSetKernelState and svcSetResourceLimitValues.
    process->resource_limit->SetLimitValue(ResourceLimitType::CpuTime, setting.limit_value);
    process->resource_limit->SetCurrentValue(ResourceLimitType::CpuTime, setting.current_value);
    limiter_->SetScheduleMode(setting.mode);
    limiter_->UpdateAppCpuLimit();
}

Result ThreadManager::SetAppCpuTimeLimit(u32 percent) {
    const std::shared_ptr<ResourceLimit> limit = kernel_.ApplicationResourceLimit();
    if (percent <= static_cast<u32>(limit->GetLimitValue(ResourceLimitType::CpuTime))) {
        limit->SetCurrentValue(ResourceLimitType::CpuTime, static_cast<s32>(percent));
        limiter_->UpdateAppCpuLimit();
    }
    return kResultSuccess;
}

u32 ThreadManager::AppCpuTimeLimit() const {
    return static_cast<u32>(
        kernel_.ApplicationResourceLimit()->GetCurrentValue(ResourceLimitType::CpuTime));
}

Core1ScheduleMode ThreadManager::ScheduleMode() const {
    return limiter_->ScheduleMode();
}
bool ThreadManager::Core1LimiterReady() const {
    return limiter_->Ready();
}
bool ThreadManager::Core1LimiterActive() const {
    return limiter_->Active();
}
Core1Phase ThreadManager::Core1CurrentPhase() const {
    return limiter_->Phase();
}
std::size_t ThreadManager::Core1HeldThreads() const {
    return limiter_->Held();
}

}  // namespace mh4n
