/* The clock a program sees (N0-35): the system tick and the shared page's two date records (offsets
 * 0x20 and 0x40: the date in milliseconds since 1900, the tick it was written at, the ticks per
 * second and the tick offset, each a u64), printed as hex words. The reference runner's --init-time
 * moves the date and nothing else: the tick, the update ticks and the rest of the page stay what they
 * are without the option, so a test runs this program twice and compares the lines. No game code. */
#include "svc.h"

#define SHARED_PAGE 0x1FF81000u

static u32 rd(u32 offset) { return *(volatile u32 *)(SHARED_PAGE + offset); }

static void record(const char *name, u32 offset) {
    char label[48];
    u32 n = 0;
    for (const char *s = "time_probe: "; *s; s++) label[n++] = *s;
    for (const char *s = name; *s; s++) label[n++] = *s;
    label[n++] = '_';
    const char *fields[4] = {"date", "update_tick", "ticks_per_second", "tick_offset"};
    for (u32 f = 0; f < 4; f++) {
        u32 k = n;
        for (const char *s = fields[f]; *s; s++) label[k++] = *s;
        label[k++] = '_'; label[k++] = 'h'; label[k++] = 'i'; label[k] = 0;
        check(label, rd(offset + 8 * f + 4));
        label[k - 2] = 'l'; label[k - 1] = 'o';
        check(label, rd(offset + 8 * f));
    }
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("time_probe: start");
    u64 tick = svcGetSystemTick();
    check("time_probe: tick_hi", (u32)(tick >> 32));
    check("time_probe: tick_lo", (u32)tick);
    /* the first update of the date is an event at time 0 (a record is written, the counter counts) */
    while (rd(0) == 0) svcSleepThread(ONE_MS);
    check("time_probe: date_time_counter", rd(0));
    record("record0", 0x20);
    record("record1", 0x40);
    finish("time_probe: done");
}
