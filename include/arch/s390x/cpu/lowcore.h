// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/lowcore.h
//
/// @brief z/Architecture lowcore layout (0x0000–0x1FFF).
///        Derived from s390-tools struct _lowcore and Linux struct lowcore.
///        Adapted for freestanding C23 — no Linux headers required.
///
///        The lowcore is 8 KB (0x2000).  Each CPU's lowcore is addressed
///        via the prefix register (SPX).  Physical address 0 = BSP lowcore.

#pragma once

#include <arch/s390x/cpu/psw.h>

#define LOWCORE_SIZE    0x2000UL

/// @brief Named lowcore field offsets for use in assembly (.S) files.
///        These MUST stay in sync with the C struct layout below.
///        Verified by the _Static_assert at the bottom of this file.
#define LC_RESTART_STACK    0x0360UL    ///< zx_lowcore_t::restart_stack
#define LC_KERNEL_ASCE      0x0388UL    ///< zx_lowcore_t::kernel_asce
#define LC_RETURN_PSW       0x0290UL    ///< zx_lowcore_t::return_psw
#define LC_KERNEL_STACK     0x0348UL    ///< zx_lowcore_t::kernel_stack

#ifndef __ASSEMBLER__

#include <zxfoundation/types.h>
#include <zxfoundation/zconfig.h>

typedef struct __attribute__((packed, aligned(8192))) {
    uint8_t     pad_0x0000[0x0014 - 0x0000];   /* 0x0000 */
    uint32_t    ipl_parmblock_ptr;              /* 0x0014 */
    uint8_t     pad_0x0018[0x0080 - 0x0018];   /* 0x0018 */

    /* Interrupt parameters */
    uint32_t    ext_params;                     /* 0x0080 */
    uint16_t    ext_cpu_addr;                   /* 0x0084 */
    uint16_t    ext_int_code;                   /* 0x0086 */
    uint16_t    svc_ilc;                        /* 0x0088 */
    uint16_t    svc_code;                       /* 0x008a */
    uint16_t    pgm_ilc;                        /* 0x008c */
    uint16_t    pgm_code;                       /* 0x008e */
    uint32_t    data_exc_code;                  /* 0x0090 */
    uint16_t    mon_class_num;                  /* 0x0094 */
    uint16_t    per_perc_atmid;                 /* 0x0096 */
    uint64_t    per_address;                    /* 0x0098 */
    uint8_t     exc_access_id;                  /* 0x00a0 */
    uint8_t     per_access_id;                  /* 0x00a1 */
    uint8_t     op_access_id;                   /* 0x00a2 */
    uint8_t     ar_access_id;                   /* 0x00a3 */
    uint8_t     pad_0x00a4[0x00a8 - 0x00a4];   /* 0x00a4 */
    uint64_t    trans_exc_code;                 /* 0x00a8 */
    uint64_t    monitor_code;                   /* 0x00b0 */
    uint16_t    subchannel_id;                  /* 0x00b8 */
    uint16_t    subchannel_nr;                  /* 0x00ba */
    uint32_t    io_int_parm;                    /* 0x00bc */
    uint32_t    io_int_word;                    /* 0x00c0 */
    uint8_t     pad_0x00c4[0x00c8 - 0x00c4];   /* 0x00c4 */
    uint32_t    stfl_fac_list;                  /* 0x00c8 */
    uint8_t     pad_0x00cc[0x00e8 - 0x00cc];   /* 0x00cc */
    uint64_t    mcck_interruption_code;         /* 0x00e8 */
    uint8_t     pad_0x00f0[0x00f4 - 0x00f0];   /* 0x00f0 */
    uint32_t    external_damage_code;           /* 0x00f4 */
    uint64_t    failing_storage_address;        /* 0x00f8 */
    uint8_t     pad_0x0100[0x0110 - 0x0100];   /* 0x0100 */
    uint64_t    breaking_event_addr;            /* 0x0110 */
    uint8_t     pad_0x0118[0x0120 - 0x0118];   /* 0x0118 */

    /* Old PSWs (saved by hardware on interrupt) */
    zx_psw_t    restart_old_psw;                /* 0x0120 */
    zx_psw_t    external_old_psw;               /* 0x0130 */
    zx_psw_t    svc_old_psw;                    /* 0x0140 */
    zx_psw_t    program_old_psw;                /* 0x0150 */
    zx_psw_t    mcck_old_psw;                   /* 0x0160 */
    zx_psw_t    io_old_psw;                     /* 0x0170 */
    uint8_t     pad_0x0180[0x01a0 - 0x0180];   /* 0x0180 */

    /* New PSWs (loaded by hardware on interrupt) — PoP §4.3.3 */
    zx_psw_t    restart_psw;                    /* 0x01a0  PSW_LC_RESTART  */
    zx_psw_t    external_new_psw;               /* 0x01b0  PSW_LC_EXTERNAL */
    zx_psw_t    svc_new_psw;                    /* 0x01c0  PSW_LC_SVC      */
    zx_psw_t    program_new_psw;                /* 0x01d0  PSW_LC_PROGRAM  */
    zx_psw_t    mcck_new_psw;                   /* 0x01e0  PSW_LC_MCCK     */
    zx_psw_t    io_new_psw;                     /* 0x01f0  PSW_LC_IO       */

    /* Save areas */
    uint64_t    save_area_sync[8];              /* 0x0200 */
    uint64_t    save_area_async[8];             /* 0x0240 */
    uint64_t    save_area_restart[1];           /* 0x0280 */
    uint64_t    pcpu;                           /* 0x0288 */

    /* Return PSWs */
    zx_psw_t    return_psw;                     /* 0x0290  LC_RETURN_PSW   */
    zx_psw_t    return_mcck_psw;                /* 0x02a0 */

    /* Timing */
    uint64_t    sys_enter_timer;                /* 0x02b0 */
    uint64_t    mcck_enter_timer;               /* 0x02b8 */
    uint64_t    exit_timer;                     /* 0x02c0 */
    uint64_t    user_timer;                     /* 0x02c8 */
    uint64_t    system_timer;                   /* 0x02d0 */
    uint64_t    steal_timer;                    /* 0x02d8 */
    uint64_t    last_update_timer;              /* 0x02e0 */
    uint64_t    last_update_clock;              /* 0x02e8 */
    uint64_t    int_clock;                      /* 0x02f0 */
    uint8_t     pad_0x02f8[0x0328 - 0x02f8];   /* 0x02f8 */
    uint64_t    clock_comparator;               /* 0x0328 */
    uint8_t     pad_0x0330[0x0340 - 0x0330];   /* 0x0330 */

    /* Current process */
    uint64_t    current_task;                   /* 0x0340 */
    uint64_t    kernel_stack;                   /* 0x0348  LC_KERNEL_STACK  */

    /* Stacks */
    uint64_t    async_stack;                    /* 0x0350 */
    uint64_t    nodat_stack;                    /* 0x0358 */
    uint64_t    restart_stack;                  /* 0x0360  LC_RESTART_STACK */
    uint64_t    mcck_stack;                     /* 0x0368 */

    /* Restart function/parameter */
    uint64_t    restart_fn;                     /* 0x0370 */
    uint64_t    restart_data;                   /* 0x0378 */
    uint32_t    restart_source;                 /* 0x0380 */
    uint32_t    restart_flags;                  /* 0x0384 */

    /* Address space */
    uint64_t    kernel_asce;                    /* 0x0388  LC_KERNEL_ASCE   */
    uint64_t    user_asce;                      /* 0x0390 */
    uint32_t    lpp;                            /* 0x0398 */
    uint32_t    current_pid;                    /* 0x039c */

    /* SMP info */
    uint32_t    cpu_nr;                         /* 0x03a0 */
    uint32_t    softirq_pending;                /* 0x03a4 */
    int32_t     preempt_count;                  /* 0x03a8 */
    uint32_t    spinlock_lockval;               /* 0x03ac */
    uint32_t    spinlock_index;                 /* 0x03b0 */
    uint8_t     pad_0x03b4[0x03b8 - 0x03b4];   /* 0x03b4 */
    uint64_t    percpu_offset;                  /* 0x03b8 */
    uint8_t     pad_0x03c0[0x0e00 - 0x03c0];   /* 0x03c0 */

    /* IPL/dump info */
    uint64_t    ipib;                           /* 0x0e00 */
    uint32_t    ipib_checksum;                  /* 0x0e08 */
    uint64_t    vmcore_info;                    /* 0x0e0c */
    uint8_t     pad_0x0e14[0x0e18 - 0x0e14];   /* 0x0e14 */
    uint64_t    os_info;                        /* 0x0e18 */
    uint8_t     pad_0x0e20[0x11b0 - 0x0e20];   /* 0x0e20 */

    uint64_t    mcesad;                         /* 0x11b0 */
    uint64_t    ext_params2;                    /* 0x11b8 */
    uint8_t     pad_0x11c0[0x1200 - 0x11c0];   /* 0x11c0 */

    /* CPU register save area */
    uint64_t    floating_pt_save_area[16];      /* 0x1200 */
    uint64_t    gpregs_save_area[16];           /* 0x1280 */
    zx_psw_t    psw_save_area;                  /* 0x1300 */
    uint8_t     pad_0x1310[0x1318 - 0x1310];   /* 0x1310 */
    uint32_t    prefixreg_save_area;            /* 0x1318 */
    uint32_t    fpt_creg_save_area;             /* 0x131c */
    uint8_t     pad_0x1320[0x1324 - 0x1320];   /* 0x1320 */
    uint32_t    tod_progreg_save_area;          /* 0x1324 */
    uint32_t    cpu_timer_save_area[2];         /* 0x1328 */
    uint32_t    clock_comp_save_area[2];        /* 0x1330 */
    uint64_t    last_break_save_area;           /* 0x1338 */
    uint32_t    access_regs_save_area[16];      /* 0x1340 */
    uint64_t    cregs_save_area[16];            /* 0x1380 */
    uint8_t     pad_0x1400[0x1800 - 0x1400];   /* 0x1400 */

    /* Transaction abort diagnostic block */
    uint64_t    pgm_tdb[32];                    /* 0x1800 */
    uint8_t     pad_0x1900[0x2000 - 0x1900];   /* 0x1900 */
} zx_lowcore_t;

_Static_assert(sizeof(zx_lowcore_t) == LOWCORE_SIZE, "zx_lowcore_t must be 8192 bytes");

/// @brief Compile-time verification that the named assembly offsets match the C struct.
_Static_assert(__builtin_offsetof(zx_lowcore_t, restart_stack) == LC_RESTART_STACK,
               "LC_RESTART_STACK mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, kernel_asce)   == LC_KERNEL_ASCE,
               "LC_KERNEL_ASCE mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, return_psw)    == LC_RETURN_PSW,
               "LC_RETURN_PSW mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, kernel_stack)  == LC_KERNEL_STACK,
               "LC_KERNEL_STACK mismatch");

/// @brief Access the BSP lowcore via absolute addressing (DAT off).
static inline zx_lowcore_t *zx_lowcore_raw(void) {
    return (zx_lowcore_t *)(uintptr_t)0x0;
}

/// @brief Access the BSP lowcore via HHDM (DAT on).
static inline zx_lowcore_t *zx_lowcore(void) {
    return (zx_lowcore_t *)(uintptr_t)hhdm_phys_to_virt(0x0);
}

/// @brief Access an AP's lowcore by its physical prefix address via HHDM.
static inline zx_lowcore_t *zx_lowcore_of(uint64_t prefix_phys) {
    return (zx_lowcore_t *)(uintptr_t)hhdm_phys_to_virt(prefix_phys);
}

/// @brief Write the restart new PSW into a lowcore for AP bringup.
///
///        Uses PSW_MASK_KERNEL (DAT off, 64-bit, all interrupts disabled).
///        The address MUST be the *physical* address of ap_entry because DAT
///        is not yet active on the AP when the restart PSW is loaded.
///
/// @param lc         AP lowcore (HHDM-mapped pointer).
/// @param entry_phys Physical address of ap_entry.
static inline void lc_set_restart_psw(zx_lowcore_t *lc, uint64_t entry_phys) {
    lc->restart_psw.mask = PSW_MASK_KERNEL;
    lc->restart_psw.addr = entry_phys;
}

/// @brief Store the kernel ASCE into an AP's lowcore so ap_entry can load it.
/// @param lc   AP lowcore (HHDM-mapped pointer).
/// @param asce Kernel ASCE value (from CR1 on the BSP after mmu_init()).
static inline void lc_set_kernel_asce(zx_lowcore_t *lc, uint64_t asce) {
    lc->kernel_asce = asce;
}

/// @brief Install the four interrupt handler new PSWs into any lowcore.
///
///        Must be called for the BSP lowcore (in zx_lowcore_setup_late) and
///        for every AP lowcore before SIGP RESTART is issued.  Each CPU's
///        prefix register points to its own lowcore; the hardware reads the
///        new PSW from that CPU's lowcore, not the BSP's.
///
/// @param lc  HHDM-mapped pointer to the target lowcore.
void lc_install_handler_psws(zx_lowcore_t *lc);

/// @brief Install disabled-wait PSWs into all six new PSW slots at physical
///        lowcore offsets.  Called pre-DAT (DAT off) during early boot.
///        This is the only safe lowcore write path before the HHDM is live.
void zx_lowcore_setup_pre_dat(void);

/// @brief Install live handler PSWs into the BSP's HHDM-mapped lowcore.
///        Called as the very first action in zxfoundation_global_initialize(),
///        after the HHDM is active.  Replaces the disabled-wait PSWs installed
///        by zx_lowcore_setup_pre_dat() with real handler entry points.
void zx_lowcore_setup_late(void);

#endif /* __ASSEMBLER__ */
