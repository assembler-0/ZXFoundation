#pragma once

#include <stdarg.h>

typedef void(*printk_putc_sink)(char);

void printk_set_sink(printk_putc_sink sink);
void printk_initialize(printk_putc_sink sink);

// vprintk - va_list variant; the core formatter used by printk and panic_emit.
int vprintk(const char *fmt, va_list ap);

// printk - printf-style wrapper around vprintk.
int printk(const char *fmt, ...);