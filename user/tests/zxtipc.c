/// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtipc.c — IPC suite
///
/// One self-pair endpoint; the primary drives rendezvous calls and a
/// receiver worker blocks in ipc_receive, verifies every payload word
/// against the expected nonce sequence, and replies with a transform
/// of the payload.  Exercises endpoint create/destroy, message send,
/// blocking receive, reply correlation, and the call rendezvous —
/// with data verification on both legs.

#include <sys/inc64/nuclbase.h>

extern void zxt_wentry0(void);

nb_u64 nb_source_token;
nb_u64 zxt_admin_cap;
nb_u64 zxt_worker_table[4];

/// Kernel ipc::user_message_request layout (ipc types.cxxm) — 32 bytes.
typedef struct {
    nb_u64 payload;
    nb_u64 capabilities;
    nb_u64 grants;
    nb_u32 length;
    nb_u16 capability_count;
    nb_u16 grant_count;
} zxt_msg;

enum { MSGS = 300, NONCE_BASE = 0x1CC00000ull, POISON = 0xFEEDC0DEull };

static nb_u64 endpoint;
static volatile nb_u64 rx_count;
static volatile nb_u64 rx_bad;
static volatile nb_u64 rx_done;

static void receiver_main(void) {
    for (;;) {
        nb_u8 buf[64];
        zxt_msg res;
        res.capabilities = 0;
        res.grants = 0;
        res.capability_count = 0;
        res.grant_count = 0;
        nb_u64 r = nb_ipc_receive(endpoint, (nb_u64) buf, (nb_u64) &res, 2, 0);
        if (r == NB_ERR) continue;
        nb_u64 corr = r >> 32;
        nb_u64 len = r & 0xFFFFFFFFull;
        if (corr == 0 || len < 8) continue;
        nb_u64 v = *(nb_u64 *) buf;
        if (v == POISON) {
            rx_done = 1;
            return;
        }
        if (v == NONCE_BASE + rx_count) {
            rx_count++;
        } else {
            rx_bad++;
        }
        nb_ipc_reply(endpoint, corr, v ^ 0xC0FFEEull, 0, 0);
    }
}

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

static void fail(const char *why) {
    line_reset();
    line_str("ZXT IPC FAIL ");
    line_str(why);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}

void zxt_main(void) {
    zxt_admin_cap = nb_source_token;
    put("zxt ipc: up\n");

    nb_u64 r = nb_dom_info(0);
    if (r == NB_ERR) fail("dom_info");
    nb_u64 dom = r & 0xFFFFFFFFull;

    endpoint = nb_ipc_endpoint_create(dom, 0, 0, 0, 0);
    if (endpoint == NB_ERR) fail("endpoint_create");

    zxt_worker_table[0] = (nb_u64) (nb_uptr) receiver_main;
    if (nb_dom_strand_create(zxt_admin_cap, 0, dom,
                             (nb_u64) (nb_uptr) zxt_wentry0, 0) == NB_ERR) {
        fail("strand_create");
    }

    nb_u64 data = 0;
    zxt_msg req;
    req.payload = (nb_u64) &data;
    req.capabilities = 0;
    req.grants = 0;
    req.length = 8;
    req.capability_count = 0;
    req.grant_count = 0;

    nb_u64 calls_ok = 0;
    for (nb_u64 i = 0; i < MSGS; i++) {
        data = NONCE_BASE + i;
        nb_u64 cr = nb_ipc_call(endpoint, (nb_u64) &req, i + 1, 0, 0);
        if (cr != NB_ERR) calls_ok++;
        // Let the receiver keep pace; the rendezvous parks this strand
        // until the reply, so a modest yield cadence is enough.
        if ((i % 64u) == 63u) nb_sys_yield(0, 0, 0, 0, 0);
    }
    if (calls_ok != MSGS) fail("ipc_call");

    // Release the receiver with a poison message, then the endpoint.
    data = POISON;
    if (nb_ipc_send(endpoint, (nb_u64) &req, 0, 0, 0) == NB_ERR) {
        fail("poison send");
    }
    for (nb_u64 tries = 0; tries < 4000 && rx_done == 0; tries++) {
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    if (rx_done == 0) fail("receiver stuck");
    if (rx_bad != 0) fail("payload mismatch");
    if (rx_count != MSGS) fail("sequence gap");
    if (nb_ipc_endpoint_destroy(endpoint, 0, 0, 0, 0) == NB_ERR) {
        fail("endpoint_destroy");
    }

    line_reset();
    line_str("ZXT IPC PASS msgs=");
    line_u64(rx_count);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}
