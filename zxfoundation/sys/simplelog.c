/// SPDX-License-Identifier: Apache-2.0
/// simplelog.c - logging functions

#include <zxfoundation/sys/simplelog.h>

static simplelog_putc_sink global_sink = nullptr;

void simplelog(const char *msg) {
    if (!global_sink) return;
    while (*msg)
        global_sink(*msg++);
}

void simplelog_set_sink(const simplelog_putc_sink sink) {
    global_sink = sink;
}

void simplelog_initialize(const simplelog_putc_sink sink) {
    global_sink = sink;
}
