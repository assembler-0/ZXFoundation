/// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtsgn.c — signal register → deliver → sigreturn suite

#include <sys/inc64/nuclbase.h>

#define ZX_SGN_USER0 5

nb_u64 nb_source_token;
nb_u64 zxt_admin_cap;
nb_u64 zxt_worker_table[4];

static volatile nb_u64 sig_seen;      // deliveries completed (after sigreturn)
static volatile nb_u64 sig_kind;      // last staged kind arg
static volatile nb_u64 sig_payload;   // last staged payload arg
static volatile nb_u64 sig_ctx;       // last staged user_data arg

static volatile nb_u64 canary_stack[4];
#define ZX_CANARY0 0xA11CE1ULL
#define ZX_CANARY1 0xA11CE2ULL

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

static void line_u64(nb_u64 v) {
    char tmp[20];
    nb_u64 n = 0;
    do {
        tmp[n++] = '0' + (char)(v % 10);
        v /= 10;
    } while (v != 0);
    while (n != 0 && line_len < sizeof(line_buf))
        line_buf[line_len++] = tmp[--n];
}

static void line_end(void) {
    if (line_len < sizeof(line_buf)) line_buf[line_len++] = '\n';
    nb_console_write(line_buf, line_len);
    line_len = 0;
}

static void fail(const char *why) {
    line_reset();
    line_str("ZXT SGN FAIL ");
    line_str(why);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}

static void sig_handler(nb_u64 kind, nb_u64 payload, nb_u64 ctx) {
    sig_kind    = kind;
    sig_payload = payload;
    sig_ctx     = ctx;
    sig_seen    = sig_seen + 1;
    put("zxt sgn: handler fired\n");
    nb_signal_return(0, 0, 0, 0, 0);
}

void zxt_main(void) {
    nb_u64 r = nb_dom_info(0);
    if (r == NB_ERR) fail("dom_info");
    const nb_u64 dom = r & 0xFFFFFFFFull;

    if (nb_sgn_register(ZX_SGN_USER0, (nb_u64)(nb_uptr)sig_handler,
                        (nb_u64)(nb_uptr)canary_stack, 0, 0) == NB_ERR) {
        fail("register");
    }
    canary_stack[0] = ZX_CANARY0;
    canary_stack[1] = ZX_CANARY1;

    if (nb_sgn_send(dom, ZX_SGN_USER0, 0xFEEDFACEULL, 0, 0) == NB_ERR)
        fail("send");
    for (int i = 0; i < 64; i++) {
        if (sig_seen != 0) break;
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    if (sig_seen != 1) fail("deliver");
    if (sig_kind != ZX_SGN_USER0) fail("kind");
    if (sig_payload != 0xFEEDFACEULL) fail("payload");
    if (sig_ctx != (nb_u64)(nb_uptr)canary_stack) fail("ctx");
    if (canary_stack[0] != ZX_CANARY0 || canary_stack[1] != ZX_CANARY1)
        fail("canary");

    if (nb_sgn_mask_set((nb_u64)(1U << ZX_SGN_USER0), 0, 0, 0, 0) == NB_ERR)
        fail("mask_set");
    if (nb_sgn_send(dom, ZX_SGN_USER0, 0xB10CC0DEULL, 0, 0) == NB_ERR)
        fail("send2");
    for (int i = 0; i < 64; i++) {
        if (sig_seen != 1) fail("mask_bypass");
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    if (nb_sgn_mask_get(0, 0, 0, 0, 0) != (nb_u64)(1U << ZX_SGN_USER0))
        fail("mask_get");
    if (nb_sgn_mask_set(0, 0, 0, 0, 0) == NB_ERR) fail("mask_clr");
    for (int i = 0; i < 128; i++) {
        if (sig_seen != 1) break;
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    if (sig_seen != 2) fail("unblock");
    if (sig_payload != 0xB10CC0DEULL) fail("payload2");
    if (sig_kind != ZX_SGN_USER0) fail("kind2");
    if (canary_stack[0] != ZX_CANARY0 || canary_stack[1] != ZX_CANARY1)
        fail("canary2");

    line_reset();
    line_str("ZXT SGN PASS seen=2 kind=");
    line_u64(sig_kind);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}
