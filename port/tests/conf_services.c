/* Kernel conformance: one simple request each to cfg:u, ptm:u and ac:u, through srv:. The
 * values are Azahar's defaults for a fresh user directory. Our own program, no game code. */
#include "svc.h"

static u8 block[0x40] __attribute__((aligned(8)));

static Handle service(Handle srv, const char *name, u32 len) {
    u32 reply[4];
    Handle h = srv_get(srv, name, len, 1, reply);
    check(name, h != 0 && reply[1] == 0);
    return h;
}

static void show(const char *what, s32 rc) {
    check(what, (u32)rc);
}

/* cfg:u GetConfig (cmd 1): size, block id, then a write-mapped buffer */
static s32 get_config(Handle cfg, u32 id, u32 size) {
    for (u32 i = 0; i < sizeof(block); i++) block[i] = 0xEE;
    const u32 w[5] = {0x00010082, size, id, (size << 4) | 0xC, (u32)block};
    s32 rc = request(cfg, w, 5);
    return rc != 0 ? rc : (s32)cmdbuf()[1];
}

static void cfg_checks(Handle cfg) {
    show("cfg.language.rc", get_config(cfg, 0x000A0002, 1));
    check("cfg.language", block[0]);
    show("cfg.country_info.rc", get_config(cfg, 0x000B0000, 4));
    check("cfg.country_info.byte0", block[0]);
    check("cfg.country_info.byte1", block[1]);
    check("cfg.country_info.byte2", block[2]);
    check("cfg.country_info.byte3", block[3]);
    show("cfg.user_name.rc", get_config(cfg, 0x000A0000, 0x1C));
    u32 len = 0;
    while (len < 10 && (block[len * 2] | block[len * 2 + 1])) len++;
    check("cfg.user_name.length", len);
    show("cfg.unknown_block.rc", get_config(cfg, 0x00FF0000, 4));
    show("cfg.wrong_size.rc", get_config(cfg, 0x000A0002, 2));

    const u32 region[1] = {0x00020000};
    show("cfg.region.send", request(cfg, region, 1));
    check("cfg.region.header", cmdbuf()[0]);
    check("cfg.region.result", cmdbuf()[1]);
    check("cfg.region", cmdbuf()[2] & 0xFF);
    const u32 model[1] = {0x00050000};
    show("cfg.model.send", request(cfg, model, 1));
    check("cfg.model.result", cmdbuf()[1]);
    check("cfg.model", cmdbuf()[2] & 0xFF);
    const u32 unknown[1] = {0x00FF0000};
    show("cfg.unknown_command.send", request(cfg, unknown, 1));
    check("cfg.unknown_command.header", cmdbuf()[0]);
}

static void simple(Handle h, const char *name, u32 header, int words) {
    char full[64];
    u32 n = 0;
    for (u32 i = 0; name[i]; i++) full[n++] = name[i];
    const char *sfx[4] = {".send", ".header", ".result", ""};
    const u32 hdr[1] = {header};
    s32 rc = request(h, hdr, 1);
    u32 *cb = cmdbuf();
    const u32 vals[4] = {(u32)rc, cb[0], cb[1], cb[2] & (words == 1 ? 0xFF : 0xFFFFFFFF)};
    for (int k = 0; k < 4; k++) {
        if (k == 3 && words == 0) break;
        u32 m = n;
        for (u32 i = 0; sfx[k][i]; i++) full[m++] = sfx[k][i];
        full[m] = 0;
        check(full, vals[k]);
    }
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_services: start");
    Handle srv = srv_connect(1);
    check("srv.handle_set", srv != 0);

    Handle cfg = service(srv, "cfg:u", 5);
    cfg_checks(cfg);

    Handle ptm = service(srv, "ptm:u", 5);
    simple(ptm, "ptm.battery_level", 0x00070000, 1);
    simple(ptm, "ptm.battery_charging", 0x00080000, 1);
    simple(ptm, "ptm.adapter_state", 0x00050000, 1);
    simple(ptm, "ptm.shell_state", 0x00060000, 1);
    simple(ptm, "ptm.unknown_command", 0x00FF0000, 0);

    Handle ac = service(srv, "ac:u", 4);
    simple(ac, "ac.status", 0x000C0000, 1);
    simple(ac, "ac.wifi_status", 0x000D0000, 1);
    simple(ac, "ac.is_connected", 0x003E0000, 1);
    simple(ac, "ac.unknown_command", 0x00FF0000, 0);

    svcCloseHandle(cfg);
    svcCloseHandle(ptm);
    svcCloseHandle(ac);
    svcCloseHandle(srv);
    finish("conf_services: done");
}
