// SPDX-License-Identifier: Apache-2.0
// arch/s390x/cpu/stsi.c

#include <arch/s390x/cpu/stsi.h>

int stsi(void *sysinfo, int fc, int sel1, int sel2) {
    int r0 = (fc << 28) | sel1;
    int cc;
    __asm__ volatile (
        "   lr      %%r0,%[r0]\n"
        "   lr      %%r1,%[r1]\n"
        "   stsi    %[sysinfo]\n"
        "   ipm     %[cc]\n"
        "   srl     %[cc],28\n"
        : [cc] "=d" (cc), [sysinfo] "=Q" (*(char *)sysinfo)
        : [r0] "d" (r0), [r1] "d" (sel2)
        : "0", "1", "cc", "memory"
    );
    return (cc == 3) ? -1 : 0;
}
