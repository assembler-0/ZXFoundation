// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/diag.h
//
/// @brief DIAG 8 hypervisor console driver for the ZXFL bootloader.
///
///        DIAG 8 provides synchronous ASCII output to the Hercules/z/VM
///        operator console.  With DIAG8CMD ENABLE, Hercules interprets the
///        buffer as a host command unless it begins with "MSG * " (EBCDIC).
///        The driver maintains a line buffer in ASCII and converts to EBCDIC
///        immediately before issuing the DIAG 8 instruction.

#ifndef ZXFOUNDATION_ZXFL_DIAG_H
#define ZXFOUNDATION_ZXFL_DIAG_H

#include <zxfoundation/types.h>

/// @brief Initialize the DIAG 8 console.
///        No-op — DIAG 8 is always available in ESA/390 and z/Arch mode.
/// @return 0 (always succeeds)
int diag_setup(void);

/// @brief Write a single ASCII character to the DIAG 8 console.
///        Buffers characters until a newline, then flushes the line.
/// @param c ASCII character
void diag_putc(char c);

/// @brief Write a buffer of ASCII characters to the DIAG 8 console.
/// @param buf Pointer to ASCII data
/// @param len Number of bytes to write
/// @return 0 on success
int diag_write(const char *buf, size_t len);

/// @brief Flush any partial line buffered in the DIAG 8 driver.
///        Equivalent to writing a newline.
void diag_flush_all(void);

/// @brief Write a NUL-terminated ASCII string to the DIAG 8 console.
///        Convenience wrapper used throughout the bootloader.
/// @param msg NUL-terminated ASCII string
static inline void print_msg(const char *msg) {
    while (*msg) {
        diag_putc(*msg++);
    }
}

#endif /* ZXFOUNDATION_ZXFL_DIAG_H */
