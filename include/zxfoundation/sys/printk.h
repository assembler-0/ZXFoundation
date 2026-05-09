// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sys/printk.h
#pragma once

#include <stdarg.h>
#include <zxfoundation/sys/log.h>

typedef void (*printk_putc_sink)(char);

/// @brief Install the console sink and initialize the log ring.
///        Must be called before any printk().
void printk_initialize(printk_putc_sink sink);

/// @brief Replace the console sink at runtime.
void printk_set_sink(printk_putc_sink sink);

/// @brief Emit raw bytes directly to the sink (no formatting, no ring store).
///        Used by early boot paths before the log ring is ready.
void printk_flush(const char *buf, size_t size);

/// @brief Format and emit a log line.  Parses an optional ZX_* level tag.
int vprintk(const char *fmt, va_list ap);

/// @brief Format and emit a log line.
int printk(const char *fmt, ...);
