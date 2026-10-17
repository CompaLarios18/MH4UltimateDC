/* Kernel conformance: system tick, GetSystemInfo, GetProcessInfo, resource limits, ids,
 * DuplicateHandle, closing handles and GetHandleInfo. Values that depend on timing or on the
 * size of this program's image are printed as page-alignment or monotonicity checks. Our own
 * program, no game code. */
#include "svc.h"

#define TICKS_PER_MS 268111ULL   /* the 3DS's ARM11 clock: 268111856 Hz */

static char name_buf[48];
static const char *cat(const char *a, u32 n, const char *b) {
    u32 k = 0;
    for (u32 i = 0; a[i]; i++) name_buf[k++] = a[i];
    if (n < 1000) {
        if (n >= 100) name_buf[k++] = (char)('0' + n / 100);
        if (n >= 10) name_buf[k++] = (char)('0' + n / 10 % 10);
        name_buf[k++] = (char)('0' + n % 10);
    }
    for (u32 i = 0; b[i]; i++) name_buf[k++] = b[i];
    name_buf[k] = 0;
    return name_buf;
}

static u32 lo(s64 v) { return (u32)v; }
static u32 hi(s64 v) { return (u32)((u64)v >> 32); }

static void tick_checks(void) {
    u64 a = svcGetSystemTick();
    u64 b = svcGetSystemTick();
    check("tick.monotonic", b >= a);
    svcSleepThread(2 * ONE_MS);
    u64 c = svcGetSystemTick();
    check("tick.advances_over_sleep", c - b >= 2 * TICKS_PER_MS);
}

static void system_info_checks(void) {
    s64 v;
    for (u32 region = 0; region < 4; region++) {
        v = -1;
        s32 rc = svcGetSystemInfo(&v, 0, (s32)region);
        check(cat("sysinfo.memory_region", region, ".rc"), (u32)rc);
        check(cat("sysinfo.memory_region", region, ".page_aligned"), (lo(v) & 0xFFF) == 0 && hi(v) == 0);
        check(cat("sysinfo.memory_region", region, ".nonzero"), v != 0);
    }
    v = -1;
    check("sysinfo.memory_region_bad.rc", (u32)svcGetSystemInfo(&v, 0, 9));
    check("sysinfo.memory_region_bad.value", lo(v));
    v = -1;
    check("sysinfo.kernel_pages.rc", (u32)svcGetSystemInfo(&v, 2, 0));
    v = -1;
    check("sysinfo.spawned_pids.rc", (u32)svcGetSystemInfo(&v, 26, 0));
    check("sysinfo.spawned_pids.value", lo(v));
    v = -1;
    check("sysinfo.unknown_type.rc", (u32)svcGetSystemInfo(&v, 0x1234, 0));
    check("sysinfo.unknown_type.value", lo(v));
    v = -1;
    check("sysinfo.new3ds.rc", (u32)svcGetSystemInfo(&v, 0x10001, 0));   /* error on an Old 3DS */
}

static void process_info_checks(void) {
    static const u32 types[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 20, 21, 22, 23, 24, 25, 100};
    for (u32 i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        s64 v = -1;
        u32 t = types[i];
        s32 rc = svcGetProcessInfo(&v, CUR_PROCESS, t);
        check(cat("procinfo.type", t, ".rc"), (u32)rc);
        if (rc != 0) continue;
        if (t == 0 || t == 2) {   /* the amount used depends on the program's size */
            check(cat("procinfo.type", t, ".page_aligned"), (lo(v) & 0xFFF) == 0 && hi(v) == 0);
            check(cat("procinfo.type", t, ".nonzero"), v != 0);
        } else if (t == 20) {
            check(cat("procinfo.type", t, ".lo"), lo(v));
            check(cat("procinfo.type", t, ".hi"), hi(v));
        }
    }
    s64 v = -1;
    check("procinfo.bad_handle.rc", (u32)svcGetProcessInfo(&v, 0x12345678, 0));
    check("procinfo.thread_handle.rc", (u32)svcGetProcessInfo(&v, CUR_THREAD, 0));
}

static void limit_checks(void) {
    Handle rl = 0;
    check("limit.get_rc", (u32)svcGetResourceLimit(&rl, CUR_PROCESS));
    check("limit.handle_set", rl != 0);
    Handle unused = 0;
    check("limit.bad_process_rc", (u32)svcGetResourceLimit(&unused, 0x12345678));
    s64 values[10];
    u32 names[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 0; i < 10; i++) values[i] = -1;
    check("limit.limits_rc", (u32)svcGetResourceLimitLimitValues(values, rl, names, 10));
    for (u32 i = 0; i < 10; i++) {
        if (i == 1 || i == 9) {   /* commit and cpu time: print only that they are set */
            check(cat("limit.limit", i, ".nonzero"), values[i] != 0);
        } else {
            check(cat("limit.limit", i, ""), lo(values[i]));
        }
    }
    u32 bad_name[1] = {10};
    check("limit.bad_name_rc", (u32)svcGetResourceLimitLimitValues(values, rl, bad_name, 1));
    check("limit.bad_handle_rc", (u32)svcGetResourceLimitLimitValues(values, 0x12345678, names, 1));

    /* creating objects raises the current values by one each */
    s64 before[10], after[10];
    svcGetResourceLimitCurrentValues(before, rl, names, 10);
    Handle e, m, s, t, a;
    svcCreateEvent(&e, 0);
    svcCreateMutex(&m, 0);
    svcCreateSemaphore(&s, 0, 1);
    svcCreateTimer(&t, 0);
    svcCreateAddressArbiter(&a);
    check("limit.current_rc", (u32)svcGetResourceLimitCurrentValues(after, rl, names, 10));
    static const u32 which[] = {3, 4, 5, 6, 8};
    for (u32 i = 0; i < 5; i++) check(cat("limit.current", which[i], ".delta"), lo(after[which[i]] - before[which[i]]));
    svcCloseHandle(e); svcCloseHandle(m); svcCloseHandle(s); svcCloseHandle(t); svcCloseHandle(a);
    svcGetResourceLimitCurrentValues(after, rl, names, 10);
    for (u32 i = 0; i < 5; i++) check(cat("limit.closed", which[i], ".delta"), lo(after[which[i]] - before[which[i]]));
    svcCloseHandle(rl);
}

static u8 stack[STACK_SIZE_CONF] __attribute__((aligned(8)));
static void idle(u32 unused) { svcExitThread(); }

static void id_checks(void) {
    u32 pid = 0xAAAA, pid2 = 0xAAAA, tid = 0xAAAA, tid2 = 0xAAAA;
    check("id.process_rc", (u32)svcGetProcessId(&pid, CUR_PROCESS));
    check("id.process", pid);
    check("id.process_bad_rc", (u32)svcGetProcessId(&pid2, 0x12345678));
    check("id.thread_rc", (u32)svcGetThreadId(&tid, CUR_THREAD));
    check("id.thread_bad_rc", (u32)svcGetThreadId(&tid2, 0x12345678));
    Handle t = spawn(idle, 0, (u32 *)stack, MAIN_PRIO + 1);
    svcGetThreadId(&tid2, t);
    check("id.second_thread_is_next", tid2 == tid + 1);
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
    u32 tid3 = 0;
    t = spawn(idle, 0, (u32 *)stack, MAIN_PRIO + 1);
    svcGetThreadId(&tid3, t);
    check("id.third_thread_is_next", tid3 == tid2 + 1);
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
}

static void handle_checks(void) {
    Handle e = 0, d = 0, d2 = 0;
    svcCreateEvent(&e, 1);
    s64 v = -1;
    check("handle.refcount_rc", (u32)svcGetHandleInfo(&v, e, 1));
    s64 base = v;
    check("handle.refcount_at_least_one", v >= 1);
    check("handle.dup_rc", (u32)svcDuplicateHandle(&d, e));
    check("handle.dup_differs", d != e && d != 0);
    svcGetHandleInfo(&v, e, 1);
    check("handle.refcount_after_dup_delta", lo(v - base));
    svcSignalEvent(e);
    check("handle.dup_sees_signal", (u32)svcWaitSynchronization(d, 0));
    check("handle.close_original", (u32)svcCloseHandle(e));
    check("handle.dup_still_works", (u32)svcWaitSynchronization(d, 0));
    check("handle.dup_of_dup_rc", (u32)svcDuplicateHandle(&d2, d));
    check("handle.close_dup", (u32)svcCloseHandle(d));
    check("handle.close_dup_twice", (u32)svcCloseHandle(d));
    check("handle.close_last", (u32)svcCloseHandle(d2));
    check("handle.close_again", (u32)svcCloseHandle(d2));
    check("handle.close_zero", (u32)svcCloseHandle(0));
    check("handle.close_garbage", (u32)svcCloseHandle(0x12345678));
    check("handle.close_cur_thread", (u32)svcCloseHandle(CUR_THREAD));
    check("handle.close_cur_process", (u32)svcCloseHandle(CUR_PROCESS));
    check("handle.dup_closed_rc", (u32)svcDuplicateHandle(&d, e));
    check("handle.dup_pseudo_rc", (u32)svcDuplicateHandle(&d, CUR_THREAD));
    if (d) svcCloseHandle(d);

    svcCreateEvent(&e, 0);
    check("handle.info_type_0_event_rc", (u32)svcGetHandleInfo(&v, e, 0));
    check("handle.info_type_2_rc", (u32)svcGetHandleInfo(&v, e, 2));
    check("handle.info_type_0x32107_rc", (u32)svcGetHandleInfo(&v, e, 0x32107));
    check("handle.info_type_3_rc", (u32)svcGetHandleInfo(&v, e, 3));
    check("handle.info_bad_handle_rc", (u32)svcGetHandleInfo(&v, 0x12345678, 1));
    check("handle.info_process_ticks_rc", (u32)svcGetHandleInfo(&v, CUR_PROCESS, 0));
    check("handle.info_process_ticks_set", v != 0);
    svcCloseHandle(e);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_info: start");
    tick_checks();
    system_info_checks();
    process_info_checks();
    limit_checks();
    id_checks();
    handle_checks();
    finish("conf_info: done");
}
