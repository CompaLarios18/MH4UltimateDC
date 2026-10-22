// Derived from Azahar src/core/hle/service/gsp/gsp_command.h, gsp_gpu.h, gsp_interrupt.h and gsp_gpu.cpp
// and src/video_core/pica/regs_external.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; each says "Licensed under GPLv2 or any later version"): the command ids, the
// framebuffer info, the interrupt ids, the offsets of the relay queue, the framebuffer updates and the
// command queue in the shared block, and the fill and transfer flag bits are the numbers and layouts of
// those files (which 3dbrew documents too).
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (gsp_command.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (gsp_gpu.h, gsp_gpu.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (gsp_interrupt.h, regs_external.h)

/* The GPU the way a game talks to it (gsp::Gpu), for the port's bare test programs (no game
 * code, no libctru): the service session, the interrupt event and the shared memory block, the
 * linear heap, GX commands queued into the shared command queue, framebuffer swaps, and waits
 * for a given interrupt. Needs svc.h.
 *
 * The layouts are the ones of 3dbrew's "GSP Services" and "GSP Shared Memory" pages (the shared
 * block is 0x1000 bytes, one 0x40-byte interrupt relay queue, two 0x40-byte framebuffer updates
 * and one 0x200-byte command queue per GSP thread). Both sides write the queues, so every access
 * goes through volatile pointers; there is only one program thread here, so no atomics.
 *
 * C has no namespaces: the spec's gsp::Gpu is struct GspGpu and its members are gsp_*. */
#ifndef PORT_TESTS_GSP_H
#define PORT_TESTS_GSP_H

#include "svc.h"

/* ── constants ──────────────────────────────────────────────────────────────────── */
#define GSP_SHM_ADDR 0x10002000u     /* where the shared block is mapped */
#define GSP_SHM_SIZE 0x1000u

enum { /* interrupt ids in the relay queue's slots */
    GSP_INT_PSC0 = 0, GSP_INT_PSC1 = 1, GSP_INT_PDC0 = 2, GSP_INT_PDC1 = 3,
    GSP_INT_PPF = 4, GSP_INT_P3D = 5, GSP_INT_DMA = 6, GSP_INT_COUNT = 7
};

enum { /* pixel formats of the display transfer and the framebuffer registers */
    GSP_RGBA8 = 0, GSP_RGB8 = 1, GSP_RGB565 = 2, GSP_RGB5A1 = 3, GSP_RGBA4 = 4
};

enum { GSP_TOP = 0, GSP_BOTTOM = 1 };

/* GX command ids (the low byte of the command's first word) */
enum { GX_REQUEST_DMA = 0, GX_SUBMIT_CMDLIST = 1, GX_MEMORY_FILL = 2, GX_DISPLAY_TRANSFER = 3,
       GX_TEXTURE_COPY = 4, GX_CACHE_FLUSH = 5 };

/* Memory fill control bits (the 16 bits per buffer of the command's last word) */
#define GX_FILL_TRIGGER 0x001u
#define GX_FILL_16BIT 0x000u
#define GX_FILL_24BIT 0x100u
#define GX_FILL_32BIT 0x200u

/* Display transfer / texture copy flags */
#define GX_FLIP_VERTICALLY (1u << 0)
#define GX_INPUT_LINEAR (1u << 1)        /* linear input, tiled output (a texture upload) */
#define GX_CROP_INPUT_LINES (1u << 2)
#define GX_RAW_COPY (1u << 3)            /* a texture copy: bytes, no conversion */
#define GX_DONT_SWIZZLE (1u << 5)        /* both linear (or both tiled) */
#define GX_IN_FORMAT(f) ((u32)(f) << 8)
#define GX_OUT_FORMAT(f) ((u32)(f) << 12)
#define GX_SCALE_X (1u << 24)            /* half the width, box filter */
#define GX_SCALE_XY (2u << 24)           /* half the width and height */
#define GX_DIM(w, h) (((u32)(h) << 16) | ((u32)(w) & 0xFFFFu))

#define GSP_RELAY_SLOTS 0x34u
#define GSP_QUEUE_SLOTS 15u

/* A framebuffer as the shared block and SetBufferSwap describe it (3dbrew: FramebufferInfo) */
typedef struct {
    u32 active_fb;     /* which of the two address pairs this update writes (0 or 1) */
    u32 address_left;  /* virtual addresses */
    u32 address_right;
    u32 stride;        /* bytes from one stored line to the next */
    u32 format;        /* GSP_RGBA8 ... (bits 0-2) */
    u32 shown_fb;      /* which address pair the screen shows */
    u32 unknown;
} GspFramebuffer;

typedef struct {
    Handle session;    /* gsp::Gpu */
    Handle event;      /* signalled when the relay queue gets an interrupt */
    Handle shm_handle;
    u32 thread;        /* this client's GSP thread index */
    volatile u8 *shm;
    u32 pending[GSP_INT_COUNT];   /* interrupts taken from the relay queue and not waited for */
    u32 vblanks;                  /* PDC0 interrupts taken from the queue so far (the VBlanks seen) */
    s32 error;                    /* the first failing result, 0 when none */
} GspGpu;

static inline void gsp_fail(GspGpu *g, const char *what, s32 r) {
    if (r != 0 && g->error == 0) {
        g->error = r;
        print_hex(what, (u32)r);
    }
}

/* A result of a GSP request: negative is an error (GSP answers 0xC.. codes with the top bit set;
 * its first-initialisation result is a nonzero success). */
static inline s32 gsp_result(s32 rc) {
    u32 *cb = cmdbuf();
    return rc != 0 ? rc : ((s32)cb[1] < 0 ? (s32)cb[1] : 0);
}

/* ── opening ───────────────────────────────────────────────────────────────────── */

/* srv: -> gsp::Gpu, the interrupt event, RegisterInterruptRelayQueue, the shared block mapped,
 * AcquireRight. Returns 0, or the first error (also in g->error). */
static inline s32 gsp_open(GspGpu *g) {
    for (u32 i = 0; i < sizeof(*g); i++) ((volatile u8 *)g)[i] = 0;
    Handle srv = srv_connect(1);
    if (!srv) { gsp_fail(g, "gsp: srv:", -1); return g->error; }
    u32 reply[4];
    g->session = srv_get(srv, "gsp::Gpu", 8, 0, reply);
    if (!g->session) { gsp_fail(g, "gsp: GetServiceHandle", (s32)reply[1]); return g->error; }
    gsp_fail(g, "gsp: CreateEvent", svcCreateEvent(&g->event, 0));   /* one-shot */

    /* RegisterInterruptRelayQueue 0x00130042: flags, handle descriptor, event handle */
    const u32 reg[4] = {0x00130042, 0, 0, g->event};
    s32 r = gsp_result(request(g->session, reg, 4));
    gsp_fail(g, "gsp: RegisterInterruptRelayQueue", r);
    u32 *cb = cmdbuf();
    g->thread = cb[2];
    g->shm_handle = cb[4];
    gsp_fail(g, "gsp: MapMemoryBlock", svcMapMemoryBlock(g->shm_handle, GSP_SHM_ADDR, 3, 0x10000000));
    g->shm = (volatile u8 *)GSP_SHM_ADDR;

    /* AcquireRight 0x00160042: flag, a copy-handle descriptor and the current process's handle */
    const u32 acq[4] = {0x00160042, 0, 0, CUR_PROCESS};
    gsp_fail(g, "gsp: AcquireRight", gsp_result(request(g->session, acq, 4)));
    return g->error;
}

/* Linear heap memory (physically contiguous: what the GPU reads and writes); 0 on failure. */
static inline u32 gsp_alloc_linear(u32 size) {
    u32 out = 0;
    return svcControlMemory(&out, 0, 0, size, 0x3u | 0x10000u, 3) == 0 ? out : 0;
}

/* ── the interrupt relay queue ──────────────────────────────────────────────────── */

/* Takes every interrupt out of this client's relay queue into g->pending. */
static inline void gsp_drain(GspGpu *g) {
    volatile u8 *q = g->shm + g->thread * 0x40u;
    while (q[1] != 0) {
        u32 index = q[0];
        u32 id = q[0x0C + index];
        if (id < GSP_INT_COUNT) g->pending[id]++;
        if (id == GSP_INT_PDC0) g->vblanks++;
        q[0] = (u8)((index + 1) % GSP_RELAY_SLOTS);
        q[1] = (u8)(q[1] - 1);
    }
}

/* Waits until the interrupt `id` is there and takes it (in the order the GSP queued them). */
static inline void gsp_wait(GspGpu *g, u32 id) {
    for (;;) {
        gsp_drain(g);
        if (g->pending[id]) {
            g->pending[id]--;
            return;
        }
        svcWaitSynchronization(g->event, FOREVER);
    }
}

/* Waits until the program has seen `count` VBlanks in all (g->vblanks >= count), whatever it
 * did in between: a hold ends on the same VBlank however long the work before it took. */
static inline void gsp_wait_vblank_count(GspGpu *g, u32 count) {
    for (;;) {
        gsp_drain(g);
        if (g->vblanks >= count) return;
        svcWaitSynchronization(g->event, FOREVER);
    }
}

/* ── GX commands ───────────────────────────────────────────────────────────────── */

/* TriggerCmdReqQueue 0x000C0000 */
static inline void gsp_trigger(GspGpu *g) {
    const u32 w[1] = {0x000C0000};
    gsp_fail(g, "gsp: TriggerCmdReqQueue", gsp_result(request(g->session, w, 1)));
}

/* Appends one 8-word command to this client's queue in shared memory; when the queue was empty
 * it asks the GSP to process it (a non-empty queue is already being processed). */
static inline void gsp_queue(GspGpu *g, const u32 cmd[8]) {
    volatile u8 *header = g->shm + 0x800u + g->thread * 0x200u;
    u32 index, count;
    for (;;) {
        index = header[0];
        count = header[1];
        if (count < GSP_QUEUE_SLOTS) break;
        svcSleepThread(ONE_MS);
    }
    volatile u32 *slot = (volatile u32 *)(header + 0x20u + ((index + count) % GSP_QUEUE_SLOTS) * 0x20u);
    for (u32 i = 0; i < 8; i++) slot[i] = cmd[i];
    header[1] = (u8)(count + 1);
    if (count == 0) gsp_trigger(g);
}

/* Fills [start, end) with a 16, 24 or 32 bit value (mode: GX_FILL_*); a second range is
 * filled by the same command when start2 is not 0. Interrupt: PSC0 once. */
static inline void gsp_memory_fill(GspGpu *g, u32 start1, u32 end1, u32 value1, u32 mode1,
                                   u32 start2, u32 end2, u32 value2, u32 mode2) {
    const u32 cmd[8] = {GX_MEMORY_FILL, start1, value1, end1, start2, value2, end2,
                        (start1 ? (mode1 | GX_FILL_TRIGGER) : 0) |
                        ((start2 ? (mode2 | GX_FILL_TRIGGER) : 0) << 16)};
    gsp_queue(g, cmd);
}

/* in_dim and out_dim are GX_DIM(width, height); interrupt: PPF. */
static inline void gsp_display_transfer(GspGpu *g, u32 in, u32 in_dim, u32 out, u32 out_dim, u32 flags) {
    const u32 cmd[8] = {GX_DISPLAY_TRANSFER, in, out, in_dim, out_dim, flags, 0, 0};
    gsp_queue(g, cmd);
}

/* Copies `size` bytes (a multiple of 16) in rows: the input is read in rows of (in_width) 16-byte
 * units and (in_gap) units are skipped after each, the output likewise; a gap of 0 means
 * contiguous. Interrupt: PPF. */
static inline void gsp_texture_copy(GspGpu *g, u32 in, u32 in_width, u32 in_gap, u32 out,
                                    u32 out_width, u32 out_gap, u32 size) {
    const u32 cmd[8] = {GX_TEXTURE_COPY, in, out, size, (in_gap << 16) | in_width,
                        (out_gap << 16) | out_width, GX_RAW_COPY, 0};
    gsp_queue(g, cmd);
}

/* FlushDataCache 0x00080082: address, size, the process handle (copy-handle descriptor 0) */
static inline void gsp_flush(GspGpu *g, u32 address, u32 size) {
    const u32 w[5] = {0x00080082, address, size, 0, CUR_PROCESS};
    gsp_fail(g, "gsp: FlushDataCache", gsp_result(request(g->session, w, 5)));
}

/* ── framebuffers ──────────────────────────────────────────────────────────────── */

/* The way libctru's gfxSwapBuffers does it: write the update into the shared block and set its
 * dirty flag; the GSP applies it at the screen's next VBlank (PDC0 for the top screen, PDC1 for
 * the bottom one). */
static inline void gsp_swap_buffers(GspGpu *g, u32 screen, const GspFramebuffer *fb) {
    volatile u8 *update = g->shm + 0x200u + (2u * g->thread + screen) * 0x40u;
    u32 index = (update[0] ^ 1u) & 1u;
    volatile u32 *info = (volatile u32 *)(update + 4u + index * 0x1Cu);
    info[0] = fb->active_fb; info[1] = fb->address_left; info[2] = fb->address_right;
    info[3] = fb->stride; info[4] = fb->format; info[5] = fb->shown_fb; info[6] = fb->unknown;
    update[0] = (u8)index;
    update[1] = 1;     /* dirty */
}

/* SetBufferSwap 0x00050200: the same update, applied at once. */
static inline void gsp_set_buffer_swap(GspGpu *g, u32 screen, const GspFramebuffer *fb) {
    const u32 w[9] = {0x00050200, screen, fb->active_fb, fb->address_left, fb->address_right,
                      fb->stride, fb->format, fb->shown_fb, fb->unknown};
    gsp_fail(g, "gsp: SetBufferSwap", gsp_result(request(g->session, w, 9)));
}

/* WriteHWRegs 0x00010082: `words` registers from `reg` (an offset from 0x1EB00000, e.g. 0x400000
 * for the GPU's first register), sent as a static buffer (at most 0x80 bytes). */
static inline void gsp_write_regs(GspGpu *g, u32 reg, const u32 *values, u32 words) {
    const u32 bytes = words * 4;
    const u32 w[5] = {0x00010082, reg, bytes, (bytes << 14) | 2u, (u32)values};
    gsp_fail(g, "gsp: WriteHWRegs", gsp_result(request(g->session, w, 5)));
}

#endif
