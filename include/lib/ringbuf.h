// SPDX-License-Identifier: Apache-2.0
// include/lib/ringbuf.h
//
/// @brief Generic freestanding power-of-2 byte ring buffer.

#pragma once

#include <zxfoundation/types.h>

/// @brief Ring buffer descriptor.  Embed in the owning structure or declare
///        as a static.  Initialize with RINGBUF_INIT or ringbuf_init().
typedef struct {
    uint8_t  *buf;          ///< Backing storage (caller-provided).
    uint32_t  capacity;     ///< Size of buf[] in bytes — MUST be power of 2.
    uint32_t  head;         ///< Write index (monotonic).
    uint32_t  tail;         ///< Read index (monotonic).
} ringbuf_t;

/// @brief Static initializer.
/// @param _buf   Array name of the backing storage.
/// @param _cap   sizeof(_buf) — must be a power of 2.
#define RINGBUF_INIT(_buf, _cap) { .buf = (_buf), .capacity = (_cap), .head = 0, .tail = 0 }

/// @brief Initialize a ring buffer at runtime.
/// @param rb    Ring buffer to initialize.
/// @param buf   Backing storage.
/// @param cap   Size of buf in bytes — must be a power of 2.
static inline void ringbuf_init(ringbuf_t *rb, uint8_t *buf, uint32_t cap) {
    rb->buf      = buf;
    rb->capacity = cap;
    rb->head     = 0;
    rb->tail     = 0;
}

/// @brief Number of bytes currently stored.
static inline uint32_t ringbuf_len(const ringbuf_t *rb) {
    return rb->head - rb->tail;
}

/// @brief Number of bytes of free space.
static inline uint32_t ringbuf_free(const ringbuf_t *rb) {
    return rb->capacity - ringbuf_len(rb);
}

/// @brief True if the buffer contains no data.
static inline bool ringbuf_empty(const ringbuf_t *rb) {
    return rb->head == rb->tail;
}

/// @brief True if the buffer is full.
static inline bool ringbuf_full(const ringbuf_t *rb) {
    return ringbuf_len(rb) == rb->capacity;
}

/// @brief Write up to @p len bytes from @p src into the ring buffer.
///        Drops the oldest data (advances tail) if the buffer is full,
///        ensuring the most recent @p capacity bytes are always retained.
/// @return Number of bytes written (always == len if len <= capacity).
uint32_t ringbuf_write(ringbuf_t *rb, const uint8_t *src, uint32_t len);

/// @brief Read and consume up to @p len bytes into @p dst.
/// @return Number of bytes actually read.
uint32_t ringbuf_read(ringbuf_t *rb, uint8_t *dst, uint32_t len);

/// @brief Peek at up to @p len bytes without consuming them.
/// @return Number of bytes copied into @p dst.
uint32_t ringbuf_peek(const ringbuf_t *rb, uint8_t *dst, uint32_t len);
