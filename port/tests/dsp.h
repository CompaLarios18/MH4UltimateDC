// Derived from Azahar src/audio_core/hle/shared_memory.h, src/audio_core/hle/hle.cpp and
// src/core/hle/service/dsp/dsp_dsp.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of
// them says "Licensed under GPLv2 or any later version"): the offsets of the fields of a shared-memory region,
// of the source configuration (its buffers, embedded buffer and dirty bits), of the DSP configuration and its
// delay effect (shared_memory.h); the table of structure addresses the audio pipe returns and the choice of
// the region the DSP reads (hle.cpp); the commands of dsp::DSP with their numbers and parameters (dsp_dsp.cpp).
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (shared_memory.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (hle.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (dsp_dsp.cpp)
//
/* Our own DSP test programs (snd_*.c): the application side of dsp::DSP, as the SDK drives it.
 * It starts the DSP (LoadComponent, the interrupt event of the audio pipe, the pipe's initialise
 * command), finds the two regions of the shared memory through the structure addresses the pipe
 * returns, and runs the frame loop: wait for the audio pipe's interrupt, record the status of the
 * sources and a CRC of the final mix from the region the DSP wrote last, let the program change the
 * configuration of that same region (it is the one the DSP reads next), then raise its frame counter.
 *
 * Nothing is printed while the DSP runs: the lines are collected and printed by dsp_finish().
 * Samples are made by the programs with integer arithmetic (no floating point anywhere: the floats of
 * the configuration are given as their IEEE bit patterns). No game code, no game data. */
#ifndef PORT_TESTS_DSP_H
#define PORT_TESTS_DSP_H
#include "svc.h"

typedef short s16;
typedef signed char s8;

/* ── memory accesses in 16-bit halves (a field may sit on a 2-byte boundary) ────── */
static inline u32 g16(u32 a) { return *(volatile u16 *)a; }
static inline void p16(u32 a, u32 v) { *(volatile u16 *)a = (u16)v; }
static inline u32 g32le(u32 a) { return g16(a) | (g16(a + 2) << 16); }
static inline void p32le(u32 a, u32 v) { p16(a, v); p16(a + 2, v >> 16); }
static inline void p32dsp(u32 a, u32 v) { p16(a, v >> 16); p16(a + 2, v); }   /* u32_dsp: halves swapped */
static inline u32 g32dsp(u32 a) { return (g16(a) << 16) | g16(a + 2); }

/* IEEE single bit patterns of the floats the programs use */
#define F0 0x00000000u
#define F0_125 0x3E000000u
#define F0_25 0x3E800000u
#define F0_5 0x3F000000u
#define F0_75 0x3F400000u
#define F1 0x3F800000u
#define F1_37 0x3FAF5C29u
#define F2 0x40000000u
#define F2_9 0x4039999Au

/* ── layout: the offsets of the fields in one region (Azahar's SharedMemory) ────── */
#define SHM_DSP_STATUS 0x800u
#define SHM_FINAL 0xA80u
#define SHM_STATUS 0xD00u          /* 24 sources * 12 bytes */
#define SHM_DSPCFG 0x2860u         /* 196 bytes */
#define SHM_INTERMEDIATE 0x2924u   /* mix1 then mix2, 2560 bytes each: pcm32[4][160] */
#define SHM_CFG 0x3D24u            /* 24 sources * 192 bytes */
#define SHM_COEFF 0x4F24u          /* 24 * 16 s16 */
#define SHM_FRAME_COUNTER 0x7FFEu

/* format, channels, interpolation, dirty bits of a source configuration */
enum { FMT_PCM8 = 0, FMT_PCM16 = 1, FMT_ADPCM = 2 };
enum { INTERP_POLYPHASE = 0, INTERP_LINEAR = 1, INTERP_NONE = 2 };
#define D_FORMAT (1u << 0)
#define D_MONO_STEREO (1u << 1)
#define D_ADPCM_COEF (1u << 2)
#define D_PARTIAL_EMBEDDED (1u << 3)
#define D_PARTIAL_RESET (1u << 4)
#define D_ENABLE (1u << 16)
#define D_INTERP (1u << 17)
#define D_RATE (1u << 18)
#define D_QUEUE (1u << 19)
#define D_PLAY_POS (1u << 21)
#define D_FILTERS (1u << 22)
#define D_SIMPLE (1u << 23)
#define D_BIQUAD (1u << 24)
#define D_GAIN(m) (1u << (25 + (m)))
#define D_SYNC (1u << 28)
#define D_RESET (1u << 29)
#define D_EMBEDDED (1u << 30)

/* ── output: lines are built here and printed by dsp_finish ─────────────────────── */
#define OUT_SIZE 160000
static char out_buf[OUT_SIZE];
static u32 out_len;
static void o_ch(char c) { if (out_len < OUT_SIZE - 1) out_buf[out_len++] = c; }
static void o_str(const char *s) { while (*s) o_ch(*s++); }
static void o_dec(u32 v) {
    static const u32 pow10[9] = {1000000000u, 100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u, 100u, 10u};
    int started = 0;
    for (int i = 0; i < 9; i++) {
        char c = '0';
        while (v >= pow10[i]) { v -= pow10[i]; c++; started = 1; }
        if (started) o_ch(c);
    }
    o_ch((char)('0' + v));
}
static void o_hex8(u32 v) { for (int i = 7; i >= 0; i--) o_ch("0123456789abcdef"[(v >> (4 * i)) & 0xF]); }
static void o_end(void) { o_ch('\n'); }

/* ── samples: integer only ──────────────────────────────────────────────────────── */
static u32 lcg_state = 0x1234;
static u32 lcg(void) { lcg_state = (lcg_state * 25173u + 13849u) & 0xFFFFu; return lcg_state; }

/* A tone by the recurrence s[n] = (c * s[n-1] >> 14) - s[n-2] (c = 2cos(w) in Q14, s[1] = amp * sin(w))
 * plus a little noise from the LCG: `n` samples, interleaved when `chans` is 2 (the second channel
 * uses another c). Returns nothing; `out` is s16, or s8 when `bits8` is set. */
static void gen_tone(void *out, u32 n, u32 chans, u32 bits8, s32 c0, s32 a0, s32 c1, s32 a1) {
    for (u32 ch = 0; ch < chans; ch++) {
        s32 c = ch ? c1 : c0, p2 = 0, p1 = ch ? a1 : a0;
        for (u32 i = 0; i < n; i++) {
            s32 s = i == 0 ? 0 : i == 1 ? p1 : ((c * p1) >> 14) - p2;
            if (i > 1) { p2 = p1; p1 = s; }
            s32 v = s + (s32)(lcg() >> 8) - 128;
            if (bits8) ((s8 *)out)[i * chans + ch] = (s8)(v >> 7);
            else ((s16 *)out)[i * chans + ch] = (s16)v;
        }
    }
}

/* Data for an ADPCM buffer of `frames` frames (8 bytes: header, 14 nibbles). The header of frame f has
 * the predictor (f + first) & 7 and the scale (3f + scale0) & smask; the nibbles come from the LCG. */
static void gen_adpcm(u8 *out, u32 frames, u32 first, u32 scale0, u32 smask) {
    for (u32 f = 0; f < frames; f++) {
        out[f * 8] = (u8)((((f + first) & 7) << 4) | ((f * 3 + scale0) & smask));
        for (u32 i = 1; i < 8; i++) out[f * 8 + i] = (u8)(lcg() >> 7);
    }
}

static u32 crc32_update(u32 crc, u8 byte) {
    crc ^= byte;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    return crc;
}

/* ── the DSP service ────────────────────────────────────────────────────────────── */
#define FRAME_TICKS 1310720ull
#define MAX_JS 700           /* interrupts before the program gives up */
static struct {
    Handle srv, h, evt;
    u32 base[2];             /* vaddr of region 0 and 1 */
    u32 fc[2];               /* the frame counters, as the program wrote them */
    u32 x;                   /* the region the DSP wrote last: the one the program changes now */
    u32 watch;               /* sources whose status is recorded every frame */
    s32 k;                   /* captured frames so far (-1 before source 0 reports enabled) */
    u32 j;                   /* interrupts seen */
    u32 done;                /* set by a step: stop after this frame */
    u32 pool, pool_end, lbase;   /* linear heap */
    u64 last_tick;
} D;

static u8 component[0x400] __attribute__((aligned(0x1000)));
static u8 pipe_buf[64] __attribute__((aligned(8)));

static void fail(const char *what, u32 v) {
    o_str("FAIL "); o_str(what); o_ch(' '); o_hex8(v); o_end();
}

static s32 ipc(const u32 *w, u32 n) {
    s32 rc = request(D.h, w, n);
    if (rc != 0) return rc;
    return (s32)cmdbuf()[1];
}

/* ReadPipeIfPossible into pipe_buf; returns the bytes read */
static u32 dsp_pipe_read(u32 pipe, u32 size) {
    u32 *sb = tls() + 0x60;   /* static buffer 0: TLS + 0x180 */
    sb[0] = (size << 14) | 2;
    sb[1] = (u32)pipe_buf;
    const u32 w[4] = {0x001000C0, pipe, 0, size};
    if (ipc(w, 4) != 0) return 0;
    return cmdbuf()[2] & 0xFFFF;
}

static void dsp_pipe_write(u32 pipe, const u8 *data, u32 size) {
    const u32 w[5] = {0x000D0082, pipe, size, (size << 14) | 0x402, (u32)data};
    s32 rc = ipc(w, 5);
    if (rc) fail("WriteProcessPipe", (u32)rc);
}

static u32 dsp_convert(u32 dsp_addr) {
    const u32 w[2] = {0x000C0040, dsp_addr};
    if (ipc(w, 2) != 0) return 0;
    return cmdbuf()[2];
}

/* linear heap: `size` bytes, 16-aligned; returns the virtual address */
static u32 dsp_alloc(u32 size) {
    if (!D.pool) {
        u32 out = 0;
        s32 rc = svcControlMemory(&out, 0, 0, 0x100000, 0x10003, 3);
        /* the linear heap starts at 0x30000000 for a process whose kernel version is 0x22C or higher
         * (Azahar's Process::GetLinearHeapAreaAddress; its default is 0x23A) and at
         * 0x14000000 below that, whatever --new-3ds says; the loader of a runner must give the same */
        if (rc || out < 0x14000000u || out >= 0x40000000u) { fail("linear heap", out); return 0; }
        D.lbase = out >= 0x30000000u ? 0x30000000u : 0x14000000u;
        D.pool = out;
        D.pool_end = out + 0x100000;
    }
    u32 a = D.pool;
    D.pool = (D.pool + size + 15) & ~15u;
    if (D.pool > D.pool_end) { fail("pool", D.pool); return 0; }
    return a;
}
/* the physical address of a linear heap address: FCRAM starts at 0x20000000 */
static inline u32 phys(u32 vaddr) { return vaddr - D.lbase + 0x20000000u; }
static void flush(u32 addr, u32 size) {
    const u32 w[5] = {0x00130082, addr, size, 0, CUR_PROCESS};
    ipc(w, 5);
}

/* ── accessors for the region the program is changing (D.x) ─────────────────────── */
static inline u32 region(void) { return D.base[D.x]; }
static inline u32 cfg(u32 i) { return region() + SHM_CFG + 192 * i; }
static inline u32 status(u32 i) { return region() + SHM_STATUS + 12 * i; }
static inline u32 dcfg(void) { return region() + SHM_DSPCFG; }
static inline void cfg_dirty(u32 i, u32 bits) { p32le(cfg(i), g32le(cfg(i)) | bits); }
static inline u32 st_enabled(u32 i) { return g16(status(i)) & 0xFF; }
static inline u32 st_pos(u32 i) { return g32dsp(status(i) + 4); }
static inline u32 st_buf(u32 i) { return g16(status(i) + 8); }
static inline u32 st_last(u32 i) { return g16(status(i) + 10); }
static inline u32 st_sync(u32 i) { return g16(status(i) + 2); }

/* source configuration fields; each sets the dirty bit that makes the DSP read it */
static void src_reset(u32 i) { cfg_dirty(i, D_RESET); }
static void src_enable(u32 i, u32 on) { *(volatile u8 *)(cfg(i) + 160) = (u8)on; cfg_dirty(i, D_ENABLE); }
static void src_sync(u32 i, u32 n) { p16(cfg(i) + 162, n); cfg_dirty(i, D_SYNC); }
static void src_rate(u32 i, u32 bits) { p32le(cfg(i) + 52, bits); cfg_dirty(i, D_RATE); }
static void src_interp(u32 i, u32 mode) { p16(cfg(i) + 56, mode); cfg_dirty(i, D_INTERP); }
/* gain[mix][0..3] = left, right, left (rear), right (rear) */
static void src_gain(u32 i, u32 mix, u32 g0, u32 g1, u32 g2, u32 g3) {
    p32le(cfg(i) + 4 + 16 * mix, g0); p32le(cfg(i) + 8 + 16 * mix, g1);
    p32le(cfg(i) + 12 + 16 * mix, g2); p32le(cfg(i) + 16 + 16 * mix, g3);
    cfg_dirty(i, D_GAIN(mix));
}
static void src_filters(u32 i, u32 simple, u32 biquad) { p16(cfg(i) + 58, simple | (biquad << 1)); cfg_dirty(i, D_FILTERS); }
static void src_simple(u32 i, s32 b0, s32 a1) { p16(cfg(i) + 60, b0); p16(cfg(i) + 62, a1); cfg_dirty(i, D_SIMPLE); }
static void src_biquad(u32 i, s32 a2, s32 a1, s32 b2, s32 b1, s32 b0) {
    p16(cfg(i) + 64, a2); p16(cfg(i) + 66, a1); p16(cfg(i) + 68, b2); p16(cfg(i) + 70, b1); p16(cfg(i) + 72, b0);
    cfg_dirty(i, D_BIQUAD);
}
static void src_coeffs(u32 i, const s16 *c) {
    for (u32 n = 0; n < 16; n++) p16(region() + SHM_COEFF + 32 * i + 2 * n, (u32)c[n]);
    cfg_dirty(i, D_ADPCM_COEF);
}
/* the embedded buffer (the one the SDK starts a sound with); it also latches the format and channels */
static void src_embedded(u32 i, u32 fmt, u32 chans, u32 addr, u32 len, u32 id, u32 loop, u32 ps, u32 yn1,
                         u32 yn2, u32 adpcm_dirty, s32 play_pos) {
    u32 c = cfg(i);
    p32dsp(c + 172, addr); p32dsp(c + 176, len);
    p16(c + 180, chans | (fmt << 2)); p16(c + 182, ps); p16(c + 184, yn1); p16(c + 186, yn2);
    p16(c + 188, (adpcm_dirty & 1) | ((loop & 1) << 1)); p16(c + 190, id);
    cfg_dirty(i, D_EMBEDDED);
    if (play_pos >= 0) { p32dsp(c + 164, (u32)play_pos); cfg_dirty(i, D_PLAY_POS); }
}
/* a buffer of the queue (slot 0..3); the format is the one latched by the last embedded buffer or
 * src_format */
static void src_queue(u32 i, u32 slot, u32 addr, u32 len, u32 id, u32 loop, u32 ps, u32 yn1, u32 yn2,
                      u32 adpcm_dirty) {
    u32 c = cfg(i) + 76 + 20 * slot;
    p32dsp(c, addr); p32dsp(c + 4, len); p16(c + 8, ps); p16(c + 10, yn1); p16(c + 12, yn2);
    *(volatile u8 *)(c + 14) = (u8)adpcm_dirty; *(volatile u8 *)(c + 15) = (u8)loop; p16(c + 16, id);
    p16(cfg(i) + 74, g16(cfg(i) + 74) | (1u << slot));
    cfg_dirty(i, D_QUEUE);
}
static void src_format(u32 i, u32 fmt, u32 chans) {
    p16(cfg(i) + 180, chans | (fmt << 2));
    cfg_dirty(i, D_FORMAT | D_MONO_STEREO);
}

/* the DSP configuration */
static void dsp_dirty(u32 bits) { p32le(dcfg(), g32le(dcfg()) | bits); }
static void dsp_master(u32 bits) { p32le(dcfg() + 4, bits); dsp_dirty(1u << 16); }
static void dsp_aux_return(u32 n, u32 bits) { p32le(dcfg() + 8 + 4 * n, bits); dsp_dirty(1u << (24 + n)); }
static void dsp_output_format(u32 f) { p16(dcfg() + 22, f); dsp_dirty(1u << 26); }
static void dsp_aux_enable(u32 n, u32 on) { p16(dcfg() + 40 + 2 * n, on); dsp_dirty(1u << (8 + n)); }
/* delay effect n: g, a, b are Q7; the delay is `frames` audio frames */
static void dsp_delay(u32 n, u32 on, u32 frames, s32 g, s32 a, s32 b) {
    u32 d = dcfg() + 44 + 20 * n;
    p16(d + 2, on); p16(d + 6, 1); p32dsp(d + 8, 0); p16(d + 12, frames);
    p16(d + 14, g); p16(d + 16, a); p16(d + 18, b); p16(d, 7);
    dsp_dirty(1u << (10 + n));
}

/* ── start, frame loop, stop ────────────────────────────────────────────────────── */
static void dsp_init(void) {
    D.srv = srv_connect(1);
    u32 reply[4];
    D.h = D.srv ? srv_get(D.srv, "dsp::DSP", 8, 0, reply) : 0;
    if (!D.h) { fail("dsp::DSP", reply[1]); return; }
    svcCreateEvent(&D.evt, 0);
    const u32 load[6] = {0x001100C2, 0x400, 0xFF, 0xFF, (0x400 << 4) | 0xA, (u32)component};
    s32 rc = ipc(load, 6);
    if (rc) fail("LoadComponent", (u32)rc);
    const u32 reg[5] = {0x00150082, 2, 2, 0, D.evt};   /* interrupt type Pipe, channel Audio */
    rc = ipc(reg, 5);
    if (rc) fail("RegisterInterruptEvents", (u32)rc);
    static const u8 initialise[4] = {0, 0, 0, 0};
    dsp_pipe_write(2, initialise, 4);
    if (svcWaitSynchronization(D.evt, 1000 * ONE_MS) != 0) { fail("pipe interrupt", 0); return; }
    u32 n = dsp_pipe_read(2, 2) ? (pipe_buf[0] | (pipe_buf[1] << 8)) : 0;
    if (n != 15 || dsp_pipe_read(2, 2 * n) != 2 * n) { fail("structure table", n); return; }
    u32 addr[15];
    for (u32 i = 0; i < 15; i++) addr[i] = pipe_buf[2 * i] | (pipe_buf[2 * i + 1] << 8);
    /* the table: frame counter, source configurations, source statuses, ADPCM coefficients, DSP
     * configuration, DSP status, final samples, intermediate mix samples, ... */
    static const u32 want[8] = {SHM_FRAME_COUNTER, SHM_CFG, SHM_STATUS, SHM_COEFF, SHM_DSPCFG,
                                SHM_DSP_STATUS, SHM_FINAL, SHM_INTERMEDIATE};
    for (u32 r = 0; r < 2; r++) {
        D.base[r] = dsp_convert(addr[0] | (r ? 0x10000 : 0)) - SHM_FRAME_COUNTER;
        for (u32 i = 1; i < 8; i++)
            if (dsp_convert(addr[i] | (r ? 0x10000 : 0)) != D.base[r] + want[i]) fail("structure address", i);
    }
    svcClearEvent(D.evt);
    D.k = -1;
}

/* One frame of the loop. step(j) changes the configuration of the next frame (in region D.x). */
static void dsp_run(void (*step)(u32 j), u32 frames) {
    while (D.j < MAX_JS && !D.done) {
        if (svcWaitSynchronization(D.evt, 1000 * ONE_MS) != 0) { fail("frame interrupt", D.j); break; }
        u64 now = svcGetSystemTick();
        if (D.j && now - D.last_tick > FRAME_TICKS + FRAME_TICKS / 2) { o_str("missed "); o_dec(D.j); o_end(); }
        D.last_tick = now;
        /* the region the DSP wrote is the one that is not read: the lower frame counter, a tie reads 1 */
        D.x = D.fc[0] > D.fc[1] ? 1 : 0;
        if (D.k < 0 && st_enabled(0)) D.k = 0;
        if (D.k >= 0) {
            for (u32 i = 0; i < 24; i++) {
                if (!(D.watch >> i & 1)) continue;
                o_str("f"); o_dec((u32)D.k); o_str(": s"); o_dec(i);
                o_str(" en="); o_dec(st_enabled(i)); o_str(" pos="); o_dec(st_pos(i));
                o_str(" buf="); o_dec(st_buf(i)); o_str(" last="); o_dec(st_last(i));
                o_str(" sync="); o_dec(st_sync(i)); o_end();
            }
            u32 crc = 0xFFFFFFFFu, peak = 0;
            for (u32 n = 0; n < 320; n++) {
                u32 v = g16(region() + SHM_FINAL + 2 * n);
                s32 s = (s32)(s16)v;
                u32 m = (u32)(s < 0 ? -s : s);
                if (m > peak) peak = m;
                crc = crc32_update(crc32_update(crc, (u8)v), (u8)(v >> 8));
            }
            o_str("f"); o_dec((u32)D.k); o_str(": mix crc="); o_hex8(~crc); o_str(" peak="); o_dec(peak); o_end();
            if ((u32)++D.k >= frames) break;
        }
        step(D.j);
        D.j++;
        if (D.done) break;
        /* publish: raise the counter of the region just changed above the other one's */
        if (svcGetSystemTick() - now >= FRAME_TICKS) { o_str("missed "); o_dec(D.j); o_end(); }   /* too late */
        D.fc[D.x] = D.fc[1 - D.x] + 1;
        p16(region() + SHM_FRAME_COUNTER, D.fc[D.x]);
    }
    if (D.j >= MAX_JS) fail("no more frames", D.j);
}

static void dsp_finish(const char *last) {
    static const u8 shutdown[4] = {1, 0, 0, 0};
    if (D.h) {
        dsp_pipe_write(2, shutdown, 4);
        const u32 recv[2] = {0x00010040, 0};
        for (u32 i = 0; i < 20; i++) {
            if (ipc(recv, 2) == 0 && (cmdbuf()[2] & 0xFF) == 1) break;
            svcSleepThread(ONE_MS);
        }
        svcCloseHandle(D.h);
        svcCloseHandle(D.evt);
        svcCloseHandle(D.srv);
    }
    /* print the collected lines */
    char line[100];
    u32 n = 0;
    for (u32 i = 0; i < out_len; i++) {
        if (out_buf[i] == '\n') { line[n] = 0; print(line); n = 0; }
        else if (n < 98) line[n++] = out_buf[i];
    }
    finish(last);
}

#endif
