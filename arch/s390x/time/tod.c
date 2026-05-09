// SPDX-License-Identifier: Apache-2.0
// arch/s390x/time/tod.c
//
/// @brief z/Architecture TOD clock, CPU timer, and clock comparator primitives.

#include <arch/s390x/cpu/processor.h>
#include <arch/s390x/time/tod.h>

static uint64_t s_tod_boot_offset;

void tod_set_boot_offset(uint64_t boot_tod) {
    s_tod_boot_offset = boot_tod;
}

uint64_t tod_boot_offset(void) {
    return s_tod_boot_offset;
}

uint64_t tod_read(void) {
    uint64_t clk;
    __asm__ volatile("stckf %0" : "=Q"(clk) :: "cc");
    return clk;
}

int64_t tod_cpu_timer_read(void) {
    int64_t val;
    __asm__ volatile("stpt %0" : "=Q"(val));
    return val;
}

void tod_cpu_timer_set(int64_t value) {
    __asm__ volatile("spt %0" :: "Q"(value));
}

uint64_t tod_clock_comparator_read(void) {
    uint64_t val;
    __asm__ volatile("stckc %0" : "=Q"(val));
    return val;
}

void tod_clock_comparator_set(uint64_t abs_tod) {
    __asm__ volatile("sckc %0" :: "Q"(abs_tod));
}

void tod_enable_ext_interrupts(void) {
    arch_ctl_set_bit(0, 11);
    arch_ctl_set_bit(0, 10);
}
