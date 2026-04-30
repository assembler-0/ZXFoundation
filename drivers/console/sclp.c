// SPDX-License-Identifier: Apache-2.0
// sclp.c - SCLP console driver for s390x

#include <arch/s390x/cpu/servc.h>
#include <drivers/console/sclp.h>

static sclp_write_sccb_t  write_sccb;
static sclp_mask_sccb_t   mask_sccb;

int sclp_setup(void) {
    sclp_write_event_mask_t *m = &mask_sccb.mask;

    m->h.length           = sizeof(sclp_write_event_mask_t);
    m->h.function_code    = SCLP_FC_NORMAL_WRITE;
    m->h.control_mask[0]  = 0;
    m->h.control_mask[1]  = 0;
    m->h.control_mask[2]  = 0;
    m->h.response_code    = 0;
    m->_reserved          = 0;
    m->mask_length        = sizeof(uint32_t);
    m->cp_receive_mask    = 0;
    m->cp_send_mask       = SCLP_EVENT_MASK_MSG_ASCII;
    m->send_mask          = 0;
    m->receive_mask       = 0;

    int cc = servc_retry(SCLP_CMD_WRITE_EVENT_MASK, m);
    if (cc == SERVC_CC_NOT_OPERATIONAL || cc == SERVC_CC_BUSY)
        return -1;

    if (m->h.response_code != SCLP_RC_NORMAL)
        return -1;

    return 0;
}

int sclp_write(const char *buf, size_t len) {
    if (len == 0)
        return 0;
    if (len > SCCB_DATA_LEN)
        len = SCCB_DATA_LEN;

    uint16_t evbuf_len = (uint16_t)(sizeof(sclp_evbuf_hdr_t) + len);
    uint16_t total_len = (uint16_t)(sizeof(sccb_header_t) + evbuf_len);

    write_sccb.h.length           = total_len;
    write_sccb.h.function_code    = SCLP_FC_NORMAL_WRITE;
    write_sccb.h.control_mask[0]  = 0;
    write_sccb.h.control_mask[1]  = 0;
    write_sccb.h.control_mask[2]  = 0;
    write_sccb.h.response_code    = 0;

    write_sccb.ebh.length         = evbuf_len;
    write_sccb.ebh.type           = SCLP_EVENT_ASCII_CONSOLE;
    write_sccb.ebh.flags          = 0;
    write_sccb.ebh.reserved       = 0;

    for (size_t i = 0; i < len; ++i)
        write_sccb.data[i] = buf[i];

    int cc = servc_retry(SCLP_CMD_WRITE_EVENT_DATA, &write_sccb);
    if (cc == SERVC_CC_NOT_OPERATIONAL || cc == SERVC_CC_BUSY)
        return -1;

    return (write_sccb.h.response_code == SCLP_RC_NORMAL) ? 0 : -1;
}

#define SCLP_LINE_BUF_SIZE 256

void sclp_putc(const char c) {
    static char   line_buf[SCLP_LINE_BUF_SIZE];
    static size_t line_len = 0;

    if (c == '\n') {
        // Append \r\n and flush the complete line.
        if (line_len < SCLP_LINE_BUF_SIZE - 1) {
            line_buf[line_len++] = '\r';
        }
        line_buf[line_len++] = '\n';
        sclp_write(line_buf, line_len);
        line_len = 0;
    } else {
        line_buf[line_len++] = c;
        if (line_len >= SCLP_LINE_BUF_SIZE - 2) {
            sclp_write(line_buf, line_len);
            line_len = 0;
        }
    }
}
