#pragma once

typedef void(*simplelog_putc_sink)(char);

void simplelog_set_sink(simplelog_putc_sink sink);
void simplelog_initialize(simplelog_putc_sink sink);
void simplelog(const char *msg);