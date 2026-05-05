/// SPDX-License-Identifier: Apache-2.0
/// printk.c - logging functions

#include <lib/vsprintf.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sync/spinlock.h>

static printk_putc_sink global_sink = nullptr;
static spinlock_t console_lock = SPINLOCK_INIT;

void printk_flush(const char *buf, size_t size) {
    if (!global_sink) return;
    
    irqflags_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    
    while (size--) {
        global_sink(*buf);
        buf++;
    }
    
    spin_unlock_irqrestore(&console_lock, flags);
}

int vprintk(const char *fmt, va_list ap) {
    char local_buf[256];
    int count = vsnprintf(local_buf, sizeof(local_buf), fmt, ap);
    printk_flush(local_buf, count);
    return count;
}

int printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

void printk_set_sink(const printk_putc_sink sink) {
    global_sink = sink;
}

void printk_initialize(const printk_putc_sink sink) {
    global_sink = sink;
}
