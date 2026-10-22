// Derived from Azahar src/video_core/pica/regs_framebuffer.h, regs_rasterizer.h, regs_pipeline.h,
// regs_shader.h, regs_texturing.h, regs_lighting.h, pica_core.cpp (CommandHeader, the registers a command
// list acts on, the default values the GSP leaves), vertex_loader.cpp, output_vertex.cpp and pica_types.h
// (the float24 layout) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; each says "Licensed
// under GPLv2 or any later version"): the numbers of the registers this program writes and the bit
// fields of their values are those of those files (which 3dbrew's "GPU/Internal Registers" documents
// too). The shader instruction words follow 3dbrew's "Shader Instruction Set" and were checked against
// nihstro's shader_bytecode.h, which Azahar vendors (BSD-3-Clause, Copyright 2014 Tony Wasserka); no
// code of it is used.
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h, regs_pipeline.h,
//   regs_shader.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (regs_texturing.h)
// Copyright 2017 Citra Emulator Project (regs_lighting.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp)
// Copyright 2023 Citra Emulator Project (vertex_loader.cpp, output_vertex.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)

/* PICA draw test program (N2b-16): draws with a PICA command list the way a game does, with no game
 * code: a vertex shader of three instructions written word by word, attribute buffers, TEV, the
 * depth test and blending, then a display transfer of the colour buffer to the top screen. The
 * picture is known; tests/test_native_gfx.py computes the expected image from the shapes below
 * and compares it with the screenshots of the reference runner (and of the native runner).
 *
 * The scene (pixel coordinates of the 240x400 colour buffer, x to the right, y up the way the
 * viewport maps it; a corner is a multiple of 15 in x and of 25 in y, so that every value is an
 * exact float of eighths):
 *   background  (0x18, 0x28, 0x38), depth cleared to 1.0
 *   1 red       triangle, depth 0.5, no blending (logic op copy)
 *   2 green     triangle, depth 0.25, overlaps the red one and is nearer: LESS passes
 *   3 yellow    triangle, depth 0.75, drawn after both and behind both: LESS fails over them, so it
 *               shows only on the background (a missing depth test shows as yellow over red and green)
 *   4 blue      quad (two triangles), depth 0.125, alpha 128, blended SRC_ALPHA, ONE_MINUS_SRC_ALPHA
 *               over all of the above and the background
 *
 * Two things the hardware does that a reader of the shader would not expect, and the test follows:
 *  - a UNSIGNED BYTE attribute reaches the shader as the number 0..255, not scaled to 0..1, and the
 *    rasteriser saturates the colour output to 0..1 before interpolating it. `MOV o1, v1` therefore
 *    turns the bytes 0 and 1 into the colours 0 and 255, and any larger byte into 255. So the quad
 *    cannot get its alpha of 128 from its vertex colour: TEV stage 0 takes the quad's alpha from the
 *    primary alpha multiplied by a constant of 128 (the stage's constant colour), and passes the
 *    primary colour's RGB through.
 *  - the program uses no shader uniforms (a uniform is a float24 written in three words).
 *
 * Steps: fills (clear the colour and depth buffers, the display buffers), the command list, a display
 * transfer RGBA8 -> RGB8 of the colour buffer to the top screen (the bottom screen shows black), the
 * swap, and a hold of 60 VBlanks. Built by port/tests/build_svc_test.py. */
#include "svc.h"
#include "gsp.h"

#define HOLD_VBLANKS 60u

/* ── memory map ───────────────────────────────────────────────────────────────────── */
#define VRAM 0x1F000000u                 /* virtual; the physical address is 0x18000000 */
#define TOP_A (VRAM + 0x000000u)         /* the top screen's buffer: 240x400 RGB8 */
#define COLOR_BUF (VRAM + 0x100000u)     /* the PICA's colour buffer: 240x400 RGBA8, tiled */
#define BOT_A (VRAM + 0x200000u)         /* the bottom screen's buffer: 240x320 RGB565 */
#define DEPTH_BUF (VRAM + 0x300000u)     /* the PICA's depth buffer: 240x400 D24S8, tiled */
#define TOP_SIZE 0x48000u
#define BOT_SIZE 0x28000u
#define BUF_BYTES 384000u                /* 240 * 400 * 4: the colour and the depth buffer */

#define LIN_VERTICES 0x0000u             /* offsets into the linear block */
#define LIN_LIST 0x1000u
#define LIST_BYTES 0x2000u
#define LIN_SIZE 0x4000u

/* ── what the test knows about (keep in step with tests/test_native_gfx.py) ──────────── */
#define BG_R 0x18
#define BG_G 0x28
#define BG_B 0x38
#define BG_FILL 0x182838FFu              /* R<<24 | G<<16 | B<<8 | A, the colour buffer's clear value */
#define DEPTH_CLEAR 0x00FFFFFFu          /* depth 1.0, stencil 0 */
#define ALPHA_BLEND 128                  /* the quad's alpha, the constant of TEV stage 0's alpha */
#define VIEW_W 240
#define VIEW_H 400
/* each corner (x, y) in pixels; each depth in eighths (Z/8, nearer is smaller) */
#define RED_X0 30
#define RED_Y0 50
#define RED_X1 210
#define RED_Y1 50
#define RED_X2 120
#define RED_Y2 300
#define RED_Z 4
#define GRN_X0 90
#define GRN_Y0 125
#define GRN_X1 225
#define GRN_Y1 125
#define GRN_X2 165
#define GRN_Y2 350
#define GRN_Z 2
#define YEL_X0 15
#define YEL_Y0 150
#define YEL_X1 225
#define YEL_Y1 150
#define YEL_X2 120
#define YEL_Y2 275
#define YEL_Z 6
#define BLU_X0 45
#define BLU_Y0 100
#define BLU_X1 195
#define BLU_Y1 100
#define BLU_X2 195
#define BLU_Y2 325
#define BLU_X3 45
#define BLU_Y3 325
#define BLU_Z 1

static GspGpu gsp;
static u32 lin;                          /* the linear block's address */

/* ── output ───────────────────────────────────────────────────────────────────────── */
static void step_line(const char *label, u32 value) { print_hex(label, value); }

/* ── float bits without floating-point instructions ───────────────────────────────────── */
/* the IEEE single of j/8, j = -8 .. 8 at index j + 8 */
static const u32 EIGHTHS[17] = {
    0xBF800000u, 0xBF600000u, 0xBF400000u, 0xBF200000u, 0xBF000000u, 0xBEC00000u, 0xBE800000u, 0xBE000000u,
    0x00000000u,
    0x3E000000u, 0x3E800000u, 0x3EC00000u, 0x3F000000u, 0x3F200000u, 0x3F400000u, 0x3F600000u, 0x3F800000u};

/* float24 (the PICA's register format: sign, 7 bits of exponent with bias 63, 16 of mantissa) */
#define F24_120 0x45E000u        /* 1.875 * 2^6 */
#define F24_200 0x469000u        /* 1.5625 * 2^7 */
#define F24_M1 0xBF0000u         /* -1.0 */
/* float31 << 1, the "inverse size" registers: 2 / 240 and 2 / 400 (never read by Azahar) */
#define F31X2_INV_W 0x38111112u
#define F31X2_INV_H 0x3747AE14u

/* ── the vertices: three floats of position and four bytes of colour, 16 bytes each ───── */
#define COLOR_RED 0x01000001u    /* bytes R G B A = 1 0 0 1 (read as 0..255, saturated to 1.0) */
#define COLOR_GREEN 0x01000100u
#define COLOR_YELLOW 0x01000101u
#define COLOR_BLUE 0x01010000u

static u32 *vertex_out;
static u32 vertex_count;

static void vertex(u32 x, u32 y, u32 z_eighths, u32 color) {
    vertex_out[0] = EIGHTHS[x / 15u];            /* NDC x = x / 120 - 1, in eighths: x / 15 - 8 */
    vertex_out[1] = EIGHTHS[y / 25u];            /* NDC y = y / 200 - 1 */
    vertex_out[2] = EIGHTHS[8u - z_eighths];     /* z in [-1, 0]: depth = -z */
    vertex_out[3] = color;
    vertex_out += 4;
    vertex_count++;
}

static void triangle(u32 x0, u32 y0, u32 x1, u32 y1, u32 x2, u32 y2, u32 z, u32 color) {
    vertex(x0, y0, z, color);
    vertex(x1, y1, z, color);
    vertex(x2, y2, z, color);
}

/* ── the command list ─────────────────────────────────────────────────────────────── */
/* A PICA command list is pairs of words: the value, then a header: register id (bits 0-15), the byte
 * mask (16-19), the number of extra words (20-30) and the "consecutive registers" flag (31). With
 * extra words and the flag clear they all go to the same register (a program upload); with it set the
 * extra words go to the next registers. A command is padded to an even number of words. */
static volatile u32 *list;
static u32 list_words;

static void emit(u32 word) {
    if (list_words < LIST_BYTES / 4u) list[list_words] = word;
    list_words++;
}

static void pad(void) { if (list_words & 1u) emit(0); }

static void pica_masked(u32 id, u32 mask, u32 value) {
    emit(value);
    emit(id | (mask << 16));
}
static void pica(u32 id, u32 value) { pica_masked(id, 0xF, value); }

/* `n` words into consecutive registers id, id + 1, ... */
static void pica_run(u32 id, const u32 *values, u32 n) {
    emit(values[0]);
    emit(id | (0xFu << 16) | ((n - 1u) << 20) | (1u << 31));
    for (u32 i = 1; i < n; i++) emit(values[i]);
    pad();
}

/* `n` words into the same register id, one after the other */
static void pica_same(u32 id, const u32 *values, u32 n) {
    emit(values[0]);
    emit(id | (0xFu << 16) | ((n - 1u) << 20));
    for (u32 i = 1; i < n; i++) emit(values[i]);
    pad();
}

/* ── the vertex shader ────────────────────────────────────────────────────────────── */
/* Three instructions, 3dbrew's "Shader Instruction Set": opcode in bits 26-31; for MOV (0x13) the
 * destination register in bits 21-25 (0x00-0x0F are the outputs o0-o15), the source in bits 12-18
 * (0x00-0x0F are the inputs v0-v15) and the operand descriptor's index in bits 0-6; END is 0x22. */
#define OP_MOV(dest, src, desc) ((0x13u << 26) | ((u32)(dest) << 21) | ((u32)(src) << 12) | (u32)(desc))
#define OP_END (0x22u << 26)
/* The operand descriptor 0 (the swizzle table, uploaded to the same place): bits 0-3 write mask (bit 3
 * is x, so 0xF writes all four), bit 4 negates source 1, bits 5-12 source 1's selectors (x is in bits
 * 11-12, w in bits 5-6: .xyzw is 0 1 2 3), bit 13 negates source 2, bits 14-21 its selectors, bit 22
 * negates source 3, bits 23-30 its selectors. Sources 2 and 3 are unused and identity here. */
#define OPDESC(mask, sel1, sel2, sel3) ((u32)(mask) | ((u32)(sel1) << 5) | ((u32)(sel2) << 14) | ((u32)(sel3) << 23))

static void shader_program(void) {
    const u32 program[3] = {
        OP_MOV(0, 0, 0),                 /* mov o0, v0   : position */
        OP_MOV(1, 1, 0),                 /* mov o1, v1   : colour */
        OP_END,                          /* end */
    };
    const u32 operand_descriptor[1] = {OPDESC(0xF, 0x1B, 0x1B, 0x1B)};
    pica(0x2CB, 0);                      /* VSH_CODETRANSFER_CONFIG: the next instruction word is word 0 */
    pica_same(0x2CC, program, 3);        /* VSH_CODETRANSFER_DATA0: three words into the same register */
    pica(0x2BF, 1);                      /* VSH_CODETRANSFER_END */
    pica(0x2D5, 0);                      /* VSH_OPDESCS_CONFIG: the next descriptor is number 0 */
    pica_same(0x2D6, operand_descriptor, 1);
}

/* ── the whole list ───────────────────────────────────────────────────────────────── */
static void draw(u32 first, u32 count) {
    pica_masked(0x25E, 0x2, 0x0000);     /* PRIMITIVE_CONFIG bits 8-9: triangle list */
    pica_masked(0x25F, 0x2, 1);          /* RESTART_PRIMITIVE: any write restarts the primitive assembly */
    pica(0x228, count);                  /* NUMVERTICES */
    pica(0x22A, first);                  /* VERTEX_OFFSET */
    pica_masked(0x229, 0x2, 0x0100);     /* GEOSTAGE_CONFIG: no geometry shader */
    pica(0x245, 0);                      /* START_DRAW_FUNC0: drawing */
    pica(0x22E, 1);                      /* DRAWARRAYS */
    pica(0x245, 1);                      /* back to configuring */
    pica(0x231, 1);                      /* VTX_FUNC: flush the vertex pipeline */
}

static void build_list(u32 vertex_phys) {
    list_words = 0;
    /* ── the render target ── */
    pica(0x111, 1);                                         /* FRAMEBUFFER_INVALIDATE */
    pica(0x112, 0xF);                                       /* colour buffer reads */
    pica(0x113, 0xF);                                       /* colour buffer writes */
    pica(0x114, 0x3);                                       /* depth and stencil reads */
    pica(0x115, 0x3);                                       /* depth and stencil writes */
    pica(0x116, 3);                                         /* depth format D24S8 */
    pica(0x117, 0u << 16);                                  /* colour format RGBA8 (bits 16-18) */
    {
        const u32 buffers[3] = {gsp_virt_to_phys(DEPTH_BUF) >> 3, gsp_virt_to_phys(COLOR_BUF) >> 3,
                                (u32)VIEW_W | ((u32)(VIEW_H - 1) << 12)};   /* 0x11C 0x11D 0x11E: the size is width, height - 1 */
        pica_run(0x11C, buffers, 3);
    }
    /* ── viewport: 240x400 at (0, 0); half sizes as float24, then their inverses ── */
    {
        const u32 viewport[4] = {F24_120, F31X2_INV_W, F24_200, F31X2_INV_H};
        pica_run(0x41, viewport, 4);                        /* VIEWPORT_WIDTH, INVW, HEIGHT, INVH */
        const u32 depth_map[2] = {F24_M1, 0};               /* DEPTHMAP_SCALE -1, OFFSET 0: z in [-1, 0] gives depth z * -1 */
        pica_run(0x4D, depth_map, 2);
    }
    pica(0x68, 0);                                          /* VIEWPORT_XY: corner (0, 0) */
    pica(0x65, 0);                                          /* scissor off */
    pica(0x6D, 1);                                          /* DEPTHMAP_ENABLE: z buffering */
    pica(0x40, 0);                                          /* CULL_CONFIG: keep all */
    pica(0x47, 0);                                          /* user clip plane off */
    /* ── vertex shader outputs: o0 is the position xyzw, o1 the colour rgba ── */
    pica(0x4F, 2);                                          /* SH_OUTMAP_TOTAL */
    {
        const u32 outmap[2] = {0x03020100u,                 /* o0: position x y z w (semantics 0 1 2 3) */
                               0x0B0A0908u};                /* o1: colour r g b a (semantics 8 9 10 11) */
        pica_run(0x50, outmap, 2);
    }
    pica(0x24A, 1);                                         /* VSH_OUTMAP_TOTAL1: outputs - 1 */
    pica(0x251, 1);                                         /* VSH_OUTMAP_TOTAL2 */
    /* ── attributes: one buffer of 16-byte vertices: a (3 floats) at 0, b (4 bytes) at 12 ── */
    {
        const u32 attributes[6] = {
            vertex_phys >> 3,                               /* 0x200 base address (in units of 16 bytes, bit 0 unused) */
            0xDBu,                                          /* 0x201 formats: attribute 0 float x 3 (3 | 2 << 2), 1 ubyte x 4 (1 | 3 << 2) << 4 */
            0x10000000u,                                    /* 0x202 no fixed attributes; two attributes in all, minus one, in bits 28-31 */
            0,                                              /* 0x203 loader 0: data offset */
            0x10u,                                          /* 0x204 loader 0: component 0 is attribute 0, component 1 attribute 1 */
            (16u << 16) | (2u << 28),                       /* 0x205 loader 0: 16 bytes per vertex, 2 components */
        };
        pica_run(0x200, attributes, 6);
    }
    pica(0x242, 1);                                         /* VSH_NUM_ATTR: inputs - 1 */
    /* ── the vertex shader ── */
    pica(0x2B0, 0x7FFF0000u);                               /* VSH_BOOLUNIFORM */
    pica(0x2B9, 0xA0000001u);                               /* VSH_INPUTBUFFER_CONFIG: vertex shader (0xA0 << 24), inputs - 1 */
    pica(0x2BA, 0x7FFF0000u);                               /* VSH_ENTRYPOINT: word 0 */
    {
        const u32 permutation[2] = {0x76543210u, 0xFEDCBA98u};  /* attribute n arrives in register n */
        pica_run(0x2BB, permutation, 2);
    }
    pica(0x2BD, 0x3);                                       /* VSH_OUTMAP_MASK: o0 and o1 */
    shader_program();
    /* ── texture environment: stage 0 the primary colour, stages 1-5 pass the previous result ──
     * each stage: sources, modifiers, operations, constant colour, scales (5 registers) */
    pica(0xE0, 0);                                          /* no fog, no combiner buffer updates */
    pica(0xFD, 0);
    pica(0x80, 0);                                          /* no texture units */
    pica(0x8F, 0);                                          /* fragment lighting off ... */
    pica(0x1C6, 1);                                         /* ... in both of the registers that say it */
    {
        const u32 primary[5] = {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u};
        const u32 previous[5] = {0x0FFF0FFFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u};
        pica_run(0xC0, primary, 5);                         /* stage 0: replace, source primary colour (0), RGB and alpha */
        pica_run(0xC8, previous, 5);                        /* stages 1-3 and 4-5: replace with the previous (0xF) */
        pica_run(0xD0, previous, 5);
        pica_run(0xD8, previous, 5);
        pica_run(0xF0, previous, 5);
        pica_run(0xF8, previous, 5);
    }
    /* ── output merger: depth test LESS, all colour channels and depth writable; no blending, a
     *    logic op that copies; the blend function stays at zeros, which would show if it were used ── */
    {
        const u32 merger[8] = {
            0x00000000u,                                    /* 0x100 blending off (bit 8), fragment operation mode default */
            0x00000000u,                                    /* 0x101 blend function */
            0x00000003u,                                    /* 0x102 logic op: copy */
            0x00000000u,                                    /* 0x103 blend constant */
            0x00000000u,                                    /* 0x104 alpha test off */
            0x00000000u,                                    /* 0x105 stencil test off */
            0x00000000u,                                    /* 0x106 stencil operations */
            0x00001F41u,                                    /* 0x107 depth test (bit 0), LESS (4 << 4), RGBA writes (bits 8-11), depth write (bit 12) */
        };
        pica_run(0x100, merger, 8);
    }
    /* ── the draws: vertices 0-2 red, 3-5 green, 6-8 yellow, 9-14 the quad ── */
    draw(0, 3);
    draw(3, 3);
    draw(6, 3);
    /* the quad: blending on with SRC_ALPHA, ONE_MINUS_SRC_ALPHA (factors 6 and 7) for the colour and the
     * alpha; stage 0's alpha becomes the primary alpha (255) times the constant alpha (128) / 255 */
    pica(0x101, 0x76760000u);                               /* equations add (0); source rgb 6, dest rgb 7, source a 6, dest a 7 */
    pica(0x100, 0x00000100u);                               /* blending on */
    pica(0xC0, 0x00E00000u);                                /* sources: colour primary x 3; alpha source 1 primary (0), source 2 constant (0xE) */
    pica(0xC2, 0x00010000u);                                /* operations: colour replace (0), alpha modulate (1 << 16) */
    pica(0xC3, (u32)ALPHA_BLEND << 24);                     /* the stage's constant colour: alpha 128 */
    draw(9, 6);
    pica(0x110, 1);                                         /* FRAMEBUFFER_FLUSH */
    pica(0x10, 0x12345678u);                                /* FINALIZE: the P3D interrupt */
    pad();
}

/* ── GX helpers ───────────────────────────────────────────────────────────────────── */
static void fill_wait(void) { gsp_wait(&gsp, GSP_INT_PSC0); }
static void transfer_wait(void) { gsp_wait(&gsp, GSP_INT_PPF); }

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("gfx_draw: start");
    if (gsp_open(&gsp) != 0) finish("gfx_draw: no gpu");
    lin = gsp_alloc_linear(LIN_SIZE);
    if (!lin) finish("gfx_draw: no linear memory");

    /* the scene's vertices, then the list that draws them */
    vertex_out = (u32 *)(lin + LIN_VERTICES);
    vertex_count = 0;
    triangle(RED_X0, RED_Y0, RED_X1, RED_Y1, RED_X2, RED_Y2, RED_Z, COLOR_RED);
    triangle(GRN_X0, GRN_Y0, GRN_X1, GRN_Y1, GRN_X2, GRN_Y2, GRN_Z, COLOR_GREEN);
    triangle(YEL_X0, YEL_Y0, YEL_X1, YEL_Y1, YEL_X2, YEL_Y2, YEL_Z, COLOR_YELLOW);
    /* the quad: corners 0 1 2 and 0 2 3 */
    triangle(BLU_X0, BLU_Y0, BLU_X1, BLU_Y1, BLU_X2, BLU_Y2, BLU_Z, COLOR_BLUE);
    triangle(BLU_X0, BLU_Y0, BLU_X2, BLU_Y2, BLU_X3, BLU_Y3, BLU_Z, COLOR_BLUE);
    list = (volatile u32 *)(lin + LIN_LIST);
    build_list(gsp_virt_to_phys(lin + LIN_VERTICES));
    if (vertex_count != 15 || list_words > LIST_BYTES / 4u || (list_words & 1u)) {
        step_line("gfx_draw: bad list words", list_words);
        finish("gfx_draw: failed");
    }
    gsp_flush(&gsp, lin, LIN_SIZE);
    print("gfx_draw: ready");
    step_line("gfx_draw: list words", list_words);

    /* clear: the colour buffer to the background, the depth buffer to 1.0; both screens' buffers to zero */
    gsp_memory_fill(&gsp, COLOR_BUF, COLOR_BUF + BUF_BYTES, BG_FILL, GX_FILL_32BIT,
                    DEPTH_BUF, DEPTH_BUF + BUF_BYTES, DEPTH_CLEAR, GX_FILL_32BIT);
    fill_wait();
    gsp_memory_fill(&gsp, TOP_A, TOP_A + TOP_SIZE, 0, GX_FILL_32BIT, BOT_A, BOT_A + BOT_SIZE, 0, GX_FILL_32BIT);
    fill_wait();

    /* the draw */
    gsp_submit_cmdlist(&gsp, lin + LIN_LIST, list_words * 4u);
    gsp_wait(&gsp, GSP_INT_P3D);
    print("gfx_draw: list done");

    /* to the screen: untile the colour buffer into the top screen's buffer, RGBA8 -> RGB8 */
    gsp_display_transfer(&gsp, COLOR_BUF, GX_DIM(VIEW_W, VIEW_H), TOP_A, GX_DIM(VIEW_W, VIEW_H),
                         GX_IN_FORMAT(GSP_RGBA8) | GX_OUT_FORMAT(GSP_RGB8));
    transfer_wait();
    {
        const GspFramebuffer top = {0, TOP_A, TOP_A, 240u * 3u, GSP_RGB8, 0, 0};
        const GspFramebuffer bottom = {0, BOT_A, BOT_A, 240u * 2u, GSP_RGB565, 0, 0};
        gsp_swap_buffers(&gsp, GSP_TOP, &top);
        gsp_swap_buffers(&gsp, GSP_BOTTOM, &bottom);
    }
    gsp_drain(&gsp);
    const u32 shown = gsp.vblanks;
    step_line("gfx_draw: shown vblank", shown);
    gsp_wait_vblank_count(&gsp, shown + HOLD_VBLANKS);
    step_line("gfx_draw: gsp error", (u32)gsp.error);
    finish("gfx_draw: done");
}
