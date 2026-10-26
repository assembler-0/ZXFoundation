// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/init/zxfl/include/arch/s390x/cpu/lowcore.h

#pragma once

#include <arch/s390x/init/zxfl/psw.h>

#define LOWCORE_SIZE    0x2000UL

/// @brief Named lowcore field offsets for use in assembly (.S) files.
#define LC_ASYNC_STACK      0x0350UL    ///< zx_lowcore::async_stack
#define LC_NODAT_STACK      0x0358UL    ///< zx_lowcore::nodat_stack
#define LC_RESTART_STACK    0x0360UL    ///< zx_lowcore::restart_stack
#define LC_MCCK_STACK       0x0368UL    ///< zx_lowcore::mcck_stack
#define LC_RESTART_FN       0x0370UL    ///< zx_lowcore::restart_fn
#define LC_RESTART_DATA     0x0378UL    ///< zx_lowcore::restart_data
#define LC_RESTART_SOURCE   0x0380UL    ///< zx_lowcore::restart_source
#define LC_RESTART_FLAGS    0x0384UL    ///< zx_lowcore::restart_flags
#define LC_PGM_CODE         0x008EUL    ///< zx_lowcore::pgm_code
#define LC_SVC_CODE         0x008AUL    ///< zx_lowcore::svc_code
#define LC_PSW_FLAG         0x0080UL    ///< zx_lowcore::psw_flag
#define LC_KERNEL_ASCE      0x0388UL    ///< zx_lowcore::kernel_asce
#define LC_RETURN_PSW       0x0290UL    ///< zx_lowcore::return_psw
#define LC_PSW_TEMP         0x0248UL    ///< Temporary PSW storage used by RESTORE_FRAME (unused padding after stack_canary)
#define LC_SCRATCH0         0x0260UL    ///< Temporary scratch for SAVE_FRAME / SAVE_NUCLEUS_FRAME (pad_0x0258)
#define LC_CURRENT_DOMAIN   0x0340UL    ///< zx_lowcore::current_domain
#define LC_KERNEL_STACK     0x0348UL    ///< zx_lowcore::kernel_stack
#define LC_AP_CR0           0x0330UL    ///< zx_lowcore::ap_cr0
#define LC_AP_CR13          0x0338UL    ///< zx_lowcore::ap_cr13

#define LC_EXT_OLD_PSW      0x0130UL
#define LC_SVC_OLD_PSW      0x0140UL
#define LC_PGM_OLD_PSW      0x0150UL
#define LC_MCCK_OLD_PSW     0x0160UL
#define LC_IO_OLD_PSW       0x0170UL

#define LC_CPU_ID_OFFSET    0x03a0UL

#define LC_PERCPU_PTR       0x0400UL

#define PCP_PREEMPT_COUNT     0x30UL   ///< percpu_data::preempt_count (48)
#define PCP_IRQ_NESTING       0x34UL   ///< percpu_data::irq_nesting (52)
#define PCP_NUCLEUS_NESTING   0x38UL   ///< percpu_data::nucleus_nesting (56)
#define PCP_IRQ_CLASS         0x40UL   ///< percpu_data::current_irq_class (64)
