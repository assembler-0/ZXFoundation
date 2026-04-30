/// SPDX-License-Identifier: Apache-2.0
/// printk.c - logging functions

#include <zxfoundation/sys/printk.h>
#include <zxfoundation/errno.h>
#include <zxfoundation/types.h>

static printk_putc_sink global_sink = nullptr;

static void print_hex(uint64_t val, int width) {
    char buf[16];
    const char *digits = "0123456789abcdef";
    int i = 15;
    do {
        buf[i--] = digits[val & 0xF];
        val >>= 4;
    } while (val && i >= 0);
    while (i >= 0 && (15 - i) < width) {
        buf[i--] = '0';
    }
    for (int j = i + 1; j < 16; ++j) {
        global_sink(buf[j]);
    }
}

static void print_dec(int64_t val) {
    if (val < 0) {
        global_sink('-');
        val = -val;
    }
    char buf[20];
    int i = 19;
    do {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    } while (val);
    for (int j = i + 1; j < 20; ++j) {
        global_sink(buf[j]);
    }
}

static void print_udec(uint64_t val) {
    char buf[20];
    int i = 19;
    do {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    } while (val);
    for (int j = i + 1; j < 20; ++j) {
        global_sink(buf[j]);
    }
}

static void print_str(const char *s) {
    if (!s) s = "(null)";
    while (*s) {
        global_sink(*s++);
    }
}

int vprintk(const char *fmt, va_list ap) {
    if (!global_sink) return -ENODEV;

    int count = 0;
    while (*fmt) {
        if (*fmt != '%') {
            global_sink(*fmt++);
            ++count;
            continue;
        }

        ++fmt;  // skip '%'
        if (*fmt == '%') {
            global_sink('%');
            ++fmt;
            ++count;
            continue;
        }

        // Parse optional zero-fill flag
        int zero_fill = 0;
        if (*fmt == '0') {
            zero_fill = 1;
            ++fmt;
        }

        // Parse optional width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        (void)zero_fill; // width already implies zero-fill for hex

        // Parse optional 'l' (long) modifier
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            ++fmt;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            print_str(s);
            count += 6;  // approximate; exact count not critical
            break;
        }
        case 'd': {
            int64_t val = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            print_dec(val);
            count += 10;
            break;
        }
        case 'u': {
            uint64_t val = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            print_udec(val);
            count += 10;
            break;
        }
        case 'x': {
            uint64_t val = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
            print_hex(val, width);
            count += width ? width : 8;
            break;
        }
        default:
            global_sink('%');
            global_sink(*fmt);
            count += 2;
            break;
        }
        ++fmt;
    }

    return count;
}

int printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

void printk_set_sink(const printk_putc_sink sink) {
    global_sink = sink;
}

void printk_initialize(const printk_putc_sink sink) {
    global_sink = sink;
}
