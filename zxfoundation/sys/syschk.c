// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/syschk.c - system check implementation

#include <zxfoundation/sys/syschk.h>
#include <arch/s390x/cpu/irq.h>
#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/cpu/lowcore.h>
#include <arch/s390x/cpu/smp.h>
#include <arch/s390x/init/zxfl/zxfl.h>
#include <zxfoundation/sys/simplelog.h>
#include <lib/vsprintf.h>

static volatile bool             g_halting;
static const zxfl_cpu_info_t    *g_cpu_map;
static uint32_t                  g_cpu_count;

static const char *const class_names[]  = {
    [ZX_SYSCHK_CLASS_FATAL] = "fatal",
    [ZX_SYSCHK_CLASS_CRITICAL] = "critical",
    [ZX_SYSCHK_CLASS_WARNING] = "warning"
};

static const char *const domain_names[] = {
    "core", "mem", "sync", "arch", "sched", "io"
};

void zx_syschk_initialize(const zxfl_boot_protocol_t *boot) {
    g_cpu_map   = boot->cpu_map;
    g_cpu_count = boot->cpu_count;
}

[[noreturn]] void zx_system_check(const zx_syschk_code_t code,
                                  const char *fmt, ...) {
    arch_local_irq_disable();

    if (g_halting)
        arch_sys_halt();
    g_halting = true;

    zx_crash_record_t *rec =
        (zx_crash_record_t *)((uint8_t *)zx_lowcore() + ZX_CRASH_RECORD_OFFSET);

    rec->magic = ZX_CRASH_RECORD_MAGIC;
    rec->code  = code;

    {
        const zx_psw_t psw = arch_extract_psw();
        rec->psw_mask = psw.mask;
        rec->psw_addr = psw.addr;
    }

    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(rec->msg, ZX_CRASH_MSG_LEN, fmt, ap);
        va_end(ap);
    }

    const unsigned int cls = ZX_SYSCHK_CLASS(code);
    const unsigned int dom = ZX_SYSCHK_DOMAIN(code);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "\nsyschk [%s/%s/0x%02x]: ",
             (cls < 16 && class_names[cls])  ? class_names[cls]  : "unknown",
             (dom < 6)                        ? domain_names[dom] : "unknown",
             ZX_SYSCHK_TYPE(code));

    simplelog(hdr);
    simplelog(rec->msg);
    simplelog("\n");

    if (g_cpu_map)
        smp_stop_all_raw(g_cpu_map, g_cpu_count);

    arch_sys_halt();
}
