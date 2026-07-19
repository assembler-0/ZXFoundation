// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/lowcore.h
//
/// @brief z/Architecture lowcore layout (0x0000–0x1FFF).

#pragma once

#include <arch/s390x/init/zxfl/psw.h>

#define LOWCORE_SIZE    0x2000UL

/// @brief Named lowcore field offsets for use in assembly (.S) files.
///        These MUST stay in sync with the C struct layout below.
///        Verified by the _Static_assert at the bottom of this file.
#define LC_ASYNC_STACK      0x0350UL    ///< zx_lowcore_t::async_stack
#define LC_NODAT_STACK      0x0358UL    ///< zx_lowcore_t::nodat_stack
#define LC_RESTART_STACK    0x0360UL    ///< zx_lowcore_t::restart_stack
#define LC_MCCK_STACK       0x0368UL    ///< zx_lowcore_t::mcck_stack
#define LC_RESTART_FN       0x0370UL    ///< zx_lowcore_t::restart_fn
#define LC_RESTART_DATA     0x0378UL    ///< zx_lowcore_t::restart_data
#define LC_RESTART_SOURCE   0x0380UL    ///< zx_lowcore_t::restart_source
#define LC_RESTART_FLAGS    0x0384UL    ///< zx_lowcore_t::restart_flags
#define LC_PGM_CODE         0x008EUL    ///< zx_lowcore_t::pgm_code
#define LC_SVC_CODE         0x008AUL    ///< zx_lowcore_t::svc_code
#define LC_PSW_FLAG         0x0080UL    ///< zx_lowcore_t::psw_flag
#define LC_KERNEL_ASCE      0x0388UL    ///< zx_lowcore_t::kernel_asce
#define LC_RETURN_PSW       0x0290UL    ///< zx_lowcore_t::return_psw
#define LC_PSW_TEMP         0x0248UL    ///< Temporary PSW storage used by RESTORE_FRAME (unused padding after stack_canary)
#define LC_SCRATCH0         0x0260UL    ///< Temporary scratch for SAVE_FRAME / SAVE_NUCLEUS_FRAME (pad_0x0258)
#define LC_CURRENT_DOMAIN   0x0340UL    ///< zx_lowcore_t::current_domain
#define LC_KERNEL_STACK     0x0348UL    ///< zx_lowcore_t::kernel_stack
#define LC_AP_CR0           0x0330UL    ///< zx_lowcore_t::ap_cr0
#define LC_AP_CR13          0x0338UL    ///< zx_lowcore_t::ap_cr13
#define LC_PREEMPT_COUNT    0x0430UL    ///< arch::s390x::cpu::percpu::preempt_count
#define LC_IRQ_NESTING      0x0438UL    ///< arch::s390x::cpu::percpu::irq_nesting
#define LC_NUCLEUS_NESTING  0x043CUL    ///< arch::s390x::cpu::percpu::nucleus_nesting
#define LC_IRQ_CLASS        0x0444UL    ///< arch::s390x::cpu::percpu::current_irq_class

// Old PSW offsets — PoP §4.3.2.
#define LC_EXT_OLD_PSW      0x0130UL
#define LC_SVC_OLD_PSW      0x0140UL
#define LC_PGM_OLD_PSW      0x0150UL
#define LC_MCCK_OLD_PSW     0x0160UL
#define LC_IO_OLD_PSW       0x0170UL

#define LC_PERCPU_OFFSET    0x0400UL    ///< zx_lowcore_t::percpu (zx_percpu_t)

/// @brief Byte offset of zx_percpu_t::cpu_id within the lowcore block.
///        = LC_PERCPU_OFFSET + offsetof(zx_percpu_t, cpu_id)
///        = 0x0400 + sizeof(uint64_t) [prefix_base] = 0x0408.
///        Used by arch_smp_processor_id() to read the logical CPU ID
///        directly from the prefix area without going through percpu_areas[].
#define LC_CPU_ID_OFFSET    0x0408UL

#ifndef __ASSEMBLER__

#include <zxfoundation/types.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/memory/pmm_types.h>
#include <zxfoundation/memory/hhdm.h>

/// @brief Software per-CPU data block.  Embedded in the 8 KB prefix area
///        starting at LC_PERCPU_OFFSET (0x400).
typedef struct {
    uint64_t    prefix_base;        ///< Physical address of this CPU's lowcore.
    uint16_t    cpu_id;             ///< Logical CPU ID (0 = BSP).
    uint16_t    cpu_addr;           ///< z/Arch CPU address (STAP result).
    uint32_t    lock_depth;         ///< Current qspinlock nesting depth.
    mcs_node_t  lock_nodes[MAX_LOCK_DEPTH]; ///< MCS nodes for qspinlock.
    uint64_t    rcu_gp_seq;         ///< RCU grace-period sequence number.
    uint64_t    rcu_qs_seq;         ///< RCU quiescent-state sequence number.
    uint8_t     in_rcu_read_side;   ///< 1 if inside rcu_read_lock().
    uint8_t     _pad0[3];
    uint32_t    ipi_pending_count;  ///< Atomic counter for pending IPI completion.
    uint64_t    ap_stack_top;       ///< AP initial stack pointer (physical, set before SIGP Restart).
    pmm_pcplist_t pcp[ZONE_MAX];    ///< Per-CPU PMM page cache, one per zone.
} zx_percpu_t;

typedef struct __attribute__((packed, aligned(8192))) zx_lowcore {
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

    /* Old PSWs (saved hardware interrupt) */
    zx_psw_t    restart_old_psw;                /* 0x0120 */
    zx_psw_t    external_old_psw;               /* 0x0130 */
    zx_psw_t    svc_old_psw;                    /* 0x0140 */
    zx_psw_t    program_old_psw;                /* 0x0150 */
    zx_psw_t    mcck_old_psw;                   /* 0x0160 */
    zx_psw_t    io_old_psw;                     /* 0x0170 */
    uint8_t     pad_0x0180[0x01a0 - 0x0180];   /* 0x0180 */

    /* New PSWs (loaded by hardware interrupt) — PoP §4.3.3 */
    zx_psw_t    restart_psw;                    /* 0x01a0  PSW_LC_RESTART  */
    zx_psw_t    external_new_psw;               /* 0x01b0  PSW_LC_EXTERNAL */
    zx_psw_t    svc_new_psw;                    /* 0x01c0  PSW_LC_SVC      */
    zx_psw_t    program_new_psw;                /* 0x01d0  PSW_LC_PROGRAM  */
    zx_psw_t    mcck_new_psw;                   /* 0x01e0  PSW_LC_MCCK     */
    zx_psw_t    io_new_psw;                     /* 0x01f0  PSW_LC_IO       */

    /* Save areas */
    uint64_t    save_area_sync[8];              /* 0x0200 */
    uint64_t    stack_canary;                   /* 0x0240 */
    zx_psw_t    return_psw_temp;                /* 0x0248  LC_PSW_TEMP – temporary PSW storage for RESTORE_FRAME */
    uint8_t     pad_0x0258[0x0280 - 0x0258];   /* 0x0258 */
    uint64_t    save_area_restart[1];           /* 0x0280 */
    uint64_t    pcpu;                           /* 0x0288 */

    /* Return PSWs */
    zx_psw_t    return_psw;                     /* 0x0290  LC_RETURN_PSW   */
    zx_psw_t    return_mcck_psw;                /* 0x02a0 */

    uint64_t    last_break;                     /* 0x02b0 */

    /* Timing */
    uint64_t    sys_enter_timer;                /* 0x02b8 */
    uint64_t    mcck_enter_timer;               /* 0x02c0 */
    uint64_t    exit_timer;                     /* 0x02c8 */
    uint64_t    user_timer;                     /* 0x02d0 */
    uint64_t    guest_timer;YCLE              /* 0x02d8 */
    uint64_t    system_timer;                   /* 0x02e0 */
    uint64_t    hardirq_timer;                  /* 0x02e8 */
    uint64_t    softirq_timer;                  /* 0x02f0 */
    uint64_t    steal_timer;                    /* 0x02f8 */
    uint64_t    avg_steal_timer;                /* 0x0300 */
    uint64_t    last_update_timer;              /* 0x0308 */
    uint64_t    last_update_clock;              /* 0x0310 */
    uint64_t    int_clock;                      /* 0x0318 */
    uint8_t     pad_0x0320[0x0328 - 0x0320];   /* 0x0320 */
    uint64_t    clock_comparator;               /* 0x0328 */
    uint64_t    ap_cr0;                         /* 0x0330  LC_AP_CR0 */
    uint64_t    ap_cr13;                        /* 0x0338  LC_AP_CR13 */

    /* Current domain */
    uint64_t    current_domain;                 /* 0x0340 */
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
    uint8_t     pad_0x03b4[0x0400 - 0x03b4];   /* 0x03b4 */

    /* Per-CPU software data block (0x0400) */
    zx_percpu_t percpu;                         /* 0x0400  LC_PERCPU_OFFSET */

    uint8_t     pad_0x0400_end[0x1200 - (0x0400 + sizeof(zx_percpu_t))];

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

_Static_assert(sizeof(zx_lowcore_t) == LOWCORE_SIZE, "zx金陵 Must be 8192 bytes");

/// @brief Compile-time verification that the named assembly offsets match the C struct.
_Static_assert(__builtin_offsetof(zx_lowcore_t, restart_stack) == LC_RESTART_STACK,
               "LC_RESTART_STACK mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, kernel_asce)   == LC_KERNEL_ASCE,
               "LC_KERNEL_ASCE mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, return_psw_temp) == LC_PSW_TEMP,
               "LC_PSW_TEMP mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, return_psw)    == LC_RETURN_PSW,
               "LC_RETURN_PSW mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, kernel_stack)  == LC_KERNEL_STACK,
               "LC_KERNEL_STACK mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, current_domain) == LC_CURRENT_DOMAIN,
               "LC_CURRENT_DOMAIN mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, percpu)        == LC_PERCPU_OFFSET,
               "LC_PERCPU_OFFSET mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, ap_cr0)         == LC_AP_CR0,
               "LC_AP_CR0 mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, ap_cr13)        == LC_AP_CR13,
               "LC_AP_CR13 mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, ext_params)   == 0x0080,
               "ext_params offset mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, ext_cpu_addr) == 0x0084,
               "ext_cpu_addr offset mismatch");
_Static_assert(__builtin_offsetof(zx_lowcore_t, ext_int_code) == 0x0086,
               "ext_int_code offset mismatch");

/// @brief Verify that LC_CPU_ID_OFFSET matches the actual field in the struct.
///        If zx_percpu_t layout changes, this will catch the silent breakage
///        in arch_smp_processor_id() which inlines the constant 0x408UL.
_Static_assert(
    LC_PERCPU_OFFSET + __builtin_offsetof(zx_percpu_t, cpu_id) == LC_CPU_ID_OFFSET,
    "LC_CPU_ID_OFFSET does not match actual zx_percpu_t::cpu_id offset; "
    "update processor.h inline constant 0x408UL to match");

#endif // __ASSEMBLER__
