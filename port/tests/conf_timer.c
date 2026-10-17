/* Kernel conformance: one-shot, sticky and pulse timers, periodic timers, cancel and clear.
 * Only what does not depend on timing is printed: whether a timer fired within a generous wait
 * (never a tick count or the number of firings, only whether a threshold was reached).
 * Our own program, no game code. */
#include "svc.h"

#define MS ONE_MS
#define GENEROUS (2000 * MS)     /* a firing is due long before this */
#define SHORT (3 * MS)           /* long enough for the timer to have fired, if it were going to */

static const char *const kinds[3] = {"oneshot", "sticky", "pulse"};

static void t_line(const char *kind, const char *what, u32 v) {
    char buf[48];
    u32 n = 0;
    for (u32 i = 0; kind[i]; i++) buf[n++] = kind[i];
    buf[n++] = '.';
    for (u32 i = 0; what[i]; i++) buf[n++] = what[i];
    buf[n] = 0;
    check(buf, v);
}

/* How many times a periodic timer fires within `tries` waits; reports only the threshold. */
static u32 fired_at_least(Handle t, u32 tries) {
    u32 fired = 0;
    for (u32 i = 0; i < tries; i++)
        if (svcWaitSynchronization(t, GENEROUS) == 0) fired++;
    return fired >= tries;
}

static void kind_checks(u32 type) {
    const char *k = kinds[type];
    Handle t = 0;
    t_line(k, "create", (u32)svcCreateTimer(&t, type));
    t_line(k, "wait0_not_set", (u32)svcWaitSynchronization(t, 0));

    /* a one-shot firing */
    t_line(k, "set_1ms", (u32)svcSetTimer(t, 1 * MS, 0));
    t_line(k, "wait0_right_after_set", (u32)svcWaitSynchronization(t, 0));
    t_line(k, "fires", (u32)svcWaitSynchronization(t, GENEROUS));
    t_line(k, "wait0_after_fire", (u32)svcWaitSynchronization(t, 0));
    svcSleepThread(SHORT);
    t_line(k, "wait0_after_sleep", (u32)svcWaitSynchronization(t, 0));
    t_line(k, "clear", (u32)svcClearTimer(t));
    t_line(k, "wait0_after_clear", (u32)svcWaitSynchronization(t, 0));

    /* initial 0 */
    t_line(k, "set_0", (u32)svcSetTimer(t, 0, 0));
    t_line(k, "fires_from_0", (u32)svcWaitSynchronization(t, GENEROUS));
    svcClearTimer(t);

    /* cancel before it fires */
    t_line(k, "set_far", (u32)svcSetTimer(t, 500 * MS, 0));
    t_line(k, "cancel", (u32)svcCancelTimer(t));
    t_line(k, "cancelled_wait", (u32)svcWaitSynchronization(t, SHORT));
    t_line(k, "cancel_again", (u32)svcCancelTimer(t));

    /* periodic: it keeps firing until cancelled */
    t_line(k, "set_periodic", (u32)svcSetTimer(t, 1 * MS, 1 * MS));
    t_line(k, "periodic_fires_5_times", fired_at_least(t, 5));
    t_line(k, "cancel_periodic", (u32)svcCancelTimer(t));
    svcClearTimer(t);
    t_line(k, "silent_after_cancel", (u32)svcWaitSynchronization(t, SHORT));

    /* setting a timer again replaces the previous setting */
    svcSetTimer(t, 500 * MS, 0);
    t_line(k, "reset", (u32)svcSetTimer(t, 1 * MS, 0));
    t_line(k, "reset_fires", (u32)svcWaitSynchronization(t, GENEROUS));
    svcClearTimer(t);

    t_line(k, "negative_initial", (u32)svcSetTimer(t, -1, 0));
    t_line(k, "negative_interval", (u32)svcSetTimer(t, 1 * MS, -1));
    svcCancelTimer(t);
    t_line(k, "close", (u32)svcCloseHandle(t));
}

static volatile u32 waiter_rc, waiter_done;
static Handle shared_timer;
static u8 stack[STACK_SIZE_CONF] __attribute__((aligned(8)));

static void waiter(u32 unused) {
    waiter_rc = (u32)svcWaitSynchronization(shared_timer, GENEROUS);
    waiter_done = 1;
    svcExitThread();
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_timer: start");
    for (u32 type = 0; type < 3; type++) kind_checks(type);

    Handle bad = 0;
    check("timer.bad_reset_type", (u32)svcCreateTimer(&bad, 3));
    check("timer.set_bad_handle", (u32)svcSetTimer(0x12345678, 1 * MS, 0));
    check("timer.cancel_bad_handle", (u32)svcCancelTimer(0x12345678));
    check("timer.clear_bad_handle", (u32)svcClearTimer(0x12345678));

    /* a blocked waiter on a pulse timer is woken by the firing */
    svcCreateTimer(&shared_timer, 2);
    waiter_done = 0;
    Handle th = spawn(waiter, 0, (u32 *)stack, MAIN_PRIO - 1);
    check("timer.waiter_blocked", waiter_done);
    svcSetTimer(shared_timer, 1 * MS, 0);
    svcWaitSynchronization(th, GENEROUS);
    check("timer.waiter_woke", waiter_done);
    check("timer.waiter_rc", waiter_rc);
    svcCloseHandle(th);
    svcCloseHandle(shared_timer);
    finish("conf_timer: done");
}
