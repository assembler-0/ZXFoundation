// SPDX-License-Identifier: Apache-2.0
// tools/gen_checksums.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <crypto/sha256.h>

#define PT_LOAD          1
#define ELFMAG           "\x7f""ELF"
#define ZXVL_PFLAGS_CKSUM 0x00200004U   /* must match link.ld checksums_seg FLAGS */

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static void wbe32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void wbe64(uint8_t *p, uint64_t v) { wbe32(p,(uint32_t)(v>>32)); wbe32(p+4,(uint32_t)v); }

#define ZXVL_CKSUM_MAGIC        0x5A58564CU
#define ZXVL_CKSUM_VERSION      0x00000001U
#define ZXVL_CKSUM_ALGO_SHA256  0x00000001U
#define ZXVL_CKSUM_MAX_ENTRIES  16U
#define SHA256_SIZE             32
#define HHDM_BASE               UINT64_C(0xFFFF800000000000)

typedef struct __attribute__((packed)) {
    uint64_t phys_start, size;
    uint8_t  digest[SHA256_SIZE];
} entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic, version, algo, count;
    entry_t  entries[ZXVL_CKSUM_MAX_ENTRIES];
} table_t;

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: gen_checksums <core.zxfoundation.nucleus>\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "r+b");
    if (!f) { perror(argv[1]); return 1; }

    /* ------------------------------------------------------------------ */
    /* 1. Read and validate the ELF64 header (64 bytes).                   */
    /* ------------------------------------------------------------------ */
    uint8_t ehdr[64];
    if (fread(ehdr, 1, 64, f) != 64 || memcmp(ehdr, ELFMAG, 4) != 0) {
        fprintf(stderr, "gen_checksums: not a valid ELF file\n");
        fclose(f); return 1;
    }
    if (ehdr[4] != 2) { /* EI_CLASS must be ELFCLASS64 */
        fprintf(stderr, "gen_checksums: not an ELF64 file\n");
        fclose(f); return 1;
    }

    const uint64_t phoff     = be64(ehdr + 32);
    const uint64_t shoff     = be64(ehdr + 40);
    const uint16_t phentsz   = be16(ehdr + 54);
    const uint16_t phnum     = be16(ehdr + 56);
    const uint16_t shentsz   = be16(ehdr + 58);
    const uint16_t shnum     = be16(ehdr + 60);
    const uint16_t shstrndx  = be16(ehdr + 62);

    if (phentsz == 0 || shentsz == 0 || phnum == 0 || shnum == 0) {
        fprintf(stderr, "gen_checksums: degenerate ELF (no phdrs or shdrs)\n");
        fclose(f); return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Load section headers and locate .zxvl_checksums file offset.    */
    /*    Used only to find where to write the table.                     */
    /* ------------------------------------------------------------------ */
    uint8_t *shdrs = malloc((size_t)shnum * shentsz);
    if (!shdrs) { perror("malloc"); fclose(f); return 1; }
    fseek(f, (long)shoff, SEEK_SET);
    fread(shdrs, shentsz, shnum, f);

    const uint8_t *shstr_hdr = shdrs + (size_t)shstrndx * shentsz;
    const uint64_t shstr_off = be64(shstr_hdr + 24);
    const uint64_t shstr_sz  = be64(shstr_hdr + 32);

    char *shstrtab = malloc(shstr_sz + 1);
    if (!shstrtab) { perror("malloc"); free(shdrs); fclose(f); return 1; }
    fseek(f, (long)shstr_off, SEEK_SET);
    fread(shstrtab, 1, shstr_sz, f);
    shstrtab[shstr_sz] = '\0';

    long cksum_file_off = -1;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = shdrs + (size_t)i * shentsz;
        const uint32_t name_idx = be32(sh);
        if (name_idx < shstr_sz && strcmp(shstrtab + name_idx, ".zxvl_checksums") == 0) {
            cksum_file_off = (long)be64(sh + 24);
            break;
        }
    }
    free(shstrtab);
    free(shdrs);

    if (cksum_file_off < 0) {
        fprintf(stderr, "gen_checksums: .zxvl_checksums section not found\n");
        fclose(f); return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 3. Load program headers and collect PT_LOAD segments to hash.      */
    /* ------------------------------------------------------------------ */
    uint8_t *phdrs = malloc((size_t)phnum * phentsz);
    if (!phdrs) { perror("malloc"); fclose(f); return 1; }
    fseek(f, (long)phoff, SEEK_SET);
    fread(phdrs, phentsz, phnum, f);

    typedef struct { uint64_t file_off, paddr, filesz; } seg_t;
    seg_t segs[ZXVL_CKSUM_MAX_ENTRIES];
    uint32_t nseg = 0;

    for (int i = 0; i < phnum && nseg < ZXVL_CKSUM_MAX_ENTRIES; i++) {
        const uint8_t *ph = phdrs + (size_t)i * phentsz;
        if (be32(ph) != PT_LOAD) continue;

        const uint64_t p_flags  = be32(ph + 4);
        const uint64_t p_offset = be64(ph + 8);
        const uint64_t p_filesz = be64(ph + 32);
        if (p_filesz == 0) continue;

        // Skip the checksums segment itself — identified by its unique p_flags
        // fingerprint.  Hashing the table while building it would be circular.
        if (p_flags == ZXVL_PFLAGS_CKSUM) {
            printf("gen_checksums: skipping checksums segment (file_off=0x%llx..0x%llx)\n",
                   (unsigned long long)p_offset,
                   (unsigned long long)(p_offset + p_filesz - 1));
            continue;
        }

        segs[nseg].file_off = p_offset;
        segs[nseg].paddr    = be64(ph + 24);
        segs[nseg].filesz   = p_filesz;
        nseg++;
    }
    free(phdrs);

    if (nseg == 0) {
        fprintf(stderr, "gen_checksums: no hashable PT_LOAD segments found\n");
        fclose(f); return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Compute SHA-256 for each segment.                               */
    /* ------------------------------------------------------------------ */
    table_t tbl;
    memset(&tbl, 0, sizeof(tbl));
    wbe32((uint8_t *)&tbl.magic,   ZXVL_CKSUM_MAGIC);
    wbe32((uint8_t *)&tbl.version, ZXVL_CKSUM_VERSION);
    wbe32((uint8_t *)&tbl.algo,    ZXVL_CKSUM_ALGO_SHA256);
    wbe32((uint8_t *)&tbl.count,   nseg);

    for (uint32_t i = 0; i < nseg; i++) {
        uint64_t phys = segs[i].paddr;
        if (phys >= HHDM_BASE) phys -= HHDM_BASE;
        wbe64((uint8_t *)&tbl.entries[i].phys_start, phys);
        wbe64((uint8_t *)&tbl.entries[i].size,       segs[i].filesz);

        uint8_t *seg_data = malloc(segs[i].filesz);
        if (!seg_data) { perror("malloc"); fclose(f); return 1; }

        fseek(f, (long)segs[i].file_off, SEEK_SET);
        if (fread(seg_data, 1, segs[i].filesz, f) != segs[i].filesz) {
            fprintf(stderr, "gen_checksums: short read on segment %u\n", i);
            free(seg_data); fclose(f); return 1;
        }

        zxfl_sha256_ctx_t ctx;
        zxfl_sha256_init(&ctx);
        zxfl_sha256_update(&ctx, seg_data, segs[i].filesz);
        zxfl_sha256_final(&ctx, tbl.entries[i].digest);
        free(seg_data);

        printf("gen_checksums: seg %u  phys=0x%016llx  size=%llu  sha256=",
               i, (unsigned long long)phys, (unsigned long long)segs[i].filesz);
        for (int j = 0; j < SHA256_SIZE; j++)
            printf("%02x", tbl.entries[i].digest[j]);
        printf("\n");
    }

    /* ------------------------------------------------------------------ */
    /* 5. Write the completed table into .zxvl_checksums — ONE write,      */
    /*    after all digests are final.                                     */
    /* ------------------------------------------------------------------ */
    fseek(f, cksum_file_off, SEEK_SET);
    if (fwrite(&tbl, 1, sizeof(tbl), f) != sizeof(tbl)) {
        fprintf(stderr, "gen_checksums: write failed: %s\n", strerror(errno));
        fclose(f); return 1;
    }
    fflush(f);
    fclose(f);

    printf("gen_checksums: patched %u segment(s) into .zxvl_checksums\n", nseg);
    return 0;
}
