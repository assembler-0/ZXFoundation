// SPDX-License-Identifier: Apache-2.0
// lib/ringbuf.c

#include <lib/ringbuf.h>
#include <lib/string.h>

uint32_t ringbuf_write(ringbuf_t *rb, const uint8_t *src, uint32_t len) {
    if (!len) return 0;

    // If len exceeds capacity, only keep the last capacity bytes.
    if (len > rb->capacity) {
        src += len - rb->capacity;
        len  = rb->capacity;
    }

    // Evict oldest bytes to make room.
    uint32_t overflow = ringbuf_len(rb) + len;
    if (overflow > rb->capacity)
        rb->tail += overflow - rb->capacity;

    uint32_t mask = rb->capacity - 1;
    uint32_t head = rb->head & mask;
    uint32_t first = rb->capacity - head;   // bytes until wrap

    if (len <= first) {
        memcpy(rb->buf + head, src, len);
    } else {
        memcpy(rb->buf + head, src, first);
        memcpy(rb->buf,        src + first, len - first);
    }

    rb->head += len;
    return len;
}

uint32_t ringbuf_read(ringbuf_t *rb, uint8_t *dst, uint32_t len) {
    uint32_t avail = ringbuf_len(rb);
    if (len > avail) len = avail;
    if (!len) return 0;

    uint32_t mask = rb->capacity - 1;
    uint32_t tail = rb->tail & mask;
    uint32_t first = rb->capacity - tail;

    if (len <= first) {
        memcpy(dst, rb->buf + tail, len);
    } else {
        memcpy(dst,         rb->buf + tail, first);
        memcpy(dst + first, rb->buf,        len - first);
    }

    rb->tail += len;
    return len;
}

uint32_t ringbuf_peek(const ringbuf_t *rb, uint8_t *dst, uint32_t len) {
    uint32_t avail = ringbuf_len(rb);
    if (len > avail) len = avail;
    if (!len) return 0;

    uint32_t mask = rb->capacity - 1;
    uint32_t tail = rb->tail & mask;
    uint32_t first = rb->capacity - tail;

    if (len <= first) {
        memcpy(dst, rb->buf + tail, len);
    } else {
        memcpy(dst,         rb->buf + tail, first);
        memcpy(dst + first, rb->buf,        len - first);
    }

    return len;
}
