// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include "zxvl.h"
#include "zxfl.h"

[[noreturn]] void kmain(zxfl_boot_protocol_t *boot) {
    while (1) __asm__ volatile("nop");
}
