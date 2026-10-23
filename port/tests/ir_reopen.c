/* ir:USER opened, finalized and opened again (N0-35): the way the game does it with the Circle Pad Pro
 * option on. A shared block at a fixed address goes to InitializeIrNopShared; the program finalizes,
 * closes its handle to the block and to the session (as libctru's iruExit does), then opens ir:USER
 * again and makes a block at the SAME address (the memory is Private again once the last reference to
 * the first block is gone). It prints the result codes. Nothing else may keep the first block alive: the
 * reference runner's --capture-hid used to, and the second CreateMemoryBlock failed with 0xE0A01BF5.
 *
 * For the capture the program keeps a frame counter at the address the runner reads the game's from
 * (0x010571d4: build_svc_test.py gives this program a section of its own there) and changes it every
 * 10 ms, in four phases of five frames; a marker at offset 0x1F0 of each block (inside the 0x200 bytes
 * the capture keeps) tells the records apart:
 *   frames 10 to 14: the first block is alive      (type-2 records, marker 'AAAA')
 *   frames 20 to 24: it is finalized and closed    (no type-2 records)
 *   frames 30 to 34: the second block is alive     (type-2 records, marker 'BBBB')
 *   frames 40 to 44: it is finalized and closed    (no type-2 records)
 * Our own program, no game code. */
#include "ir.h"

#define BLOCK 0x08DF0000u             /* where the game puts its block */
#define BLOCK_SIZE 0x1000u
#define MARKER (BLOCK + 0x1F0u)

/* at the game's frame counter address (see above) */
static volatile u32 frame_counter __attribute__((section(".bss.gamecounter")));

static void frames(u32 first) {
    for (u32 i = 0; i < 5; i++) {
        frame_counter = first + i;
        svcSleepThread(10 * ONE_MS);
    }
}

/* "ir_reopen: <round>_<what>: <value>" */
static void result(const char *round, const char *what, u32 value) {
    char label[48];
    u32 n = 0;
    for (const char *s = "ir_reopen: "; *s; s++) label[n++] = *s;
    for (; *round; round++) label[n++] = *round;
    label[n++] = '_';
    for (; *what; what++) label[n++] = *what;
    label[n] = 0;
    check(label, value);
}

/* open ir:USER, make the block, initialize, mark it, show five frames, finalize, close both handles */
static void open_and_close(const char *round, u32 marker, u32 first_frame) {
    Handle srv = srv_connect(1);
    u32 reply[4] = {0, 0, 0, 0};
    ir_h = srv ? srv_get(srv, "ir:USER", 7, 0, reply) : 0;
    result(round, "open_rc", reply[1]);
    if (srv) svcCloseHandle(srv);
    if (!ir_h) return;

    ir_block = 0;
    const s32 block_rc = svcCreateMemoryBlock(&ir_block, BLOCK, BLOCK_SIZE, 3, 3);
    result(round, "block_rc", (u32)block_rc);
    if (!block_rc) {
        /* shared size, receive buffer size and packet count, send buffer size and count, baud rate; a
         * copied handle */
        const u32 init[9] = {0x00180182, BLOCK_SIZE, IR_RECV_SIZE, IR_RECV_PACKETS, 0x400, 8, 3, 0, ir_block};
        result(round, "initialize_rc", (u32)ir_call(init, 9));
        *(volatile u32 *)MARKER = marker;
    }
    frames(first_frame);

    const u32 finalize[1] = {0x00020000};
    result(round, "finalize_rc", (u32)ir_call(finalize, 1));
    result(round, "close_block_rc", block_rc ? 0 : (u32)svcCloseHandle(ir_block));
    result(round, "close_session_rc", (u32)svcCloseHandle(ir_h));
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("ir_reopen: start");
    u32 out = 0;      /* the heap behind the block is committed once, as the game's is */
    check("ir_reopen: heap_rc", (u32)svcControlMemory(&out, BLOCK, 0, BLOCK_SIZE, 3, 3));
    check("ir_reopen: frame_counter_address", (u32)&frame_counter);
    open_and_close("first", 0x41414141, 10);        /* 'AAAA' */
    frames(20);
    open_and_close("second", 0x42424242, 30);       /* 'BBBB' */
    frames(40);
    finish("ir_reopen: done");
}
