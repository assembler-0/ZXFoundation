/// SPDX-License-Identifier: Apache-2.0
/// printk.c - logging functions

#include <zxfoundation/sys/printk.h>
#include <zxfoundation/errno.h>

static printk_putc_sink global_sink = nullptr;

int printk(const char *fmt, ...) {
    if (!global_sink) return -ENODEV;
    int count = 0;
    while (*fmt) {
        global_sink(*fmt++);
        ++count;
    }
    return count;
}

void printk_set_sink(const printk_putc_sink sink) {
    global_sink = sink;
}

void printk_initialize(const printk_putc_sink sink) {
    printk_set_sink(sink);
}