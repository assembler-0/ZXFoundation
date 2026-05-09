// SPDX-License-Identifier: Apache-2.0
// include/zxfoundation/sys/log.h
//
/// @brief ZXFoundation kernel logging subsystem.
///
///        LOG LEVELS
///        ==========
///        Eight levels, Linux-compatible numeric values, z/OS-style single-
///        character display codes:
///
///          Level  Tag string   Display  Meaning
///          -----  ----------   -------  -------
///            0    ZX_EMERG     X        System unusable — imminent halt
///            1    ZX_ALERT     A        Action required immediately
///            2    ZX_CRIT      C        Critical condition
///            3    ZX_ERROR     E        Error condition
///            4    ZX_WARN      W        Warning
///            5    ZX_NOTICE    N        Normal but significant
///            6    ZX_INFO      I        Informational (default)
///            7    ZX_DEBUG     D        Debug detail

#pragma once

#include <zxfoundation/types.h>
#include <zxfoundation/time/ktime.h>

#define ZX_EMERG    "\x01<0>"   ///< Level 0 — system unusable
#define ZX_ALERT    "\x01<1>"   ///< Level 1 — action required
#define ZX_CRIT     "\x01<2>"   ///< Level 2 — critical
#define ZX_ERROR    "\x01<3>"   ///< Level 3 — error
#define ZX_WARN     "\x01<4>"   ///< Level 4 — warning
#define ZX_NOTICE   "\x01<5>"   ///< Level 5 — notice
#define ZX_INFO     "\x01<6>"   ///< Level 6 — informational (default)
#define ZX_DEBUG    "\x01<7>"   ///< Level 7 — debug

/// @brief Numeric log level values.
typedef enum {
    ZX_LVL_EMERG  = 0,
    ZX_LVL_ALERT  = 1,
    ZX_LVL_CRIT   = 2,
    ZX_LVL_ERROR  = 3,
    ZX_LVL_WARN   = 4,
    ZX_LVL_NOTICE = 5,
    ZX_LVL_INFO   = 6,
    ZX_LVL_DEBUG  = 7,
} zx_log_level_t;

/// @brief Maximum length of the message body in a stored record (including NUL).
#define ZX_LOG_MSG_MAX  116U

/// @brief One stored log record.
///        Fixed size so the ring buffer holds an integer number of records
///        and random-access by index is O(1).
typedef struct __attribute__((packed)) {
    ktime_t         timestamp;              ///< ktime_get() at emit time (ns).
    uint8_t         level;                  ///< zx_log_level_t value.
    uint8_t         cpu;                    ///< Logical CPU ID.
    uint16_t        seq;                    ///< Sequence number (wraps at 65535).
    char            msg[ZX_LOG_MSG_MAX];    ///< NUL-terminated message body.
} zx_log_record_t;

_Static_assert(sizeof(zx_log_record_t) == 128, "zx_log_record_t must be 128 bytes");


/// @brief Initialize the log ring.  Called from printk_initialize().
void log_ring_init(void);

/// @brief Store a record in the ring.  Overwrites oldest on overflow.
///        Safe to call with IRQs disabled (spinlock, irqsave internally).
void log_ring_store(const zx_log_record_t *rec);

/// @brief Read up to @p count records starting at sequence @p start_seq
///        into @p out.  Records older than the ring's oldest are skipped.
/// @return Number of records copied.
uint32_t log_ring_read(uint16_t start_seq, zx_log_record_t *out, uint32_t count);

/// @brief Return the sequence number of the most recently stored record,
///        or 0 if the ring is empty.
uint16_t log_ring_last_seq(void);
