// Derived from Azahar src/core/hle/kernel/svc.cpp, svc_wrapper.h, errors.h and src/core/memory.cpp
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says so; svc_wrapper.h,
// errors.h and memory.cpp say GPL-2.0-or-later): the SVC table (numbers and names), the arguments
// each wrapper decodes and the registers it uses (SVCWrapper's ABI), the order of the checks and the
// result codes of each call, the GetSystemInfo, GetProcessInfo, GetHandleInfo and GetThreadInfo
// values, and the bounded string read of the debug output (MemorySystem::ReadCString).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (svc.cpp, svc_wrapper.h, memory.cpp)
// Copyright 2017 Citra Emulator Project (errors.h)
//
// The SVC table (N1-07): see mh4n/svc.h. The memory calls (ControlMemory, QueryMemory and the
// memory blocks) are in svc_memory.cpp.
//
// Register ABI (svc_wrapper.h): the result is r0 and the output pointers' values follow in r1 up;
// the inputs keep their ARM-ABI positions (r0-r3, a 64-bit one on an even pair, the output
// pointers' slots skipped) and the ones that do not fit are taken from r0 up where a slot is free,
// then r4 up. So CreateThread has the priority in r0 and the processor id in r4, and SetTimer, with a
// hole at r1 (the 64-bit `initial` sits on r2/r3), has the low word of `interval` in r1 and its
// high word in r4.
#include "mh4n/svc.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "deterministic.h"
#include "mh4n/assert.h"
#include "mh4n/log.h"
#include "svc_internal.h"

// The registers of a guest thread (RCtx). Last: recomp.h defines short macros.
#include "../cpu/recomp_c.h"

namespace mh4n {

namespace {

// Azahar's SVC numbers that this table implements (src/core/hle/kernel/svc.cpp, SVC_Table).
enum SvcNumber : u32 {
    kControlMemory = 0x01,
    kQueryMemory = 0x02,
    kExitProcess = 0x03,
    kCreateThread = 0x08,
    kExitThread = 0x09,
    kSleepThread = 0x0A,
    kGetThreadPriority = 0x0B,
    kSetThreadPriority = 0x0C,
    kCreateMutex = 0x13,
    kReleaseMutex = 0x14,
    kCreateSemaphore = 0x15,
    kReleaseSemaphore = 0x16,
    kCreateEvent = 0x17,
    kSignalEvent = 0x18,
    kClearEvent = 0x19,
    kCreateTimer = 0x1A,
    kSetTimer = 0x1B,
    kCancelTimer = 0x1C,
    kClearTimer = 0x1D,
    kCreateMemoryBlock = 0x1E,
    kMapMemoryBlock = 0x1F,
    kUnmapMemoryBlock = 0x20,
    kCreateAddressArbiter = 0x21,
    kArbitrateAddress = 0x22,
    kCloseHandle = 0x23,
    kWaitSynchronization1 = 0x24,
    kWaitSynchronizationN = 0x25,
    kDuplicateHandle = 0x27,
    kGetSystemTick = 0x28,
    kGetHandleInfo = 0x29,
    kGetSystemInfo = 0x2A,
    kGetProcessInfo = 0x2B,
    kGetThreadInfo = 0x2C,
    kConnectToPort = 0x2D,
    kSendSyncRequest = 0x32,
    kGetProcessId = 0x35,
    kGetThreadId = 0x37,
    kGetResourceLimit = 0x38,
    kGetResourceLimitLimitValues = 0x39,
    kGetResourceLimitCurrentValues = 0x3A,
    kBreak = 0x3C,
    kOutputDebugString = 0x3D,
    kControlProcess = 0xB3,
};

// svcGetHandleInfo's types (HandleInfoType).
enum HandleInfoType : u32 {
    kHandleInfoProcessElapsedTicks = 0,
    kHandleInfoReferenceCount = 1,
    kHandleInfoStubbed1 = 2,
    kHandleInfoStubbed2 = 0x32107,
};

// svcGetSystemInfo's types and the parameters of the ones that have them (SystemInfoType,
// SystemInfoMemUsageRegion, SystemInfoLumaCFWInformation).
enum SystemInfoType : u32 {
    kSystemInfoRegionMemoryUsage = 0,
    kSystemInfoKernelAllocatedPages = 2,
    kSystemInfoKernelSpawnedPids = 26,
    kSystemInfoLumaCfwInfo = 0x10000,
    kSystemInfoNew3dsInfo = 0x10001,
};
enum MemUsageRegion : u32 { kMemUsageAll = 0, kMemUsageApplication = 1, kMemUsageSystem = 2, kMemUsageBase = 3 };
inline constexpr u32 kLumaRealAppRegionSize = 0x80;
inline constexpr u32 kLumaIsN3ds = 0x201;

// svcGetProcessInfo's types (ProcessInfoType, without the Luma ones).
enum ProcessInfoType : u32 {
    kProcessInfoPrivateAndSharedUsedMemory = 0,
    kProcessInfoSupervisorAndHandleUsedMemory = 1,
    kProcessInfoPrivateSharedSupervisorHandleUsedMemory = 2,
    kProcessInfoSupervisorAndHandleUsedMemory2 = 3,
    kProcessInfoUsedHandleCount = 4,
    kProcessInfoHighestHandleCount = 5,
    kProcessInfoKProcess0x234 = 6,
    kProcessInfoThreadCount = 7,
    kProcessInfoMaxThreadAmount = 8,
    kProcessInfoLinearBaseAddrOffset = 20,
    kProcessInfoQtmMemoryBlockConversionOffset = 21,
    kProcessInfoQtmMemoryAddress = 22,
    kProcessInfoQtmMemorySize = 23,
};

inline constexpr u32 kThreadInfoTlsAddress = 0x10000;

// ControlProcessOP; only the one the thread manager needs is implemented.
inline constexpr u32 kProcessOpDisableCreateThreadRestrictions = 7;

// Azahar's PortNameMaxLength.
inline constexpr std::size_t kPortNameMaxLength = 11;

// The longest OutputDebugString text that is read (1 MiB; the game's messages are lines).
inline constexpr s32 kMaxDebugStringLength = 0x100000;

// ---- registers -----------------------------------------------------------------------------------------

// The result in r0 and one output in r1; a failed call writes 0 to its outputs.
void Return(RCtx& c, Result result) {
    c.r[0] = result;
}
void Return(RCtx& c, Result result, u32 out1) {
    c.r[0] = result;
    c.r[1] = IsError(result) ? 0 : out1;
}
// A 64-bit output in r1/r2 (GetHandleInfo, GetSystemInfo, GetProcessInfo, GetThreadInfo).
void Return64(RCtx& c, Result result, u64 out) {
    c.r[0] = result;
    SvcPut64(c, 1, 2, IsError(result) ? 0 : out);
}

// Reads a NUL-terminated string of at most `max_length` bytes; an unmapped byte ends it, as
// Azahar's ReadCString does (its Read8 of an unmapped address gives 0).
std::string ReadCString(const GuestMemory& memory, u32 address, std::size_t max_length) {
    std::string text;
    for (std::size_t i = 0; i < max_length; ++i) {
        const auto byte = memory.Read<u8>(address + static_cast<u32>(i));
        if (!byte || *byte == 0) {
            break;
        }
        text.push_back(static_cast<char>(*byte));
    }
    return text;
}

}  // namespace

u64 SvcGet64(const RCtx& ctx, int low, int high) {
    return static_cast<u64>(ctx.r[low]) | (static_cast<u64>(ctx.r[high]) << 32);
}

void SvcPut64(RCtx& ctx, int low, int high, u64 value) {
    ctx.r[low] = static_cast<u32>(value);
    ctx.r[high] = static_cast<u32>(value >> 32);
}

// ---- the public class ------------------------------------------------------------------------------------

SvcTable::SvcTable(Kernel& kernel, GuestScheduler& scheduler, ThreadManager& threads)
    : impl_(std::make_unique<Impl>(kernel, scheduler, threads)) {}

SvcTable::~SvcTable() = default;

void SvcTable::Install(CodeHost& host) {
    host.SetSvcHandler([this](RCtx& ctx, u32 imm, u32 next_pc) { CallSvc(ctx, imm, next_pc); });
}

void SvcTable::CallSvc(RCtx& ctx, u32 imm, u32 next_pc) {
    impl_->Call(ctx, imm, next_pc);
}

void SvcTable::SetHooks(SvcHooks hooks) {
    impl_->hooks = std::move(hooks);
}

void SvcTable::SetConnectToPortHandler(ConnectToPortHandler handler) {
    impl_->connect_to_port = std::move(handler);
}

void SvcTable::SetSendSyncRequestHandler(SendSyncRequestHandler handler) {
    impl_->send_sync_request = std::move(handler);
}

const std::optional<SvcExit>& SvcTable::Exit() const {
    return impl_->exit;
}

std::vector<SvcCall> SvcTable::PendingCalls() const {
    std::vector<SvcCall> calls;
    for (const auto& [thread_id, call] : impl_->pending) {
        calls.push_back(call);
    }
    return calls;
}

// ---- dispatch -------------------------------------------------------------------------------------------

void SvcTable::Impl::Call(RCtx& ctx, u32 imm, u32 next_pc) {
    // For traces and failure reports, as the Azahar backend leaves it: the address after the
    // instruction, in ARM or Thumb state alike.
    ctx.r[15] = next_pc & ~1u;
    Thread* const thread = sched.CurrentThread();
    MH4N_ASSERT(thread != nullptr, "svc 0x{:02X} outside a guest thread", imm);

    SvcScope scope(sched, imm);
    SvcCall call;
    call.thread_id = thread->thread_id;
    call.number = imm;
    call.pc = ctx.r[15];
    call.lr = ctx.r[14];
    std::copy_n(ctx.r, call.regs.size(), call.regs.begin());
    // (the later of the two cores' clocks in the deterministic mode, as the reference's `t`)
    call.ticks = kernel_detail::TraceTicks(sched.NowTicks());
    // The deterministic mode (N1-25) charges the call's cycles from Azahar's table here: after the time
    // the trace records (the reference's trace shows the tick before the cost, as its `t=` does) and
    // before the call runs, so that a call that reads the clock sees its own cost. Nothing otherwise.
    kernel_detail::ChargeCycles(SvcCycles(imm));
    pending[call.thread_id] = call;
    if (hooks.before) {
        hooks.before(call);
    }

    SvcFrame frame{ctx, *thread, imm};
    Dispatch(frame);

    // The call is over for this thread: it returned, or it blocked and the thread runs again.
    pending.erase(call.thread_id);
    if (hooks.complete) {
        hooks.complete(call, {ctx.r[0], ctx.r[1], ctx.r[2], ctx.r[3]});
    }
}

void SvcTable::Impl::Dispatch(SvcFrame& f) {
    switch (f.number) {
    case kControlMemory: ControlMemory(f); break;
    case kQueryMemory: QueryMemory(f); break;
    case kExitProcess:
        if (!exit) {
            exit.emplace(SvcExit{0, {}});
        }
        EndRun(f);
    case kCreateThread: CreateThread(f); break;
    case kExitThread:
        // The last switch of this fiber: nothing owning may be on its stack (cpu.h).
        sched.ExitCurrent();
    case kSleepThread: SleepThread(f); break;
    case kGetThreadPriority: GetThreadPriority(f); break;
    case kSetThreadPriority: SetThreadPriority(f); break;
    case kCreateMutex: CreateMutex(f); break;
    case kReleaseMutex: ReleaseMutex(f); break;
    case kCreateSemaphore: CreateSemaphore(f); break;
    case kReleaseSemaphore: ReleaseSemaphore(f); break;
    case kCreateEvent: CreateEvent(f); break;
    case kSignalEvent: SignalEvent(f); break;
    case kClearEvent: ClearEvent(f); break;
    case kCreateTimer: CreateTimer(f); break;
    case kSetTimer: SetTimer(f); break;
    case kCancelTimer: CancelTimer(f); break;
    case kClearTimer: ClearTimer(f); break;
    case kCreateMemoryBlock: CreateMemoryBlock(f); break;
    case kMapMemoryBlock: MapMemoryBlock(f); break;
    case kUnmapMemoryBlock: UnmapMemoryBlock(f); break;
    case kCreateAddressArbiter: CreateAddressArbiter(f); break;
    case kArbitrateAddress: ArbitrateAddress(f); break;
    case kCloseHandle: CloseHandle(f); break;
    case kWaitSynchronization1: WaitSynchronization1(f); break;
    case kWaitSynchronizationN: WaitSynchronizationN(f); break;
    case kDuplicateHandle: DuplicateHandle(f); break;
    case kGetSystemTick: GetSystemTick(f); break;
    case kGetHandleInfo: GetHandleInfo(f); break;
    case kGetSystemInfo: GetSystemInfo(f); break;
    case kGetProcessInfo: GetProcessInfo(f); break;
    case kGetThreadInfo: GetThreadInfo(f); break;
    case kConnectToPort: ConnectToPort(f); break;
    case kSendSyncRequest: SendSyncRequest(f); break;
    case kGetProcessId: GetProcessId(f); break;
    case kGetThreadId: GetThreadId(f); break;
    case kGetResourceLimit: GetResourceLimit(f); break;
    case kGetResourceLimitLimitValues: GetResourceLimitValues(f, /*current=*/false); break;
    case kGetResourceLimitCurrentValues: GetResourceLimitValues(f, /*current=*/true); break;
    case kBreak: Break(f); break;
    case kOutputDebugString: OutputDebugString(f); break;
    case kControlProcess: ControlProcess(f); break;
    default: LogUnknown(f); break;
    }
}

void SvcTable::Impl::LogUnknown(const SvcFrame& f) {
    // Azahar logs "unimplemented SVC function" for a number it knows and has no code for, and
    // reads past its table for a number it does not know; both leave the registers alone.
    if (unknown_logged.insert(f.number).second) {
        MH4N_LOG(Error, "Kernel.SVC", "unimplemented svc 0x{:02X} at 0x{:08X}", f.number, f.ctx.r[14]);
    }
}

void SvcTable::Impl::EndRun(const SvcFrame&) {
    sched.RequestStop();
    // The thread does not come back. Nothing here owns anything: the run is over.
    sched.ExitCurrent();
}

// ---- threads --------------------------------------------------------------------------------------------

void SvcTable::Impl::CreateThread(SvcFrame& f) {
    // SVC::CreateThread: r0 priority, r1 entry point, r2 argument, r3 stack top, r4 processor id;
    // out r1 the handle. ThreadManager::CreateThread does the checks and the rest.
    RCtx& c = f.ctx;
    ThreadSpec spec;
    spec.priority = c.r[0];
    spec.entry_point = c.r[1];
    spec.arg = c.r[2];
    spec.stack_top = c.r[3];
    spec.processor_id = static_cast<s32>(c.r[4]);
    Handle handle = 0;
    std::shared_ptr<Thread> created;
    const Result result = threads.CreateThread(spec, &handle, &created);
    if (result == kResultSuccess && hooks.thread_created) {
        SvcThreadCreated info;
        info.parent_id = f.thread.thread_id;
        info.thread_id = created->thread_id;
        info.entry = spec.entry_point;
        info.arg = spec.arg;
        info.stack_top = spec.stack_top;
        info.priority = spec.priority;
        info.requested_processor = spec.processor_id;
        info.resolved_processor = created->processor_id;
        hooks.thread_created(info);
    }
    Return(c, result, handle);
}

void SvcTable::Impl::SleepThread(SvcFrame& f) {
    // r0/r1: the duration in nanoseconds. No outputs: the registers stay as they were.
    sched.Sleep(static_cast<s64>(SvcGet64(f.ctx, 0, 1)));
}

void SvcTable::Impl::GetThreadPriority(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto thread = kernel.CurrentProcess()->handle_table.Get<Thread>(c.r[1]);
    if (!thread) {
        Return(c, kResultInvalidHandle, 0);
        return;
    }
    Return(c, kResultSuccess, thread->GetPriority());
}

void SvcTable::Impl::SetThreadPriority(SvcFrame& f) {
    RCtx& c = f.ctx;
    const Handle handle = c.r[0];
    const u32 priority = c.r[1];
    if (priority > kThreadPrioLowest) {
        Return(c, kResultOutOfRange);
        return;
    }
    Process& process = *kernel.CurrentProcess();
    const auto thread = process.handle_table.Get<Thread>(handle);
    if (!thread) {
        Return(c, kResultInvalidHandle);
        return;
    }
    // Azahar: "The kernel uses the current process's resource limit instead of the one from the
    // thread owner's resource limit." (No exception for a process without thread restrictions.)
    const u32 max_priority =
        static_cast<u32>(process.resource_limit->GetLimitValue(ResourceLimitType::Priority));
    if (max_priority > priority) {
        Return(c, kResultNotAuthorized);
        return;
    }
    thread->SetPriority(priority);
    thread->UpdatePriority();
    // The mutexes this thread waits for follow its priority.
    for (const auto& mutex : thread->pending_mutexes) {
        mutex->UpdatePriority();
    }
    kernel.GetScheduler().PrepareReschedule();
    Return(c, kResultSuccess);
}

// ---- synchronisation objects ----------------------------------------------------------------------------

void SvcTable::Impl::CreateMutex(SvcFrame& f) {
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const std::shared_ptr<ResourceLimit> limit = process.resource_limit;
    Handle handle = 0;
    Result result;
    if (!limit->Reserve(ResourceLimitType::Mutex, 1)) {
        result = kResultOutOfMutexes;
    } else {
        auto mutex = kernel.NewMutex(c.r[1] != 0, fmt::format("mutex-{:08x}", c.r[14]));
        mutex->resource_limit = limit;
        result = process.handle_table.Create(&handle, std::move(mutex));
    }
    Return(c, result, handle);
}

void SvcTable::Impl::ReleaseMutex(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto mutex = kernel.CurrentProcess()->handle_table.Get<Mutex>(c.r[0]);
    if (!mutex) {
        Return(c, kResultInvalidHandle);
        return;
    }
    Return(c, mutex->Release(&f.thread));
}

void SvcTable::Impl::CreateSemaphore(SvcFrame& f) {
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const std::shared_ptr<ResourceLimit> limit = process.resource_limit;
    Handle handle = 0;
    Result result;
    if (!limit->Reserve(ResourceLimitType::Semaphore, 1)) {
        result = kResultOutOfSemaphores;
    } else {
        // As in Azahar, the slot reserved above is not given back when the arguments are refused.
        std::shared_ptr<Semaphore> semaphore;
        result = kernel.NewSemaphore(&semaphore, static_cast<s32>(c.r[1]), static_cast<s32>(c.r[2]),
                                     fmt::format("semaphore-{:08x}", c.r[14]));
        if (!IsError(result)) {
            semaphore->resource_limit = limit;
            result = process.handle_table.Create(&handle, std::move(semaphore));
        }
    }
    Return(c, result, handle);
}

void SvcTable::Impl::ReleaseSemaphore(SvcFrame& f) {
    // r1 handle, r2 release count; out r1 the count before the call.
    RCtx& c = f.ctx;
    const auto semaphore = kernel.CurrentProcess()->handle_table.Get<Semaphore>(c.r[1]);
    if (!semaphore) {
        Return(c, kResultInvalidHandle, 0);
        return;
    }
    s32 count = 0;
    const Result result = semaphore->Release(&count, static_cast<s32>(c.r[2]));
    Return(c, result, static_cast<u32>(count));
}

void SvcTable::Impl::CreateEvent(SvcFrame& f) {
    // r1 reset type (0 one shot, 1 sticky, 2 pulse; anything else behaves as sticky, as in Azahar).
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const std::shared_ptr<ResourceLimit> limit = process.resource_limit;
    Handle handle = 0;
    Result result;
    if (!limit->Reserve(ResourceLimitType::Event, 1)) {
        result = kResultOutOfEvents;
    } else {
        auto event = kernel.NewEvent(static_cast<ResetType>(c.r[1]),
                                     fmt::format("event-{:08x}", c.r[14]));
        event->resource_limit = limit;
        result = process.handle_table.Create(&handle, std::move(event));
    }
    Return(c, result, handle);
}

void SvcTable::Impl::SignalEvent(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto event = kernel.CurrentProcess()->handle_table.Get<Event>(c.r[0]);
    if (!event) {
        Return(c, kResultInvalidHandle);
        return;
    }
    event->Signal();
    Return(c, kResultSuccess);
}

void SvcTable::Impl::ClearEvent(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto event = kernel.CurrentProcess()->handle_table.Get<Event>(c.r[0]);
    if (!event) {
        Return(c, kResultInvalidHandle);
        return;
    }
    event->Clear();
    Return(c, kResultSuccess);
}

void SvcTable::Impl::CreateTimer(SvcFrame& f) {
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const std::shared_ptr<ResourceLimit> limit = process.resource_limit;
    Handle handle = 0;
    Result result;
    if (!limit->Reserve(ResourceLimitType::Timer, 1)) {
        result = kResultOutOfTimers;
    } else {
        auto timer = kernel.NewTimer(static_cast<ResetType>(c.r[1]),
                                     fmt::format("timer-{:08x}", c.r[14]));
        timer->resource_limit = limit;
        result = process.handle_table.Create(&handle, std::move(timer));
    }
    Return(c, result, handle);
}

void SvcTable::Impl::SetTimer(SvcFrame& f) {
    // r0 handle, r2/r3 `initial`, and `interval` split around the hole at r1: low word in r1,
    // high word in r4 (the ABI of svc_wrapper.h for Result(Handle, s64, s64)).
    RCtx& c = f.ctx;
    const s64 initial = static_cast<s64>(SvcGet64(c, 2, 3));
    const s64 interval = static_cast<s64>(SvcGet64(c, 1, 4));
    if (initial < 0 || interval < 0) {
        Return(c, kResultOutOfRangeKernel);
        return;
    }
    const auto timer = kernel.CurrentProcess()->handle_table.Get<Timer>(c.r[0]);
    if (!timer) {
        Return(c, kResultInvalidHandle);
        return;
    }
    timer->Set(initial, interval);
    Return(c, kResultSuccess);
}

void SvcTable::Impl::CancelTimer(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto timer = kernel.CurrentProcess()->handle_table.Get<Timer>(c.r[0]);
    if (!timer) {
        Return(c, kResultInvalidHandle);
        return;
    }
    timer->Cancel();
    Return(c, kResultSuccess);
}

void SvcTable::Impl::ClearTimer(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto timer = kernel.CurrentProcess()->handle_table.Get<Timer>(c.r[0]);
    if (!timer) {
        Return(c, kResultInvalidHandle);
        return;
    }
    timer->Clear();
    Return(c, kResultSuccess);
}

void SvcTable::Impl::CreateAddressArbiter(SvcFrame& f) {
    RCtx& c = f.ctx;
    Process& process = *kernel.CurrentProcess();
    const std::shared_ptr<ResourceLimit> limit = process.resource_limit;
    Handle handle = 0;
    Result result;
    if (!limit->Reserve(ResourceLimitType::AddressArbiter, 1)) {
        result = kResultOutOfAddressArbiters;
    } else {
        auto arbiter = kernel.NewAddressArbiter();
        arbiter->resource_limit = limit;
        result = process.handle_table.Create(&handle, std::move(arbiter));
    }
    Return(c, result, handle);
}

void SvcTable::Impl::ArbitrateAddress(SvcFrame& f) {
    // r0 arbiter, r1 address, r2 type, r3 value, r4/r5 timeout in nanoseconds.
    RCtx& c = f.ctx;
    Result result;
    {
        const auto arbiter = kernel.CurrentProcess()->handle_table.Get<AddressArbiter>(c.r[0]);
        if (!arbiter) {
            Return(c, kResultInvalidHandle);
            return;
        }
        result = arbiter->ArbitrateAddress(sched.CurrentThreadPtr(),
                                           static_cast<ArbitrationType>(c.r[2]), c.r[1],
                                           static_cast<s32>(c.r[3]),
                                           static_cast<s64>(SvcGet64(c, 4, 5)));
    }
    // Azahar: "TODO: identify in which specific cases this call should cause a reschedule."
    kernel.GetScheduler().PrepareReschedule();
    if (f.thread.status != ThreadStatus::Running) {
        // The arbiter put the thread to wait. A signal, a timeout or the end of the wait chose the
        // result the call returns.
        result = sched.Park(f.thread).result.value_or(result);
    }
    Return(c, result);
}

void SvcTable::Impl::CloseHandle(SvcFrame& f) {
    Return(f.ctx, kernel.CurrentProcess()->handle_table.Close(f.ctx.r[0]));
}

void SvcTable::Impl::WaitSynchronization1(SvcFrame& f) {
    // r0 handle, r2/r3 timeout in nanoseconds; result in r0.
    RCtx& c = f.ctx;
    std::vector<std::shared_ptr<WaitObject>> objects;
    {
        auto object = kernel.CurrentProcess()->handle_table.Get<WaitObject>(c.r[0]);
        if (!object) {
            Return(c, kResultInvalidHandle);
            return;
        }
        objects.push_back(std::move(object));
    }
    const s64 timeout = static_cast<s64>(SvcGet64(c, 2, 3));
    const WaitOutcome outcome = sched.Block(f.thread, std::move(objects), /*wait_all=*/false, timeout);
    // Azahar returns Timeout when the thread waits, and a signal turns it into Success.
    Return(c, outcome.result.value_or(kResultTimeout));
}

void SvcTable::Impl::WaitSynchronizationN(SvcFrame& f) {
    // r1 handles address, r2 count, r3 wait for all, r0/r4 timeout in nanoseconds (low, high);
    // result in r0, the index of the object that woke a wait on any in r1.
    RCtx& c = f.ctx;
    const u32 handles_address = c.r[1];
    const s32 handle_count = static_cast<s32>(c.r[2]);
    const bool wait_all = c.r[3] != 0;
    const s64 timeout = static_cast<s64>(SvcGet64(c, 0, 4));

    // Azahar: IsValidVirtualAddress of the array's first byte, then the count.
    if (!memory.IsMapped(handles_address, 1)) {
        Return(c, kResultInvalidPointer, 0);
        return;
    }
    if (handle_count < 0) {
        Return(c, kResultOutOfRange, 0);
        return;
    }
    std::vector<std::shared_ptr<WaitObject>> objects;
    {
        const HandleTable& table = kernel.CurrentProcess()->handle_table;
        for (s32 i = 0; i < handle_count; ++i) {
            const u32 address = handles_address + static_cast<u32>(i) * sizeof(Handle);
            // Azahar's Read32 of an unmapped address logs and gives 0, which is not a handle.
            const Handle handle = memory.Read<u32>(address).value_or(0);
            auto object = table.Get<WaitObject>(handle);
            if (!object) {
                Return(c, kResultInvalidHandle, 0);
                return;
            }
            objects.push_back(std::move(object));
        }
    }
    const WaitOutcome outcome = sched.Block(f.thread, std::move(objects), wait_all, timeout);
    c.r[0] = outcome.result.value_or(kResultTimeout);
    // The index is an output when the thread waited (-1 unless a wait on any was signalled) or a
    // wait on any found its object at once. A wait on all that is satisfied at once, and a refusal
    // with a timeout of 0, do not set it: Azahar's wrapper then writes an uninitialised variable
    // (port/tests/expected/conf_wait.txt reads 0 for a wait on all), and so does this.
    c.r[1] = (outcome.blocked || outcome.index >= 0) ? static_cast<u32>(outcome.index) : 0;
}

void SvcTable::Impl::DuplicateHandle(SvcFrame& f) {
    Handle duplicate = 0;
    const Result result =
        kernel.CurrentProcess()->handle_table.Duplicate(&duplicate, f.ctx.r[1]);
    Return(f.ctx, result, duplicate);
}

// ---- information ----------------------------------------------------------------------------------------

void SvcTable::Impl::GetSystemTick(SvcFrame& f) {
    // The result is the s64 in r0/r1. Azahar: the CPU timer's ticks.
    SvcPut64(f.ctx, 0, 1, sched.NowTicks());
}

void SvcTable::Impl::GetHandleInfo(SvcFrame& f) {
    // r1 handle, r2 type; out r1/r2 an s64.
    RCtx& c = f.ctx;
    const std::shared_ptr<Object> object = kernel.CurrentProcess()->handle_table.GetGeneric(c.r[1]);
    if (!object) {
        Return64(c, kResultInvalidHandle, 0);
        return;
    }
    s64 value = 0;
    switch (c.r[2]) {
    case kHandleInfoProcessElapsedTicks:
        // Azahar returns the tick the process was created at.
        if (const auto process = ObjectCast<Process>(object)) {
            value = static_cast<s64>(process->creation_time_ticks);
        }
        break;
    case kHandleInfoReferenceCount:
        // "The closest approximation we can get without a full KObject implementation."
        value = static_cast<s64>(object.use_count()) - 1;
        break;
    case kHandleInfoStubbed1:
    case kHandleInfoStubbed2:
        break;  // stubbed in the real kernel too
    default:
        Return64(c, kResultInvalidEnumValue, 0);
        return;
    }
    Return64(c, kResultSuccess, static_cast<u64>(value));
}

void SvcTable::Impl::GetSystemInfo(SvcFrame& f) {
    // r1 type, r2 parameter; out r1/r2 an s64. "This function never returns an error, even if
    // invalid parameters were passed" (except NEW_3DS_INFO on an Old 3DS).
    RCtx& c = f.ctx;
    const u32 type = c.r[1];
    const s32 param = static_cast<s32>(c.r[2]);
    FcramRegions& regions = kernel.Regions();
    s64 value = 0;
    Result result = kResultSuccess;
    // Azahar tells a New 3DS by its four cores; here the FCRAM size says it.
    const bool new_3ds = regions.TotalSize() == kFcramSizeNew3ds;
    switch (type) {
    case kSystemInfoRegionMemoryUsage:
        switch (param) {
        case kMemUsageAll:
            value = static_cast<s64>(regions.Used(FcramRegion::Application)) +
                    regions.Used(FcramRegion::System) + regions.Used(FcramRegion::Base);
            break;
        case kMemUsageApplication: value = regions.Used(FcramRegion::Application); break;
        case kMemUsageSystem: value = regions.Used(FcramRegion::System); break;
        case kMemUsageBase: value = regions.Used(FcramRegion::Base); break;
        default:
            MH4N_LOG(Error, "Kernel.SVC", "unknown GetSystemInfo type=0 region: param={}", param);
            break;
        }
        break;
    case kSystemInfoKernelAllocatedPages:
        MH4N_LOG(Error, "Kernel.SVC", "unimplemented GetSystemInfo type=2 param={}", param);
        break;
    case kSystemInfoKernelSpawnedPids:
        value = 5;
        break;
    case kSystemInfoLumaCfwInfo:
        switch (static_cast<u32>(param)) {
        case kLumaRealAppRegionSize: value = regions.Size(FcramRegion::Application); break;
        case kLumaIsN3ds: value = new_3ds ? 1 : 0; break;
        default:
            MH4N_LOG(Error, "Kernel.SVC", "unknown GetSystemInfo type=0x10000 region: param={}",
                     param);
            break;
        }
        break;
    case kSystemInfoNew3dsInfo:
        // Azahar does not implement the subtypes: "homebrew just check this doesn't return an
        // error in n3ds to know the system type".
        result = new_3ds ? kResultSuccess : kResultInvalidEnumValue;
        break;
    default:
        // Includes 0x20000, Azahar's own information about the emulator: this is not Azahar.
        MH4N_LOG(Error, "Kernel.SVC", "unknown GetSystemInfo type={} param={}", type, param);
        break;
    }
    Return64(c, result, static_cast<u64>(value));
}

void SvcTable::Impl::GetProcessInfo(SvcFrame& f) {
    // r1 process handle, r2 type; out r1/r2 an s64.
    RCtx& c = f.ctx;
    const auto process = kernel.CurrentProcess()->handle_table.Get<Process>(c.r[1]);
    if (!process) {
        Return64(c, kResultInvalidHandle, 0);
        return;
    }
    s64 value = 0;
    Result result = kResultSuccess;
    switch (c.r[2]) {
    case kProcessInfoPrivateAndSharedUsedMemory:
    case kProcessInfoPrivateSharedSupervisorHandleUsedMemory:
        // Azahar: type 0 "returns a slightly higher number than type 2, but I'm not sure what's the
        // difference between them"; both give the memory used.
        value = process->memory_used;
        if (process->memory_used % kPageSize != 0) {
            MH4N_LOG(Error, "Kernel.SVC", "GetProcessInfo: memory size not page-aligned");
            result = kResultMisalignedSize;
        }
        break;
    case kProcessInfoSupervisorAndHandleUsedMemory:
    case kProcessInfoSupervisorAndHandleUsedMemory2:
    case kProcessInfoUsedHandleCount:
    case kProcessInfoHighestHandleCount:
    case kProcessInfoKProcess0x234:
    case kProcessInfoThreadCount:
    case kProcessInfoMaxThreadAmount:
        // Valid, but Azahar does not implement them and leaves the output unset (it answers
        // success): 0 here.
        MH4N_LOG(Error, "Kernel.SVC", "unimplemented GetProcessInfo type={}", c.r[2]);
        break;
    case kProcessInfoLinearBaseAddrOffset:
        // 32-bit arithmetic as in Azahar (FCRAM_PADDR - the linear heap's area), then widened.
        value = static_cast<u32>(kFcramPaddr - LinearHeapArea());
        break;
    case kProcessInfoQtmMemoryBlockConversionOffset:
    case kProcessInfoQtmMemoryAddress:
    case kProcessInfoQtmMemorySize:
        // "These return a different error value than higher invalid values."
        MH4N_LOG(Error, "Kernel.SVC", "unknown GetProcessInfo type={}", c.r[2]);
        result = kResultNotImplemented;
        break;
    default:
        // Azahar also answers the Luma3DS types of 3GX plugins (0x10000 and up); applications
        // do not use them.
        MH4N_LOG(Error, "Kernel.SVC", "unknown GetProcessInfo type={}", c.r[2]);
        result = kResultInvalidEnumValue;
        break;
    }
    Return64(c, result, static_cast<u64>(value));
}

void SvcTable::Impl::GetThreadInfo(SvcFrame& f) {
    // r1 thread handle, r2 type; out r1/r2 an s64.
    RCtx& c = f.ctx;
    const auto thread = kernel.CurrentProcess()->handle_table.Get<Thread>(c.r[1]);
    if (!thread) {
        Return64(c, kResultInvalidHandle, 0);
        return;
    }
    if (c.r[2] != kThreadInfoTlsAddress) {
        MH4N_LOG(Error, "Kernel.SVC", "unknown GetThreadInfo type={}", c.r[2]);
        Return64(c, kResultInvalidEnumValue, 0);
        return;
    }
    Return64(c, kResultSuccess, thread->tls_address);
}

void SvcTable::Impl::GetProcessId(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto process = kernel.CurrentProcess()->handle_table.Get<Process>(c.r[1]);
    if (!process) {
        Return(c, kResultInvalidHandle, 0);
        return;
    }
    Return(c, kResultSuccess, process->process_id);
}

void SvcTable::Impl::GetThreadId(SvcFrame& f) {
    RCtx& c = f.ctx;
    const auto thread = kernel.CurrentProcess()->handle_table.Get<Thread>(c.r[1]);
    if (!thread) {
        Return(c, kResultInvalidHandle, 0);
        return;
    }
    Return(c, kResultSuccess, thread->thread_id);
}

void SvcTable::Impl::GetResourceLimit(SvcFrame& f) {
    RCtx& c = f.ctx;
    Process& current = *kernel.CurrentProcess();
    const auto process = current.handle_table.Get<Process>(c.r[1]);
    if (!process) {
        Return(c, kResultInvalidHandle, 0);
        return;
    }
    Handle handle = 0;
    const Result result = current.handle_table.Create(&handle, process->resource_limit);
    Return(c, result, handle);
}

void SvcTable::Impl::GetResourceLimitValues(SvcFrame& f, bool current) {
    // r0 values address, r1 resource limit, r2 names address, r3 count. Each name is a u32 in
    // guest memory and each value an s64 written next to the previous one.
    RCtx& c = f.ctx;
    const u32 values = c.r[0];
    const u32 names = c.r[2];
    const u32 name_count = c.r[3];
    const auto limit = kernel.CurrentProcess()->handle_table.Get<ResourceLimit>(c.r[1]);
    if (!limit) {
        Return(c, kResultInvalidHandle);
        return;
    }
    for (u32 i = 0; i < name_count; ++i) {
        // Azahar's Read32 of an unmapped address logs and gives 0.
        const u32 name = memory.Read<u32>(names + i * static_cast<u32>(sizeof(u32))).value_or(0);
        // Azahar checks the name for the limit values only and indexes its array with whatever the
        // current-values call is given; both are refused here.
        if (name >= static_cast<u32>(ResourceLimitType::Max)) {
            Return(c, kResultInvalidEnumValue);
            return;
        }
        const auto type = static_cast<ResourceLimitType>(name);
        const s64 value = current ? limit->GetCurrentValue(type) : limit->GetLimitValue(type);
        if (!memory.Write<s64>(values + i * static_cast<u32>(sizeof(u64)), value)) {
            MH4N_LOG(Error, "Kernel.SVC", "resource limit values: write to unmapped address 0x{:08X}",
                     values + i * static_cast<u32>(sizeof(u64)));
        }
    }
    Return(c, kResultSuccess);
}

// ---- IPC ------------------------------------------------------------------------------------------------

void SvcTable::Impl::ConnectToPort(SvcFrame& f) {
    // r1 the address of the port name; out r1 the client session's handle.
    RCtx& c = f.ctx;
    const u32 name_address = c.r[1];
    if (!memory.IsMapped(name_address, 1)) {
        Return(c, kResultNotFound, 0);
        return;
    }
    // One character past the longest name allowed, to tell a name that is too long.
    const std::string name = ReadCString(memory, name_address, kPortNameMaxLength + 1);
    if (name.size() > kPortNameMaxLength) {
        Return(c, kResultPortNameTooLong, 0);
        return;
    }
    if (!connect_to_port) {
        Return(c, kResultNotFound, 0);
        return;
    }
    std::shared_ptr<Object> session;
    Result result = connect_to_port(name, &session);
    Handle handle = 0;
    if (!IsError(result)) {
        MH4N_ASSERT(session != nullptr, "the handler for port '{}' succeeded without a session", name);
        result = kernel.AddHandle(&handle, std::move(session));
    }
    Return(c, result, handle);
}

void SvcTable::Impl::SendSyncRequest(SvcFrame& f) {
    RCtx& c = f.ctx;
    Result result = kResultInvalidHandle;
    if (send_sync_request) {
        result = send_sync_request(f.thread, c.r[0]);
    }
    if (result != kResultInvalidHandle) {
        kernel.GetScheduler().PrepareReschedule();  // Azahar, once the handle is known to be a session
    }
    if (f.thread.status != ThreadStatus::Running) {
        // The reply is not ready: the thread waits for it. Its r0 is the handler's result unless the
        // wake-up chose another.
        result = sched.Park(f.thread).result.value_or(result);
    }
    Return(c, result);
}

// ---- the end of the run -----------------------------------------------------------------------------------

void SvcTable::Impl::Break(SvcFrame& f) {
    RCtx& c = f.ctx;
    const u8 reason = static_cast<u8>(c.r[0]);
    const char* reason_text = reason == 0 ? "PANIC" : reason == 1 ? "ASSERT" : reason == 2 ? "USER" : "UNKNOWN";
    MH4N_LOG(Critical, "Debug.Emulated", "Emulated program broke execution! Reason: {}", reason_text);
    {
        std::string report = fmt::format("svcBreak: reason {} ({}) in thread {}\n", reason_text,
                                         static_cast<u32>(reason), f.thread.thread_id);
        for (int i = 0; i < 16; ++i) {
            report += fmt::format("r{:<2}={:08X}{}", i, c.r[i], i % 4 == 3 ? "\n" : " ");
        }
        MH4N_LOG(Critical, "Debug.Emulated", "{}", report);
        if (!exit) {
            exit.emplace(SvcExit{3, std::move(report)});
        }
    }  // nothing owning is left on the stack for the last switch of this fiber
    EndRun(f);
}

void SvcTable::Impl::OutputDebugString(SvcFrame& f) {
    // r0 address, r1 length. No outputs. A length of 0 is the GDB HIO request, which only exists
    // with Azahar's gdb stub.
    RCtx& c = f.ctx;
    const u32 address = c.r[0];
    const s32 length = static_cast<s32>(c.r[1]);
    if (!memory.IsMapped(address, 1)) {
        MH4N_LOG(Warning, "Kernel.SVC", "OutputDebugString called with invalid address {:X}", address);
        return;
    }
    if (length == 0) {
        return;
    }
    if (length < 0) {
        MH4N_LOG(Warning, "Kernel.SVC", "OutputDebugString called with invalid length {}", length);
        return;
    }
    if (length > kMaxDebugStringLength) {
        // Azahar allocates a string of any length; a guest must not be able to ask for gigabytes.
        MH4N_LOG(Warning, "Kernel.SVC", "OutputDebugString called with a length of {}: ignored",
                 length);
        return;
    }
    // ReadBlock of memory that is partly unmapped gives zeros for the missing part in Azahar.
    std::string text(static_cast<std::size_t>(length), '\0');
    for (u32 done = 0; done < static_cast<u32>(length);) {
        const u32 here = address + done;
        const u32 chunk = std::min<u32>(static_cast<u32>(length) - done, kPageSize - (here & kPageMask));
        if (!memory.ReadBlock(text.data() + done, here, chunk)) {
            std::memset(text.data() + done, 0, chunk);
        }
        done += chunk;
    }
    MH4N_LOG(Debug, "Debug.Emulated", "{}", text);
    if (hooks.debug_string) {
        hooks.debug_string(text);
    }
}

void SvcTable::Impl::ControlProcess(SvcFrame& f) {
    // r0 process handle, r1 operation, r2 and r3 their arguments. Only the operation the thread
    // manager needs is implemented.
    RCtx& c = f.ctx;
    const auto process = kernel.CurrentProcess()->handle_table.Get<Process>(c.r[0]);
    if (!process) {
        Return(c, kResultInvalidHandle);
        return;
    }
    if (c.r[1] == kProcessOpDisableCreateThreadRestrictions) {
        threads.SetNoThreadRestrictions(c.r[2] == 1);
        Return(c, kResultSuccess);
        return;
    }
    MH4N_LOG(Error, "Kernel.SVC", "Unknown ControlProcessOp type={}", c.r[1]);
    Return(c, kResultNotImplemented);
}

}  // namespace mh4n
