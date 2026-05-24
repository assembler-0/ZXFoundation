// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/ipi.h
#pragma once

#include <zxfoundation/types.h>

/// @brief IPI message types for ZXFoundation.
typedef enum {
    IPI_DRAIN_PCP = 0,      ///< Request target CPU to drain its PMM PCPs.
    IPI_HALT      = 1,      ///< Request target CPU to enter disabled-wait.
    IPI_MAX
} ipi_msg_t;

/// @brief Initialize IPI subsystem on the current CPU.
void arch_ipi_init(void);

/// @brief Send an IPI to all other CPUs and wait for completion.
/// @param msg  The IPI message type to broadcast.
void arch_ipi_broadcast_wait(ipi_msg_t msg);

/// @brief The C handler for SIGP Emergency Signal interrupts.
///        Called from do_ext_interrupt() when ext_code == 0x1201.
void arch_ipi_handle_emergency(void);
