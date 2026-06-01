/// SPDX-License-Identifier: Apache-2.0
/// @file printk.h
/// @brief Kernel print capabilities.

#pragma once

#include <zxfoundation/sys/log.h>

/// @brief printk sink type
typedef void (*printk_putc_sink)(char);

/// @brief Initialize the kernel console.
/// @param[in] sink Console sink function.
void printk_initialize(printk_putc_sink sink);

/// @brief Change the console sink function.
/// @param[in] sink New console sink function.
void printk_set_sink(printk_putc_sink sink);

/// @brief Flush buffered output.
/// @param[in] buf Buffer to flush.
/// @param[in] size Number of bytes in buffer.
void printk_flush(const char *buf, size_t size);

/// @brief Format and emit a log record.
/// @param[in] fmt Format string.
/// @param[in] ap  Variable argument list.
/// @return Number of bytes emitted.
int vprintk(const char *fmt, va_list ap);

/// @brief Format and emit a log record.
/// @param[in] fmt Format string.
/// @param[in] ... Variable arguments.
/// @return Number of bytes emitted.
int printk(const char *fmt, ...);
