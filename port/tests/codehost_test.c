/* A small freestanding 3DS program for the native code host's tests (native/tests/cpu_test.cpp):
 * a CRC-32, a Thumb function called through a pointer and a VFP dot product whose result depends
 * on the rounding mode, each printed with OutputDebugString, then ExitProcess. The host computes
 * the same values in C++. With r0 = 1 at the entry it calls an address that is not recompiled
 * instead, to test the missing-function path; with r0 = 2 it returns from the entry, which a thread
 * must never do.
 *
 * Our own code, no game data. Built by port/tests/build_svc_test.py (clang, armv6k, hard-float). */
#include "svc.h"

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
static u32 (*volatile mix_pointer)(u32, u32) = thumb_mix;

/* Left to right, one rounding per operation. The inputs are volatile so that nothing is folded at
 * compile time, which would round to nearest whatever the thread's FPSCR says. */
static volatile float va[4] = {0.1f, 0.7f, 1.3f, 3.1f};
static volatile float vb[4] = {1.9f, 2.3f, 0.3f, 0.7f};
static volatile float vc[4] = {7.7f, 0.9f, 2.9f, 1.1f};
static volatile float vd[4] = {0.3f, 5.1f, 0.6f, 9.7f};

static float dot4(const volatile float *a, const volatile float *b) {
    float s = a[0] * b[0];
    s += a[1] * b[1];
    s += a[2] * b[2];
    s += a[3] * b[3];
    return s;
}

static void print_float(const char *label, float f) {
    u32 bits;
    __builtin_memcpy(&bits, &f, 4);
    print_hex(label, bits);
}

static void (*volatile missing_function)(void) = (void (*)(void))0x00700000u;

void _start(u32 mode) __attribute__((section(".text.start")));
void _start(u32 mode) {
    print("codehost_test: start");
    if (mode == 2) {
        print("codehost_test: returning from the entry");
        return;
    }
    if (mode == 1) {
        print("codehost_test: calling code that was not recompiled");
        missing_function();
        print("codehost_test: returned from it");
        svcExitProcess();
        for (;;) {}
    }
    static const u8 text[] = "native code host test";
    print_hex("crc32", crc32(text, sizeof(text) - 1, 0));
    print_hex("thumb_mix", mix_pointer(0x12345678u, 0x9ABCDEF0u));
    /* Each dot product is computed right after a system call, where the host has handled the
     * call and must have put the thread's rounding mode back. */
    print_float("dot_a", dot4(va, vb));
    print_float("dot_b", dot4(vc, vd));
    print("codehost_test: done");
    svcExitProcess();
    for (;;) {}
}
