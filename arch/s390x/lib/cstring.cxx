/// SPDX-License-Identifier: Apache-2.0
/// @file arch/s390x/lib/cstring.cxx
/// @brief z/Architecture string/memory operations.

import zxfoundation.base.types;
import arch.s390x.cpu.features;
import arch.s390x.cpu.processor_types;

using arch::s390x::cpu::features::facility;

extern "C" {

    auto __zx_clcle(const char *s1, usize l1,
                    const char *s2, usize l2) -> condition_code {
        using arch::s390x::cpu::processor::register_pair;
        register_pair r1{};
        register_pair r3{};
        r1.even = reinterpret_cast<u64>(s1);
        r1.odd = l1;
        r3.even = reinterpret_cast<u64>(s2);
        r3.odd = l2;

        condition_code cc;
        __asm__ volatile(
            "0:     clcle   %[r1],%[r3],0\n"
            "       jo      0b\n"
            "       ipm     %[cc]\n"
            "       srl     %[cc],28"
            : [cc] "=d" (cc), [r1] "+d" (r1.pair), [r3] "+d" (r3.pair)
            :
            : "cc", "memory");
        return cc;
    }

    auto __zx_memcpy(void *dest, const void *src, usize n) -> void * {
        auto *d = static_cast<char *>(dest);
        const auto *s = static_cast<const char *>(src);

        if (!n) return dest;

        while (n >= 256) {
            __asm__ volatile(
                "       mvc     0(256,%[dest]),0(%[src])"
                :
                : [dest] "a" (d), [src] "a" (s)
                : "memory");
            d += 256;
            s += 256;
            n -= 256;
        }
        if (n) {
            __asm__ volatile(
                "       exrl    %[n],1f\n"
                "       j       2f\n"
                "1:     mvc     0(1,%[dest]),0(%[src])\n"
                "2:"
                :
                : [dest] "a" (d), [src] "a" (s), [n] "a" (n - 1)
                : "memory");
        }
        return dest;
    }

    auto __zx_memset(void *s, int c, usize n) -> void * {
        auto *xs = static_cast<char *>(s);

        if (!n) return s;

        if (!c) {
            while (n >= 256) {
                __asm__ volatile(
                    "       xc      0(256,%[xs]),0(%[xs])"
                    :
                    : [xs] "a" (xs)
                    : "cc", "memory");
                xs += 256;
                n -= 256;
            }
            if (!n) return s;
            __asm__ volatile(
                "       exrl    %[n],0f\n"
                "       j       1f\n"
                "0:     xc      0(1,%[xs]),0(%[xs])\n"
                "1:"
                :
                : [xs] "a" (xs), [n] "a" (n - 1)
                : "cc", "memory");
        } else {
            while (n >= 256) {
                *xs = static_cast<char>(c);
                __asm__ volatile(
                    "       mvc     1(255,%[xs]),0(%[xs])"
                    :
                    : [xs] "a" (xs)
                    : "memory");
                xs += 256;
                n -= 256;
            }
            if (!n) return s;
            *xs = static_cast<char>(c);
            if (n == 1) return s;
            __asm__ volatile(
                "       exrl    %[n],0f\n"
                "       j       1f\n"
                "0:     mvc     1(1,%[xs]),0(%[xs])\n"
                "1:"
                :
                : [xs] "a" (xs), [n] "a" (n - 2)
                : "memory");
        }
        return s;
    }

    auto __zx_memmove(void *dest, const void *src, usize n) -> void * {
        auto *d = static_cast<char *>(dest);
        const auto *s = static_cast<const char *>(src);

        if (!n) return dest;

        if (d <= s || d >= s + n) {
            while (n >= 256) {
                __asm__ volatile(
                    "       mvc     0(256,%[d]),0(%[s])\n"
                    :
                    : [d] "a" (d), [s] "a" (s)
                    : "memory");
                d += 256;
                s += 256;
                n -= 256;
            }
            if (n) {
                __asm__ volatile(
                    "       exrl    %[n],0f\n"
                    "       j       1f\n"
                    "0:     mvc     0(1,%[d]),0(%[s])\n"
                    "1:"
                    :
                    : [d] "a" (d), [s] "a" (s), [n] "a" (n - 1)
                    : "memory");
            }
            return dest;
        }

        if (arch::s390x::cpu::features::has(facility::misc_inst_ext1)) {
            while (n >= 256) {
                __asm__ volatile(
                    "       lghi    %%r0,255\n"
                    "       .insn   sse,0xe50a00000000,%[d],%[s]\n"
                    : [d] "=Q" (*(d + n - 256))
                    : [s] "Q" (*(s + n - 256))
                    : "0", "memory");
                n -= 256;
            }
            if (n) {
                __asm__ volatile(
                    "       lgr     %%r0,%[n]\n"
                    "       .insn   sse,0xe50a00000000,%[d],%[s]\n"
                    : [d] "=Q" (*d)
                    : [s] "Q" (*s), [n] "d" (n - 1)
                    : "0", "memory");
            }
        } else {
            while (n--) d[n] = s[n];
        }

        return dest;
    }

    auto __zx_memcmp(const void *s1, const void *s2, usize n) -> int {
        using arch::s390x::cpu::processor::register_pair;
        register_pair r1{};
        register_pair r3{};
        r1.even = reinterpret_cast<u64>(s1);
        r1.odd = n;
        r3.even = reinterpret_cast<u64>(s2);
        r3.odd = n;

        condition_code cc;
        __asm__ volatile(
            "0:     clcle   %[r1],%[r3],0\n"
            "       jo      0b\n"
            "       ipm     %[cc]\n"
            "       srl     %[cc],28"
            : [cc] "=d" (cc), [r1] "+d" (r1.pair), [r3] "+d" (r3.pair)
            :
            : "cc", "memory");

        if (cc) cc = (cc == 1) ? -1 : 1;
        return cc;
    }

    auto __zx_strcmp(const char *s1, const char *s2) -> int {
        int ret = 0;
        __asm__ volatile(
            "       lghi    0,0\n"
            "0:     clst    %[s1],%[s2]\n"
            "       jo      0b\n"
            "       je      1f\n"
            "       ic      %[ret],0(%[s1])\n"
            "       ic      0,0(%[s2])\n"
            "       sr      %[ret],0\n"
            "1:"
            : [ret] "+&d" (ret), [s1] "+&a" (s1), [s2] "+&a" (s2)
            :
            : "cc", "memory", "0");
        return ret;
    }

    auto __zx_strend(const char *s) -> char * {
        u64 e = 0; // will hold the result
        __asm__ volatile(
            "       lghi    0,0\n"
            "0:     srst    %[e],%[s]\n"
            "       jo      0b"
            : [e] "+&a" (e), [s] "+&a" (s)
            :
            : "cc", "memory", "0");
        return reinterpret_cast<char *>(e);
    }

    auto __zx_strnend(const char *s, usize n) -> char * {
        const char *p = s + n;
        __asm__ volatile(
            "       lghi    0,0\n"
            "0:     srst    %[p],%[s]\n"
            "       jo      0b"
            : [p] "+&d" (p), [s] "+&a" (s)
            :
            : "cc", "memory", "0");
        return const_cast<char *>(p);
    }

    auto __zx_strcat(char *dest, const char *src) -> char * {
        u64 dummy = 0;
        char *ret = dest;
        __asm__ volatile(
            "       lghi    0,0\n"
            "0:     srst    %[dummy],%[dest]\n"
            "       jo      0b\n"
            "1:     mvst    %[dummy],%[src]\n"
            "       jo      1b"
            : [dummy] "+&a" (dummy), [dest] "+&a" (dest), [src] "+&a" (src)
            :
            : "cc", "memory", "0");
        return ret;
    }

} // extern "C"
