// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/diag.c - DIAG 8 hypervisor console driver for the ZXFL bootloader.

#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/ebcdic.h>

static inline void diag8_write(const char *addr, uint32_t len) {
    register uint32_t r2 __asm__("2") = (uint32_t)(uintptr_t)addr;
    register uint32_t r3 __asm__("3") = len;
    __asm__ __volatile__ (
        "diag %[r2], %[r3], 8\n"
        : /* no outputs */
        : [r2] "d" (r2), [r3] "d" (r3)
        : "memory"
    );
}

#define DIAG_LINE_BUF_SIZE  256U
#define MSG_PREFIX_LEN      6U

#define EBCDIC_M  0xD4U
#define EBCDIC_S  0xE2U
#define EBCDIC_G  0xC7U
#define EBCDIC_SP 0x40U
#define EBCDIC_ST 0x5CU  // '*'

/// @brief Flush the ASCII line_buf to DIAG 8.
///
///        Strategy: build a separate EBCDIC transmit buffer.
///        - Bytes 0-5: EBCDIC "MSG * " written directly as constants.
///        - Bytes 6-N: payload converted from ASCII to EBCDIC.
///        The ASCII line_buf is never modified, so there is no
///        double-conversion hazard and no need for a prefix_reset.
static void diag_flush(const char *line_buf, uint32_t line_len) {
    if (line_len <= MSG_PREFIX_LEN) return;

    static char tx[DIAG_LINE_BUF_SIZE];

    tx[0] = (char)EBCDIC_M;
    tx[1] = (char)EBCDIC_S;
    tx[2] = (char)EBCDIC_G;
    tx[3] = (char)EBCDIC_SP;
    tx[4] = (char)EBCDIC_ST;
    tx[5] = (char)EBCDIC_SP;

    // Convert the payload (everything after the prefix slot) to EBCDIC.
    uint32_t payload_len = line_len - MSG_PREFIX_LEN;
    for (uint32_t i = 0; i < payload_len; i++) {
        tx[MSG_PREFIX_LEN + i] =
            (char)ascii_to_ebcdic((uint8_t)line_buf[MSG_PREFIX_LEN + i]);
    }

    diag8_write(tx, line_len);
}

int diag_setup(void) {
    return 0;
}

void diag_putc(const char c) {
    static char     line_buf[DIAG_LINE_BUF_SIZE];
    static uint32_t line_len = MSG_PREFIX_LEN;

    if (c == '\n') {
        diag_flush(line_buf, line_len);
        line_len = MSG_PREFIX_LEN;
    } else {
        line_buf[line_len++] = c;
        if (line_len >= DIAG_LINE_BUF_SIZE - 1U) {
            diag_flush(line_buf, line_len);
            line_len = MSG_PREFIX_LEN;
        }
    }
}

int diag_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        diag_putc(buf[i]);
    }
    return 0;
}

void diag_flush_all(void) {
    // Force a flush of any partial line (e.g. on panic before newline).
    diag_putc('\n');
}
