/* DSP test program: three looping sources and the DSP's mixers. Source 0 is a stereo tone, 1 and 2 are
 * mono tones. The timeline (the interrupt count t) steps through: the master volume; the gains of a
 * source into the main mix (left, right, rear); source 2, which is in the main mix and in an aux bus
 * at once, with the first bus internal, then going through the application's memory (the program
 * halves the samples in it for eight frames), then the second bus; the delay effect on each bus; the simple and the biquad filter of a source; the
 * output format (stereo is the SDK's default; mono and surround follow); and the sources stopping. The
 * lines are the three sources' status and the CRC of the final mix of each frame.
 * Our own program, no game code. */
#include "dsp.h"

static u32 tone[3];   /* the buffers: 400 stereo, 300 mono, 240 mono samples */

static void play(u32 i, u32 chans, u32 len, u32 gm0, u32 gm1) {
    src_reset(i);
    src_sync(i, i + 1);
    src_rate(i, F1);
    src_interp(i, INTERP_LINEAR);
    src_gain(i, 0, gm0, gm0, 0, 0);
    src_gain(i, 1, gm1, gm1, 0, 0);
    src_gain(i, 2, 0, 0, 0, 0);
    src_embedded(i, FMT_PCM16, chans, phys(tone[i]), len, 10 + i, 1, 0, 0, 0, 0, -1);
    src_enable(i, 1);
    D.watch |= 1u << i;
}

/* the application's own effect on aux bus n: half volume, in place, on the samples the DSP read back next */
static void halve(u32 n) {
    u32 p = region() + SHM_INTERMEDIATE + 2560 * n;
    for (u32 k = 0; k < 640; k++) p32le(p + 4 * k, (u32)((s32)g32le(p + 4 * k) >> 1));
}

/* the timeline: at interrupt t, operation op on source/bus i with arguments x, a, b, c */
enum { MASTER, PLAY, GAIN, AUX_RETURN, AUX_ENABLE, DELAY, FILTERS, SIMPLE, BIQUAD, FORMAT, ENABLE, HALVE, END };
typedef struct { u16 t; u8 op, i; u32 x, a, b, c; } Ev;
static const Ev ev[] = {
    {0, MASTER, 0, 0, F1}, {0, FORMAT, 0, 0, 1}, {0, PLAY, 0, 2, 400, F1, 0},
    /* master volume */
    {12, MASTER, 0, 0, F0_5}, {24, MASTER, 0, 0, F0_25}, {36, MASTER, 0, 0, F0}, {42, MASTER, 0, 0, F1},
    /* source 1 into the main mix: both channels, left only, right only, and with the rear channels */
    {48, PLAY, 1, 1, 300, F0_25, 0}, {60, GAIN, 1, 0, F0_5, 0, 0}, {72, GAIN, 1, 0, 0, F0_5, 0},
    {84, GAIN, 1, 0, F0_25, F0_25, F0_25},
    /* source 2 into aux bus 0, which stays inside the DSP; then through the application's memory, where
     * the program halves the samples for eight frames; then the second bus */
    {96, PLAY, 2, 1, 240, F0_125, F0_5}, {96, AUX_RETURN, 0, 0, F1}, {108, AUX_ENABLE, 0, 0, 1}, {120, HALVE, 0, 0, 8},
    {132, AUX_ENABLE, 0, 0, 0}, {132, GAIN, 2, 1, 0, 0, 0}, {132, GAIN, 2, 2, F0_5, F0_5, 0}, {132, AUX_RETURN, 1, 0, F0_5},
    {133, AUX_ENABLE, 1, 0, 1}, {146, AUX_ENABLE, 1, 0, 0},
    /* the delay effect on each bus: two frames with feedback (g, a, b in Q7), then one frame plain */
    {156, GAIN, 2, 1, F0_5, F0_5, 0}, {156, DELAY, 0, 1, 2, 64 | (96 << 8) | (32 << 16)},
    {180, DELAY, 0, 0, 0, 0}, {180, DELAY, 1, 1, 1, 128 << 8}, {204, DELAY, 1, 0, 0, 0}, {204, GAIN, 2, 1, 0, 0, 0},
    /* filters: simple on source 0, biquad on source 0, both, then source 1's simple filter */
    {210, SIMPLE, 0, 0, 0x4000, 0x2000}, {210, FILTERS, 0, 0, 1, 0}, {222, BIQUAD, 0}, {222, FILTERS, 0, 0, 0, 1},
    {234, FILTERS, 0, 0, 1, 1}, {234, SIMPLE, 0, 0, 0x4000, 0x2000}, {246, FILTERS, 0, 0, 0, 0}, {246, SIMPLE, 1, 0, 0x2000, 0x5000},
    {246, FILTERS, 1, 0, 1, 0}, {258, FILTERS, 1, 0, 0, 0},
    /* the output format: mono, surround, stereo again */
    {264, FORMAT, 0, 0, 0}, {276, FORMAT, 0, 0, 2}, {288, FORMAT, 0, 0, 1},
    /* the sources stop */
    {294, MASTER, 0, 0, F2}, {300, MASTER, 0, 0, F1}, {300, ENABLE, 2, 0, 0}, {306, ENABLE, 1, 0, 0}, {312, ENABLE, 0, 0, 0}, {318, END},
};
static u32 halve_bus, halve_until;

static void step(u32 t) {
    for (u32 n = 0; n < sizeof(ev) / sizeof(ev[0]); n++) {
        const Ev *e = &ev[n];
        if (e->t != t) continue;
        u32 i = e->i;
        switch (e->op) {
        case MASTER: dsp_master(e->a); break;
        case PLAY: play(i, e->x, e->a, e->b, e->c); break;
        case GAIN: src_gain(i, e->x, e->a, e->b, e->c, e->c); break;
        case AUX_RETURN: dsp_aux_return(i, e->a); break;
        case AUX_ENABLE: dsp_aux_enable(i, e->a); break;
        case DELAY: dsp_delay(i, e->x, e->a, (s32)(e->b & 0xFF), (s32)((e->b >> 8) & 0xFF), (s32)((e->b >> 16) & 0xFF)); break;
        case FILTERS: src_filters(i, e->a, e->b); break;
        case SIMPLE: src_simple(i, e->a, e->b); break;
        case BIQUAD: src_biquad(i, -0xC00, 0x2400, 0x800, 0x1000, 0x800); break;
        case FORMAT: dsp_output_format(e->a); break;
        case ENABLE: src_enable(i, e->a); break;
        case HALVE: halve_bus = i; halve_until = t + e->a; break;
        case END: D.done = 1; break;
        }
    }
    if (t < halve_until) halve(halve_bus);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("snd_mix: start");
    dsp_init();
    for (u32 i = 0; i < 3; i++) tone[i] = dsp_alloc(1024 * 4);
    gen_tone((void *)tone[0], 400, 2, 0, 32297, 2028, 31553, 3238);
    gen_tone((void *)tone[1], 300, 1, 0, 32538, 1419, 0, 0);
    gen_tone((void *)tone[2], 240, 1, 0, 30555, 4335, 0, 0);
    for (u32 i = 0; i < 3; i++) flush(tone[i], 1024 * 4);
    dsp_run(step, 400);
    dsp_finish("snd_mix: done");
}
