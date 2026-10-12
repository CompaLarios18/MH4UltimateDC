/* A small freestanding 3DS program for testing CPU backends end to end under Azahar's
 * kernel: threads on fibers, events, sleeping, mutexes, LDREX/STREX atomics, VFP (including
 * a short-vector routine), Thumb code and function pointers. It prints its results with
 * svcOutputDebugString; a backend is correct when its output matches Azahar's JIT.
 *
 * Built by port/tests/build_svc_test.py (clang, armv6k, hard-float). No game code. */
typedef unsigned char u8;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;
typedef u32 Handle;

/* ── system calls (register usage as in libctru's svc.s) ───────────────────────── */
#define NAKED __attribute__((naked, noinline))
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

/* ── output ─────────────────────────────────────────────────────────────────────── */
static u32 str_len(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static void print(const char *s) { svcOutputDebugString(s, (s32)str_len(s)); }

static void print_hex(const char *label, u32 v) {
    char buf[64];
    u32 n = 0;
    while (label[n] && n < 40) { buf[n] = label[n]; n++; }
    buf[n++] = ' ';
    for (int i = 7; i >= 0; i--) buf[n++] = "0123456789abcdef"[(v >> (4 * i)) & 0xF];
    buf[n] = 0;
    print(buf);
}

/* ── computation ─────────────────────────────────────────────────────────────────── */
static u32 crc32(const u8 *p, u32 n, u32 crc) {
    crc = ~crc;
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

__attribute__((target("thumb"), noinline)) static u32 thumb_mix(u32 a, u32 b) {
    for (int i = 0; i < 16; i++) a = (a << 3 | a >> 29) ^ (b + (u32)i);
    return a;
}

static float dot4(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }

/* s8-s11 += s16-s19 * scalar, in VFP short-vector mode (LEN=3) like the game's mixers */
NAKED void vec_madd(float *acc, const float *v, const float *k) {
    __asm__ volatile(
        "vpush {d8-d9}\n vpush {d10-d11}\n"
        "vmrs ip, fpscr\n str ip, [sp, #-4]!\n"
        "bic ip, ip, #0x370000\n orr ip, ip, #0x30000\n vmsr fpscr, ip\n"
        "vldmia r0, {s8-s11}\n vldmia r1, {s16-s19}\n vldr s0, [r2]\n"
        "vmla.f32 s8, s16, s0\n"
        "vstmia r0, {s8-s11}\n"
        "ldr ip, [sp], #4\n vmsr fpscr, ip\n"
        "vpop {d10-d11}\n vpop {d8-d9}\n bx lr");
}

typedef u32 (*op_fn)(u32, u32);
static u32 op_add(u32 a, u32 b) { return a + b; }
static u32 op_xor(u32 a, u32 b) { return a ^ b; }
static u32 op_mul(u32 a, u32 b) { return a * b; }
static op_fn ops[3] = {op_add, op_xor, op_mul};

static u32 dispatch(u32 sel, u32 a, u32 b) {
    switch (sel % 7) {
    case 0: return a + 1;
    case 1: return b - 1;
    case 2: return a << (b & 7);
    case 3: return a / (b | 1);
    case 4: return a % (b | 3);
    case 5: return thumb_mix(a, b);
    default: return ops[sel % 3](a, b);
    }
}

/* ── threads ─────────────────────────────────────────────────────────────────────── */
#define WORKERS 3
#define STACK_SIZE 0x4000
static u8 stacks[WORKERS][STACK_SIZE] __attribute__((aligned(8)));
static Handle done_events[WORKERS];
static Handle mutex;
static volatile u32 counter;     /* incremented with LDREX/STREX */
static u32 locked_sum;           /* updated under the kernel mutex */
static u32 results[WORKERS];

static void worker(u32 id) {
    u32 acc = 0x9E3779B9u * (id + 1);
    for (u32 i = 0; i < 40; i++) {
        acc = dispatch(acc ^ i, acc, i + id);
        __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
        if (i % 8 == id) {
            svcWaitSynchronization(mutex, -1);
            locked_sum += acc & 0xFFFF;
            svcReleaseMutex(mutex);
        }
        if (i % 10 == 0) svcSleepThread(200000 * (id + 1));
    }
    results[id] = acc;
    svcSignalEvent(done_events[id]);
    svcExitThread();
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("svc_test: start");

    static const u8 text[] = "Monster Hunter 4 Ultimate";
    print_hex("crc32", crc32(text, sizeof(text) - 1, 0));
    print_hex("thumb_mix", thumb_mix(0x12345678u, 0x9ABCDEF0u));
    u32 d = 0;
    for (u32 i = 0; i < 64; i++) d = dispatch(i * 2654435761u, d + i, i);
    print_hex("dispatch", d);

    static const float a[4] = {1.5f, -2.25f, 3.0f, 0.125f}, b[4] = {4.0f, 0.5f, -1.0f, 8.0f};
    float f = dot4(a, b);
    print_hex("dot4", *(u32 *)&f);
    float acc[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    static const float v[4] = {0.5f, 0.25f, -0.75f, 10.0f}, k = 3.0f;
    vec_madd(acc, v, &k);
    for (int i = 0; i < 4; i++) print_hex("vec_madd", *(u32 *)&acc[i]);

    if (svcCreateMutex(&mutex, 0) != 0) print("create mutex failed");
    for (u32 i = 0; i < WORKERS; i++) {
        if (svcCreateEvent(&done_events[i], 0) != 0) print("create event failed");
    }
    for (u32 i = 0; i < WORKERS; i++) {
        Handle t;
        s32 r = svcCreateThread(&t, worker, i, (u32 *)(stacks[i] + STACK_SIZE), 0x30 + (s32)i, -2);
        if (r != 0) print_hex("create thread failed", (u32)r);
    }
    for (u32 i = 0; i < WORKERS; i++) {
        svcWaitSynchronization(done_events[i], -1);
        print_hex("worker", results[i]);
    }
    print_hex("counter", counter);
    print_hex("locked_sum", locked_sum);
    print("svc_test: done");
    svcExitProcess();
}
