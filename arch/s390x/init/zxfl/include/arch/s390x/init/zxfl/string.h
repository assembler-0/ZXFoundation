// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/init/zxfl/include/arch/s390x/init/zxfl/string.h
/// @brief Freestanding string/memory utilities for the ZXFL bootloader.

#ifndef ZXFOUNDATION_ZXFL_STRING_H
#define ZXFOUNDATION_ZXFL_STRING_H

#include <zxfoundation/types.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t zxfl_strlen(const char *s);


#endif /* ZXFOUNDATION_ZXFL_STRING_H */
