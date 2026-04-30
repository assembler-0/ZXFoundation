// SPDX-License-Identifier: Apache-2.0
// arch/s390x/trap/trap.c - s390x exception dispatcher.

#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/trap/trap.h>
#include <zxfoundation/sys/panic.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/zconfig.h>

extern void trap_ext_entry(void);
extern void trap_svc_entry(void);
extern void trap_pgm_entry(void);
extern void trap_mchk_entry(void);
extern void trap_io_entry(void);

static inline void lc_write_psw(uint32_t offset, uint64_t mask, uint64_t addr) {
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)offset;
    p[0] = mask;
    p[1] = addr;
}

static inline uint16_t lc_read_u16(uint32_t offset) {
    return *(volatile uint16_t *)(uintptr_t)offset;
}

static const char *pgm_code_name(uint16_t code) {
    switch (code & 0x007F) {
    case PGM_OPERATION:         return "operation exception";
    case PGM_PRIVILEGED_OP:     return "privileged-operation exception";
    case PGM_EXECUTE:           return "execute exception";
    case PGM_PROTECTION:        return "protection exception";
    case PGM_ADDRESSING:        return "addressing exception";
    case PGM_SPECIFICATION:     return "specification exception";
    case PGM_DATA:              return "data exception";
    case PGM_FIXED_OVERFLOW:    return "fixed-point overflow";
    case PGM_FIXED_DIVIDE:      return "fixed-point divide";
    case PGM_DECIMAL_OVERFLOW:  return "decimal overflow";
    case PGM_DECIMAL_DIVIDE:    return "decimal divide";
    case PGM_SEGMENT_TRANS:     return "segment-translation exception";
    case PGM_PAGE_TRANS:        return "page-translation exception";
    case PGM_TRANS_SPEC:        return "translation-specification exception";
    case PGM_SPECIAL_OP:        return "special-operation exception";
    case PGM_OPERAND:           return "operand exception";
    default:                    return "unknown program exception";
    }
}

static void dump_regs(const pt_regs_t *regs) {
    printk("CPU registers at time of exception:\n");
    printk("  PSW  mask=%016lx  addr=%016lx\n",
           regs->psw_mask, regs->psw_addr);
    for (int i = 0; i < 16; i += 2) {
        printk("  r%-2d  %016lx    r%-2d  %016lx\n",
               i,     regs->gprs[i],
               i + 1, regs->gprs[i + 1]);
    }
}

void trap_init(void) {
    const uint64_t psw_mask = CONFIG_PSW_ARCH_BITS;

    lc_write_psw(LC_EXT_NEW_PSW,  psw_mask, (uint64_t)(uintptr_t)trap_ext_entry);
    lc_write_psw(LC_SVC_NEW_PSW,  psw_mask, (uint64_t)(uintptr_t)trap_svc_entry);
    lc_write_psw(LC_PGM_NEW_PSW,  psw_mask, (uint64_t)(uintptr_t)trap_pgm_entry);
    lc_write_psw(LC_MCHK_NEW_PSW, psw_mask, (uint64_t)(uintptr_t)trap_mchk_entry);
    lc_write_psw(LC_IO_NEW_PSW,   psw_mask, (uint64_t)(uintptr_t)trap_io_entry);
}

void do_ext_interrupt(pt_regs_t *regs) {
    uint16_t code = lc_read_u16(LC_EXT_INT_CODE);
    printk("EXT interrupt: code=0x%04x  PSW addr=%016lx\n",
           (unsigned)code, regs->psw_addr);
    // TODO: dispatch to registered external interrupt handlers.
    (void)regs;
}

void do_svc_interrupt(pt_regs_t *regs, uint16_t svc_code) {
    printk("SVC %u  PSW addr=%016lx\n",
           (unsigned)svc_code, regs->psw_addr);
    // TODO: dispatch to syscall table.
    (void)regs;
}

void do_pgm_exception(pt_regs_t *regs, uint16_t pgm_code, uint16_t ilc) {
    dump_regs(regs);
    panic("Program exception: %s (code=0x%04x, ILC=%u, PSW addr=%016lx)\n",
          pgm_code_name(pgm_code),
          (unsigned)pgm_code,
          (unsigned)(ilc >> 1),
          regs->psw_addr);
}

void do_mchk_interrupt(pt_regs_t *regs) {
    dump_regs(regs);
    panic("Machine check interrupt at PSW addr=%016lx\n", regs->psw_addr);
}

void do_io_interrupt(pt_regs_t *regs) {
    printk("I/O interrupt: PSW addr=%016lx\n", regs->psw_addr);
    // TODO: dispatch to I/O subsystem.
    (void)regs;
}
