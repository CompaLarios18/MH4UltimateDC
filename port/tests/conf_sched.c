/* Kernel conformance: scheduling. A higher-priority thread runs before CreateThread returns,
 * equal priorities run first in, first out, SleepThread(0) yields, thread priorities,
 * joining on a thread handle, and CreateThread's argument checks. Threads append a letter to a
 * log that main prints after joining, never from racing threads. Our own program, no game code. */
#include "svc.h"

static u8 stacks[6][STACK_SIZE_CONF] __attribute__((aligned(8)));
static char log_[64];
static volatile u32 log_len;
static Handle gate;

static void put(char c) { log_[log_len++] = c; log_[log_len] = 0; }

static void letter(u32 c) {
    put((char)c);
    svcExitThread();
}

/* logs `c`, yields, logs the lower case letter, exits */
static void yielder(u32 c) {
    put((char)c);
    svcSleepThread(0);
    put((char)(c + 32));
    svcExitThread();
}

/* blocks on the gate event, then logs */
static void gated(u32 c) {
    svcWaitSynchronization(gate, FOREVER);
    put((char)c);
    svcExitThread();
}

static void join_close(Handle t) {
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
}

static void order_checks(void) {
    Handle t[6];
    put('m');
    t[0] = spawn(letter, 'A', (u32 *)stacks[0], MAIN_PRIO - 1);   /* runs at once */
    put('n');
    check_text("sched.higher_runs_first", log_);
    join_close(t[0]);

    /* equal priority: nothing runs until main blocks or yields; then first in, first out */
    log_len = 0;
    put('m');
    t[0] = spawn(letter, 'B', (u32 *)stacks[0], MAIN_PRIO);
    t[1] = spawn(letter, 'C', (u32 *)stacks[1], MAIN_PRIO);
    t[2] = spawn(letter, 'D', (u32 *)stacks[2], MAIN_PRIO);
    put('n');
    check_text("sched.equal_before_yield", log_);
    svcSleepThread(0);
    put('o');
    check_text("sched.equal_after_yield", log_);
    for (int i = 0; i < 3; i++) join_close(t[i]);

    /* lower priority threads wait until main blocks */
    log_len = 0;
    put('m');
    t[0] = spawn(letter, 'E', (u32 *)stacks[0], MAIN_PRIO + 1);
    svcSleepThread(0);
    put('n');
    check_text("sched.lower_after_yield", log_);
    svcWaitSynchronization(t[0], FOREVER);
    put('o');
    check_text("sched.lower_after_join", log_);
    svcCloseHandle(t[0]);

    /* yielding threads of the same priority take turns */
    log_len = 0;
    t[0] = spawn(yielder, 'P', (u32 *)stacks[0], MAIN_PRIO + 1);
    t[1] = spawn(yielder, 'Q', (u32 *)stacks[1], MAIN_PRIO + 1);
    t[2] = spawn(yielder, 'R', (u32 *)stacks[2], MAIN_PRIO + 1);
    for (int i = 0; i < 3; i++) join_close(t[i]);
    check_text("sched.round_robin", log_);

    /* threads released by an event run in priority order, then creation order */
    log_len = 0;
    svcCreateEvent(&gate, 1);
    t[0] = spawn(gated, 'a', (u32 *)stacks[0], MAIN_PRIO - 1);
    t[1] = spawn(gated, 'b', (u32 *)stacks[1], MAIN_PRIO - 3);
    t[2] = spawn(gated, 'c', (u32 *)stacks[2], MAIN_PRIO - 2);
    t[3] = spawn(gated, 'd', (u32 *)stacks[3], MAIN_PRIO - 3);
    svcSignalEvent(gate);
    check_text("sched.event_release_order", log_);
    for (int i = 0; i < 4; i++) join_close(t[i]);
    svcCloseHandle(gate);
}

static void priority_checks(void) {
    s32 p = -1;
    check("prio.get_current_rc", (u32)svcGetThreadPriority(&p, CUR_THREAD));
    check("prio.get_current", (u32)p);
    check("prio.set_current_rc", (u32)svcSetThreadPriority(CUR_THREAD, MAIN_PRIO - 1));
    svcGetThreadPriority(&p, CUR_THREAD);
    check("prio.after_set", (u32)p);
    check("prio.set_64_rc", (u32)svcSetThreadPriority(CUR_THREAD, 64));
    svcGetThreadPriority(&p, CUR_THREAD);
    check("prio.after_set_64", (u32)p);
    check("prio.set_negative_rc", (u32)svcSetThreadPriority(CUR_THREAD, -1));
    check("prio.set_bad_handle_rc", (u32)svcSetThreadPriority(0x12345678, MAIN_PRIO));
    check("prio.get_bad_handle_rc", (u32)svcGetThreadPriority(&p, 0x12345678));
    svcSetThreadPriority(CUR_THREAD, MAIN_PRIO);

    /* raising another thread above main preempts main before SetThreadPriority returns */
    log_len = 0;
    Handle t = spawn(letter, 'X', (u32 *)stacks[0], MAIN_PRIO + 1);
    put('m');
    check("prio.other_get_rc", (u32)svcGetThreadPriority(&p, t));
    check("prio.other_get", (u32)p);
    svcSetThreadPriority(t, MAIN_PRIO - 1);
    put('n');
    check_text("prio.raise_preempts", log_);
    join_close(t);

    /* lowering main below a ready thread lets that thread run */
    log_len = 0;
    t = spawn(letter, 'Y', (u32 *)stacks[0], MAIN_PRIO + 1);
    svcSetThreadPriority(CUR_THREAD, MAIN_PRIO + 2);
    put('m');
    svcSetThreadPriority(CUR_THREAD, MAIN_PRIO);
    check_text("prio.lower_self_yields", log_);
    join_close(t);
}

static void create_checks(void) {
    Handle h = 0;
    static u32 *const stack = (u32 *)(stacks[5] + STACK_SIZE_CONF);
    struct { const char *name; s32 prio, core; } tests[] = {
        {"create.prio_63_core_default", 63, -2}, {"create.prio_64", 64, -2},
        {"create.prio_0x100", 0x100, -2},        {"create.prio_negative", -1, -2},
        {"create.prio_0", 0, -2},                {"create.prio_8", 8, -2},
        {"create.prio_0x18", 0x18, -2},          {"create.core_0", MAIN_PRIO, 0},
        {"create.core_1", MAIN_PRIO, 1},         {"create.core_all", MAIN_PRIO, -1},
        {"create.core_4", MAIN_PRIO, 4},         {"create.core_5", MAIN_PRIO, 5},
        {"create.core_minus_3", MAIN_PRIO, -3},
    };
    for (u32 i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        h = 0;
        s32 r = svcCreateThread(&h, letter, '-', stack, tests[i].prio, tests[i].core);
        check(tests[i].name, (u32)r);
        if (r == 0) {   /* let it run and finish before the stack is used again */
            svcSleepThread(0);
            svcWaitSynchronization(h, FOREVER);
            svcCloseHandle(h);
        }
    }
}

static void id_checks(void) {
    Handle t = spawn(letter, 'Z', (u32 *)stacks[0], MAIN_PRIO + 1);
    u32 main_id = 0, other_id = 0;
    check("thread.id_rc", (u32)svcGetThreadId(&main_id, CUR_THREAD));
    svcGetThreadId(&other_id, t);
    check("thread.ids_differ", main_id != other_id);
    join_close(t);
    check("thread.join_closed_handle", (u32)svcWaitSynchronization(t, 0));
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_sched: start");
    order_checks();
    priority_checks();
    id_checks();
    create_checks();
    finish("conf_sched: done");
}
