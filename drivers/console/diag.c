// SPDX-License-Identifier: Apache-2.0
// drivers/console/diag.c
//
/// @brief DIAG 8 hypervisor console driver for the ZXFoundation kernel.
///
///        Identical fix to arch/s390x/init/zxfl/diag.c: after each flush
///        the "MSG * " prefix is restored to ASCII so the next conversion
///        does not double-convert it into garbage.
///
///        The kernel runs in 64-bit z/Arch mode, so R2/R3 are 64-bit.

#include <drivers/console/diag.h>
#include <lib/ebcdic.h>
#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// DIAG 8 raw write (64-bit kernel mode)
// ---------------------------------------------------------------------------

/// @brief Issue the DIAG 8 instruction with a 64-bit buffer address.
static inline void diag8_write(const char *addr, uint64_t len) {
    register uint64_t r2 __asm__("2") = (uint64_t)(uintptr_t)addr;
    register uint64_t r3 __asm__("3") = len;
    __asm__ __volatile__ (
        "diag %[r2], %[r3], 8\n"
        : /* no outputs */
        : [r2] "d" (r2), [r3] "d" (r3)
        : "memory"
    );
}

// ---------------------------------------------------------------------------
// Line-buffered output
// ---------------------------------------------------------------------------

#define DIAG_LINE_BUF_SIZE  256U
#define MSG_PREFIX_LEN      6U

// Pre-built EBCDIC "MSG * " (CP037) — written directly, no table lookup.
#define EBCDIC_M  0xD4U
#define EBCDIC_S  0xE2U
#define EBCDIC_G  0xC7U
#define EBCDIC_SP 0x40U
#define EBCDIC_ST 0x5CU

/// @brief Flush line_buf to DIAG 8 using a separate EBCDIC transmit buffer.
///        The ASCII line_buf is never modified — no double-conversion hazard.
static void diag_flush(const char *line_buf, size_t line_len) {
    if (line_len <= MSG_PREFIX_LEN) return;

    static char tx[DIAG_LINE_BUF_SIZE];

    tx[0] = (char)EBCDIC_M;
    tx[1] = (char)EBCDIC_S;
    tx[2] = (char)EBCDIC_G;
    tx[3] = (char)EBCDIC_SP;
    tx[4] = (char)EBCDIC_ST;
    tx[5] = (char)EBCDIC_SP;

    size_t payload_len = line_len - MSG_PREFIX_LEN;
    for (size_t i = 0; i < payload_len; i++) {
        tx[MSG_PREFIX_LEN + i] =
            (char)ascii_to_ebcdic((uint8_t)line_buf[MSG_PREFIX_LEN + i]);
    }

    diag8_write(tx, (uint64_t)line_len);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int diag_setup(void) {
    return 0;
}

void diag_putc(const char c) {
    static char   line_buf[DIAG_LINE_BUF_SIZE];
    static size_t line_len = MSG_PREFIX_LEN;

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
    diag_putc('\n');
}
