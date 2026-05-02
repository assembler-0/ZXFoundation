// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/string.c

#include <arch/s390x/init/zxfl/string.h>

void *zxfl_memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    uint8_t  v = (uint8_t)c;
    for (size_t i = 0; i < n; i++)
        p[i] = v;
    return dst;
}

void *zxfl_memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

size_t zxfl_strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

bool zxfl_memcmp(void *s1, void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }

    return 0;
}