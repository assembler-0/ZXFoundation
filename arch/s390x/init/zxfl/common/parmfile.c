// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/parmfile.c
//
/// @brief Freestanding syssize= parser for the ZXFL bootloader.
///
///        No libc, no kernel headers — only zxfl string primitives.

#include <arch/s390x/init/zxfl/parmfile.h>

static inline bool is_digit(char c) {
    return (uint8_t)(c - '0') <= 9U;
}

static inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

uint64_t parse_syssize(const char *cmdline, uint32_t len) {
    if (!cmdline || len == 0U)
        return 0;

    // "syssize=" is 8 characters.
    static const char key[] = "syssize=";
    const uint32_t key_len  = 8U;

    uint32_t i = 0;
    while (i + key_len <= len) {
        // Token must start at the beginning of the string or after whitespace.
        if (i > 0 && !is_space(cmdline[i - 1U])) {
            i++;
            continue;
        }

        // Match "syssize=" at position i.
        bool match = true;
        for (uint32_t k = 0; k < key_len; k++) {
            if (cmdline[i + k] != key[k]) {
                match = false;
                break;
            }
        }
        if (!match) {
            i++;
            continue;
        }

        // Advance past "syssize=".
        i += key_len;

        // Parse decimal integer.
        if (i >= len || !is_digit(cmdline[i]))
            return 0;

        uint64_t value = 0;
        while (i < len && is_digit(cmdline[i])) {
            // Overflow guard: 512*1024 MB fits in 32 bits; reject anything larger.
            if (value > PARMFILE_SYSSIZE_MAX_MB) return 0;
            value = value * 10U + (uint64_t)(cmdline[i] - '0');
            i++;
        }

        // Optional suffix: M (megabytes, default) or G (gigabytes).
        uint64_t multiplier = 1UL << 20; // default: MB → bytes
        if (i < len) {
            if (cmdline[i] == 'G' || cmdline[i] == 'g') {
                multiplier = 1UL << 30;
                // Convert G to MB for range check.
                if (value > PARMFILE_SYSSIZE_MAX_MB / 1024UL) return 0;
                value *= 1024UL; // now in MB
            } else if (cmdline[i] == 'M' || cmdline[i] == 'm') {
                // already in MB
            } else if (!is_space(cmdline[i]) && cmdline[i] != '\0') {
                // Unexpected character after digits — malformed.
                return 0;
            }
        }

        if (value < PARMFILE_SYSSIZE_MIN_MB || value > PARMFILE_SYSSIZE_MAX_MB)
            return 0;

        (void)multiplier; // suppress unused warning — always 1<<20 after G conversion
        return value << 20; // value (MB) * 2^20 = bytes
    }

    return 0; // not found
}
