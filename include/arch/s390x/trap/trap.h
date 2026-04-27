#pragma once

// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/trap/trap.h
//
// s390x trap / exception subsystem public interface.
//
// Covers:
//   - pt_regs  : full general-purpose register save frame
//   - Program-exception (PGM) interrupt codes
//   - trap_init()  : install exception vectors into the lowcore
//   - trap handler declarations

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// pt_regs - full CPU state saved on every exception entry.
//
// Layout (all 8-byte slots, 256 bytes total):
//   [0..15]  : GPRs r0..r15  (saved/restored by trap.S stubs)
//   [16]     : PSW mask      (from the hardware-saved old PSW)
//   [17]     : PSW addr      (faulting / interrupted instruction address)
//
// The struct is placed on the kernel stack by the assembly stubs in trap.S.
// C handlers receive a pointer to it and may read (but should not modify)
// the register values for diagnostic purposes.
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t gprs[16];      // r0 .. r15
    uint64_t psw_mask;      // PSW mask at time of exception
    uint64_t psw_addr;      // PSW address (faulting instruction + ILC)
} pt_regs_t;

// ---------------------------------------------------------------------------
// s390x Program-Exception interrupt codes (pgm_code field in lowcore).
// Reference: z/Architecture PoO, Table 6-3.
// ---------------------------------------------------------------------------
#define PGM_OPERATION           0x0001  // Operation exception
#define PGM_PRIVILEGED_OP       0x0002  // Privileged-operation exception
#define PGM_EXECUTE             0x0003  // Execute exception
#define PGM_PROTECTION          0x0004  // Protection exception
#define PGM_ADDRESSING          0x0005  // Addressing exception
#define PGM_SPECIFICATION       0x0006  // Specification exception
#define PGM_DATA                0x0007  // Data exception
#define PGM_FIXED_OVERFLOW      0x0008  // Fixed-point overflow
#define PGM_FIXED_DIVIDE        0x0009  // Fixed-point divide
#define PGM_DECIMAL_OVERFLOW    0x000A  // Decimal overflow
#define PGM_DECIMAL_DIVIDE      0x000B  // Decimal divide
#define PGM_HFP_EXPONENT_OVF    0x000C  // HFP exponent overflow
#define PGM_HFP_EXPONENT_UNF    0x000D  // HFP exponent underflow
#define PGM_HFP_SIGNIFICANCE    0x000E  // HFP significance
#define PGM_HFP_DIVIDE          0x000F  // HFP divide
#define PGM_SEGMENT_TRANS       0x0010  // Segment-translation exception
#define PGM_PAGE_TRANS          0x0011  // Page-translation exception
#define PGM_TRANS_SPEC          0x0012  // Translation-specification exception
#define PGM_SPECIAL_OP          0x0013  // Special-operation exception
#define PGM_OPERAND             0x0015  // Operand exception
#define PGM_TRACE_TABLE         0x0016  // Trace-table exception
#define PGM_VECTOR_PROCESSING   0x001B  // Vector processing exception
#define PGM_SPACE_SWITCH        0x001C  // Space-switch event
#define PGM_HFP_SQRT            0x001D  // HFP square-root exception
#define PGM_PC_TRANS_SPEC       0x001F  // PC-translation specification
#define PGM_AFX_TRANS           0x0020  // AFX-translation exception
#define PGM_ASX_TRANS           0x0021  // ASX-translation exception
#define PGM_LX_TRANS            0x0022  // LX-translation exception
#define PGM_EX_TRANS            0x0023  // EX-translation exception
#define PGM_PRIMARY_AUTH        0x0024  // Primary-authority exception
#define PGM_SECONDARY_AUTH      0x0025  // Secondary-authority exception
#define PGM_ALET_SPEC           0x0028  // ALET-specification exception
#define PGM_ALEN_TRANS          0x0029  // ALEN-translation exception
#define PGM_ALE_SEQ             0x002A  // ALE-sequence exception
#define PGM_ASTE_VALIDITY       0x002B  // ASTE-validity exception
#define PGM_ASTE_SEQ            0x002C  // ASTE-sequence exception
#define PGM_EXTENDED_AUTH       0x002D  // Extended-authority exception
#define PGM_STACK_FULL          0x0030  // Stack-full exception
#define PGM_STACK_EMPTY         0x0031  // Stack-empty exception
#define PGM_STACK_SPEC          0x0032  // Stack-specification exception
#define PGM_STACK_TYPE          0x0033  // Stack-type exception
#define PGM_STACK_OP            0x0034  // Stack-operation exception
#define PGM_MONITOR             0x0040  // Monitor event
#define PGM_PER                 0x0080  // PER event (bit, may combine)
#define PGM_CRYPTO_OP           0x0119  // Crypto-operation exception

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// trap_init - install all exception new-PSWs into the lowcore.
// Must be called early in boot, before enabling any interrupts.
void trap_init(void);

// Individual C-level handlers (called from assembly stubs in trap.S).
// Each receives a pointer to the saved register frame.
void do_ext_interrupt(pt_regs_t *regs);
void do_svc_interrupt(pt_regs_t *regs, uint16_t svc_code);
void do_pgm_exception(pt_regs_t *regs, uint16_t pgm_code, uint16_t ilc);
void do_mchk_interrupt(pt_regs_t *regs);
void do_io_interrupt(pt_regs_t *regs);
