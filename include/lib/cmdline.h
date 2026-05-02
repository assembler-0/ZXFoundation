#pragma once

#include <zxfoundation/types.h>

int cmdline_find_option_bool(const char *cmdline, int max_cmdline_size,
                           const char *option);
int cmdline_find_option(const char *cmdline, int max_cmdline_size,
                      const char *option, char *buffer, int bufsize);