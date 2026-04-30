#pragma once

#include <stdarg.h>

typedef void(*printk_putc_sink)(char);

void printk_set_sink(printk_putc_sink sink);
void printk_initialize(printk_putc_sink sink);
int vprintk(const char *fmt, va_list ap);
int printk(const char *fmt, ...);