/// SPDX-License-Identifier: Apache-2.0
/// @file simplelog.h
/// @brief simplelog protocol (effectively printk_flush without formatting)

#pragma once

/// @brief simplelog sink function
typedef void(*simplelog_putc_sink)(char);

/// @brief set global sink
/// @param sink new global sink, or nullptr to disable logging sink to use
void simplelog_set_sink(simplelog_putc_sink sink);

/// @brief initialize simplelog
/// @param sink global sink, or nullptr to disable logging sink to use
void simplelog_initialize(simplelog_putc_sink sink);

/// @brief log to sink
/// @param msg message to log (should be null-terminated)
/// @warning not thread-safe
void simplelog(const char *msg);