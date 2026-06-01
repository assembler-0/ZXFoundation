/// SPDX-License-Identifier: Apache-2.0
/// @file simplelog.c
/// @brief logging functions

#include <zxfoundation/sys/simplelog.h>

static simplelog_putc_sink global_sink = nullptr;

/// @brief log to sink
/// @param msg message to log (should be null-terminated)
/// @warning not thread-safe
void simplelog(const char *msg) {
    if (!global_sink) return;
    while (*msg)
        global_sink(*msg++);
}

/// @brief set global sink
/// @param sink new global sink, or nullptr to disable logging sink to use
void simplelog_set_sink(const simplelog_putc_sink sink) {
    global_sink = sink;
}

/// @brief initialize simplelog
/// @param sink global sink, or nullptr to disable logging sink to use
void simplelog_initialize(const simplelog_putc_sink sink) {
    simplelog_set_sink(sink);
}
