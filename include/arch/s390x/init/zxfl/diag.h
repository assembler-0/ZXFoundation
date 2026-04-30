/// SPDX-License-Identifier: Apache-2.0
/// include/drivers/console/diag.h - DIAG 8 hypervisor console driver
///
/// @brief DIAG 8 provides direct ASCII output to the Hercules/z/VM console.
///        This is ideal for early boot debugging before SCLP is available.

#ifndef ZXFOUNDATION_DRIVERS_CONSOLE_DIAG_H
#define ZXFOUNDATION_DRIVERS_CONSOLE_DIAG_H

#include <zxfoundation/types.h>

/// @brief Initialize DIAG 8 console (no-op, always available)
/// @return 0 on success
int diag_setup(void);

/// @brief Write a buffer to DIAG 8 console
/// @param buf ASCII string buffer
/// @param len Length in bytes
/// @return 0 on success, -1 on error
int diag_write(const char *buf, size_t len);

/// @brief Write a single character to DIAG 8 console
/// @param c ASCII character
void diag_putc(const char c);

void diag_flush_all(void);

static inline void print_msg(const char *msg) {
    while (*msg) {
        diag_putc(*msg++);
    }
}

#endif /* ZXFOUNDATION_DRIVERS_CONSOLE_DIAG_H */
