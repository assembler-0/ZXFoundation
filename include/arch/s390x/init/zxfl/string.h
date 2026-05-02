// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/string.h
//
/// @brief Freestanding string/memory utilities for the ZXFL bootloader.
///        These replace libc equivalents which are unavailable in -nostdlib.

#ifndef ZXFOUNDATION_ZXFL_STRING_H
#define ZXFOUNDATION_ZXFL_STRING_H

#include <zxfoundation/types.h>

void *zxfl_memset(void *dst, int c, size_t n);
void *zxfl_memcpy(void *dst, const void *src, size_t n);
bool zxfl_memcmp(void *s1, void *s2, size_t n);
size_t zxfl_strlen(const char *s);


#endif /* ZXFOUNDATION_ZXFL_STRING_H */
