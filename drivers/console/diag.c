/// SPDX-License-Identifier: Apache-2.0
/// drivers/console/diag.c - DIAG 8 hypervisor console driver
///
/// @brief DIAG 8 provides direct output to Hercules/z/VM console.
///        With DIAG8CMD ENABLE: expects EBCDIC, goes to command line
///        Prefix with "MSG * " to display as message instead of command

#include <drivers/console/diag.h>
#include <zxfoundation/ebcdic.h>
#include <zxfoundation/types.h>

/// @brief DIAG 8 instruction - write string to hypervisor console
static inline void diag8_write(const char *addr, size_t len) {
    register uint64_t r2 __asm__("2") = (uint64_t)(uintptr_t)addr;
    register uint64_t r3 __asm__("3") = (uint64_t)len;
    
    __asm__ __volatile__(
        "diag %[r2], %[r3], 8\n"
        : /* no outputs */
        : [r2] "d" (r2), [r3] "d" (r3)
        : "memory"
    );
}

int diag_setup(void) {
    return 0;
}

int diag_write(const char *buf, size_t len) {
    if (len == 0)
        return 0;
    
    diag8_write(buf, len);
    return 0;
}

/// @brief Line buffer for DIAG 8 output
/// Format: "MSG * " + message content (in EBCDIC)
#define DIAG_LINE_BUF_SIZE 256
#define MSG_PREFIX "MSG * "
#define MSG_PREFIX_LEN 6

/// @brief Flush the current line buffer to DIAG 8
static void diag_flush(char *line_buf, size_t *line_len) {
    if (*line_len > MSG_PREFIX_LEN) {
        // Convert entire buffer (including prefix) to EBCDIC
        ascii_to_ebcdic_buf(line_buf, *line_len);
        diag8_write(line_buf, *line_len);
        
        // Reset buffer with ASCII "MSG * " prefix for next line
        for (size_t i = 0; i < MSG_PREFIX_LEN; i++) {
            line_buf[i] = MSG_PREFIX[i];
        }
        *line_len = MSG_PREFIX_LEN;
    }
}

void diag_putc(const char c) {
    static char   line_buf[DIAG_LINE_BUF_SIZE];
    static size_t line_len = MSG_PREFIX_LEN;
    
    // Initialize buffer with "MSG * " prefix on first call
    static int initialized = 0;
    if (!initialized) {
        for (size_t i = 0; i < MSG_PREFIX_LEN; i++) {
            line_buf[i] = MSG_PREFIX[i];
        }
        initialized = 1;
    }

    if (c == '\n') {
        // Flush the complete line with MSG prefix
        diag_flush(line_buf, &line_len);
    } else {
        line_buf[line_len++] = c;
        // Flush early if buffer is nearly full
        if (line_len >= DIAG_LINE_BUF_SIZE - 1) {
            diag_flush(line_buf, &line_len);
        }
    }
}

/// @brief Force flush any pending output (for panic/halt situations)
void diag_flush_all(void) {
    // This is a bit of a hack - we need access to the static buffer
    // For now, just send a newline to force a flush
    diag_putc('\n');
}
