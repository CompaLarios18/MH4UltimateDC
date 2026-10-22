/* The Circle Pad Pro on ir:USER (N3-08): a program connects device 1 (the Circle Pad Pro), configures
 * the polling and reads the calibration data with SendIrNop, then receives 50 status packets one at a
 * time, with no input (the stick centred, ZL, ZR and R not held), and prints the calibration answers
 * and every status packet's payload as hex. A few requests Azahar ignores (a read past the end, a
 * configuration of the wrong size, an unknown request) follow the two calibration reads: they print
 * nothing but must not change the packets that follow. At the end it disconnects and checks that no
 * packet arrives any more. Our own program, no game code. */
#include "ir.h"

static void hex_line(const char *prefix, const u8 *data, u32 n) {
    char line[256];
    u32 len = 0;
    for (; prefix[len]; len++) line[len] = prefix[len];
    for (u32 i = 0; i < n; i++) {
        line[len++] = "0123456789abcdef"[data[i] >> 4];
        line[len++] = "0123456789abcdef"[data[i] & 15];
    }
    line[len] = 0;
    print(line);
}

static void rc_line(const char *name, s32 rc) { check(name, (u32)rc); }

static void dec2(char *dst, u32 v) { dst[0] = (char)('0' + v / 10); dst[1] = (char)('0' + v % 10); }

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("ir_cpp: start");
    if (!ir_init()) finish("ir_cpp: done");

    rc_line("ir_cpp: require_unknown_device", ir_require_connection(7));
    rc_line("ir_cpp: require_rc", ir_require_connection(1));
    check("ir_cpp: connection_event", (u32)svcWaitSynchronization(ir_conn, 1000 * ONE_MS));
    check("ir_cpp: header_status", ir_byte(IR_SHM + 8) | (ir_byte(IR_SHM + 9) << 8) | (ir_byte(IR_SHM + 10) << 16));
    check("ir_cpp: header_connected", ir_byte(IR_SHM + 12) | (ir_byte(IR_SHM + 14) << 8));

    static const u8 polling[3] = {1, 20, 0};              /* every 20 ms */
    static const u8 calibration[6] = {2, 0, 0x00, 0x00, 0x40, 0x00};
    static const u8 unaligned[6] = {2, 0, 0x13, 0x00, 0x1F, 0x00};
    static const u8 past_the_end[6] = {2, 0, 0x30, 0x00, 0x20, 0x00};
    static const u8 wrong_size[2] = {1, 5};
    static const u8 unknown[4] = {7, 1, 2, 3};
    rc_line("ir_cpp: send_polling_rc", ir_send(polling, 3));
    rc_line("ir_cpp: send_calibration_rc", ir_send(calibration, 6));
    rc_line("ir_cpp: send_unaligned_rc", ir_send(unaligned, 6));
    rc_line("ir_cpp: send_past_the_end_rc", ir_send(past_the_end, 6));
    rc_line("ir_cpp: send_wrong_size_rc", ir_send(wrong_size, 2));
    rc_line("ir_cpp: send_unknown_rc", ir_send(unknown, 4));

    u32 statuses = 0, answers = 0;
    u64 first_tick = 0, last_tick = 0;
    while (statuses < 50) {
        if (svcWaitSynchronization(ir_recv, 1000 * ONE_MS) != 0) { check("FAIL receive event", statuses); break; }
        u8 payload[80];
        while (statuses < 50) {
            u32 n = ir_take(payload, sizeof(payload));
            if (!n) break;
            if (payload[0] == 0x10 && n == 6) {
                char name[] = "ir_cpp: pkt 00 ";
                dec2(name + 12, ++statuses);
                if (statuses == 1) first_tick = svcGetSystemTick();
                last_tick = svcGetSystemTick();
                hex_line(name, payload, n);
            } else if (payload[0] == 0x11) {
                hex_line(++answers == 1 ? "ir_cpp: calibration " : "ir_cpp: calibration_unaligned ", payload, n);
            } else {
                hex_line("ir_cpp: FAIL other ", payload, n);
            }
        }
    }
    check("ir_cpp: statuses", statuses);
    check("ir_cpp: calibration_answers", answers);
    /* the 49 periods between the first and the last packet last 49 * 20 ms = 980 ms (262,749,618 ticks),
     * within 5 % on either clock */
    const u64 elapsed = last_tick - first_tick;
    check("ir_cpp: period_within_5_percent", elapsed >= 249612137ull && elapsed <= 275886098ull);

    /* disconnect: the packets stop. Packets that were already in the buffer are dropped. */
    const u32 disconnect[1] = {0x00090000};
    rc_line("ir_cpp: disconnect_rc", ir_call(disconnect, 1));
    u8 scratch[80];
    while (ir_take(scratch, sizeof(scratch))) {}
    svcClearEvent(ir_recv);
    check("ir_cpp: quiet_after_disconnect", svcWaitSynchronization(ir_recv, 100 * ONE_MS) != 0 && ir_rd(IR_INFO + 8) == 0);
    rc_line("ir_cpp: send_after_disconnect_rc", ir_send(polling, 3));

    const u32 finalize[1] = {0x00020000};
    rc_line("ir_cpp: finalize_rc", ir_call(finalize, 1));
    svcCloseHandle(ir_conn); svcCloseHandle(ir_recv); svcCloseHandle(ir_sent);
    svcCloseHandle(ir_block); svcCloseHandle(ir_h);
    finish("ir_cpp: done");
}
