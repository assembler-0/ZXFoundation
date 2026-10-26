// SPDX-License-Identifier: Apache-2.0
/// @file user/init/init.c — centralized test runner.
///
/// Boots the DASD driver, then sequences the VTOC-packed test suites
/// (one binary per subsystem, each in its own domain):
///   - ZXTPCSS: the runner drives the caller side of the PC-ss suite —
///     gate RPC storm with data verification, depth-40 BAKR/EREG/ESTA/
///     MSTA nesting on its own linkage stack, deferred-gate parks with
///     worker stat snapshots — against three callee domains;
///   - ZXTSCHED, ZXTDOM, ZXTIPC: fire-and-forget suites that verify
///     themselves and print "ZXT <NAME> PASS/FAIL ..." verdict lines.
/// A missing verdict line is a failure; the run's verdict is the set
/// of lines on the console, closed by the runner's banner.

#include <sys/inc64/nuclbase.h>

extern void pcss_rpc(nb_u64 pcnum, nb_u64 nonce, nb_u64 salt,
                     nb_u64 seq, nb_u64 k1);
extern nb_u64 nest_storm(nb_u64 depth, nb_u64 seed);

nb_u64 pcss_reply_r0;
nb_u64 pcss_reply_r1;
nb_u64 pcss_back_r2;
nb_u64 pcss_back_r3;
nb_u64 pcss_back_r4;
nb_u64 pcss_back_r5;
nb_u64 pcss_back_r6;
nb_u64 pcss_back_r15;

nb_u64 nest_mark[64];
nb_u64 nest_depth_total;
nb_u64 nest_fail_level;

nb_u64 nb_source_token;
nb_u64 admin_cap;
nb_u64 domain_id;

// dasd.c
int dasd_init(nb_u64 dev_no);
int dasd_read_dataset(const char *dsname, nb_u8 *dst, nb_u32 max_blocks);

static nb_u8 suite_image[64 * 4096] __attribute__((aligned(4096)));

static void put(const char *s) {
    nb_u64 len = 0;
    while (s[len] != '\0') len++;
    nb_console_write(s, len);
}

static void put_u64(nb_u64 v) {
    char buf[21];
    unsigned n = 0;
    char *p = buf + 20;
    *p = '\0';
    do {
        *--p = '0' + (unsigned)(v % 10);
        v /= 10;
        n++;
    } while (v != 0);
    nb_console_write(p, n);
}

// One console_write per log line: every fragment is a separate kernel
// printk, so multi-part puts flood the console with partial lines.
static char line_buf[192];
static nb_u64 line_len;

static void line_reset(void) { line_len = 0; }

static void line_str(const char *s) {
    while (*s != '\0' && line_len < sizeof(line_buf) - 1)
        line_buf[line_len++] = *s++;
}

static void line_u64(nb_u64 v) {
    char tmp[20];
    nb_u64 n = 0;
    do {
        tmp[n++] = '0' + (char)(v % 10);
        v /= 10;
    } while (v != 0);
    while (n != 0 && line_len < sizeof(line_buf) - 1)
        line_buf[line_len++] = tmp[--n];
}

static void line_end(void) {
    if (line_len < sizeof(line_buf)) line_buf[line_len++] = '\n';
    nb_console_write(line_buf, line_len);
    line_len = 0;
}

#define FAIL() do { line_reset(); line_str("ZXT RUNNER FAIL line="); \
                    line_u64((__LINE__)); line_end(); \
                    nb_dom_exit(); for(;;){} } while (0)

/// @brief C mirror of the zxtpcss callee mix — must agree bit for bit.
static nb_u64 rotl64(nb_u64 v, unsigned s) {
    return (v << s) | (v >> (64u - s));
}

static nb_u64 mix64(nb_u64 a, nb_u64 b, nb_u64 c) {
    nb_u64 x = a ^ rotl64(b, 17u) ^ (c * 0x9E3779B97F4A7C15ull);
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return x;
}

static void smoke(void) {
    nb_u64 r = nb_cap_acquire(nb_source_token, 0x00800000ull, 0, 0, 0);
    if (r == NB_ERR) FAIL();
    admin_cap = r;

    r = nb_mem_alloc(4096, 0, 0, 0, 0);
    if (r == NB_ERR) FAIL();
    volatile nb_u8 *page = (volatile nb_u8 *)r;
    page[0] = 0xA5;
    if (page[0] != 0xA5) FAIL();

    r = nb_dom_info(0);
    if (r == NB_ERR) FAIL();
    domain_id = r & 0xFFFFFFFFull;
}

static int load_suite(const char *dsname) {
    int blocks = dasd_read_dataset(dsname, suite_image, 16);
    if (blocks <= 0) {
        put("ZXT RUNNER: ");
        put(dsname);
        put(" not found\n");
        return 0;
    }
    return blocks;
}

static nb_u64 spawn_suite(const char *name, int blocks) {
    nb_u64 d = nb_dom_create(admin_cap, name);
    if (d == NB_ERR) FAIL();
    if (nb_dom_exec(admin_cap, d, (nb_u64) suite_image,
                    (nb_u64) blocks * 4096ull, 0) == NB_ERR) {
        FAIL();
    }
    return d;
}

/// Wait for a suite domain to exit.  dom_info on a dead domain is
/// expected to fail; if it does not, the yield budget still bounds the
/// wait and the suite's own verdict line remains the record.
static void wait_suite(nb_u64 d, const char *name) {
    for (nb_u64 i = 0; i < 30000; i++) {
        if (nb_dom_info(d) == NB_ERR) return;
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    line_reset();
    line_str("ZXT RUNNER: ");
    line_str(name);
    line_str(" still running");
    line_end();
}

void zx_init_main(void) {
    put("[init] orchestrator up\n");
    smoke();

    if (dasd_init(0x0100ull) < 0) {
        put("[init] dasd_init failed\n");
        FAIL();
    }
    put("[init] dasd bound, window mapped\n");

    // ======================= PC-ss suite =======================
    enum { BENCH_DOMAINS = 3, TRIPS = 50000,
           NEST_DEPTH = 40, NEST_ITERS = 100, DEFERS = 120 };

    int blocks = load_suite("ZXTPCSS");
    if (blocks == 0) FAIL();

    nb_u64 pdom[BENCH_DOMAINS];
    static const char *pnames[BENCH_DOMAINS] = {"pcss0", "pcss1", "pcss2"};
    for (int i = 0; i < BENCH_DOMAINS; i++) {
        pdom[i] = spawn_suite(pnames[i], blocks);
    }

    // Service names embed the assigned domain ids; build them from the
    // dom_create results, not hardcoded numbers.
    char rpc_names[BENCH_DOMAINS][16];
    char def_names[BENCH_DOMAINS][16];
    nb_u64 rpc_entry[BENCH_DOMAINS];
    nb_u64 def_entry[BENCH_DOMAINS];
    for (int i = 0; i < BENCH_DOMAINS; i++) {
        for (int k = 0; k < 16; k++) {
            rpc_names[i][k] = 0;
            def_names[i][k] = 0;
        }
        rpc_names[i][0] = 'Z'; rpc_names[i][1] = 'X'; rpc_names[i][2] = 'T';
        rpc_names[i][3] = '.'; rpc_names[i][4] = 'P'; rpc_names[i][5] = 'C';
        rpc_names[i][6] = 'S'; rpc_names[i][7] = 'S'; rpc_names[i][8] = '.';
        def_names[i][0] = 'Z'; def_names[i][1] = 'X'; def_names[i][2] = 'T';
        def_names[i][3] = '.'; def_names[i][4] = 'P'; def_names[i][5] = 'C';
        def_names[i][6] = 'S'; def_names[i][7] = 'S'; def_names[i][8] = 'D';
        def_names[i][9] = '.';
        rpc_names[i][9] = (char)('0' + (int)(pdom[i] % 10u));
        def_names[i][10] = (char)('0' + (int)(pdom[i] % 10u));
        rpc_entry[i] = 0;
        def_entry[i] = 0;
        for (nb_u64 tries = 0; tries < 400; tries++) {
            if (rpc_entry[i] == 0 || rpc_entry[i] == NB_ERR)
                rpc_entry[i] = nb_svc_lookup((nb_u64) rpc_names[i], 0, 0, 0, 0);
            if (def_entry[i] == 0 || def_entry[i] == NB_ERR)
                def_entry[i] = nb_svc_lookup((nb_u64) def_names[i], 0, 0, 0, 0);
            if (rpc_entry[i] != NB_ERR && rpc_entry[i] != 0
                && def_entry[i] != NB_ERR && def_entry[i] != 0) break;
            nb_sys_yield(0, 0, 0, 0, 0);
        }
        if (rpc_entry[i] == NB_ERR || rpc_entry[i] == 0
            || def_entry[i] == NB_ERR || def_entry[i] == 0) {
            put("ZXT PCSS FAIL services never registered\n");
            FAIL();
        }
    }

    nb_u64 gate[BENCH_DOMAINS];
    nb_u64 dgate[BENCH_DOMAINS];
    for (int i = 0; i < BENCH_DOMAINS; i++) {
        gate[i] = nb_gate_create(admin_cap, 0, 0, pdom[i], rpc_entry[i]);
        if (gate[i] == NB_ERR) FAIL();
        dgate[i] = nb_gate_create(admin_cap, 0, 0, pdom[i], def_entry[i]);
        if (dgate[i] == NB_ERR) FAIL();
    }
    nb_u64 pc[BENCH_DOMAINS];
    nb_u64 dpc[BENCH_DOMAINS];
    for (int i = 0; i < BENCH_DOMAINS; i++) {
        pc[i] = nb_gate_query(admin_cap, gate[i]);
        if (pc[i] == NB_ERR) FAIL();
        dpc[i] = nb_gate_query(admin_cap, dgate[i]);
        if (dpc[i] == NB_ERR) FAIL();
    }

    // --- Phase A: gate RPC storm with data verification ---
    nb_u64 seq = 0;
    nb_u64 rpc_ok = 0;
    for (nb_u64 t = 0; t < TRIPS; t++) {
        for (int i = 0; i < BENCH_DOMAINS; i++) {
            nb_u64 nonce = 0xC0FFEE000000ull + seq;
            nb_u64 salt = (pdom[i] << 48) ^ (seq * 0x9E37ull);
            pcss_rpc(pc[i], nonce, salt, seq, 0xABCDull);
            nb_u64 e0 = mix64(nonce, salt, seq ^ 0xABCDull);
            nb_u64 e1 = mix64(e0, 0xBEEFull, nonce);
            if (pcss_reply_r0 == e0 && pcss_reply_r1 == e1
                && pcss_back_r2 == nonce && pcss_back_r3 == salt
                && pcss_back_r4 == salt && pcss_back_r5 == seq
                && pcss_back_r6 == 0xABCDull) {
                rpc_ok++;
            }
            seq++;
        }
        if ((t % 500u) == 499u) {
            line_reset();
            line_str("ZXT RUNNER: t=");
            line_u64(t);
            line_str(" ok=");
            line_u64(rpc_ok);
            line_end();
        }
    }
    if (rpc_ok != (nb_u64)TRIPS * BENCH_DOMAINS) {
        line_reset();
        line_str("ZXT PCSS FAIL rpc mismatch ok=");
        line_u64(rpc_ok);
        line_end();
        FAIL();
    }

    // --- Phase B: deep nesting on the runner's own linkage stack ---
    nb_u64 nest_ok = 0;
    for (nb_u64 i = 0; i < NEST_ITERS; i++) {
        nb_u64 r = nest_storm(NEST_DEPTH, 0x600D0000ull + i);
        if (r == 0) {
            nest_ok++;
        } else {
            line_reset();
            line_str("ZXT PCSS FAIL nest kind=");
            line_u64(r);
            line_str(" level=");
            line_u64(nest_fail_level);
            line_end();
            FAIL();
        }
    }

    // --- Phase C: deferred gates + worker stat snapshots ---
    nb_u64 def_ok = 0;
    nb_u64 last_iters[BENCH_DOMAINS] = {0, 0, 0};
    nb_u64 last_yield[BENCH_DOMAINS] = {0, 0, 0};
    for (nb_u64 i = 0; i < DEFERS; i++) {
        for (int j = 0; j < BENCH_DOMAINS; j++) {
            nb_u64 p0 = 0xDEFA0000ull + i;
            nb_u64 p1 = 0x17B00000ull + i;
            nb_u64 p2 = i;
            nb_u64 p3 = 0xAAAA0000ull + i;
            pcss_rpc(dpc[j], p0, p1, p2, p3);
            nb_u64 iters = pcss_reply_r0 >> 32;
            nb_u64 fails = pcss_reply_r0 & 0xFFFFFFFFull;
            nb_u64 yiters = pcss_reply_r1;
            if (pcss_back_r2 == p0 && pcss_back_r3 == p1
                && pcss_back_r4 == p1 && pcss_back_r5 == p2
                && pcss_back_r6 == p3 && pcss_back_r15 == 0
                && fails == 0 && iters >= last_iters[j]) {
                def_ok++;
                last_iters[j] = iters;
                last_yield[j] = yiters;
            }
        }
    }
    if (def_ok != (nb_u64)DEFERS * BENCH_DOMAINS) {
        line_reset();
        line_str("ZXT PCSS FAIL defer mismatch ok=");
        line_u64(def_ok);
        line_end();
        FAIL();
    }
    nb_u64 wnest = last_iters[0] + last_iters[1] + last_iters[2];

    line_reset();
    line_str("ZXT PCSS PASS rpc=");
    line_u64(rpc_ok);
    line_str(" nest=");
    line_u64(nest_ok);
    line_str("x");
    line_u64(NEST_DEPTH);
    line_str(" defer=");
    line_u64(def_ok);
    line_str(" wnest=");
    line_u64(wnest);
    line_end();

    // --- PC-ss teardown: gates, then the callee domains ---
    for (int i = 0; i < BENCH_DOMAINS; i++) {
        if (nb_gate_destroy(admin_cap, gate[i]) == NB_ERR
            || nb_gate_destroy(admin_cap, dgate[i]) == NB_ERR) {
            put("ZXT RUNNER: gate destroy failed\n");
        }
    }
    for (int i = 0; i < BENCH_DOMAINS; i++) {
        if (nb_dom_destroy(admin_cap, pdom[i]) == NB_ERR) {
            put("ZXT RUNNER: pcss dom destroy failed\n");
        }
    }

    // ================= self-contained suites =================
    static const char *suite_ds[3] = {"ZXTSCHED", "ZXTDOM", "ZXTIPC"};
    static const char *suite_nm[3] = {"sched", "dom", "ipc"};
    for (int s = 0; s < 3; s++) {
        int b = load_suite(suite_ds[s]);
        if (b == 0) FAIL();
        nb_u64 d = spawn_suite(suite_nm[s], b);
        wait_suite(d, suite_nm[s]);
    }

    put("ZXT RUN COMPLETE\n");
    for (;;) {
        nb_sys_yield(0, 0, 0, 0, 0);
    }
}
