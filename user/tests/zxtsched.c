/// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtsched.c — concurrent scheduling suite

#include <sys/inc64/nuclbase.h>

extern void zxt_wentry0(void);
extern void zxt_wentry1(void);
extern void zxt_wentry2(void);
extern void zxt_wentry3(void);

nb_u64 nb_source_token;
nb_u64 zxt_admin_cap;
nb_u64 zxt_worker_table[4];

static volatile nb_u64 run_flag = 1;

typedef struct {
    volatile nb_u64 a;
    volatile nb_u64 b;
} ping_cell;

static ping_cell cell0;
static ping_cell cell1;
static volatile nb_u64 iters[4];

static void ping_half(volatile nb_u64 *mine, volatile nb_u64 *theirs,
                      nb_u64 slot) {
    nb_u64 seen = 0;
    while (run_flag != 0) {
        if (*theirs != seen) {
            seen = *theirs;
            (*mine)++;
            iters[slot]++;
            nb_mem_futex_wake((nb_u64) mine, 1, 0, 0, 0);
        } else {
            nb_mem_futex_wait((nb_u64) theirs, seen, 0, 0, 0);
        }
    }
    // Final bump so the peer also observes the exit.
    (*mine)++;
    nb_mem_futex_wake((nb_u64) mine, 1, 0, 0, 0);
}

static void ping0(void) { ping_half(&cell0.a, &cell0.b, 0); }
static void pong0(void) { ping_half(&cell0.b, &cell0.a, 1); }
static void ping1(void) { ping_half(&cell1.a, &cell1.b, 2); }
static void pong1(void) { ping_half(&cell1.b, &cell1.a, 3); }

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
    line_str("ZXT SCHED FAIL ");
    line_str(why);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}

void zxt_main(void) {
    zxt_admin_cap = nb_source_token;
    put("zxt sched: up\n");

    zxt_worker_table[0] = (nb_u64) (nb_uptr) ping0;
    zxt_worker_table[1] = (nb_u64) (nb_uptr) pong0;
    zxt_worker_table[2] = (nb_u64) (nb_uptr) ping1;
    zxt_worker_table[3] = (nb_u64) (nb_uptr) pong1;

    static const nb_u64 entries[4] = {
        (nb_u64) (nb_uptr) zxt_wentry0, (nb_u64) (nb_uptr) zxt_wentry1,
        (nb_u64) (nb_uptr) zxt_wentry2, (nb_u64) (nb_uptr) zxt_wentry3};
    nb_u64 r = nb_dom_info(0);
    if (r == NB_ERR) fail("dom_info");
    nb_u64 dom = r & 0xFFFFFFFFull;
    for (int i = 0; i < 4; i++) {
        if (nb_dom_strand_create(zxt_admin_cap, 0, dom, entries[i], 0)
                == NB_ERR) {
            fail("strand_create");
        }
    }

    // Cold-start latch: every worker parks on futex_wait(theirs, seen=0)
    // and only a wake following a value change advances the protocol —
    // without a primer the four workers deadlock before one iteration.
    // Prime both pairs' 'a' halves and wake their pongs; the kernel's
    // value-checked wait makes this race-free against workers that have
    // not parked yet.
    cell0.a = 1;
    cell1.a = 1;
    nb_mem_futex_wake((nb_u64) &cell0.a, 1, 0, 0, 0);
    nb_mem_futex_wake((nb_u64) &cell1.a, 1, 0, 0, 0);

    nb_u64 t0 = nb_sys_now(0, 0, 0, 0, 0);
    if (t0 == NB_ERR) fail("now");
    for (nb_u64 i = 0; i < 20000; i++) {
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    nb_u64 t1 = nb_sys_now(0, 0, 0, 0, 0);
    nb_u64 t2 = nb_sys_now(0, 0, 0, 0, 0);
    if (t1 == NB_ERR || t2 == NB_ERR) fail("now");
    if (t1 < t0 || t2 < t1) fail("clock");

    run_flag = 0;
    // Release any parked halves; the value protocol makes this safe to
    // fire before or after the workers check the flag.
    nb_mem_futex_wake((nb_u64) &cell0.a, 2, 0, 0, 0);
    nb_mem_futex_wake((nb_u64) &cell0.b, 2, 0, 0, 0);
    nb_mem_futex_wake((nb_u64) &cell1.a, 2, 0, 0, 0);
    nb_mem_futex_wake((nb_u64) &cell1.b, 2, 0, 0, 0);
    for (nb_u64 i = 0; i < 1000; i++) {
        nb_sys_yield(0, 0, 0, 0, 0);
    }

    nb_u64 skew0 = iters[0] > iters[1] ? iters[0] - iters[1]
                                       : iters[1] - iters[0];
    nb_u64 skew1 = iters[2] > iters[3] ? iters[2] - iters[3]
                                       : iters[3] - iters[2];
    if (iters[0] == 0 || iters[1] == 0
        || iters[2] == 0 || iters[3] == 0) {
        fail("pair starved");
    }
    // Strict alternation bounds the skew; allow a small tail for the
    // final handoff after run_flag drops.
    if (skew0 > 4 || skew1 > 4) fail("pair skew");

    line_reset();
    line_str("ZXT SCHED PASS pairs=");
    line_u64(iters[0] + iters[1] + iters[2] + iters[3]);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}
