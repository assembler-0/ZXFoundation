#pragma once

#include <zxfoundation/types.h>

struct sclp_buffer {
    uint16_t length;
    uint16_t flags;
    char data[1];
} __attribute__((packed, aligned(8)));

struct sclp_req {
    uint16_t length;
    uint16_t function_code;
    uint32_t response_code;
    uint32_t reserved;

    struct sclp_buffer buffer;
} __attribute__((packed, aligned(8)));

void sclp_write(const char *buf, int len);
void sclp_putc(char c);