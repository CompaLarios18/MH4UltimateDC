/* Kernel conformance: WaitSynchronization1 and N (timeouts, wait on any and on all, indices,
 * invalid handles). Our own program, no game code. */
#include "svc.h"

static u8 stacks[2][STACK_SIZE_CONF] __attribute__((aligned(8)));
static Handle ev_[4];     /* one-shot events */
static Handle sticky;
static volatile u32 which;

/* the index is only meaningful when the call succeeded (Azahar leaves garbage otherwise) */
static s32 waitn(s32 *idx, const Handle *h, s32 n, u32 all, s64 ns) {
    s32 r = svcWaitSynchronizationN(idx, h, n, all, ns);
    if (r != 0) *idx = -1;
    return r;
}

static void line(const char *name, u32 v) { check(name, v); }

/* signals ev_[which] and exits */
static void signaller(u32 id) {
    svcSignalEvent(ev_[which]);
    svcExitThread();
}

static void wait1_checks(void) {
    Handle e = 0;
    svcCreateEvent(&e, 0);
    line("w1.unsignalled_0", (u32)svcWaitSynchronization(e, 0));
    line("w1.unsignalled_1ms", (u32)svcWaitSynchronization(e, ONE_MS));
    line("w1.unsignalled_negative_short", (u32)svcWaitSynchronization(e, 1));
    svcSignalEvent(e);
    line("w1.signalled_0", (u32)svcWaitSynchronization(e, 0));
    line("w1.consumed_0", (u32)svcWaitSynchronization(e, 0));
    svcSignalEvent(e);
    line("w1.signalled_forever", (u32)svcWaitSynchronization(e, FOREVER));
    line("w1.zero_handle", (u32)svcWaitSynchronization(0, 0));
    line("w1.bad_handle", (u32)svcWaitSynchronization(0x12345678, 0));
    line("w1.closed_handle", (u32)(svcCloseHandle(e), svcWaitSynchronization(e, 0)));
    line("w1.cur_thread_pseudo", (u32)svcWaitSynchronization(CUR_THREAD, 0));
    line("w1.cur_process_pseudo", (u32)svcWaitSynchronization(CUR_PROCESS, 0));
    /* a thread that signals while main waits with a finite timeout */
    svcCreateEvent(&ev_[0], 0);
    which = 0;
    Handle t = spawn(signaller, 0, (u32 *)stacks[0], MAIN_PRIO + 1);
    line("w1.wakes_before_timeout", (u32)svcWaitSynchronization(ev_[0], 500 * ONE_MS));
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
    svcCloseHandle(ev_[0]);
}

static void waitn_checks(void) {
    s32 idx;
    for (int i = 0; i < 4; i++) svcCreateEvent(&ev_[i], 0);
    svcCreateEvent(&sticky, 1);
    svcSignalEvent(ev_[1]);
    svcSignalEvent(ev_[2]);

    idx = 0x77;
    line("wn.any_rc", (u32)waitn(&idx, ev_, 4, 0, 0));
    line("wn.any_index", (u32)idx);            /* first signalled object in the list */
    idx = 0x77;
    line("wn.any_again_rc", (u32)waitn(&idx, ev_, 4, 0, 0));
    line("wn.any_again_index", (u32)idx);      /* the first one was consumed */
    idx = 0x77;
    line("wn.any_none_rc", (u32)waitn(&idx, ev_, 4, 0, 0));
    line("wn.any_none_index", (u32)idx);
    idx = 0x77;
    line("wn.any_timeout_rc", (u32)waitn(&idx, ev_, 4, 0, ONE_MS));

    /* wait on all: nothing is consumed until every object is signalled */
    svcSignalEvent(ev_[0]);
    svcSignalEvent(ev_[1]);
    svcSignalEvent(ev_[2]);
    idx = 0x77;
    line("wn.all_partial_rc", (u32)waitn(&idx, ev_, 4, 1, 0));
    line("wn.all_partial_0", (u32)svcWaitSynchronization(ev_[0], 0));   /* still signalled? */
    svcSignalEvent(ev_[0]);
    svcSignalEvent(ev_[3]);
    idx = 0x77;
    line("wn.all_full_rc", (u32)waitn(&idx, ev_, 4, 1, 0));
    line("wn.all_full_index", (u32)idx);
    for (int i = 0; i < 4; i++) {
        char name[16] = "wn.all_after_0";
        name[13] = (char)('0' + i);
        line(name, (u32)svcWaitSynchronization(ev_[i], 0));
    }

    /* a sticky object stays signalled, so the list is satisfied again */
    svcSignalEvent(sticky);
    Handle mix[3] = {ev_[0], sticky, ev_[1]};
    idx = 0x77;
    line("wn.sticky_rc", (u32)waitn(&idx, mix, 3, 0, 0));
    line("wn.sticky_index", (u32)idx);
    idx = 0x77;
    line("wn.sticky_again_rc", (u32)waitn(&idx, mix, 3, 0, 0));
    line("wn.sticky_again_index", (u32)idx);

    /* the same handle twice */
    Handle twice[2] = {ev_[2], ev_[2]};   /* waiting on all of these asserts in Azahar */
    svcSignalEvent(ev_[2]);
    idx = 0x77;
    line("wn.same_twice_any_rc", (u32)waitn(&idx, twice, 2, 0, 0));
    line("wn.same_twice_any_index", (u32)idx);

    /* bad arguments */
    Handle bad[2] = {ev_[0], 0x12345678};
    idx = 0x77;
    line("wn.bad_handle_rc", (u32)waitn(&idx, bad, 2, 0, 0));
    idx = 0x77;
    line("wn.count_zero_any_rc", (u32)waitn(&idx, ev_, 0, 0, 0));
    idx = 0x77;
    line("wn.count_zero_all_rc", (u32)waitn(&idx, ev_, 0, 1, 0));
    idx = 0x77;
    line("wn.count_negative_rc", (u32)waitn(&idx, ev_, -1, 0, 0));
    idx = 0x77;
    line("wn.count_too_many_rc", (u32)waitn(&idx, ev_, 0x41, 0, 0));
    idx = 0x77;
    line("wn.null_list_rc", (u32)waitn(&idx, (const Handle *)0, 2, 0, 0));

    /* a thread signals the third event while main waits on all four with no timeout */
    which = 2;
    svcSignalEvent(ev_[0]);
    svcSignalEvent(ev_[1]);
    svcSignalEvent(ev_[3]);
    Handle t = spawn(signaller, 0, (u32 *)stacks[0], MAIN_PRIO + 1);
    idx = 0x77;
    line("wn.blocking_all_rc", (u32)waitn(&idx, ev_, 4, 1, FOREVER));
    line("wn.blocking_all_index", (u32)idx);
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);

    /* ... and on any: the index is the one that was signalled */
    which = 3;
    t = spawn(signaller, 0, (u32 *)stacks[1], MAIN_PRIO + 1);
    idx = 0x77;
    line("wn.blocking_any_rc", (u32)waitn(&idx, ev_, 4, 0, FOREVER));
    line("wn.blocking_any_index", (u32)idx);
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_wait: start");
    wait1_checks();
    waitn_checks();
    finish("conf_wait: done");
}
