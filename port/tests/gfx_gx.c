/* GX test program (N2b-15): drives the GPU through the GSP the way a game does, with no game code:
 * memory fills, display transfers, a texture copy with a gap, buffer swaps of both screens and
 * VBlank waits. Every picture is known exactly; tests/test_native_gfx.py computes the expected
 * image of each step from the parameters below and the formats, and compares it with the
 * screenshots of the reference runner (and of the native runner).
 *
 * Geometry. A screen is stored rotated, as the hardware wants it: the top screen (400x240 as
 * shown) is 400 stored lines of 240 pixels, the bottom screen (320x240) 320 lines of 240. A
 * display transfer therefore moves 240x400 images. Source images are tiled (8x8 Morton blocks).
 *
 * Steps (each one waits for a VBlank so that it starts on a frame, prints a line, does its GX
 * work, swaps, and holds the picture for 60 VBlanks):
 *   1 fills       memory fills (two buffers in one command: 32-bit and 24-bit values; one buffer
 *                 with a 16-bit value), a display transfer of the 32-bit one to the top screen
 *                 (RGBA8 -> RGB8); the bottom screen shows the 16-bit fill (RGB565)
 *   2 rgb565      the same source transferred as RGB565
 *   3 pattern     the 24-bit pattern (a 3-byte period over 4-byte pixels) transferred as RGB8
 *   4 pattern565  the same pattern transferred as RGB565
 *   5 flip        a test card transferred as RGB565 with the vertical flip, to the second buffer
 *   6 scale       a 480x800 test card transferred with the 2x downscale (box filter), RGB8
 *   7 texcopy     a texture copy with a gap in the source into the bottom screen's buffer, shown
 *                 with the SetBufferSwap request
 *   8 swap        both screens swapped in one frame (top back to step 5's buffer through the shared
 *                 block, bottom to the buffer of step 1 by writing the framebuffer registers)
 *   9 alternate   120 VBlanks in which the top screen alternates between two buffers on every
 *                 PDC0 interrupt; prints the VBlank count
 * The lines end in the number of VBlanks the program has seen (8 hex digits), which the test
 * turns into the times of its screenshots. Built by port/tests/build_svc_test.py. */
#include "svc.h"
#include "gsp.h"

#define HOLD_VBLANKS 60u
#define FLIP_VBLANKS 120u

/* ── memory map ───────────────────────────────────────────────────────────────────── */
#define VRAM 0x1F000000u                 /* virtual; the physical address is 0x18000000 */
#define TOP_A (VRAM + 0x000000u)         /* 0x48000 bytes each: room for 240x400 RGB8 */
#define TOP_B (VRAM + 0x100000u)
#define BOT_A (VRAM + 0x200000u)         /* 0x28000 bytes each: room for 240x320 RGB565 */
#define BOT_B (VRAM + 0x300000u)
#define TOP_SIZE 0x48000u
#define BOT_SIZE 0x28000u

#define LIN_SRC_A 0x000000u              /* offsets into the linear block */
#define LIN_SRC_B 0x060000u              /* 240x400 RGBA8 = 384000 bytes each */
#define LIN_SRC_C 0x0C0000u
#define LIN_SRC_D 0x120000u              /* 480x800 RGBA8 = 1536000 bytes */
#define LIN_TEX 0x298000u                /* 320 rows of 512 bytes */
#define LIN_SIZE 0x2C0000u

/* ── what the test knows about (keep in step with tests/test_native_gfx.py) ──────────── */
#define FILL32 0x2090E0FFu               /* R<<24 | G<<16 | B<<8 | A */
#define FILL24 0x00563412u               /* bytes 12 34 56 repeated */
#define FILL16 0xA945u                   /* RGB565 */
#define SRC_BYTES 384000u                /* 240 * 400 * 4 */
#define BOT_BYTES 153600u                /* 240 * 320 * 2 */
#define TEX_PITCH 512u                   /* bytes between the rows of the texture source */
#define TEX_ROW 480u                     /* bytes of pixels in each */
#define PAD_BYTE 0xA5u

static GspGpu gsp;
static u32 lin;                          /* the linear block's address */

/* ── output ───────────────────────────────────────────────────────────────────────── */
static void step_line(const char *label, u32 value) { print_hex(label, value); }

/* ── test cards, written by the CPU ──────────────────────────────────────────────── */
/* the position inside an 8x8 tile: the bits of x and y alternate, x first (0 1 4 5 16 ... along the
 * bottom row, 0 2 8 10 32 ... up the first column) */
static u32 morton(u32 x, u32 y) {
    u32 n = 0;
    for (u32 bit = 0; bit < 3; bit++) n |= ((x >> bit & 1u) << (2 * bit)) | ((y >> bit & 1u) << (2 * bit + 1));
    return n;
}

/* the pixel number of (x, y) in an image `width` pixels wide, stored in 8x8 tiles */
static u32 tiled_index(u32 x, u32 y, u32 width) {
    return (y & ~7u) * width + (x & ~7u) * 8 + morton(x, y);
}

/* The 240x400 card: red grows with x, green with y, blue is a 16-pixel checkerboard. */
static void card_rgb(u32 x, u32 y, u32 *r, u32 *g, u32 *b) {
    *r = x * 255 / 239;
    *g = y * 255 / 399;
    *b = (((x >> 4) + (y >> 4)) & 1) ? 0xFF : 0x00;
}

static void draw_card(u32 addr) {
    volatile u32 *px = (volatile u32 *)addr;
    for (u32 y = 0; y < 400; y++) {
        for (u32 x = 0; x < 240; x++) {
            u32 r, g, b;
            card_rgb(x, y, &r, &g, &b);
            px[tiled_index(x, y, 240)] = r << 24 | g << 16 | b << 8 | 0xFF;
        }
    }
}

/* The 480x800 card: every pixel of the 240x400 card four times, but red is its value with the
 * two low bits cleared plus x%2 + 2*(y%2), so that the box filter's average is not the pixel
 * itself: (4r + 6) / 4 = r + 1 for the r with cleared low bits. */
static void draw_card_2x(u32 addr) {
    volatile u32 *px = (volatile u32 *)addr;
    for (u32 y = 0; y < 800; y++) {
        for (u32 x = 0; x < 480; x++) {
            u32 r, g, b;
            card_rgb(x >> 1, y >> 1, &r, &g, &b);
            r = (r & 0xFCu) + (x & 1) + 2 * (y & 1);
            px[tiled_index(x, y, 480)] = r << 24 | g << 16 | b << 8 | 0xFF;
        }
    }
}

/* The bottom card, as the texture a copy takes from: line y (320 of them) of 240 RGB565 pixels
 * followed by 32 bytes of padding that must not reach the screen. */
static void draw_texture(u32 addr) {
    for (u32 y = 0; y < 320; y++) {
        volatile unsigned short *row = (volatile unsigned short *)(addr + y * TEX_PITCH);
        for (u32 x = 0; x < 240; x++) {
            u32 r5 = x * 31 / 239, g6 = y * 63 / 319, b5 = (((x >> 4) ^ (y >> 4)) & 1) ? 31 : 0;
            row[x] = (unsigned short)(r5 << 11 | g6 << 5 | b5);
        }
        volatile u8 *pad = (volatile u8 *)row + TEX_ROW;
        for (u32 i = 0; i < TEX_PITCH - TEX_ROW; i++) pad[i] = PAD_BYTE;
    }
}

/* ── GX helpers ───────────────────────────────────────────────────────────────────── */
static void fill_wait(void) { gsp_wait(&gsp, GSP_INT_PSC0); }
static void transfer_wait(void) { gsp_wait(&gsp, GSP_INT_PPF); }

static void transfer(u32 in, u32 in_w, u32 in_h, u32 out, u32 out_w, u32 out_h, u32 flags) {
    gsp_display_transfer(&gsp, in, GX_DIM(in_w, in_h), out, GX_DIM(out_w, out_h), flags);
    transfer_wait();
}

static void show_top(u32 address, u32 format, u32 slot) {
    const GspFramebuffer fb = {slot, address, address, 240u * (format == GSP_RGB8 ? 3u : 2u),
                               format, slot, 0};
    gsp_swap_buffers(&gsp, GSP_TOP, &fb);
}

/* the three ways to show a buffer on the bottom screen: the shared block (what games do), the
 * SetBufferSwap request, and the framebuffer registers written directly */
static void show_bottom(u32 address) {
    const GspFramebuffer fb = {0, address, address, 240u * 2u, GSP_RGB565, 0, 0};
    gsp_swap_buffers(&gsp, GSP_BOTTOM, &fb);
}

static void show_bottom_request(u32 address) {
    const GspFramebuffer fb = {0, address, address, 240u * 2u, GSP_RGB565, 0, 0};
    gsp_set_buffer_swap(&gsp, GSP_BOTTOM, &fb);
}

/* the bottom screen's framebuffer registers (gsp.h) hold physical addresses */
static void show_bottom_registers(u32 address) {
    const u32 physical = address - VRAM + 0x18000000u;
    const u32 buffers[3] = {physical, physical, GSP_RGB565};
    const u32 shown = 0, stride = 240u * 2u;
    gsp_write_regs(&gsp, GSP_REGS_FB_BOTTOM + GSP_REG_FB_ADDRESS_LEFT1, buffers, 3);
    gsp_write_regs(&gsp, GSP_REGS_FB_BOTTOM + GSP_REG_FB_ACTIVE, &shown, 1);
    gsp_write_regs(&gsp, GSP_REGS_FB_BOTTOM + GSP_REG_FB_STRIDE, &stride, 1);
}

static u32 hold_until;                   /* the VBlank count at which the step ends */

/* Starts a step on a frame boundary: waits for the next VBlank (the ones that came while the CPU
 * was busy are not counted against the hold) and prints the step's line with the number of
 * VBlanks seen. The step ends HOLD_VBLANKS later, so the steps start 61 VBlanks apart. */
static void begin_step(const char *label) {
    gsp_drain(&gsp);
    const u32 seen = gsp.vblanks;
    gsp_wait_vblank_count(&gsp, seen + 1);
    step_line(label, gsp.vblanks);
    hold_until = gsp.vblanks + HOLD_VBLANKS;
}

static void hold(void) { gsp_wait_vblank_count(&gsp, hold_until); }

/* ── the steps ────────────────────────────────────────────────────────────────────── */
static void step1_fills(void) {
    begin_step("gfx_gx: step 1 fills vblank");
    /* the display buffers start as zeros (two buffers in one command, a 32-bit value) */
    gsp_memory_fill(&gsp, TOP_A, TOP_A + TOP_SIZE, 0, GX_FILL_32BIT, TOP_B, TOP_B + TOP_SIZE, 0, GX_FILL_32BIT);
    fill_wait();
    gsp_memory_fill(&gsp, BOT_A, BOT_A + BOT_SIZE, 0, GX_FILL_32BIT, BOT_B, BOT_B + BOT_SIZE, 0, GX_FILL_32BIT);
    fill_wait();
    /* the sources: a 32-bit and a 24-bit fill in one command (one PSC0 interrupt) */
    gsp_memory_fill(&gsp, lin + LIN_SRC_A, lin + LIN_SRC_A + SRC_BYTES, FILL32, GX_FILL_32BIT,
                    lin + LIN_SRC_B, lin + LIN_SRC_B + SRC_BYTES, FILL24, GX_FILL_24BIT);
    fill_wait();
    /* the bottom screen: one buffer, a 16-bit value */
    gsp_memory_fill(&gsp, BOT_A, BOT_A + BOT_BYTES, FILL16, GX_FILL_16BIT, 0, 0, 0, 0);
    fill_wait();
    transfer(lin + LIN_SRC_A, 240, 400, TOP_A, 240, 400, GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB8));
    show_top(TOP_A, GSP_RGB8, 0);
    show_bottom(BOT_A);
    hold();
}

static void step2_rgb565(void) {
    begin_step("gfx_gx: step 2 rgb565 vblank");
    transfer(lin + LIN_SRC_A, 240, 400, TOP_A, 240, 400, GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB565));
    show_top(TOP_A, GSP_RGB565, 0);
    hold();
}

static void step3_pattern(void) {
    begin_step("gfx_gx: step 3 pattern vblank");
    transfer(lin + LIN_SRC_B, 240, 400, TOP_A, 240, 400, GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB8));
    show_top(TOP_A, GSP_RGB8, 0);
    hold();
}

static void step4_pattern565(void) {
    begin_step("gfx_gx: step 4 pattern565 vblank");
    transfer(lin + LIN_SRC_B, 240, 400, TOP_A, 240, 400, GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB565));
    show_top(TOP_A, GSP_RGB565, 0);
    hold();
}

static void step5_flip(void) {
    begin_step("gfx_gx: step 5 flip vblank");
    transfer(lin + LIN_SRC_C, 240, 400, TOP_B, 240, 400,
             GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB565) | GX_FLIP_VERTICALLY);
    show_top(TOP_B, GSP_RGB565, 0);
    hold();
}

static void step6_scale(void) {
    begin_step("gfx_gx: step 6 scale vblank");
    /* with the downscale the output registers hold the input's size; the image written is half */
    transfer(lin + LIN_SRC_D, 480, 800, TOP_A, 480, 800,
             GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB8) | GX_SCALE_XY);
    show_top(TOP_A, GSP_RGB8, 0);
    hold();
}

static void step7_texcopy(void) {
    begin_step("gfx_gx: step 7 texcopy vblank");
    gsp_flush(&gsp, lin + LIN_TEX, 320 * TEX_PITCH);
    gsp_texture_copy(&gsp, lin + LIN_TEX, TEX_ROW / 16, (TEX_PITCH - TEX_ROW) / 16, BOT_B, 0, 0, BOT_BYTES);
    transfer_wait();
    show_bottom_request(BOT_B);
    hold();
}

static void step8_swap(void) {
    begin_step("gfx_gx: step 8 swap vblank");
    show_top(TOP_B, GSP_RGB565, 0);
    show_bottom_registers(BOT_A);
    hold();
}

static void step9_alternate(void) {
    begin_step("gfx_gx: step 9 alternate vblank");
    const u32 start = gsp.vblanks;
    /* two buffers in the two address slots: slot 0 is TOP_A (RGB8), slot 1 is TOP_B (RGB565); a
     * swap writes the slot it names and shows it */
    show_top(TOP_A, GSP_RGB8, 0);
    u32 slot = 0;
    for (u32 i = 1; i <= FLIP_VBLANKS; i++) {
        gsp_wait_vblank_count(&gsp, start + i);
        slot ^= 1;
        show_top(slot ? TOP_B : TOP_A, slot ? GSP_RGB565 : GSP_RGB8, slot);
    }
    step_line("gfx_gx: flip vblanks", gsp.vblanks - start);
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("gfx_gx: start");
    /* The three cards are drawn before the first VBlank wait. The relay queue drops PDC interrupts
     * once 0x20 are pending (about 16 frames of PDC0 and PDC1), so a much longer setup, on a slower
     * runner, would make the V counts undercount the real VBlanks; the reference needs about 5 frames. */
    if (gsp_open(&gsp) != 0) finish("gfx_gx: no gpu");
    lin = gsp_alloc_linear(LIN_SIZE);
    if (!lin) finish("gfx_gx: no linear memory");
    draw_card(lin + LIN_SRC_C);
    draw_card_2x(lin + LIN_SRC_D);
    draw_texture(lin + LIN_TEX);
    gsp_flush(&gsp, lin + LIN_SRC_C, SRC_BYTES);
    gsp_flush(&gsp, lin + LIN_SRC_D, 4 * SRC_BYTES);
    print("gfx_gx: ready");

    step1_fills();
    step2_rgb565();
    step3_pattern();
    step4_pattern565();
    step5_flip();
    step6_scale();
    step7_texcopy();
    step8_swap();
    step9_alternate();
    step_line("gfx_gx: gsp error", (u32)gsp.error);
    finish("gfx_gx: done");
}
