// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/trap.S
//
// Low-level exception entry stubs for s390x.
//
// Each stub:
//   1. Saves all 16 GPRs onto the kernel stack (or a dedicated emergency
//      stack if r15 is suspect).
//   2. Loads the hardware-saved old PSW from the lowcore into the frame.
//   3. Calls the appropriate C handler with a pointer to pt_regs_t.
//   4. Restores GPRs and returns (for resumable exceptions like SVC/EXT/IO).
//      Fatal handlers (PGM, MCHK) never return.
//
// Stack frame layout (pt_regs_t, 144 bytes):
//   SP+0   .. SP+127  : r0..r15  (16 × 8 bytes)
//   SP+128 .. SP+135  : PSW mask
//   SP+136 .. SP+143  : PSW addr
//
// The frame is allocated by subtracting PT_REGS_SIZE from r15 *before*
// saving, so r15 in the frame reflects the pre-exception stack pointer.

#include <zxfoundation/zconfig.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define PT_REGS_SIZE        144     // sizeof(pt_regs_t) = 16*8 + 8 + 8
#define PT_REGS_PSWM        128     // offsetof(pt_regs_t, psw_mask)
#define PT_REGS_PSWA        136     // offsetof(pt_regs_t, psw_addr)

// Lowcore offsets for old PSWs (mask at offset, addr at offset+8)
#define LC_EXT_OLD_PSW      0x018
#define LC_SVC_OLD_PSW      0x020
#define LC_PGM_OLD_PSW      0x028
#define LC_MCHK_OLD_PSW     0x030
#define LC_IO_OLD_PSW       0x038

// Lowcore fields read by SVC/PGM handlers
#define LC_SVC_INT_CODE     0x088   // uint16_t
#define LC_PGM_ILC          0x08C   // uint16_t
#define LC_PGM_CODE         0x08E   // uint16_t

// ---------------------------------------------------------------------------
// SAVE_REGS macro
//
// Saves r0..r15 into the pt_regs frame at the current r15.
// r15 already points to the base of the frame (after ALLOC_FRAME).
//
// Clobbers: nothing (uses stmg which is non-destructive to the registers).
// ---------------------------------------------------------------------------
.macro SAVE_REGS
    stmg    %r0, %r15, 0(%r15)
.endm

// ---------------------------------------------------------------------------
// ALLOC_FRAME macro
//
// Allocates PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA bytes on the stack.
// The save area (160 bytes) is required by the s390x ABI for called
// functions to spill their register arguments.
//
// After this macro:
//   r15 = new SP (frame base)
//   The old r15 value is NOT yet saved (SAVE_REGS does that).
// ---------------------------------------------------------------------------
.macro ALLOC_FRAME
    aghi    %r15, -(PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
.endm

// ---------------------------------------------------------------------------
// LOAD_OLD_PSW  lc_offset, reg_tmp
//
// Loads the hardware-saved old PSW (mask + addr) from the lowcore into
// the pt_regs frame.
//
//   lc_offset  - byte offset of the old PSW in the lowcore (e.g. LC_PGM_OLD_PSW)
//   reg_tmp    - a scratch register (will be clobbered)
// ---------------------------------------------------------------------------
.macro LOAD_OLD_PSW lc_offset, reg_tmp
    lghi    \reg_tmp, 0
    lg      \reg_tmp, \lc_offset(\reg_tmp)          // PSW mask
    stg     \reg_tmp, (PT_REGS_PSWM + CONFIG_S390X_SAVE_AREA)(%r15)
    lghi    \reg_tmp, 0
    lg      \reg_tmp, (\lc_offset + 8)(\reg_tmp)    // PSW addr
    stg     \reg_tmp, (PT_REGS_PSWA + CONFIG_S390X_SAVE_AREA)(%r15)
.endm

// ---------------------------------------------------------------------------
// RESTORE_REGS macro
//
// Restores r0..r15 from the pt_regs frame and returns to the interrupted
// context by loading the old PSW.  Used only for resumable exceptions.
// ---------------------------------------------------------------------------
.macro RESTORE_REGS lc_old_psw
    lmg     %r0, %r15, CONFIG_S390X_SAVE_AREA(%r15)
.endm

    .text

// ===========================================================================
// External Interrupt handler
// ===========================================================================
    .globl  trap_ext_entry
    .type   trap_ext_entry, @function
trap_ext_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_EXT_OLD_PSW, %r1

    // Compute pointer to pt_regs (frame base + save-area offset)
    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)

    brasl   %r14, do_ext_interrupt

    // Restore and return via old PSW
    lmg     %r0, %r14, CONFIG_S390X_SAVE_AREA(%r15)
    aghi    %r15, (PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
    lghi    %r1, 0
    lpswe   LC_EXT_OLD_PSW(%r1)

// ===========================================================================
// Supervisor Call (SVC) handler
// ===========================================================================
    .globl  trap_svc_entry
    .type   trap_svc_entry, @function
trap_svc_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_SVC_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)   // arg1: pt_regs *

    // arg2: svc_code (uint16_t from lowcore 0x088)
    lghi    %r1, 0
    llgh    %r3, LC_SVC_INT_CODE(%r1)

    brasl   %r14, do_svc_interrupt

    lmg     %r0, %r14, CONFIG_S390X_SAVE_AREA(%r15)
    aghi    %r15, (PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
    lghi    %r1, 0
    lpswe   LC_SVC_OLD_PSW(%r1)

// ===========================================================================
// Program Exception handler  (fatal - do_pgm_exception calls panic)
// ===========================================================================
    .globl  trap_pgm_entry
    .type   trap_pgm_entry, @function
trap_pgm_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_PGM_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)   // arg1: pt_regs *

    // arg2: pgm_code (uint16_t from lowcore 0x08E)
    lghi    %r1, 0
    llgh    %r3, LC_PGM_CODE(%r1)

    // arg3: ilc (uint16_t from lowcore 0x08C)
    lghi    %r1, 0
    llgh    %r4, LC_PGM_ILC(%r1)

    brasl   %r14, do_pgm_exception
    // do_pgm_exception calls panic() which never returns.
    // If somehow we get here, spin.
.Lpgm_spin:
    j       .Lpgm_spin

// ===========================================================================
// Machine Check handler  (fatal)
// ===========================================================================
    .globl  trap_mchk_entry
    .type   trap_mchk_entry, @function
trap_mchk_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_MCHK_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)

    brasl   %r14, do_mchk_interrupt
.Lmchk_spin:
    j       .Lmchk_spin

// ===========================================================================
// I/O Interrupt handler
// ===========================================================================
    .globl  trap_io_entry
    .type   trap_io_entry, @function
trap_io_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_IO_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)

    brasl   %r14, do_io_interrupt

    lmg     %r0, %r14, CONFIG_S390X_SAVE_AREA(%r15)
    aghi    %r15, (PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
    lghi    %r1, 0
    lpswe   LC_IO_OLD_PSW(%r1)
