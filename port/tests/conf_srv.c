/* Kernel conformance: the srv: port. RegisterClient, GetServiceHandle for services that exist
 * and for one that does not (flags = 1: don't wait; with bit 0 clear Azahar would wait
 * forever), EnableNotification and the result codes. Our own program, no game code. */
#include "svc.h"

static void show(const char *name, const u32 *reply) {
    char buf[48];
    u32 n = 0;
    for (u32 i = 0; name[i]; i++) buf[n++] = name[i];
    const char *sfx[3] = {".header", ".result", ".handle_set"};
    const u32 vals[3] = {reply[0], reply[1], reply[1] == 0 && reply[3] != 0};
    char full[64];
    for (int k = 0; k < 3; k++) {
        u32 m = n;
        for (int i = 0; i < (int)n; i++) full[i] = buf[i];
        for (u32 i = 0; sfx[k][i]; i++) full[m++] = sfx[k][i];
        full[m] = 0;
        check(full, vals[k]);
    }
}

void _start(void) __attribute__((section(".text.start")));
void _start(void) {
    print("conf_srv: start");
    Handle h = 0;
    check("port.bad_name_rc", (u32)svcConnectToPort(&h, "nope:"));
    Handle srv = 0;
    check("port.srv_rc", (u32)svcConnectToPort(&srv, "srv:"));
    check("port.srv_handle_set", srv != 0);

    u32 reply[4];

    /* GetServiceHandle before RegisterClient */
    Handle early = srv_get(srv, "cfg:u", 5, 0, reply);
    show("early_get", reply);
    if (early) svcCloseHandle(early);

    const u32 reg[3] = {0x00010002, 0x20, 0};
    check("register.send_rc", (u32)request(srv, reg, 3));
    check("register.header", cmdbuf()[0]);
    check("register.result", cmdbuf()[1]);

    Handle cfg = srv_get(srv, "cfg:u", 5, 0, reply);
    show("cfg_u", reply);
    Handle ptm = srv_get(srv, "ptm:u", 5, 0, reply);
    show("ptm_u", reply);
    Handle cfg2 = srv_get(srv, "cfg:u", 5, 0, reply);
    show("cfg_u_again", reply);
    check("cfg_u.handles_differ", cfg != cfg2 && cfg != 0);
    check("cfg_u.same_handle_as_ptm", cfg == ptm);

    /* unknown service: don't wait */
    Handle none = srv_get(srv, "nope:u", 6, 1, reply);
    show("unknown_dont_wait", reply);
    check("unknown.handle_zero", none == 0);
    show("cfg_u_len_short", (srv_get(srv, "cfg:u", 3, 1, reply), reply));
    show("cfg_u_len_long", (srv_get(srv, "cfg:u", 9, 0, reply), reply));
    show("cfg_u_len_zero", (srv_get(srv, "cfg:u", 0, 1, reply), reply));
    show("cfg_u_len_8", (srv_get(srv, "cfg:u", 8, 0, reply), reply));
    show("cfg_u_flags_1", (srv_get(srv, "cfg:u", 5, 1, reply), reply));
    show("cfg_u_flags_2", (srv_get(srv, "cfg:u", 5, 2, reply), reply));
    show("err_f", (srv_get(srv, "err:f", 5, 1, reply), reply));
    show("srv_itself", (srv_get(srv, "srv:", 4, 1, reply), reply));

    /* EnableNotification */
    const u32 en[1] = {0x00020000};
    check("notify.send_rc", (u32)request(srv, en, 1));
    u32 *cb = cmdbuf();
    check("notify.header", cb[0]);
    check("notify.result", cb[1]);
    check("notify.handle_set", cb[3] != 0);
    Handle sem = cb[3];
    check("notify.wait0", (u32)svcWaitSynchronization(sem, 0));
    check("notify.close", (u32)svcCloseHandle(sem));

    /* a command srv: does not have */
    const u32 bad[1] = {0x00FF0000};
    check("unknown_command.send_rc", (u32)request(srv, bad, 1));
    check("unknown_command.header", cmdbuf()[0]);
    check("unknown_command.result", cmdbuf()[1]);

    /* a second connection that never registers */
    Handle srv2 = srv_connect(0);
    check("second_session.different_handle", srv2 != 0 && srv2 != srv);
    srv_get(srv2, "ptm:u", 5, 0, reply);
    show("second_session_get", reply);

    check("close.srv", (u32)svcCloseHandle(srv));
    check("close.srv2", (u32)svcCloseHandle(srv2));
    check("send_on_closed_rc", (u32)request(srv, en, 1));
    if (cfg) svcCloseHandle(cfg);
    if (cfg2) svcCloseHandle(cfg2);
    if (ptm) svcCloseHandle(ptm);
    finish("conf_srv: done");
}
