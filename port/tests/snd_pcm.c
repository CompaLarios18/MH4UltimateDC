/* DSP test program: PCM sources, one at a time (source 0, gain 1.0 to the main mix, 0 elsewhere).
 * Each case resets the source and plays one buffer of a made-up tone (the programs make their own
 * samples, integers only): PCM16 mono and stereo, PCM8 mono and stereo; rates 0.25 to 2.9; the three
 * interpolation modes; a looping buffer; queued buffers with ids out of order; and the odd cases of
 * the DSP's buffer handling. The sync count of the source reports the case number (1-based), so the
 * lines of a case are those with that sync=. Cases 1-12 are PCM16 mono at the rates 1.0, 0.5, 1.37,
 * 2.9, each in the modes none, linear, polyphase. Our own program, no game code. */
#include "dsp.h"

/* what a case does besides playing its buffer */
enum { S_NONE, S_QSAME, S_QLATER, S_QONLY, S_PRESET, S_PLAYPOS, S_RATE, S_SYNC, S_TOGGLE, S_GAIN, S_BADRATE,
       S_BADADDR, S_MISALIGNED, S_LOOPQ };
#define N INTERP_NONE
#define L INTERP_LINEAR
#define P INTERP_POLYPHASE
/* format, channels, interpolation, loop, special, frames the case may not end before (the source
 * is disabled on purpose), length in samples, rate multiplier (float bits) */
typedef struct { u8 fmt, chans, interp, loop, special, hold; u16 len; u32 rate; } Case;
static const Case cases[] = {
    /* 1-12: PCM16 mono, rate-major: case 1 + 3 * rate + mode (none, linear, polyphase) */
#define A3(r) {1, 1, N, 0, 0, 0, 480, r}, {1, 1, L, 0, 0, 0, 480, r}, {1, 1, P, 0, 0, 0, 480, r}
    A3(F1), A3(F0_5), A3(F1_37), A3(F2_9),
    /* 13-18: PCM16 stereo */
    {1, 2, L, 0, 0, 0, 480, F1}, {1, 2, L, 0, 0, 0, 480, F0_5}, {1, 2, L, 0, 0, 0, 480, F1_37},
    {1, 2, L, 0, 0, 0, 480, F2_9}, {1, 2, N, 0, 0, 0, 480, F1_37}, {1, 2, P, 0, 0, 0, 480, F1_37},
    /* 19-24: PCM8 mono */
    {0, 1, L, 0, 0, 0, 480, F1}, {0, 1, L, 0, 0, 0, 480, F0_5}, {0, 1, L, 0, 0, 0, 480, F1_37},
    {0, 1, L, 0, 0, 0, 480, F2_9}, {0, 1, N, 0, 0, 0, 480, F1_37}, {0, 1, P, 0, 0, 0, 480, F0_5},
    /* 25-28: looping buffers (ten frames each); the last gets a one-shot buffer with a lower id */
    {1, 1, L, 1, 0, 0, 200, F1}, {1, 2, N, 1, 0, 0, 150, F1_37}, {0, 1, L, 1, 0, 0, 100, F0_5},
    {1, 1, L, 1, S_LOOPQ, 0, 120, F1},
    /* 29-33: queues with ids out of order (the last: several buffers in one frame); a partial reset */
    {1, 1, L, 0, S_QSAME, 0, 160, F1}, {1, 1, L, 0, S_QLATER, 0, 160, F1}, {1, 1, L, 0, S_QONLY, 0, 200, F1},
    {1, 1, L, 0, S_QONLY, 0, 40, F1}, {1, 1, L, 0, S_PRESET, 0, 800, F1},
    /* 34-41: a play position, changes while playing, bad rate, bad and misaligned address */
    {1, 1, L, 0, S_PLAYPOS, 0, 480, F1}, {1, 1, L, 0, S_RATE, 0, 1000, F1}, {1, 1, L, 0, S_SYNC, 0, 640, F1},
    {1, 1, L, 0, S_TOGGLE, 5, 640, F1}, {1, 1, L, 0, S_GAIN, 0, 640, F1}, {1, 1, L, 0, S_BADRATE, 0, 320, F0},
    {1, 1, L, 0, S_BADADDR, 0, 100, F1}, {1, 1, L, 0, S_MISALIGNED, 0, 320, F1},
    /* 42-47: buffers shorter than the interpolation's window, and empty */
    {1, 1, L, 0, 0, 0, 1, F1}, {1, 1, L, 0, 0, 0, 2, F1}, {1, 1, L, 0, 0, 0, 3, F1}, {1, 1, L, 0, 0, 0, 5, F1},
    {1, 2, N, 0, 0, 0, 4, F2_9}, {1, 1, L, 0, 0, 0, 0, F1},
    /* 48-53: more rates, and PCM8 stereo */
    {1, 1, L, 0, 0, 0, 480, F0_25}, {1, 2, L, 0, 0, 0, 480, F0_25}, {1, 1, L, 0, 0, 0, 480, F2},
    {0, 2, L, 0, 0, 0, 480, F1}, {0, 2, N, 0, 0, 0, 480, F1_37}, {0, 2, P, 0, 0, 0, 480, F0_5},
};
#define NCASES (sizeof(cases) / sizeof(cases[0]))

static u32 b16m, b16s, b8m;   /* sample buffers: 1024 samples each */
static u32 cur, start_j;

static u32 buf_of(const Case *c) { return c->fmt == FMT_PCM8 ? b8m : c->chans == 2 ? b16s : b16m; }

static void start(const Case *c, u32 n) {
    u32 a = phys(buf_of(c));
    src_reset(0);
    src_sync(0, n + 1);
    src_gain(0, 0, F1, F1, 0, 0);
    src_rate(0, c->rate);
    src_interp(0, c->interp);
    if (c->special == S_QONLY) {   /* no embedded buffer: four queued ones, ids 7 3 12 1 */
        src_format(0, c->fmt, c->chans);
        static const u8 ids[4] = {7, 3, 12, 1};
        for (u32 i = 0; i < 4; i++) src_queue(0, i, a + 64 * i, c->len, ids[i], 0, 0, 0, 0, 0);
    } else if (c->special == S_QSAME) {   /* embedded 5, queued 9 and 2 in the same frame */
        src_embedded(0, c->fmt, c->chans, a, c->len, 5, 0, 0, 0, 0, 0, -1);
        src_queue(0, 0, a + 64, c->len, 9, 0, 0, 0, 0, 0);
        src_queue(0, 1, a + 128, c->len, 2, 0, 0, 0, 0, 0);
    } else {
        u32 addr = c->special == S_BADADDR ? 0 : c->special == S_MISALIGNED ? a + 2 : a;
        src_embedded(0, c->fmt, c->chans, addr, c->len, n + 1, c->loop, 0, 0, 0, 0, c->special == S_PLAYPOS ? 200 : -1);
    }
    src_enable(0, 1);
}

static void during(const Case *c, u32 rel) {
    u32 a = phys(buf_of(c));
    switch (c->special) {
    case S_QLATER:   /* queued while the embedded buffer plays: the lower id (4) is played first */
        if (rel == 2) { src_queue(0, 0, a + 64, c->len, 8, 0, 0, 0, 0, 0); src_queue(0, 1, a + 128, c->len, 4, 0, 0, 0, 0, 0); }
        break;
    case S_PRESET:   /* ids 6 and 7 are queued, then dropped by a partial reset; id 4 is queued after it */
        if (rel == 1) { src_queue(0, 0, a, 100, 6, 0, 0, 0, 0, 0); src_queue(0, 1, a, 100, 7, 0, 0, 0, 0, 0); }
        if (rel == 3) { cfg_dirty(0, D_PARTIAL_RESET); src_queue(0, 0, a + 64, 200, 4, 0, 0, 0, 0, 0); }
        break;
    case S_RATE: if (rel == 2) src_rate(0, F0_5); if (rel == 4) src_rate(0, F2_9); break;
    case S_SYNC: if (rel == 2) src_sync(0, 100); if (rel == 3) src_sync(0, 101); break;
    case S_TOGGLE: if (rel == 2) src_enable(0, 0); if (rel == 4) src_enable(0, 1); break;
    case S_GAIN:
        if (rel == 2) src_gain(0, 0, F0_5, F0_5, 0, 0);
        if (rel == 4) src_gain(0, 0, F1, 0, 0, 0);
        break;
    case S_LOOPQ: if (rel == 4) src_queue(0, 0, a + 64, 80, 2, 0, 0, 0, 0, 0); break;
    }
}

static void step(u32 j) {
    if (j == 0) { dsp_master(F1); cur = ~0u; }
    const Case *c = cur == ~0u ? 0 : &cases[cur];
    u32 rel = j - start_j;
    /* the next case: when the source has stopped (or a looping case has had its ten frames) */
    if (!c || (c->loop ? rel >= 10 : (rel > c->hold + 1 && !st_enabled(0)))) {
        if (++cur >= NCASES) { D.done = 1; return; }
        start_j = j;
        start(&cases[cur], cur);
    } else {
        during(c, rel);
    }
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("snd_pcm: start");
    dsp_init();
    b16m = dsp_alloc(2048); b16s = dsp_alloc(4096); b8m = dsp_alloc(1024);
    gen_tone((void *)b16m, 1024, 1, 0, 32297, 2028, 0, 0);       /* periods of 37 and 23 samples */
    gen_tone((void *)b16s, 1024, 2, 0, 32297, 2028, 31553, 3238);
    gen_tone((void *)b8m, 1024, 1, 1, 32538, 1419, 0, 0);
    flush(b16m, 2048); flush(b16s, 4096); flush(b8m, 1024);
    D.watch = 1;
    dsp_run(step, 400);
    dsp_finish("snd_pcm: done");
}
