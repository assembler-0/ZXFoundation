/// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtpcss.c — PC-ss suite (callee side)

#include <sys/inc64/nuclbase.h>

extern void zxt_rpc(void);
extern void zxt_defer_entry(void);
extern void zxt_wentry0(void);
extern void zxt_wentry1(void);
extern nb_u64 nest_storm(nb_u64 depth, nb_u64 seed);

nb_u64 nb_source_token;
nb_u64 zxt_admin_cap;

// common.S worker trampoline table
nb_u64 zxt_worker_table[4];

// nest_storm bookkeeping (asm)
nb_u64 nest_mark[64];
nb_u64 nest_depth_total;
nb_u64 nest_fail_level;

// Worker stats — the defer server's reply carries a snapshot.
volatile nb_u64 nest_iters;
volatile nb_u64 nest_fails;

static nb_u64 my_dom_id;

static void put(const char *s) {
    nb_u64 len = 0;
    while (s[len] != '\0') len++;
    nb_console_write(s, len);
}

static void defer_server_main(void) {
    for (;;) {
        // Reply payload: r0 = (nest_iters << 32) | nest_fails,
        // r1 = 0, r15 = status 0.  The kernel writes these into the
        // parked caller's state entry.
        nb_u64 iters = nest_iters;
        nb_u64 fails = nest_fails;
        nb_gate_resume((iters << 32) | (fails & 0xFFFFFFFFull), 0, 0, 0, 0);
    }
}

static void storm_worker_main(void) {
    nb_u64 seed = 0xBEEF0000ull + my_dom_id;
    for (;;) {
        for (int i = 0; i < 4; i++) {
            nb_u64 r = nest_storm(40, seed + i);
            if (r != 0) nest_fails++;
            nest_iters++;
        }
        nb_sys_yield(0, 0, 0, 0, 0);
    }
}

void zxt_main(void) {
    zxt_admin_cap = nb_source_token;
    put("zxt pcss: up\n");

    nb_u64 r = nb_dom_info(0);
    if (r == NB_ERR) {
        nb_console_write("ZXT PCSS FAIL dom_info\n", 23);
        nb_dom_exit();
        for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
    }
    my_dom_id = r & 0xFFFFFFFFull;

    // Per-domain service names embed the assigned domain id; the
    // orchestrator builds the same names from its dom_create results.
    static char rpc_name[16] = "ZXT.PCSS.";
    static char def_name[16] = "ZXT.PCSSD.";
    rpc_name[9] = (char)('0' + (int)(my_dom_id % 10u));
    def_name[10] = (char)('0' + (int)(my_dom_id % 10u));
    nb_svc_register(zxt_admin_cap, (nb_u64) rpc_name,
                    (nb_u64) (nb_uptr) zxt_rpc, 0, 0);
    nb_svc_register(zxt_admin_cap, (nb_u64) def_name,
                    (nb_u64) (nb_uptr) zxt_defer_entry, 0, 0);

    zxt_worker_table[0] = (nb_u64) (nb_uptr) defer_server_main;
    zxt_worker_table[1] = (nb_u64) (nb_uptr) storm_worker_main;
    r = nb_dom_strand_create(zxt_admin_cap, 0, my_dom_id,
                             (nb_u64) (nb_uptr) zxt_wentry0, 0);
    if (r == NB_ERR) {
        nb_console_write("ZXT PCSS FAIL defer spawn\n", 26);
        nb_dom_exit();
    }
    r = nb_dom_strand_create(zxt_admin_cap, 0, my_dom_id,
                             (nb_u64) (nb_uptr) zxt_wentry1, 0);
    if (r == NB_ERR) {
        nb_console_write("ZXT PCSS FAIL nest spawn\n", 25);
        nb_dom_exit();
    }

    // The orchestrator drives this suite; stay runnable as load.
    for (;;) {
        nb_sys_yield(0, 0, 0, 0, 0);
    }
}
