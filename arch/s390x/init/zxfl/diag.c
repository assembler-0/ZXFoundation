// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/diag.c
//
/// @brief DIAG 8 hypervisor console driver for the ZXFL bootloader.
///
///        With DIAG8CMD ENABLE, Hercules interprets the DIAG 8 buffer as a
///        host command unless it begins with "MSG * " (in EBCDIC).  We
///        maintain a line buffer in ASCII, prepend "MSG * " in ASCII, and
///        convert the entire buffer to EBCDIC immediately before issuing
///        the DIAG 8 instruction.  After each flush the prefix is
///        re-written in ASCII so the next conversion is clean.
///
///        The bug that caused HHC01600E was: after a mid-line flush (buffer
///        nearly full), the prefix bytes were left in EBCDIC from the
///        previous conversion.  The next flush converted them again,
///        producing garbage that Hercules did not recognise as "MSG * ".

#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/ebcdic.h>

// ---------------------------------------------------------------------------
// DIAG 8 raw write
// ---------------------------------------------------------------------------

/// @brief Issue the DIAG 8 instruction.
///        @p addr must point to an EBCDIC buffer; @p len is the byte count.
///        The buffer is consumed by the hypervisor synchronously — no DMA,
///        no interrupt.  The volatile qualifier on the asm prevents the
///        compiler from reordering or eliminating the instruction.
static inline void diag8_write(const char *addr, uint32_t len) {
    // R2 = buffer address, R3 = length.  Both must be in general registers.
    register uint32_t r2 __asm__("2") = (uint32_t)(uintptr_t)addr;
    register uint32_t r3 __asm__("3") = len;
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

// Pre-built EBCDIC encoding of "MSG * " (CP037).
// Keeping this as a constant avoids any dependency on the conversion table
// being correct at the time the prefix is written, and eliminates the
// double-conversion hazard entirely.
//   M=0xD4  S=0xE2  G=0xC7  space=0x40  *=0x5C  space=0x40
#define EBCDIC_M  0xD4U
#define EBCDIC_S  0xE2U
#define EBCDIC_G  0xC7U
#define EBCDIC_SP 0x40U
#define EBCDIC_ST 0x5CU  // asterisk '*'

/// @brief Flush the ASCII line_buf to DIAG 8.
///
///        Strategy: build a separate EBCDIC transmit buffer.
///        - Bytes 0-5: EBCDIC "MSG * " written directly as constants.
///        - Bytes 6-N: payload converted from ASCII to EBCDIC.
///        The ASCII line_buf is never modified, so there is no
///        double-conversion hazard and no need for a prefix_reset.
static void diag_flush(const char *line_buf, uint32_t line_len) {
    if (line_len <= MSG_PREFIX_LEN) return;

    // Separate transmit buffer — never aliased with line_buf.
    static char tx[DIAG_LINE_BUF_SIZE];

    // Write EBCDIC prefix directly; no table lookup needed.
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int diag_setup(void) {
    return 0;
}

void diag_putc(const char c) {
    // line_buf holds ASCII payload only; the prefix slot (bytes 0-5) is
    // reserved but never written — diag_flush fills it from constants.
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
