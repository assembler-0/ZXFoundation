/// SPDX-License-Identifier: Apache-2.0
/// sclp.c - minimal SCLP implementation

#include <drivers/console/sclp.h>

static struct sclp_req req;

void sclp_write(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        req.buffer.data[i] = buf[i];
    }

    req.length = sizeof(req) + len;
    req.function_code = 0x0077;
    req.response_code = 0;

    req.buffer.length = len + sizeof(struct sclp_buffer);
    req.buffer.flags = 0;

    register unsigned long r1 __asm__("1") = (unsigned long)&req;

    __asm__ volatile(
        "svc 0"
        :
        : "d"(r1)
        : "memory"
    );
}
void sclp_putc(char c) {
    if (c == '\n') {
        char crlf[2] = {'\r', '\n'};
        sclp_write(crlf, 2);
    } else {
        sclp_write(&c, 1);
    }
}