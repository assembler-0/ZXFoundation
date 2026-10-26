/// SPDX-License-Identifier: Apache-2.0
/// @file user/tests/zxtdom.c — memory/capability/strand lifecycle suite

#include <sys/inc64/nuclbase.h>

extern void zxt_wentry0(void);
extern void zxt_wentry1(void);

nb_u64 nb_source_token;
nb_u64 zxt_admin_cap;
nb_u64 zxt_worker_table[4];

static volatile nb_u64 w_iters[2];

static void worker_main(void) {
    for (;;) {
        for (int i = 0; i < 64; i++) {
            nb_sys_yield(0, 0, 0, 0, 0);
        }
        w_iters[0]++;
    }
}

static void worker_main2(void) {
    for (;;) {
        for (int i = 0; i < 64; i++) {
            nb_sys_yield(0, 0, 0, 0, 0);
        }
        w_iters[1]++;
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
    line_str("ZXT DOM FAIL ");
    line_str(why);
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}

static void fill_verify(nb_u64 *page, nb_u64 tag, const char *why) {
    for (nb_u64 w = 0; w < 512; w++) {
        page[w] = tag * 0x10001ull + w;
    }
    for (nb_u64 w = 0; w < 512; w++) {
        if (page[w] != tag * 0x10001ull + w) fail(why);
    }
}

void zxt_main(void) {
    zxt_admin_cap = nb_source_token;
    put("zxt dom: up\n");

    enum { PAGES = 4, CAPS = 64 };
    nb_u64 page[PAGES];

    // --- memory: alloc, verify, free, re-alloc, verify ---
    for (int i = 0; i < PAGES; i++) {
        page[i] = nb_mem_alloc(4096, 0, 0, 0, 0);
        if (page[i] == NB_ERR) fail("mem_alloc");
        fill_verify((nb_u64 *) page[i], 0xA0 + (nb_u64) i, "pattern1");
    }
    for (int i = 0; i < PAGES; i++) {
        if (nb_mem_free(page[i], 4096, 0, 0, 0) == NB_ERR) fail("mem_free");
    }
    for (int i = 0; i < PAGES; i++) {
        page[i] = nb_mem_alloc(4096, 0, 0, 0, 0);
        if (page[i] == NB_ERR) fail("mem_realloc");
        fill_verify((nb_u64 *) page[i], 0xB0 + (nb_u64) i, "pattern2");
    }

    // --- capabilities: acquire/release cycle ---
    nb_u64 acquired = 0;
    for (nb_u64 i = 0; i < CAPS; i++) {
        nb_u64 cap = nb_cap_acquire(nb_source_token, 0x00800000ull, 0, 0, 0);
        if (cap == NB_ERR) break;
        if (nb_cap_release(cap, 0, 0, 0, 0) == NB_ERR) fail("cap_release");
        acquired++;
    }
    if (acquired == 0) fail("cap_acquire");

    // --- strands: spawn, progress, teardown via domain exit ---
    nb_u64 r = nb_dom_info(0);
    if (r == NB_ERR) fail("dom_info");
    nb_u64 dom = r & 0xFFFFFFFFull;
    zxt_worker_table[0] = (nb_u64) (nb_uptr) worker_main;
    zxt_worker_table[1] = (nb_u64) (nb_uptr) worker_main2;
    if (nb_dom_strand_create(zxt_admin_cap, 0, dom,
                             (nb_u64) (nb_uptr) zxt_wentry0, 0) == NB_ERR) {
        fail("strand_create0");
    }
    if (nb_dom_strand_create(zxt_admin_cap, 0, dom,
                             (nb_u64) (nb_uptr) zxt_wentry1, 0) == NB_ERR) {
        fail("strand_create1");
    }
    for (nb_u64 i = 0; i < 6000; i++) {
        nb_sys_yield(0, 0, 0, 0, 0);
    }
    if (w_iters[0] == 0 || w_iters[1] == 0) fail("worker starved");

    line_reset();
    line_str("ZXT DOM PASS mem=8 caps=");
    line_u64(acquired);
    line_str(" workers=2");
    line_end();
    nb_dom_exit();
    for (;;) { nb_sys_yield(0, 0, 0, 0, 0); }
}
