// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/mmu.c

#include <arch/s390x/init/zxfl/zxfl.h>
#include <arch/s390x/init/zxfl/diag.h>

#define PAGE_SIZE 4096UL
#define VIRT_OFFSET 0xFFFF800000000000ULL

static uint64_t r1_table[2048] __attribute__((aligned(4096)));
static uint64_t r2_table[2048] __attribute__((aligned(4096)));
static uint64_t r3_table[2048] __attribute__((aligned(4096)));
static uint64_t seg_table[2048] __attribute__((aligned(4096)));
static uint64_t page_table[1024] __attribute__((aligned(4096)));

[[noreturn]] void zxfl_mmu_setup_and_jump(uint64_t entry, uint64_t boot_proto) {
    print("zxfl: setting up page tables\n");

    // Clear all tables (invalid bit set)
    for (int i = 0; i < 2048; i++) {
        r1_table[i] = 0x20;
        r2_table[i] = 0x20;
        r3_table[i] = 0x20;
        seg_table[i] = 0x20;
    }
    for (int i = 0; i < 1024; i++) page_table[i] = 0x20;

    // Region-1 table: TT=11b (R1), TL=11b (2048 entries)
    // Map index 0 (0..8PB) and index 2047 (top 8PB) to r2_table
    r1_table[0] = (uint64_t)(uintptr_t)r2_table | 0x0F;
    r1_table[2047] = (uint64_t)(uintptr_t)r2_table | 0x0F;

    // Region-2 table: TT=10b (R2), TL=11b
    // Map RX2=0 (for identity) and RX2=2016 (for VIRT_OFFSET) to r3_table
    r2_table[0] = (uint64_t)(uintptr_t)r3_table | 0x0B;
    r2_table[2016] = (uint64_t)(uintptr_t)r3_table | 0x0B;

    // Region-3 table: TT=01b (R3), TL=11b
    r3_table[0] = (uint64_t)(uintptr_t)seg_table | 0x07;

    // Segment table: TT=00b (Seg), TL=11b
    // Map first 4MB (identity and high-half) using page_table
    for (int i = 0; i < 4; i++) {
        seg_table[i] = (uint64_t)(uintptr_t)&page_table[i * 256] | 0x03;
    }

    // Page table entries: map virtual to physical 1:1 for the first 4MB
    for (uint64_t i = 0; i < 1024; i++) {
        page_table[i] = (i * PAGE_SIZE);
    }

    // ASCE: Region-1 table, DT=11b (R1), TL=11b
    uint64_t asce = (uint64_t)(uintptr_t)r1_table | 0x0F;
    __asm__ volatile("lctlg 1,1,%0" :: "Q"(asce));

    // Setup kernel PSW
    // Mask: bit 5=DAT, bit 13=Machine Check, bit 31=EA, bit 32=BA
    // Hex: 0x0404000180000000ULL
    static uint64_t kernel_psw[2] __attribute__((aligned(16)));
    kernel_psw[0] = 0x0404000180000000ULL;
    kernel_psw[1] = entry;                   // Kernel entry point

    // Load R2 with boot protocol pointer and jump
    __asm__ volatile(
        "lgr   %%r2, %[proto]\n"
        "lpswe %[psw]\n"
        :: [psw] "Q"(kernel_psw), [proto] "r"(boot_proto)
        : "r2"
    );
    __builtin_unreachable();
}
