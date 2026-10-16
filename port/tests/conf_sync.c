/* Kernel conformance: events of each reset type, mutexes and semaphores. Prints one line per
 * check; the expected output is what Azahar's kernel prints (port/tests/expected/). Our own
 * program, no game code. */
#include "svc.h"

static u8 stacks[3][STACK_SIZE_CONF] __attribute__((aligned(8)));
static volatile u32 woke, result_a, result_b;
static Handle shared;

static const char *const kinds[3] = {"oneshot", "sticky", "pulse"};

static void label(char *out, const char *a, const char *b) {
    u32 n = 0;
    for (u32 i = 0; a[i]; i++) out[n++] = a[i];
    out[n++] = '.';
    for (u32 i = 0; b[i]; i++) out[n++] = b[i];
    out[n] = 0;
}

static void ev(const char *kind, const char *what, u32 v) {
    char buf[48];
    label(buf, kind, what);
    check(buf, v);
}

/* waits forever on `shared`, then records the result */
static void waiter(u32 id) {
    s32 r = svcWaitSynchronization(shared, FOREVER);
    if (id == 0) result_a = (u32)r; else result_b = (u32)r;
    woke++;
    svcExitThread();
}

static void event_checks(u32 type) {
    const char *k = kinds[type];
    Handle e = 0;
    ev(k, "create", (u32)svcCreateEvent(&e, type));
    ev(k, "wait0_unsignalled", (u32)svcWaitSynchronization(e, 0));
    ev(k, "signal", (u32)svcSignalEvent(e));
    ev(k, "wait0_first", (u32)svcWaitSynchronization(e, 0));
    ev(k, "wait0_second", (u32)svcWaitSynchronization(e, 0));
    svcSignalEvent(e);
    ev(k, "clear", (u32)svcClearEvent(e));
    ev(k, "wait0_after_clear", (u32)svcWaitSynchronization(e, 0));

    /* a higher-priority thread blocks on the event; the signal wakes it before SignalEvent
       returns, and what is left afterwards shows the reset type */
    shared = e;
    woke = 0;
    Handle t = spawn(waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    ev(k, "waiter_blocked", woke);
    svcSignalEvent(e);
    ev(k, "waiter_woke", woke);
    ev(k, "waiter_result", result_a);
    ev(k, "wait0_after_waiter", (u32)svcWaitSynchronization(e, 0));
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);

    /* two waiters, one signal */
    svcClearEvent(e);
    woke = 0;
    Handle t0 = spawn(waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    Handle t1 = spawn(waiter, 1, (u32 *)stacks[1], MAIN_PRIO - 1);
    svcSignalEvent(e);
    ev(k, "two_waiters_woke", woke);
    ev(k, "two_waiters_wait0", (u32)svcWaitSynchronization(e, 0));
    if (woke < 2) {                       /* let the other one go so it can exit */
        svcSignalEvent(e);
        svcSignalEvent(e);
    }
    svcWaitSynchronization(t0, FOREVER);
    svcWaitSynchronization(t1, FOREVER);
    svcCloseHandle(t0);
    svcCloseHandle(t1);
    ev(k, "close", (u32)svcCloseHandle(e));
}

static volatile u32 release_rc, try_rc, got_it;
static Handle mtx;

static void release_other(u32 unused) {
    release_rc = (u32)svcReleaseMutex(mtx);       /* not the owner */
    try_rc = (u32)svcWaitSynchronization(mtx, 0);  /* owned by main */
    svcExitThread();
}

static void block_on_mutex(u32 unused) {
    svcWaitSynchronization(mtx, FOREVER);
    got_it = 1;
    svcReleaseMutex(mtx);
    svcExitThread();
}

static void mutex_checks(void) {
    ev("mutex", "create_unlocked", (u32)svcCreateMutex(&mtx, 0));
    ev("mutex", "acquire", (u32)svcWaitSynchronization(mtx, 0));
    ev("mutex", "recurse", (u32)svcWaitSynchronization(mtx, 0));
    Handle t = spawn(release_other, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    ev("mutex", "other_release_rc", release_rc);
    ev("mutex", "other_wait0_rc", try_rc);
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
    ev("mutex", "release1", (u32)svcReleaseMutex(mtx));
    ev("mutex", "release2", (u32)svcReleaseMutex(mtx));
    ev("mutex", "release3_not_held", (u32)svcReleaseMutex(mtx));

    /* a waiter blocks until the owner has released the recursion count to zero */
    svcWaitSynchronization(mtx, 0);
    svcWaitSynchronization(mtx, 0);
    got_it = 0;
    t = spawn(block_on_mutex, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    svcReleaseMutex(mtx);
    ev("mutex", "waiter_after_first_release", got_it);
    svcReleaseMutex(mtx);
    ev("mutex", "waiter_after_last_release", got_it);
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
    ev("mutex", "close", (u32)svcCloseHandle(mtx));

    Handle locked = 0;
    ev("mutex", "create_locked", (u32)svcCreateMutex(&locked, 1));
    ev("mutex", "locked_wait0", (u32)svcWaitSynchronization(locked, 0));
    ev("mutex", "locked_release", (u32)svcReleaseMutex(locked));
    ev("mutex", "locked_release_again", (u32)svcReleaseMutex(locked));
    ev("mutex", "locked_release_last", (u32)svcReleaseMutex(locked));
    svcCloseHandle(locked);
}

static void semaphore_checks(void) {
    Handle s = 0;
    s32 prev = -1;
    ev("sem", "create_1_of_3", (u32)svcCreateSemaphore(&s, 1, 3));
    ev("sem", "wait0_first", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "wait0_empty", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "release2", (u32)svcReleaseSemaphore(&prev, s, 2));
    ev("sem", "release2_prev", (u32)prev);
    prev = -1;
    ev("sem", "release2_past_max", (u32)svcReleaseSemaphore(&prev, s, 2));
    ev("sem", "release1_to_max", (u32)svcReleaseSemaphore(&prev, s, 1));
    ev("sem", "release1_to_max_prev", (u32)prev);
    ev("sem", "release0", (u32)svcReleaseSemaphore(&prev, s, 0));
    ev("sem", "release_negative", (u32)svcReleaseSemaphore(&prev, s, -1));
    ev("sem", "wait0_a", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "wait0_b", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "wait0_c", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "wait0_d", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "close", (u32)svcCloseHandle(s));

    Handle bad = 0;
    ev("sem", "create_initial_over_max", (u32)svcCreateSemaphore(&bad, 4, 3));
    ev("sem", "create_negative_max", (u32)svcCreateSemaphore(&bad, 0, -1));

    /* a blocked waiter takes the released count */
    ev("sem", "create_empty", (u32)svcCreateSemaphore(&s, 0, 2));
    shared = s;
    woke = 0;
    Handle t = spawn(waiter, 0, (u32 *)stacks[0], MAIN_PRIO - 1);
    ev("sem", "waiter_blocked", woke);
    svcReleaseSemaphore(&prev, s, 2);
    ev("sem", "waiter_woke", woke);
    ev("sem", "waiter_result", result_a);
    ev("sem", "left_over", (u32)svcWaitSynchronization(s, 0));
    ev("sem", "left_over_second", (u32)svcWaitSynchronization(s, 0));
    svcWaitSynchronization(t, FOREVER);
    svcCloseHandle(t);
    svcCloseHandle(s);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_sync: start");
    for (u32 type = 0; type < 3; type++) event_checks(type);
    Handle bad = 0;
    check("event.bad_reset_type", (u32)svcCreateEvent(&bad, 3));
    mutex_checks();
    semaphore_checks();
    finish("conf_sync: done");
}
