// SPDX-License-Identifier: Acpache-2.0

#include <zxfoundation/types.h>
#include <zxfoundation/errno.h>

#define COMMAND_LINE_SIZE 4096

static inline int isspace(uint8_t c) {
    return c <= ' ';
}

int cmdline_find_option_bool(const char *cmdline, int max_cmdline_size,
                           const char *option) {
    char c;
    int pos = 0, wstart = 0;
    const char *opptr = nullptr;
    enum {
        st_wordstart = 0, /* Start of word/after whitespace */
        st_wordcmp, /* Comparing this word */
        st_wordskip, /* Miscompare, skip */
    } state = st_wordstart;

    if (!cmdline)
        return -EINVAL; /* No command line */

    while (pos < max_cmdline_size) {
        c = *(char *) cmdline++;
        pos++;

        switch (state) {
            case st_wordstart:
                if (!c)
                    return 0;
                if (isspace(c))
                    break;

                state = st_wordcmp;
                opptr = option;
                wstart = pos;
                __attribute__((fallthrough));

            case st_wordcmp:
                if (!*opptr) {
                    if (!c || isspace(c))
                        return wstart;
                } else if (!c) {
                    return 0;
                } else if (c == *opptr++) {
                    break;
                }
                state = st_wordskip;
                __attribute__((fallthrough));

            case st_wordskip:
                if (!c)
                    return 0;
                if (isspace(c))
                    state = st_wordstart;
                break;
        }
    }

    return 0; /* Buffer overrun */
}

int cmdline_find_option(const char *cmdline, int max_cmdline_size,
                      const char *option, char *buffer, int bufsize) {
    char c;
    int pos = 0, len = -1;
    const char *opptr = nullptr;
    char *bufptr = buffer;
    enum {
        st_wordstart = 0, /* Start of word/after whitespace */
        st_wordcmp, /* Comparing this word */
        st_wordskip, /* Miscompare, skip */
        st_bufcpy, /* Copying this to buffer */
    } state = st_wordstart;

    if (!cmdline)
        return -EINVAL;

    while (pos++ < max_cmdline_size) {
        c = *(char *) cmdline++;
        if (!c)
            break;

        switch (state) {
            case st_wordstart:
                if (isspace(c))
                    break;

                state = st_wordcmp;
                opptr = option;
                __attribute__((fallthrough));

            case st_wordcmp:
                if ((c == '=') && !*opptr) {
                    len = 0;
                    bufptr = buffer;
                    state = st_bufcpy;
                    break;
                }
                if (c == *opptr++) {
                    break;
                }
                state = st_wordskip;
                __attribute__((fallthrough));

            case st_wordskip:
                if (isspace(c))
                    state = st_wordstart;
                break;

            case st_bufcpy:
                if (isspace(c)) {
                    state = st_wordstart;
                } else {
                    if (++len < bufsize)
                        *bufptr++ = c;
                }
                break;
        }
    }

    if (bufsize)
        *bufptr = '\0';

    return len;
}