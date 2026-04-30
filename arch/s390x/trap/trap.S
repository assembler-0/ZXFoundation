// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/trap.S

#include <zxfoundation/zconfig.h>

#define PT_REGS_SIZE        144     // sizeof(pt_regs_t) = 16*8 + 8 + 8
#define PT_REGS_PSWM        128     // offsetof(pt_regs_t, psw_mask)
#define PT_REGS_PSWA        136     // offsetof(pt_regs_t, psw_addr)

#define LC_EXT_OLD_PSW      0x018
#define LC_SVC_OLD_PSW      0x028
#define LC_PGM_OLD_PSW      0x038
#define LC_MCHK_OLD_PSW     0x048
#define LC_IO_OLD_PSW       0x0F8

#define LC_SVC_INT_CODE     0x088   // uint16_t
#define LC_PGM_ILC          0x08C   // uint16_t
#define LC_PGM_CODE         0x08E   // uint16_t

.macro SAVE_REGS
    stmg    %r0, %r15, CONFIG_S390X_SAVE_AREA(%r15)
.endm

.macro ALLOC_FRAME
    aghi    %r15, -(PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
.endm

.macro LOAD_OLD_PSW lc_offset, reg_tmp
    lghi    \reg_tmp, 0
    lg      \reg_tmp, \lc_offset(\reg_tmp)          // PSW mask
    stg     \reg_tmp, (PT_REGS_PSWM + CONFIG_S390X_SAVE_AREA)(%r15)
    lghi    \reg_tmp, 0
    lg      \reg_tmp, (\lc_offset + 8)(\reg_tmp)    // PSW addr
    stg     \reg_tmp, (PT_REGS_PSWA + CONFIG_S390X_SAVE_AREA)(%r15)
.endm

.macro RESTORE_REGS lc_old_psw
    lmg     %r0, %r15, CONFIG_S390X_SAVE_AREA(%r15)
.endm

    .text

    .globl  trap_ext_entry
    .type   trap_ext_entry, @function
trap_ext_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_EXT_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)

    brasl   %r14, do_ext_interrupt

    lmg     %r0, %r14, CONFIG_S390X_SAVE_AREA(%r15)
    aghi    %r15, (PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
    lghi    %r1, 0
    lpswe   LC_EXT_OLD_PSW(%r1)

    .globl  trap_svc_entry
    .type   trap_svc_entry, @function
trap_svc_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_SVC_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)

    lghi    %r1, 0
    llgh    %r3, LC_SVC_INT_CODE(%r1)

    brasl   %r14, do_svc_interrupt

    lmg     %r0, %r14, CONFIG_S390X_SAVE_AREA(%r15)
    aghi    %r15, (PT_REGS_SIZE + CONFIG_S390X_SAVE_AREA)
    lghi    %r1, 0
    lpswe   LC_SVC_OLD_PSW(%r1)

    .globl  trap_pgm_entry
    .type   trap_pgm_entry, @function
trap_pgm_entry:
    ALLOC_FRAME
    SAVE_REGS
    LOAD_OLD_PSW LC_PGM_OLD_PSW, %r1

    la      %r2, CONFIG_S390X_SAVE_AREA(%r15)

    lghi    %r1, 0
    llgh    %r3, LC_PGM_CODE(%r1)

    lghi    %r1, 0
    llgh    %r4, LC_PGM_ILC(%r1)

    brasl   %r14, do_pgm_exception
.Lpgm_spin:
    j       .Lpgm_spin

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

    .section .note.GNU-stack, "", @progbits