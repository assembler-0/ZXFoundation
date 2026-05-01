#include <arch/s390x/init/zxfl/diag.h>

[[noreturn]] void panic(const char *msg) {
    print_msg("*** STOP: ");
    print_msg(msg);
    print_msg("\n");
    while (true) {
        __asm__ volatile("nop");
    }
    __builtin_unreachable();
}