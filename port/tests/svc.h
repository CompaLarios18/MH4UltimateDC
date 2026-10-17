/* Freestanding helpers for the port's 3DS test programs: types, system calls (register
 * usage as in libctru's svc.s) and debug output. No game code. */
#ifndef PORT_TESTS_SVC_H
#define PORT_TESTS_SVC_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;
typedef u32 Handle;

/* ── system calls (register usage as in libctru's svc.s) ───────────────────────── */
#define NAKED static __attribute__((naked, noinline, used))
NAKED s32 svcOutputDebugString(const char *s, s32 len) { __asm__ volatile("svc 0x3D\n bx lr"); }
NAKED void svcExitProcess(void) { __asm__ volatile("svc 0x03\n bx lr"); }
NAKED void svcExitThread(void) { __asm__ volatile("svc 0x09\n bx lr"); }
NAKED void svcSleepThread(s64 ns) { __asm__ volatile("svc 0x0A\n bx lr"); }
NAKED s32 svcCloseHandle(Handle h) { __asm__ volatile("svc 0x23\n bx lr"); }
NAKED s32 svcSignalEvent(Handle h) { __asm__ volatile("svc 0x18\n bx lr"); }
NAKED s32 svcWaitSynchronization(Handle h, s64 ns) { __asm__ volatile("svc 0x24\n bx lr"); }
NAKED s32 svcReleaseMutex(Handle h) { __asm__ volatile("svc 0x14\n bx lr"); }
NAKED s32 svcCreateEvent(Handle *out, u32 reset_type) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x17\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcCreateMutex(Handle *out, u32 initially_locked) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x13\n ldr r3, [sp], #4\n str r1, [r3]\n bx lr");
}
NAKED s32 svcCreateThread(Handle *out, void (*entry)(u32), u32 arg, u32 *stack_top, s32 prio, s32 core) {
    __asm__ volatile("push {r0, r4}\n ldr r0, [sp, #8]\n ldr r4, [sp, #12]\n svc 0x08\n"
                     "pop {r2, r4}\n str r1, [r2]\n bx lr");
}

/* More system calls, for the conformance programs (conf_*.c). Each wrapper follows the kernel's
 * register layout (libctru's svc.s): inputs in r0-r4 (r0 may carry a stack argument when the C
 * signature starts with an output pointer), results in r0 and r1-r5. */
#define CUR_THREAD ((Handle)0xFFFF8000u)
#define CUR_PROCESS ((Handle)0xFFFF8001u)
typedef unsigned short u16;
NAKED s32 svcControlMemory(u32 *out, u32 addr0, u32 addr1, u32 size, u32 op, u32 perm) {
    __asm__ volatile("push {r0, r4}\n ldr r0, [sp, #8]\n ldr r4, [sp, #12]\n svc 0x01\n"
                     "ldr r2, [sp], #4\n str r1, [r2]\n pop {r4}\n bx lr");
}
/* out[0..3] = base, size, permission, state; out[4] = page flags */
NAKED s32 svcQueryMemory(u32 *out, u32 addr) {
    __asm__ volatile("push {r0, r4, r5}\n mov r2, r1\n svc 0x02\n ldr ip, [sp]\n"
                     "str r1, [ip]\n str r2, [ip, #4]\n str r3, [ip, #8]\n str r4, [ip, #12]\n"
                     "str r5, [ip, #16]\n pop {r2, r4, r5}\n bx lr");
}
NAKED s32 svcGetThreadPriority(s32 *out, Handle h) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x0B\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcSetThreadPriority(Handle h, s32 prio) { __asm__ volatile("svc 0x0C\n bx lr"); }
NAKED s32 svcCreateSemaphore(Handle *out, s32 initial, s32 max) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x15\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcReleaseSemaphore(s32 *prev, Handle h, s32 count) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x16\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcClearEvent(Handle h) { __asm__ volatile("svc 0x19\n bx lr"); }
NAKED s32 svcCreateTimer(Handle *out, u32 reset_type) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x1A\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcSetTimer(Handle h, s64 initial, s64 interval) {
    __asm__ volatile("push {r4}\n ldr r1, [sp, #4]\n ldr r4, [sp, #8]\n svc 0x1B\n pop {r4}\n bx lr");
}
NAKED s32 svcCancelTimer(Handle h) { __asm__ volatile("svc 0x1C\n bx lr"); }
NAKED s32 svcClearTimer(Handle h) { __asm__ volatile("svc 0x1D\n bx lr"); }
NAKED s32 svcCreateMemoryBlock(Handle *out, u32 addr, u32 size, u32 my_perm, u32 other_perm) {
    __asm__ volatile("str r0, [sp, #-4]!\n ldr r0, [sp, #4]\n svc 0x1E\n ldr r2, [sp], #4\n"
                     "str r1, [r2]\n bx lr");
}
NAKED s32 svcMapMemoryBlock(Handle h, u32 addr, u32 my_perm, u32 other_perm) {
    __asm__ volatile("svc 0x1F\n bx lr");
}
NAKED s32 svcUnmapMemoryBlock(Handle h, u32 addr) { __asm__ volatile("svc 0x20\n bx lr"); }
NAKED s32 svcCreateAddressArbiter(Handle *out) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x21\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcArbitrateAddress(Handle h, u32 addr, u32 type, s32 value, s64 ns) {
    __asm__ volatile("push {r4, r5}\n ldr r4, [sp, #8]\n ldr r5, [sp, #12]\n svc 0x22\n pop {r4, r5}\n bx lr");
}
NAKED s32 svcWaitSynchronizationN(s32 *out, const Handle *hs, s32 count, u32 all, s64 ns) {
    __asm__ volatile("push {r0, r4}\n ldr r0, [sp, #8]\n ldr r4, [sp, #12]\n svc 0x25\n"
                     "ldr r2, [sp], #4\n str r1, [r2]\n pop {r4}\n bx lr");
}
NAKED s32 svcDuplicateHandle(Handle *out, Handle h) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x27\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED u64 svcGetSystemTick(void) { __asm__ volatile("svc 0x28\n bx lr"); }
NAKED s32 svcGetHandleInfo(s64 *out, Handle h, u32 type) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x29\n ldr r3, [sp], #4\n str r1, [r3]\n str r2, [r3, #4]\n bx lr");
}
NAKED s32 svcGetSystemInfo(s64 *out, u32 type, s32 param) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x2A\n ldr r3, [sp], #4\n str r1, [r3]\n str r2, [r3, #4]\n bx lr");
}
NAKED s32 svcGetProcessInfo(s64 *out, Handle h, u32 type) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x2B\n ldr r3, [sp], #4\n str r1, [r3]\n str r2, [r3, #4]\n bx lr");
}
NAKED s32 svcConnectToPort(Handle *out, const char *name) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x2D\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcSendSyncRequest(Handle h) { __asm__ volatile("svc 0x32\n bx lr"); }
NAKED s32 svcGetProcessId(u32 *out, Handle h) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x35\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcGetThreadId(u32 *out, Handle h) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x37\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcGetResourceLimit(Handle *out, Handle process) {
    __asm__ volatile("str r0, [sp, #-4]!\n svc 0x38\n ldr r2, [sp], #4\n str r1, [r2]\n bx lr");
}
NAKED s32 svcGetResourceLimitLimitValues(s64 *values, Handle limit, const u32 *names, s32 count) {
    __asm__ volatile("svc 0x39\n bx lr");
}
NAKED s32 svcGetResourceLimitCurrentValues(s64 *values, Handle limit, const u32 *names, s32 count) {
    __asm__ volatile("svc 0x3A\n bx lr");
}

static inline u32 *tls(void) {
    u32 *p;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(p));
    return p;
}

/* ── output ─────────────────────────────────────────────────────────────────────── */
static inline u32 str_len(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static inline void print(const char *s) { svcOutputDebugString(s, (s32)str_len(s)); }

static inline void print_hex(const char *label, u32 v) {
    char buf[64];
    u32 n = 0;
    while (label[n] && n < 40) { buf[n] = label[n]; n++; }
    buf[n++] = ' ';
    for (int i = 7; i >= 0; i--) buf[n++] = "0123456789abcdef"[(v >> (4 * i)) & 0xF];
    buf[n] = 0;
    print(buf);
}

/* ── conformance programs' helpers ──────────────────────────────────────────────── */
#define STACK_SIZE_CONF 0x2000
#define MAIN_PRIO 0x30     /* the main thread's priority under Azahar's ELF loader */
#define ONE_MS 1000000LL
#define FOREVER (-1LL)

/* "label: 0000abcd", one line per check */
static inline void check(const char *label, u32 v) {
    char buf[64];
    u32 n = 0;
    while (label[n] && n < 40) { buf[n] = label[n]; n++; }
    buf[n++] = ':';
    buf[n++] = ' ';
    for (int i = 7; i >= 0; i--) buf[n++] = "0123456789abcdef"[(v >> (4 * i)) & 0xF];
    buf[n] = 0;
    print(buf);
}

static inline void check_text(const char *label, const char *text) {
    char buf[96];
    u32 n = 0;
    while (label[n] && n < 40) { buf[n] = label[n]; n++; }
    buf[n++] = ':';
    buf[n++] = ' ';
    for (u32 i = 0; text[i] && n < 90; i++) buf[n++] = text[i];
    buf[n] = 0;
    print(buf);
}

/* A thread at priority prio on the default core; returns the handle (0 when it failed). */
static inline Handle spawn(void (*fn)(u32), u32 arg, u32 *stack_bottom, s32 prio) {
    Handle h = 0;
    s32 r = svcCreateThread(&h, fn, arg, (u32 *)((u8 *)stack_bottom + STACK_SIZE_CONF), prio, -2);
    if (r != 0) check("spawn failed", (u32)r);
    return r ? 0 : h;
}

/* Every program ends with "<name>: done" and ExitProcess. */
static inline void finish(const char *line) {
    print(line);
    svcExitProcess();
    for (;;) {}
}

/* ── IPC helpers (command buffer at TLS + 0x80) ─────────────────────────────────── */
static inline u32 *cmdbuf(void) { return tls() + 0x20; }

/* Copies `n` words into the command buffer and sends them; the reply is left in cmdbuf(). */
static inline s32 request(Handle h, const u32 *words, u32 n) {
    u32 *cb = cmdbuf();
    for (u32 i = 0; i < n; i++) cb[i] = words[i];
    return svcSendSyncRequest(h);
}

/* "srv:" RegisterClient; returns the session handle (0 when it failed). */
static inline Handle srv_connect(int register_client) {
    Handle srv = 0;
    if (svcConnectToPort(&srv, "srv:") != 0) return 0;
    if (register_client) {
        const u32 reg[3] = {0x00010002, 0x20, 0};
        request(srv, reg, 3);
    }
    return srv;
}

/* srv: GetServiceHandle. Fills `reply` with the first 4 words of the reply (header, result,
   descriptor, handle); returns the handle (0 on failure). */
static inline Handle srv_get(Handle srv, const char *name, u32 name_len, u32 flags, u32 *reply) {
    u32 w[5] = {0x00050100, 0, 0, name_len, flags};
    for (u32 i = 0; i < 8 && name[i]; i++) w[1 + i / 4] |= (u32)(u8)name[i] << (8 * (i % 4));
    s32 rc = request(srv, w, 5);
    u32 *cb = cmdbuf();
    reply[0] = cb[0]; reply[1] = cb[1]; reply[2] = cb[2]; reply[3] = cb[3];
    if (rc != 0) reply[1] = (u32)rc;
    if (reply[1] != 0) reply[2] = reply[3] = 0;   /* an error reply has only two words */
    return (rc == 0 && cb[1] == 0) ? cb[3] : 0;
}

#endif
