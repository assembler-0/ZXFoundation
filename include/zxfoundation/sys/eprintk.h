#pragma once

#include <stdarg.h>

typedef void(*eprintk_putc_sink)(char);

void eprintk_set_sink(eprintk_putc_sink sink);
void eprintk_initialize(eprintk_putc_sink sink);

// vprintk - va_list variant; the core formatter used by printk and panic_emit.
int veprintk(const char *fmt, va_list ap);

// printk - printf-style wrapper around vprintk.
int eprintk(const char *fmt, ...);
