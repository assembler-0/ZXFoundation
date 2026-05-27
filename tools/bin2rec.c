// SPDX-License-Identifier: Apache-2.0
// bin2rec.c — convert a flat binary into an card-format text deck
//
// Record layout (80 bytes per card):
//
//  Offset  Len  Field
//  ------  ---  -----
//    0      1   Flag byte (0x02 = data record; ignored by most firmware
//                          but must be present)
//    1      3   EBCDIC "TXT" or "END"
//    4      1   Reserved (zero)
//    5      3   Load address, big-endian 24-bit (bits 8..31 of uint32)
//    8      2   Reserved (zero)
//   10      2   Data length in this record, big-endian (always 56 for TXT)
//   12      4   Reserved (zero)
//   16     56   Data payload (zero-padded on the last record)
//   72      8   Reserved (zero)
//  ------  ---
//   80      total
//
// The END record is a full 80-byte card whose flag/type fields are set to
// END and whose address field carries the entry-point address so that
// firmware implementations that branch via the END record address work
// correctly.  All other fields in the END card are zero.
//
// Bugs fixed vs. the original flatboot implementation:
//   1. feof() was tested AFTER fread(), causing one spurious zero TXT record
//      at EOF on exact-multiple-of-56 inputs.
//   2. The END record was emitted inside the main loop after a stale fread(),
//      potentially writing uninitialized data as a TXT record first.
//   3. Short final reads (file not a multiple of 56 bytes) were not zero-padded,
//      so the last TXT record contained garbage in its tail bytes.
//   4. The END record's load address was always zero; it now carries the
//      caller-supplied entry point.
//   5. No error checking on fread/fwrite; errors are now detected and reported.
//   6. MAX_REC_SIZE silently truncated large inputs; now treated as a hard error.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EBCDIC_TXT      "\xE3\xE7\xE3"     // 'T', 'X', 'T' in EBCDIC
#define EBCDIC_END      "\xC5\xD5\xC4"     // 'E', 'N', 'D' in EBCDIC

#define CARD_WIDTH      80u
#define DATA_PER_CARD   56u                 // bytes of payload per TXT card
#define MAX_LOAD_ADDR   0x00FFFFFFu         // 24-bit address ceiling

// IPL text records may not describe more than 32 KB of loaded data.
// This is a firmware-level constraint, not a disk constraint.
#define MAX_IPL_BYTES   32768u

// Write exactly `n` bytes from `buf` to `out`; return 0 on success, -1 on error.
static int checked_write(FILE *out, const void *buf, size_t n)
{
    if (fwrite(buf, 1, n, out) != n) {
        fprintf(stderr, "bin2rec: write error\n");
        return -1;
    }
    return 0;
}

// Emit one 80-byte card to `out`.
// `type`    — EBCDIC_TXT or EBCDIC_END (3 bytes, not NUL-terminated)
// `addr`    — 24-bit load / entry address
// `datalen` — number of valid bytes in `data` (0..56); rest is zero-padded
// `data`    — payload (may be NULL when datalen == 0, i.e. for END cards)
static int emit_card(FILE *out,
                     const char *type,
                     uint32_t    addr,
                     size_t      datalen,
                     const void *data) {
    unsigned char card[CARD_WIDTH];
    memset(card, 0, sizeof(card));
    card[0] = 0x02;

    memcpy(&card[1], type, 3);

    card[5] = (addr >> 16) & 0xFF;
    card[6] = (addr >>  8) & 0xFF;
    card[7] = (addr      ) & 0xFF;

    if (datalen > 0) {
        uint16_t be_len = (uint16_t)datalen;
        card[10] = (be_len >> 8) & 0xFF;
        card[11] = (be_len     ) & 0xFF;
    }

    if (datalen > 0 && data != NULL)
        memcpy(&card[16], data, datalen);

    return checked_write(out, card, CARD_WIDTH);
}

// bin2rec — convert the binary stream `in` into an IPL1 card deck on `out`.
// Returns 0 on success, -1 on any error.
int bin2rec(FILE *in, FILE *out, uint32_t entry)
{
    uint32_t addr = 0;

    if (entry > MAX_LOAD_ADDR) {
        fprintf(stderr, "bin2rec: entry point 0x%08X exceeds 24-bit address space\n", entry);
        return -1;
    }

    for (;;) {
        unsigned char buf[DATA_PER_CARD];
        memset(buf, 0, sizeof(buf));        // zero-pad so short reads are clean

        size_t got = fread(buf, 1, DATA_PER_CARD, in);

        if (got == 0) {
            if (ferror(in)) {
                fprintf(stderr, "bin2rec: read error\n");
                return -1;
            }
            break;
        }

        if (addr + got > MAX_IPL_BYTES) {
            fprintf(stderr,
                    "bin2rec: input exceeds maximum IPL1 size of %u bytes "
                    "(currently at offset 0x%05X)\n",
                    MAX_IPL_BYTES, addr);
            return -1;
        }

        if (addr > MAX_LOAD_ADDR) {
            fprintf(stderr,
                    "bin2rec: load address 0x%08X exceeds 24-bit limit\n", addr);
            return -1;
        }

        if (emit_card(out, EBCDIC_TXT, addr, DATA_PER_CARD, buf) != 0)
            return -1;

        addr += (uint32_t)got;

        if (got < DATA_PER_CARD) {
            if (ferror(in)) {
                fprintf(stderr, "bin2rec: read error\n");
                return -1;
            }
            break;
        }
    }

    // Always emit the END card last.
    if (emit_card(out, EBCDIC_END, entry, 0, NULL) != 0)
        return -1;

    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <infile> <outfile> [entry_address]\n"
            "\n"
            "  infile        flat binary to convert (e.g. loader00.bin)\n"
            "  outfile       IPL1 card-format output (e.g. loader00.sys)\n"
            "  entry_address optional hex entry point for the END card\n"
            "                (default: 0x000000, i.e. absolute address 0)\n"
            "\n"
            "The input may be at most %u bytes.\n"
            "Each output record is exactly 80 bytes (56 bytes of payload).\n",
            prog, MAX_IPL_BYTES);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    uint32_t entry = 0;
    if (argc == 4) {
        char *end;
        unsigned long v = strtoul(argv[3], &end, 16);
        if (*end != '\0' || v > MAX_LOAD_ADDR) {
            fprintf(stderr, "bin2rec: invalid entry address '%s'\n", argv[3]);
            return EXIT_FAILURE;
        }
        entry = (uint32_t)v;
    }

    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        fclose(in);
        return EXIT_FAILURE;
    }

    int rc = bin2rec(in, out, entry);

    if (fclose(out) != 0 && rc == 0) {
        perror(argv[2]);
        rc = -1;
    }
    fclose(in);

    if (rc != 0) {
        remove(argv[2]);
        return EXIT_FAILURE;
    }

    printf("bin2rec: successfully converted %s to %s (entry 0x%06X)\n",
          argv[1], argv[2], entry);

    return EXIT_SUCCESS;
}