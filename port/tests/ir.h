// Derived from Azahar src/core/hle/service/ir/ir_user.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: it says "Licensed under GPLv2 or any later version"): the layout of ir:USER's
// shared memory (the header, BufferInfo, the PacketInfo queue and the circular data buffer) and the
// packet format (0xA5, the network id, a one or two byte size, the payload, a CRC-8), read here from
// the application's side.
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project
/* Our own ir:USER test programs (ir_*.c, N3-08): the application side of ir:USER as libctru's IRU
 * drives it: a shared memory block over the program's own heap, InitializeIrNopShared, the three
 * events, RequireConnection, SendIrNop, and the receive buffer read packet by packet with
 * ReleaseReceivedData. Nothing is printed by the helpers except failures ("FAIL ..."). No game code,
 * no game data. */
#ifndef PORT_TESTS_IR_H
#define PORT_TESTS_IR_H
#include "svc.h"

#define IR_SHM 0x08000000u          /* the program's heap: committed here, then given to ir:USER */
#define IR_SHM_SIZE 0x1000u
#define IR_RECV_SIZE 0x400u         /* the receive buffer, with its PacketInfo queue */
#define IR_RECV_PACKETS 8u
#define IR_INFO (IR_SHM + 0x10u)    /* BufferInfo: begin index, end index, packet count, unknown */
#define IR_QUEUE (IR_SHM + 0x20u)   /* PacketInfo {offset, size} * IR_RECV_PACKETS, then the data */
#define IR_DATA_SIZE (IR_RECV_SIZE - 8u * IR_RECV_PACKETS)

static Handle ir_h, ir_block, ir_conn, ir_recv, ir_sent;

static inline u32 ir_rd(u32 a) { return *(volatile u32 *)a; }
static inline u32 ir_byte(u32 a) { return *(volatile u8 *)a; }

static void ir_fail(const char *what, u32 v) { print_hex(what, v); }

/* a command of ir:USER: the reply's result code */
static s32 ir_call(const u32 *w, u32 n) {
    s32 rc = request(ir_h, w, n);
    return rc ? rc : (s32)cmdbuf()[1];
}

/* a command that returns one handle (the three event getters) */
static Handle ir_event(u32 command) {
    const u32 w[1] = {command};
    s32 rc = ir_call(w, 1);
    if (rc) { ir_fail("FAIL event", (u32)rc); return 0; }
    return cmdbuf()[3];
}

/* srv:, ir:USER, the shared memory, InitializeIrNopShared and the events; 0 when something failed */
static int ir_init(void) {
    Handle srv = srv_connect(1);
    u32 reply[4] = {0, 0, 0, 0};
    ir_h = srv ? srv_get(srv, "ir:USER", 7, 0, reply) : 0;
    if (!ir_h) { ir_fail("FAIL ir:USER", reply[1]); return 0; }
    u32 out = 0;
    s32 rc = svcControlMemory(&out, IR_SHM, 0, 0x4000, 3, 3);
    if (rc) { ir_fail("FAIL heap", (u32)rc); return 0; }
    rc = svcCreateMemoryBlock(&ir_block, IR_SHM, IR_SHM_SIZE, 3, 3);
    if (rc) { ir_fail("FAIL memory block", (u32)rc); return 0; }
    /* shared size, receive buffer size and packet count, send buffer size and count, baud rate; a
     * copied handle */
    const u32 init[9] = {0x00180182, IR_SHM_SIZE, IR_RECV_SIZE, IR_RECV_PACKETS, 0x400, 8, 3, 0, ir_block};
    rc = ir_call(init, 9);
    if (rc) { ir_fail("FAIL InitializeIrNopShared", (u32)rc); return 0; }
    ir_recv = ir_event(0x000A0000);
    ir_sent = ir_event(0x000B0000);
    ir_conn = ir_event(0x000C0000);
    return ir_recv && ir_sent && ir_conn;
}

static s32 ir_require_connection(u32 device) {
    const u32 w[2] = {0x00060040, device};
    return ir_call(w, 2);
}

/* SendIrNop: the request is a static buffer */
static s32 ir_send(const u8 *data, u32 n) {
    static u8 request_buf[16];
    for (u32 i = 0; i < n; i++) request_buf[i] = data[i];
    const u32 w[4] = {0x000D0042, n, (n << 14) | 2, (u32)request_buf};
    return ir_call(w, 4);
}

static u32 ir_crc8(const u8 *p, u32 n) {
    u32 crc = 0;
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
    }
    return crc;
}

/* The oldest packet of the receive buffer, checked (0xA5, size, CRC-8) and released. Its payload goes
 * to `payload` (at most `max` bytes); returns the payload size, or 0 when the buffer is empty or the
 * packet is damaged (a FAIL line says so). */
static u32 ir_take(u8 *payload, u32 max) {
    if (ir_rd(IR_INFO + 8) == 0) return 0;
    const u32 begin = ir_rd(IR_INFO);
    const u32 offset = ir_rd(IR_QUEUE + 8 * begin), size = ir_rd(IR_QUEUE + 8 * begin + 4);
    u8 packet[96];
    if (size > sizeof(packet)) { ir_fail("FAIL packet size", size); return 0; }
    for (u32 i = 0; i < size; i++) packet[i] = (u8)ir_byte(IR_QUEUE + 8 * IR_RECV_PACKETS + (offset + i) % IR_DATA_SIZE);
    u32 head = 3, n = size >= 3 ? packet[2] : 0;
    if (size >= 4 && (packet[2] & 0x40)) { head = 4; n = ((packet[2] & 0x3Fu) << 8) | packet[3]; }
    int good = size > head && packet[0] == 0xA5 && head + n + 1 == size && ir_crc8(packet, size - 1) == packet[size - 1];
    if (!good) ir_fail("FAIL packet", size);
    const u32 release[2] = {0x00190040, 1};
    s32 rc = ir_call(release, 2);
    if (rc) ir_fail("FAIL ReleaseReceivedData", (u32)rc);
    if (!good || n > max) return 0;
    for (u32 i = 0; i < n; i++) payload[i] = packet[head + i];
    return n;
}

#endif
