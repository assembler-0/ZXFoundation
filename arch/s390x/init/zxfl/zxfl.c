// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/zxfl.c - ZXFoundation Bootloader Core

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/diag.h>
#include <arch/s390x/init/zxfl/ebcdic.h>

// Standard CCW Format 1
typedef struct __attribute__((packed, aligned(8))) {
    uint8_t cmd;
    uint8_t flags;
    uint16_t count;
    uint32_t cda;
} ccw1_t;

// ORB
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t intparm;
    uint32_t flags;
    uint32_t cpa;
    uint8_t  amsw[4];
    uint32_t reserved[4];
} orb_t;

// Seek argument
typedef struct __attribute__((packed)) {
    uint16_t reserved;
    uint16_t cyl;
    uint16_t head;
} dasd_seek_arg_t;

// Search argument
typedef struct __attribute__((packed)) {
    uint16_t cyl;
    uint16_t head;
    uint8_t  rec;
} dasd_search_arg_t;

// ELF64 Headers
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

// Basic static structures in BSS
static zxfl_boot_protocol_t boot_protocol;
static zxfl_mmap_entry_t memory_map[16];
static char cmdline_buf[256] = "console=ttyS0";

// Fast BSS clear using mvcl
void zxfl_clear_bss(uint32_t bss_start, uint32_t bss_size) {
    register uint32_t r0 __asm__("0") = 0; // Padding char 0
    register uint32_t r2 __asm__("2") = (uint32_t)bss_start;
    register uint32_t r3 __asm__("3") = (uint32_t)bss_size;
    register uint32_t r4 __asm__("4") = (uint32_t)bss_start;
    register uint32_t r5 __asm__("5") = 0;

    __asm__ volatile (
        "1: mvcl %0, %2\n"
        "   jo   1b\n"
        : "+d" (r2), "+d" (r3), "+d" (r4), "+d" (r5)
        : "d" (r0)
        : "cc", "memory"
    );
}

static inline int memcmp(const void *s1, const void *s2, uint32_t n) {
    const uint8_t *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

static inline void memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static inline void memset(void *s, int c, uint32_t n) {
    uint8_t *p = s;
    while (n--) *p++ = c;
}

// Low-level IO: SIO/SSCH to the IPL device
static int do_sync_io(uint32_t schid, ccw1_t *ccw) {
    orb_t orb = {0};
    orb.flags = 0x0080FF00; // Format 1 CCWs, LPM=0xFF
    orb.cpa = (uint32_t)(unsigned long)ccw;

    register uint16_t _schid __asm__("1") = (uint16_t)schid;
    int cc;

    __asm__ volatile (
        "ssch %1\n"
        "ipm %0\n"
        "srl %0,28\n"
        : "=d" (cc)
        : "m" (orb), "d" (_schid)
        : "cc", "memory"
    );

    if (cc != 0) return -1;

    // Wait for interrupt
    uint32_t irb[24];
    do {
        __asm__ volatile (
            "tsch %1\n"
            "ipm %0\n"
            "srl %0,28\n"
            : "=d" (cc)
            : "m" (irb), "d" (_schid)
            : "cc", "memory"
        );
    } while (cc == 1);

    // Check device status in IRB (irb[2] contains SCSW word 2: Status & Count)
    uint32_t status = irb[2];
    if (status & 0x02000000) return -1; // Unit Check (Device error)
    if (status & 0x01000000) return -1; // Unit Exception
    // Channel status is in bits 8-15 (0x00FF0000). Any non-zero usually means an error,
    // except perhaps PCI or IL, but we use SLI so IL shouldn't happen.
    if (status & 0x00FF0000) return -1; // Channel status error

    return 0;
}

// Read a single record from DASD
static int dasd_read_record(uint32_t schid, uint16_t cyl, uint16_t head, uint8_t rec, uint8_t rd_cmd, void* buf, uint32_t len) {
    static dasd_seek_arg_t seek_arg;
    static dasd_search_arg_t search_arg;
    static ccw1_t chain[4] __attribute__((aligned(8)));

    seek_arg.reserved = 0; 
    seek_arg.cyl = cyl; 
    seek_arg.head = head;
    search_arg.cyl = cyl; 
    search_arg.head = head; 
    search_arg.rec = rec;

    chain[0].cmd = 0x07; // SEEK
    chain[0].flags = 0x60; // CC | SLI
    chain[0].count = 6;
    chain[0].cda = (uint32_t)(unsigned long)&seek_arg;

    chain[1].cmd = 0x31; // SEARCH ID EQ
    chain[1].flags = 0x60;
    chain[1].count = 5;
    chain[1].cda = (uint32_t)(unsigned long)&search_arg;

    chain[2].cmd = 0x08; // TIC
    chain[2].flags = 0x00;
    chain[2].count = 0;
    chain[2].cda = (uint32_t)(unsigned long)&chain[1];

    chain[3].cmd = rd_cmd; // READ DATA or READ KEY AND DATA
    chain[3].flags = 0x20; // SLI (Suppress Length Indication)
    chain[3].count = len;
    chain[3].cda = (uint32_t)(unsigned long)buf;

    return do_sync_io(schid, chain);
}

// Find dataset (e.g. "SYS1.NUCLEUS") in VTOC on Track 14 (Cyl 0, Head 14)
int zxfl_find_dataset(uint32_t schid, const char* dsname, uint16_t* out_cyl, uint16_t* out_head) {
    uint8_t dscb[140];
    uint8_t target_name[44];
    memset(target_name, 0x40, 44); // 0x40 is EBCDIC space
    for (int i = 0; dsname[i] && i < 44; i++) {
        target_name[i] = ascii_to_ebcdic((uint8_t)dsname[i]);
    }

    // Read records on Cyl 0, Head 14 (VTOC) - try up to 100 records
    for (uint8_t rec = 1; rec <= 100; rec++) {
        if (dasd_read_record(schid, 0, 14, rec, 0x0E, dscb, sizeof(dscb)) < 0) continue;
        
        // DSCB Format 1: First 44 bytes are name, byte 44 is format identifier
        if (dscb[44] == '1' || dscb[44] == 0xF1) { // 0xF1 is EBCDIC '1'
            if (memcmp(dscb, target_name, 44) == 0) {
                // Found it! Extent descriptor 1 is at offset 105
                // Format: 105=type, 106=seq, 107-108=start_cyl, 109-110=start_head
                // Note: CCHH is big-endian on mainframe
                *out_cyl = (dscb[107] << 8) | dscb[108];
                *out_head = (dscb[109] << 8) | dscb[110];
                return 0;
            }
        }
    }
    return -1;
}

// Buffer to read blocks from DASD
static uint8_t disk_buf[4096] __attribute__((aligned(4096)));

// Load Kernel ELF and get entry. Returns the kernel size footprint.
uint32_t zxfl_load_elf64_kernel(uint32_t schid, uint16_t cyl, uint16_t head, uint64_t* out_entry) {
    // Read the first block to get ELF header
    if (dasd_read_record(schid, cyl, head, 1, 0x06, disk_buf, 4096) < 0) {
        return -1;
    }

    elf64_ehdr_t *ehdr = (elf64_ehdr_t*)disk_buf;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E') {
        // Fallback: flat binary. Just load records starting at 0x10000 (64KB).
        // We will assume it's small enough for now.
        for (int i=0; i<200; i++) {
            dasd_read_record(schid, cyl, head, i+1, 0x06, (void*)(uintptr_t)(0x10000 + i*4096), 4096);
        }
        *out_entry = 0x10400;
        return 0x200000; // Just a dummy size if fallback
    }

    *out_entry = ehdr->e_entry;

    uint32_t max_addr = 0x10000;

    // Parse program headers
    elf64_phdr_t *phdr = (elf64_phdr_t*)(disk_buf + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            uint64_t vaddr = phdr[i].p_vaddr;
            uint64_t offset = phdr[i].p_offset;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t memsz = phdr[i].p_memsz;

            // Clear the entire memory range first (BSS clearing)
            zxfl_clear_bss(vaddr, memsz);

            // Read the file data
            uint32_t rec_start = (offset / 4096) + 1; // 1-indexed records
            uint32_t rec_count = (filesz + 4095) / 4096;

            for (uint32_t r = 0; r < rec_count; r++) {
                // We read directly into the virtual address (which equals physical address)
                int err = dasd_read_record(schid, cyl, head, rec_start + r, 0x06, (void*)(uintptr_t)((uint32_t)vaddr + r*4096), 4096);
                if (err < 0) {
                    print_msg("zxfl: IO err on load\n");
                }
            }
            if ((vaddr + memsz) > max_addr) {
                max_addr = vaddr + memsz;
            }
        }
    }
    
    return max_addr - 0x10000; // Total loaded size
}

extern uint32_t detect_memory_size(void);

[[noreturn]] void zxfl_jump_to_kernel(zxfl_boot_protocol_t* protocol, uint64_t entry) {
    register uint32_t r2 __asm__("2") = (uint32_t)(unsigned long)protocol;
    register uint32_t r8 __asm__("8") = (uint32_t)entry;
    
    __asm__ volatile (
        ".machinemode zarch\n"
        // Switch to 64-bit z/Arch mode
        "stfl 0\n"
        "lhi 1, 1\n"
        "sigp 1, 0, 0x12\n" // SIGP set architecture to z/Arch
        "sam64\n"
        
        // Setup Control Registers for 64-bit
        "larl 1, .Lcregs\n"
        "lctlg 0, 15, 0(1)\n"
        
        // Jump to kernel, clearing upper half of R2 and R8 implicitly or explicitly
        "llgfr 2, 2\n"
        "llgfr 8, 8\n"
        "bsm 0, 8\n"
        
        ".align 8\n"
        ".Lcregs: .quad 0x0000000000000000\n"
        "         .skip 112\n"
        : 
        : "r" (r2), "r" (r8)
        : "memory", "1"
    );
    while(1);
}

void zxfl_main(void) {
    uint32_t ipl_schid;
    __asm__ volatile("l %0, 0xb8" : "=d" (ipl_schid));

    boot_protocol.magic = ZXFL_MAGIC;
    boot_protocol.version = ZXFL_VERSION_1;
    boot_protocol.ipl_schid = ipl_schid;
    boot_protocol.ipl_dev_type = 0x3390;
    print_msg("zxfl: ZXFoundationLoader 26h1 copyright (c) 2026 assembler-0 All rights reserved\n");

    uint16_t cyl = 1, head = 0; // Track 15 = Cyl 1, Head 0 (15 heads per cyl on 3390)
    if (zxfl_find_dataset(ipl_schid, "SYS1.NUCLEUS", &cyl, &head) < 0) {
        print_msg("zxfl: VTOC not found, using 1/0\n");
    } else {
        print_msg("zxfl: Found SYS1.NUCLEUS\n");
    }

    uint64_t entry_point = 0x10400;
    print_msg("zxfl: Loading kernel\n");
    uint32_t ksize = zxfl_load_elf64_kernel(ipl_schid, cyl, head, &entry_point);

    if (ksize == (uint32_t)-1) {
        print_msg("zxfl: Kernel load failed. aborting\n");
        return;
    }

    boot_protocol.kernel_start = 0x10000;
    boot_protocol.kernel_size = ksize;
    boot_protocol.loader_start = 0;
    boot_protocol.loader_size = 0x10000; // Up to 64KB is reserved for loader/lowcore
    
    print_msg("zxfl: Detecting mem\n");
    uint64_t total_mem = detect_memory_size();
    
    boot_protocol.mmap_count = 2;
    // Map 0: Bootloader and Lowcore
    memory_map[0].start = 0;
    memory_map[0].size = 0x10000; 
    memory_map[0].type = ZXFL_MEM_LOADER;
    
    // Map 1: Available memory above kernel (kernel occupies 0x10000 to 0x10000+ksize)
    memory_map[1].start = 0x10000 + ksize;
    memory_map[1].size = total_mem - (0x10000 + ksize); 
    memory_map[1].type = ZXFL_MEM_AVAILABLE;
    
    boot_protocol.mmap = memory_map;
    boot_protocol.cmdline = cmdline_buf;

    print_msg("zxfl: Launching kernel\n");
    zxfl_jump_to_kernel(&boot_protocol, entry_point);
}
