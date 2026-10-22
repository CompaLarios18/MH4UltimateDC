/* DSP test program: ADPCM sources, one at a time (source 0, gain 1.0 to the main mix). Each case
 * resets the source and plays a made-up ADPCM buffer (the programs make their own samples) with
 * coefficients the program sets: the predictor and scale of each frame come from the data, the buffer's
 * own predictor/scale field is set to its first frame's header, the history (yn1, yn2) with or without
 * the buffer's "dirty" flag; looping ADPCM buffers, two buffers in a row (the history carries over),
 * coefficients changed between buffers, odd lengths, and partial updates of the embedded buffer (an
 * ADPCM source ignores them, a PCM16 one takes the new length). The sync count reports the case number
 * (1-based). Cases 1-12 are the rates 1.0, 0.5, 1.37, 2.9, each in the modes none, linear, polyphase.
 * Our own program, no game code. */
#include "dsp.h"

enum { S_NONE, S_YN, S_YN_IGN, S_NOCOEF, S_TWO, S_COEFCHG, S_PARTIAL, S_PARTIAL_PCM, S_PARTIAL_LOOP, S_PARTIAL_SHRINK, S_PLAYPOS, S_QONLY };
#define N INTERP_NONE
#define L INTERP_LINEAR
#define P INTERP_POLYPHASE
/* fmt, interpolation, loop, special, coefficient set, data set (A: quiet, B: louder), length, rate */
typedef struct { u8 fmt, interp, loop, special, set, data; u16 len; u32 rate; } Case;
#define A3(r) {2, N, 0, 0, 0, 0, 1120, r}, {2, L, 0, 0, 0, 0, 1120, r}, {2, P, 0, 0, 0, 0, 1120, r}
static const Case cases[] = {
    A3(F1), A3(F0_5), A3(F1_37), A3(F2_9),                                         /* 1-12: rate-major */
    {2, L, 0, 0, 1, 1, 560, F1}, {2, L, 0, 0, 1, 1, 560, F0_5}, {2, N, 0, 0, 1, 1, 560, F1_37},   /* 13-16: set B, data B */
    {2, L, 0, 0, 1, 1, 560, F2_9},
    {2, L, 0, S_YN, 0, 0, 560, F1}, {2, L, 0, S_YN_IGN, 0, 0, 560, F1},            /* 17-20: the history; no */
    {2, L, 0, S_NOCOEF, 0, 0, 560, F1}, {2, L, 0, 0, 1, 0, 560, F1},               /* coefficients; set B */
    {2, L, 0, S_TWO, 0, 0, 280, F1}, {2, L, 0, S_COEFCHG, 0, 0, 280, F1},          /* 21-22: buffers in a row */
    {2, L, 1, 0, 0, 0, 140, F1}, {2, N, 1, 0, 1, 1, 98, F1_37}, {2, L, 1, S_YN, 0, 0, 70, F0_5},   /* 23-25: loops */
    {2, L, 0, 0, 0, 0, 13, F1}, {2, L, 0, 0, 0, 0, 15, F1}, {2, L, 0, 0, 0, 0, 27, F1},   /* 26-29: odd lengths */
    {2, L, 0, 0, 0, 0, 1, F1},
    {2, L, 0, S_PARTIAL, 0, 0, 280, F0_5}, {1, L, 0, S_PARTIAL_PCM, 0, 0, 160, F0_5},     /* 30-33: partial */
    {1, L, 0, S_PARTIAL_LOOP, 0, 0, 160, F1}, {1, L, 0, S_PARTIAL_SHRINK, 0, 0, 160, F1},  /* updates (PCM16 too) */
    {2, L, 0, S_PLAYPOS, 0, 0, 560, F1}, {2, L, 0, S_QONLY, 0, 0, 140, F1},        /* 34-35 */
    {2, L, 0, 0, 0, 0, 560, F0_25}, {2, N, 0, 0, 1, 1, 560, F2}, {2, P, 0, 0, 0, 1, 560, F0_25},   /* 36-38 */
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

/* the coefficients: eight pairs of Q11 numbers, set A and set B */
static const s16 coef[2][16] = {
    {0, 0, 0x500, -0x180, 0x705, -0x200, 0xDC1, -0x673, -0x400, 0, 0xA00, -0x500, 0x600, 0, 0xD00, -0x580},
    {0x800, 0, 0xC00, -0x600, 0x7FF, -0x400, 0, 0x400, 0xA00, -0x400, -0x800, 0x400, 0x300, 0x300, 0x900, -0x300},
};
static u32 data[2], pcm;   /* 80 frames of ADPCM each; 1024 PCM16 samples */
static u32 cur, start_j;

static void start(const Case *c, u32 n) {
    u32 id = n + 1, a = phys(c->fmt == 1 ? pcm : data[c->data]);
    src_reset(0);
    src_sync(0, id);
    src_gain(0, 0, F1, F1, 0, 0);
    src_rate(0, c->rate);
    src_interp(0, c->interp);
    if (c->fmt == 2 && c->special != S_NOCOEF) src_coeffs(0, coef[c->set]);
    u32 ps = c->fmt == 2 ? *(volatile u8 *)data[c->data] : 0;   /* the first frame's predictor and scale */
    u32 yn = c->special == S_YN || c->special == S_YN_IGN;
    if (c->special == S_QONLY) {   /* three queued buffers: history kept, history given, history kept */
        src_format(0, FMT_ADPCM, 1);
        src_queue(0, 0, a, c->len, 3, 0, ps, 0, 0, 0);
        src_queue(0, 1, a + 80, c->len, 1, 0, ps, 0x2000, 0xE000, 1);
        src_queue(0, 2, a + 160, c->len, 2, 0, ps, 0, 0, 0);
    } else {
        src_embedded(0, c->fmt, 1, a, c->len, id, c->loop, ps, yn ? 0x2000 : 0, yn ? 0xE000 : 0,
                     c->special == S_YN, c->special == S_PLAYPOS ? 50 : -1);
    }
    src_enable(0, 1);
}

static void during(const Case *c, u32 n, u32 rel) {
    u32 a = phys(data[c->data]), id = n + 1, ps = *(volatile u8 *)data[c->data];
    switch (c->special) {
    case S_TWO:   /* the second buffer goes on from the history the first left, the third restarts it */
        if (rel == 1) {
            src_queue(0, 0, a + 160, c->len, id + 1, 0, ps, 0, 0, 0);
            src_queue(0, 1, a + 320, c->len, id + 2, 0, ps, 0x1000, 0xF000, 1);
        }
        break;
    case S_COEFCHG:   /* new coefficients: they decode the buffer that is queued after the change */
        if (rel == 2) { src_coeffs(0, coef[1]); src_queue(0, 0, a, c->len, id + 1, 0, ps, 0, 0, 0); }
        break;
    case S_PARTIAL: case S_PARTIAL_PCM: case S_PARTIAL_LOOP: case S_PARTIAL_SHRINK:   /* a new length */
        if (rel == 2) {
            p32dsp(cfg(0) + 176, c->special == S_PARTIAL_SHRINK ? 100 : 3 * c->len);
            p16(cfg(0) + 188, c->special == S_PARTIAL_LOOP ? 2 : 0);
            cfg_dirty(0, D_PARTIAL_EMBEDDED);
        }
        break;
    }
}

static void step(u32 j) {
    if (j == 0) { dsp_master(F1); cur = ~0u; }
    const Case *c = cur == ~0u ? 0 : &cases[cur];
    u32 rel = j - start_j;
    if (!c || (c->loop ? rel >= 12 : (rel > 1 && !st_enabled(0)))) {
        if (++cur >= NCASES) { D.done = 1; return; }
        start_j = j;
        start(&cases[cur], cur);
    } else {
        during(c, cur, rel);
    }
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("snd_adpcm: start");
    dsp_init();
    data[0] = dsp_alloc(640); data[1] = dsp_alloc(640); pcm = dsp_alloc(2048);
    gen_adpcm((u8 *)data[0], 80, 1, 1, 7);   /* the first frame selects pair 1 (not (0, 0)), so a history shows */
    gen_adpcm((u8 *)data[1], 80, 3, 2, 9);
    gen_tone((void *)pcm, 1024, 1, 0, 32297, 2028, 0, 0);
    flush(data[0], 640); flush(data[1], 640); flush(pcm, 2048);
    D.watch = 1;
    dsp_run(step, 400);
    dsp_finish("snd_adpcm: done");
}
