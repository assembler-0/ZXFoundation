/// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtsurf.c — user-op surface coverage suite

#include <sys/inc64/nuclbase.h>

typedef struct {
    nb_u8  class_id;   // 0=idle 1=fair 2=rt 3=deadline
    nb_u8  rt_mode;
    nb_u8  rt_priority;
    nb_u8  _pad0[6];
    nb_u64 runtime_ns;
    nb_u64 deadline_ns;
    nb_u64 period_ns;
    nb_u64 allowed_cpus;
    nb_u64 allowed_nodes;
    nb_u8  allow_smt;
    nb_u8  isolation_sensitive;
    nb_u8  user_visible;
    nb_u8  _pad1;
} zx_sched_policy;
_Static_assert(sizeof(zx_sched_policy) == 64, "sched_policy ABI mirror");

// signal_kind user1.
#define ZX_SGN_USER1 6
#define ZX_PAGE      4096ULL

static volatile nb_u64 surf_opts;

static void put(const char *s) {
    nb_u64 len = 0;
    while (s[len] != '\0') len++;
    nb_console_write(s, len);
}

static char line_buf[192];
static nb_u64 line_len;

static void line_reset(void) { line_len = 0; }

static void line_str(const char *s) {
    while (*s != '\0' && line_len < sizeof(line_buf) - 1)
        line_buf[line_len++] = *s++;
}

static void line_end(void) {
    if (line_len < sizeof(line_buf)) line_buf[line_len++] = '\n';
    nb_console_write(line_buf, line_len);
    line_len = 0;
}

static void fail(const char *why) {
    line_reset();
    line_str("ZXT SURF FAIL ");
    line_str(why);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}

extern void zxt_wentry0(void);

// Worker: exits with a code the main strand joins on.
static void surf_worker(void) {
    nb_dom_strand_exit(0xC0DEULL, 0, 0, 0, 0);
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}

// common.S references these (every suite links it).
nb_u64 nb_source_token;
nb_u64 zxt_admin_cap;
nb_u64 zxt_worker_table[4];

static volatile nb_u64 consult_mode;    // 0 allow, 1 deny, 2 never, 3 exit
static volatile nb_u64 consult_served;
static nb_u64 consult_ep;

typedef struct {
    nb_u64 payload;
    nb_u64 capabilities;
    nb_u64 grants;
    nb_u32 length;
    nb_u16 capability_count;
    nb_u16 grant_count;
} sxx_msg;

extern void zxt_wentry1(void);

static void surf_consult_worker(void) {
    static nb_u8 rbuf[64];
    sxx_msg res;
    res.capabilities = 0;
    res.grants = 0;
    for (;;) {
        const nb_u64 r = nb_ipc_receive(consult_ep, (nb_u64)rbuf,
                                        (nb_u64)&res, 2, 0);
        if (r == NB_ERR) {
            for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
        }
        consult_served++;
        if (consult_mode == 3) nb_dom_strand_exit(0, 0, 0, 0, 0);
        nb_ipc_reply(consult_ep, r >> 32,
                     consult_mode == 1 ? 0xE1ULL : 0ULL, 0, 0);
        if (consult_mode == 3) nb_dom_strand_exit(0, 0, 0, 0, 0);
    }
}

void zxt_main(void) {
    nb_u64 r = nb_dom_info(0);
    if (r == NB_ERR) fail("dom_info");
    const nb_u64 dom = r & 0xFFFFFFFFull;
    zxt_admin_cap = nb_source_token;

    zx_sched_policy pol;
    __builtin_memset(&pol, 0, sizeof(pol));
    pol.class_id = 1;  // fair
    pol.allow_smt = 1;
    if (nb_dom_strand_set_policy(zxt_admin_cap, 0, (nb_u64)(nb_uptr)&pol,
                                 0, 0) == NB_ERR) fail("set_policy");
    r = nb_dom_strand_query(zxt_admin_cap, 0, 0, 0, 0);
    if (r == NB_ERR) fail("strand_query");
    if (((r >> 8) & 0xFULL) != 1) fail("policy_class");
    if (nb_dom_strand_set_affin(zxt_admin_cap, 0, 0x3ULL, 0, 0) == NB_ERR)
        fail("set_affin");
    if (nb_dom_strand_set_affin(zxt_admin_cap, 0, 0x1ULL, 0, 0) == NB_ERR)
        fail("set_affin_1");
    // Negative: policy updates on a non-primary strand id are rejected.
    if (nb_dom_strand_set_policy(zxt_admin_cap, 7, (nb_u64)(nb_uptr)&pol,
                                 0, 0) != NB_ERR) fail("set_policy_neg");

    zxt_worker_table[0] = (nb_u64)(nb_uptr)surf_worker;
    const nb_u64 sid = nb_dom_strand_create(zxt_admin_cap, 0, dom,
                                            (nb_u64)(nb_uptr)zxt_wentry0, 0);
    if (sid == NB_ERR) fail("strand_create");
    if (nb_dom_strand_join(sid, 0, 0, 0, 0) != 0xC0DEULL) fail("join");

    const nb_u64 va = nb_mem_alloc(8 * ZX_PAGE, 0, 0, 0, 0);
    if (va == NB_ERR || (va & (ZX_PAGE - 1)) != 0) fail("alloc");
    *(volatile nb_u64*)va = 0xFEEDFACEFEEDFACEULL;
    if (*(volatile nb_u64*)va != 0xFEEDFACEFEEDFACEULL) fail("rw_readback");
    surf_opts++;
    if (nb_mem_protect(va, ZX_PAGE, 1, 0, 0) == NB_ERR) fail("protect_ro");
    if (nb_mem_protect(va + ZX_PAGE, 3 * ZX_PAGE, 3, 0, 0) == NB_ERR)
        fail("protect_rw");
    if (nb_mem_unmap(va + 4 * ZX_PAGE, 4 * ZX_PAGE, 0, 0, 0) == NB_ERR)
        fail("unmap_half");
    if (nb_mem_unmap(va, 4 * ZX_PAGE, 0, 0, 0) == NB_ERR) fail("unmap");
    if (nb_mem_free(0, ZX_PAGE, 0, 0, 0) != NB_ERR) fail("free_neg");
    if (nb_mem_map(0xBAD0ULL, va, 0, ZX_PAGE, 0) != NB_ERR) fail("map_neg");

    const nb_u64 t2 = nb_cap_acquire(zxt_admin_cap, 0x00800000ULL, 0, 0, 0);
    if (t2 == NB_ERR) fail("cap_acquire");
    if (nb_cap_validate(t2, 0x00800000ULL, 0, 0, 0) == NB_ERR) fail("cap_validate");
    if (nb_cap_revoke(t2, 0, 0, 0, 0) == NB_ERR) fail("cap_revoke");
    if (nb_cap_validate(t2, 0x00800000ULL, 0, 0, 0) != NB_ERR) fail("cap_revoked");

    if (nb_sgn_mask_set((nb_u64)1 << ZX_SGN_USER1, 0, 0, 0, 0) == NB_ERR)
        fail("mask_bl");
    if (nb_sgn_send(dom, ZX_SGN_USER1, 0x5EFULL, 0, 0) == NB_ERR) fail("send_u1");
    {
        int i;
        for (i = 0; i < 64; i++) {
            if (nb_sgn_poll(0, 0, 0, 0, 0) == ((nb_u64)1 << ZX_SGN_USER1)) break;
            nb_sys_yield(0, 0, 0, 0, 0);
        }
        if (nb_sgn_poll(0, 0, 0, 0, 0) != ((nb_u64)1 << ZX_SGN_USER1))
            fail("poll_users1");
        (void)i;
    }
    if (nb_sgn_wait(0, 0, 0, 0, 0) != ((nb_u64)1 << ZX_SGN_USER1)) fail("wait");
    if (nb_sgn_mask_get(0, 0, 0, 0, 0) != ((nb_u64)1 << ZX_SGN_USER1)) fail("mask_get");
    if (nb_sgn_mask_set(0, 0, 0, 0, 0) == NB_ERR) fail("mask_clr");
    nb_sgn_wait(0, 0, 0, 0, 0);  // second wait rides any dispatch
    if (nb_sgn_mask_set((nb_u64)1 << ZX_SGN_USER1, 0, 0, 0, 0) == NB_ERR)
        fail("mask_bl2");
    if (nb_sgn_send(dom, ZX_SGN_USER1, 0x5EFULL, 0, 0) == NB_ERR) fail("send_u1b");
    {
        int i;
        for (i = 0; i < 64; i++) {
            if (nb_sgn_poll(0, 0, 0, 0, 0) == ((nb_u64)1 << ZX_SGN_USER1)) break;
            nb_sys_yield(0, 0, 0, 0, 0);
        }
        if (nb_sgn_poll(0, 0, 0, 0, 0) != ((nb_u64)1 << ZX_SGN_USER1))
            fail("poll_users1b");
        (void)i;
    }
    if (nb_sgn_mask_set(0, 0, 0, 0, 0) == NB_ERR) fail("mask_clr2");
    nb_sgn_wait(0, 0, 0, 0, 0);
    if (nb_sgn_ack((nb_u64)1 << ZX_SGN_USER1, 0, 0, 0, 0) == NB_ERR) fail("ack");
    if (nb_sgn_poll(0, 0, 0, 0, 0) != 0) fail("poll_empty");

    const nb_u64 t0 = nb_sys_now(0, 0, 0, 0, 0);
    if (t0 == NB_ERR) fail("now");
    const nb_u64 dead = nb_sys_deadline(10000000ULL, 0, 0, 0, 0);
    if (dead == NB_ERR || dead <= t0 || dead > t0 + 1000000000ULL) fail("deadline");
    if (nb_sys_sleep(2000000ULL, 0, 0, 0, 0) == NB_ERR) fail("sleep");
    const nb_u64 t1 = nb_sys_now(0, 0, 0, 0, 0);
    if (t1 < t0 + 2000000ULL) fail("slept_long");
    surf_opts++;
    nb_mem_free(0, ZX_PAGE, 0, 0, 0);  // deliberate failure
    const nb_u64 le = nb_last_error(0, 0, 0, 0, 0);
    if (le == NB_ERR) fail("last_error");

    static char svcname[16] = "zxtsurf.svc";
    const nb_u64 sidx = nb_svc_register(zxt_admin_cap, (nb_u64)(nb_uptr)svcname,
                                        0x1F4ULL, 0, 0);
    if (sidx == NB_ERR) fail("svc_register");
    if (nb_svc_lookup((nb_u64)(nb_uptr)svcname, 0, 0, 0, 0) != 0x1F4ULL)
        fail("svc_lookup");
    if (nb_svc_unregister(sidx, 0, 0, 0, 0) == NB_ERR) fail("svc_unregister");
    if (nb_svc_lookup((nb_u64)(nb_uptr)svcname, 0, 0, 0, 0) != NB_ERR)
        fail("svc_gone");

    const nb_u64 sv = nb_mem_alloc(4 * ZX_PAGE, 0, 0, 0, 0);
    if (sv == NB_ERR) fail("shm_alloc");
    const nb_u64 dv1 = nb_mem_alloc(4 * ZX_PAGE, 0, 0, 0, 0);
    const nb_u64 dv2 = nb_mem_alloc(4 * ZX_PAGE, 0, 0, 0, 0);
    if (dv1 == NB_ERR || dv2 == NB_ERR) fail("shm_mpi_alloc");
    *(volatile nb_u64*)sv = 0x5AFEBABE5AFEBABEULL;
    for (nb_u64 i = 0; i < 4; i++)
        *(volatile nb_u64*)(sv + i * ZX_PAGE) = 0x5AFEBABE164E0ULL;
    const nb_u64 gt = nb_shm_export(sv, 4 * ZX_PAGE, dom, 0, 0);
    if (gt == NB_ERR) fail("shm_export");
    surf_opts++;
    const nb_u64 bind = nb_shm_import(gt, dv1, 4 * ZX_PAGE, 0, 0);
    if (bind == NB_ERR) fail("shm_import");
    const nb_u64 mapr = nb_shm_map(gt, dv2, 4 * ZX_PAGE, 0, 0);
    if (mapr == NB_ERR) fail("shm_map");
    if ((bind & 0xFFFFFFFFULL) == 0) fail("bind_id");
    if ((mapr & 0xFFFFFFFFULL) == 0) fail("map_id");
    if (bind == mapr) fail("bind_same");
    surf_opts++;
    // Teardown: portals of the bindings, then token, then regions.
    const nb_u64 bpid = bind >> 32;
    const nb_u64 mpid = mapr >> 32;
    if (bpid != 0 && nb_portal_destroy(zxt_admin_cap, bpid, 0, 0, 0) == NB_ERR)
        fail("portal_destroy_b");
    if (mpid != 0 && nb_portal_destroy(zxt_admin_cap, mpid, 0, 0, 0) == NB_ERR)
        fail("portal_destroy_m");
    if (nb_cap_release(gt, 0, 0, 0, 0) == NB_ERR) fail("shm_release");
    if (nb_mem_unmap(sv, 4 * ZX_PAGE, 0, 0, 0) == NB_ERR) fail("shm_unmap");
    if (nb_mem_unmap(dv1, 4 * ZX_PAGE, 0, 0, 0) == NB_ERR) fail("shm_unmap1");
    if (nb_mem_unmap(dv2, 4 * ZX_PAGE, 0, 0, 0) == NB_ERR) fail("shm_unmap2");

    const nb_u64 ep = nb_ipc_endpoint_create(dom, 0, 0, 0, 0);
    if (ep == NB_ERR) fail("ep_create");
    const nb_u64 pb = nb_policy_register(4 /*services*/, (nb_u32)ep, 0, 0, 0);
    if (pb == NB_ERR) fail("policy_register");
    if (nb_policy_unregister(4, 0, 0, 0, 0) == NB_ERR) fail("policy_unregister");
    if (nb_ipc_endpoint_destroy(ep, 0, 0, 0, 0) == NB_ERR) fail("ep_destroy");

    const nb_u64 pv = nb_mem_alloc(2 * ZX_PAGE, 0, 0, 0, 0);
    if (pv == NB_ERR) fail("portal_alloc");
    *(volatile nb_u64*)pv = 0x50AD0000ULL;             // residency: page 0
    *(volatile nb_u64*)(pv + ZX_PAGE) = 0x50AD1000ULL; // residency: page 1
    const nb_u64 pid = nb_portal_create(zxt_admin_cap, dom, pv,
                                        pv + 2 * ZX_PAGE, 0);
    if (pid == NB_ERR) fail("portal_create");
    surf_opts++;
    if (nb_portal_destroy(zxt_admin_cap, pid, 0, 0, 0) == NB_ERR)
        fail("portal_destroy");
    if (nb_mem_unmap(pv, 2 * ZX_PAGE, 0, 0, 0) == NB_ERR) fail("portal_unmap");

    {
        static nb_u64 gids[8];
        static nb_u64 pc0[8];
        static nb_u64 pc2[8];
        nb_u64 entry = (nb_u64)(nb_uptr)surf_worker;
        for (int i = 0; i < 8; i++) {
            gids[i] = nb_gate_create(zxt_admin_cap, 0, 0, dom, entry);
            if (gids[i] == NB_ERR) fail("gate_mint");
            pc0[i] = nb_gate_query(zxt_admin_cap, gids[i]);
            // PC number 0 is legal (the init runner asserts only NB_ERR).
            if (pc0[i] == NB_ERR) fail("gate_query0");
        }
        for (int i = 0; i < 8; i++) {
            if (nb_gate_destroy(zxt_admin_cap, gids[i]) == NB_ERR)
                fail("gate_destroy");
        }
        for (int i = 0; i < 8; i++) {
            gids[i] = nb_gate_create(zxt_admin_cap, 0, 0, dom, entry);
            if (gids[i] == NB_ERR) fail("gate_remint");
            pc2[i] = nb_gate_query(zxt_admin_cap, gids[i]);
            if (pc2[i] == NB_ERR) fail("gate_query2");
        }
        for (int i = 0; i < 8; i++) {
            if (nb_gate_destroy(zxt_admin_cap, gids[i]) == NB_ERR)
                fail("gate_destroy2");
        }
        surf_opts++;
    }

    // ---- policy plane: the consult broker (allow / deny / timeout) ----
    // The planted server (this domain's worker strand) answers the kernel's
    // decision request.  Leg C exercises RC-08.4's rule: no reply degrades
    // to the kernel default after the deadline — never a wedge.
    {
        consult_ep = nb_ipc_endpoint_create(dom, 0, 0, 0, 0);
        if (consult_ep == NB_ERR) fail("consult_ep");
        zxt_worker_table[1] = (nb_u64)(nb_uptr)surf_consult_worker;
        if (nb_dom_strand_create(zxt_admin_cap, 0, dom,
                                 (nb_u64)(nb_uptr)zxt_wentry1, 0)
                == NB_ERR) {
            fail("consult_worker");
        }
        // One server per kind: services, answered by our endpoint.
        if (nb_policy_register(4 /*services*/, consult_ep, 0, 0, 0)
                == NB_ERR) {
            fail("consult_server");
        }

        static char csvc[16] = "zxtsurf.consult";
        // Leg A — allow: worker replies 0; registration proceeds.
        consult_mode = 0;
        const nb_u64 creg = nb_svc_register(zxt_admin_cap,
                                            (nb_u64)(nb_uptr)csvc,
                                            0x2F4ULL, 0, 0);
        if (creg == NB_ERR) fail("consult_allow");
        if (nb_svc_unregister(creg, 0, 0, 0, 0) == NB_ERR) fail("consult_unreg2");

        // Leg B — deny: worker replies a nonzero status; the operation must
        // fail with it (a server can only deny).
        consult_mode = 1;
        if (nb_svc_register(zxt_admin_cap, (nb_u64)(nb_uptr)csvc,
                             0x3F4ULL, 0, 0) != NB_ERR) {
            fail("consult_deny");
        }
        if (nb_last_error(0, 0, 0, 0, 0) == NB_ERR) fail("consult_code");

        // Leg C — never replies: bounded wait, then the kernel default.
        consult_mode = 2;
        const nb_u64 c0 = nb_sys_now(0, 0, 0, 0, 0);
        const nb_u64 creg2 = nb_svc_register(zxt_admin_cap,
                                             (nb_u64)(nb_uptr)csvc,
                                             0x4F4ULL, 0, 0);
        const nb_u64 c1 = nb_sys_now(0, 0, 0, 0, 0);
        if (creg2 == NB_ERR) fail("consult_timeout");
        if (c1 < c0 + 90000000ULL) fail("consult_early");
        if (nb_svc_unregister(creg2, 0, 0, 0, 0) == NB_ERR)
            fail("consult_unreg3");
        if (nb_policy_unregister(4, 0, 0, 0, 0) == NB_ERR) fail("server_unreg");
        if (nb_ipc_endpoint_destroy(consult_ep, 0, 0, 0, 0) == NB_ERR)
            fail("consult_ep_destroy");
        // The worker is parked in a blocked receive on a destroyed endpoint;
        // its strand is reaped with the domain at exit (quiesce organ).
        surf_opts++;
    }

    {
        const nb_u64 it = nb_io_bind(0xFFFEULL, 0, 0, 0, 0);
        if (it != NB_ERR && nb_io_unbind(it, 0, 0, 0, 0) == NB_ERR)
            fail("io_unbind_ret");
    }
    if (nb_io_unbind(0xDEAD0000ULL, 0, 0, 0, 0) != NB_ERR) fail("io_unbind_neg");

    line_reset();
    line_str("ZXT SURF PASS policy+affin+join+mem+cap+sgn+deadline+sleep+lasterr+svc+shm+policy+portal+gates+consult");
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}
