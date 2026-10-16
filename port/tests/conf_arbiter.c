/* Kernel conformance: address arbiters. The five arbitration types, signal counts of 1 and -1
 * (and the order threads are woken in), timeouts. Our own program, no game code. */
#include "svc.h"

enum { SIGNAL, WAIT_LT, DEC_WAIT_LT, WAIT_LT_TIMEOUT, DEC_WAIT_LT_TIMEOUT };

static u8 stacks[4][STACK_SIZE_CONF] __attribute__((aligned(8)));
static Handle arb;
static volatile s32 word;                /* the arbitrated address */
static volatile u32 done[4], rc[4];
static volatile u32 order;               /* wake order, one hex digit per thread */

static void waiter(u32 id) {
    rc[id] = (u32)svcArbitrateAddress(arb, (u32)&word, WAIT_LT, 100, FOREVER);
    done[id] = 1;
    order = order * 16 + id + 1;
    svcExitThread();
}

static void timed_waiter(u32 id) {
    rc[id] = (u32)svcArbitrateAddress(arb, (u32)&word, WAIT_LT_TIMEOUT, 100, 400 * ONE_MS);
    done[id] = 1;
    order = order * 16 + id + 1;
    svcExitThread();
}

static void reset(void) {
    for (int i = 0; i < 4; i++) done[i] = rc[i] = 0;
    order = 0;
    word = 0;
}

static u32 done_mask(void) { return done[0] | done[1] << 1 | done[2] << 2 | done[3] << 3; }

static void immediate_checks(void) {
    check("arb.create", (u32)svcCreateAddressArbiter(&arb));

    word = 10;   /* the value is not below the compare value: nobody waits */
    check("arb.wait_lt_not_less_rc", (u32)svcArbitrateAddress(arb, (u32)&word, WAIT_LT, 5, 0));
    check("arb.wait_lt_not_less_word", (u32)word);
    check("arb.dec_not_less_rc", (u32)svcArbitrateAddress(arb, (u32)&word, DEC_WAIT_LT, 5, 0));
    check("arb.dec_not_less_word", (u32)word);
    check("arb.wait_timeout_not_less_rc",
          (u32)svcArbitrateAddress(arb, (u32)&word, WAIT_LT_TIMEOUT, 5, ONE_MS));
    check("arb.dec_timeout_not_less_rc",
          (u32)svcArbitrateAddress(arb, (u32)&word, DEC_WAIT_LT_TIMEOUT, 5, ONE_MS));
    check("arb.dec_timeout_not_less_word", (u32)word);
    word = -3;   /* the comparison is signed */
    check("arb.signed_timeout_rc", (u32)svcArbitrateAddress(arb, (u32)&word, WAIT_LT_TIMEOUT, 0, ONE_MS));
    word = 3;    /* below: waits and times out */
    check("arb.wait_timeout_rc", (u32)svcArbitrateAddress(arb, (u32)&word, WAIT_LT_TIMEOUT, 5, ONE_MS));
    check("arb.wait_timeout_word", (u32)word);
    check("arb.dec_timeout_rc", (u32)svcArbitrateAddress(arb, (u32)&word, DEC_WAIT_LT_TIMEOUT, 5, ONE_MS));
    check("arb.dec_timeout_word", (u32)word);   /* decremented, because the thread waited */
    check("arb.signal_nobody_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, 1, 0));
    check("arb.signal_all_nobody_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, -1, 0));
    check("arb.signal_zero_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, 0, 0));
    check("arb.bad_type_rc", (u32)svcArbitrateAddress(arb, (u32)&word, 5, 0, 0));
    check("arb.bad_type_big_rc", (u32)svcArbitrateAddress(arb, (u32)&word, 0xFFFFFFFF, 0, 0));
    check("arb.bad_handle_rc", (u32)svcArbitrateAddress(0x12345678, (u32)&word, WAIT_LT, 5, 0));
}

static void signal_checks(void) {
    /* three waiters, priorities 0x2E, 0x2D, 0x2E (created in that order) */
    static const s32 prio[3] = {MAIN_PRIO - 2, MAIN_PRIO - 3, MAIN_PRIO - 2};
    Handle t[3];
    reset();
    for (u32 i = 0; i < 3; i++) t[i] = spawn(waiter, i, (u32 *)stacks[i], prio[i]);
    check("arb.three_blocked", done_mask());
    check("arb.signal_1_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, 1, 0));
    check("arb.signal_1_woken", done_mask());          /* the highest priority one */
    check("arb.signal_1_rc_of_waiter", rc[1]);
    check("arb.signal_2_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, 2, 0));
    check("arb.signal_2_woken", done_mask());
    check("arb.wake_order", order);                    /* equal priorities: first in, first out */
    for (u32 i = 0; i < 3; i++) { svcWaitSynchronization(t[i], FOREVER); svcCloseHandle(t[i]); }

    reset();
    for (u32 i = 0; i < 3; i++) t[i] = spawn(waiter, i, (u32 *)stacks[i], prio[i]);
    check("arb.signal_all_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, -1, 0));
    check("arb.signal_all_woken", done_mask());
    check("arb.signal_all_order", order);
    for (u32 i = 0; i < 3; i++) { svcWaitSynchronization(t[i], FOREVER); svcCloseHandle(t[i]); }

    /* more signals than waiters */
    reset();
    t[0] = spawn(waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    check("arb.signal_5_rc", (u32)svcArbitrateAddress(arb, (u32)&word, SIGNAL, 5, 0));
    check("arb.signal_5_woken", done_mask());
    svcWaitSynchronization(t[0], FOREVER);
    svcCloseHandle(t[0]);

    /* a timed waiter woken by a signal still gets the timeout code (Azahar) */
    reset();
    t[0] = spawn(timed_waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    svcArbitrateAddress(arb, (u32)&word, SIGNAL, 1, 0);
    check("arb.timed_signalled_woken", done_mask());
    check("arb.timed_signalled_rc", rc[0]);
    svcWaitSynchronization(t[0], FOREVER);
    svcCloseHandle(t[0]);

    /* a timed waiter that nobody wakes times out; main waits with a longer timeout */
    reset();
    t[0] = spawn(timed_waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    check("arb.timed_blocked", done_mask());
    check("arb.timed_join_rc", (u32)svcWaitSynchronization(t[0], 5000 * ONE_MS));
    check("arb.timed_woken", done_mask());
    check("arb.timed_rc", rc[0]);
    svcCloseHandle(t[0]);

    /* the arbiter does not look at the address when signalling a different one */
    reset();
    t[0] = spawn(timed_waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    static volatile s32 other;
    svcArbitrateAddress(arb, (u32)&other, SIGNAL, -1, 0);
    check("arb.other_address_woken", done_mask());
    svcWaitSynchronization(t[0], 5000 * ONE_MS);
    svcCloseHandle(t[0]);

    /* waiting on WAIT_LT leaves the word alone */
    reset();
    word = 5;
    t[0] = spawn(timed_waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    check("arb.wait_does_not_change_word", (u32)word);
    svcArbitrateAddress(arb, (u32)&word, SIGNAL, -1, 0);
    svcWaitSynchronization(t[0], FOREVER);
    svcCloseHandle(t[0]);
    check("arb.close", (u32)svcCloseHandle(arb));
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_arbiter: start");
    immediate_checks();
    signal_checks();
    finish("conf_arbiter: done");
}
