// SPDX-License-Identifier: Apache-2.0
// zxfoundation/sys/printk.c
//
/// @brief Kernel logging — tag parsing, z/OS format, ring storage, sink emit.
///
///        FLOW
///        ====
///        vprintk(fmt, ap):
///          1. Parse optional level tag "\x01<N>" from fmt.
///          2. vsnprintf the message body into a stack buffer.
///          3. Strip trailing newline from body (we add our own).
///          4. Format the output line:  ZXF-SSSS.UUUUUU L body\n
///          5. Store a zx_log_record_t in the log ring (irqsave spinlock).
///          6. Emit the formatted line to the console sink (irqsave spinlock).
///
///        TAG PARSING
///        ===========
///        A tag is the 4-byte sequence "\x01<N>" where N is '0'..'7'.
///        If the format string does not start with \x01, level = ZX_LVL_INFO.
///        The tag is consumed before vsnprintf so it never appears in output.
///
///        TIMESTAMP
///        =========
///        ktime_get() returns nanoseconds since boot.  We split into seconds
///        and microseconds for the SSSS.UUUUUU field.  Before time_init()
///        runs, ktime_get() returns 0 → "ZXF-0000.000000" which is correct.

#include <arch/s390x/cpu/processor.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sys/log.h>
#include <zxfoundation/time/ktime.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/zconfig.h>
#include <lib/vsprintf.h>
#include <lib/string.h>

// ---------------------------------------------------------------------------
// Log ring storage
// ---------------------------------------------------------------------------

// Number of records the ring holds.
#define LOG_RING_RECORDS    (CONFIG_LOG_BUF_SIZE / sizeof(zx_log_record_t))

static zx_log_record_t s_log_ring[LOG_RING_RECORDS];
static uint32_t        s_ring_head;     // next write slot (mod LOG_RING_RECORDS)
static uint32_t        s_ring_count;    // records stored (saturates at LOG_RING_RECORDS)
static uint16_t        s_seq;           // global sequence counter
static spinlock_t      s_ring_lock = SPINLOCK_INIT;

void log_ring_init(void) {
    s_ring_head  = 0;
    s_ring_count = 0;
    s_seq        = 0;
}

void log_ring_store(const zx_log_record_t *rec) {
    irqflags_t f;
    spin_lock_irqsave(&s_ring_lock, &f);

    s_log_ring[s_ring_head] = *rec;
    s_ring_head = (s_ring_head + 1) % LOG_RING_RECORDS;
    if (s_ring_count < LOG_RING_RECORDS)
        s_ring_count++;

    spin_unlock_irqrestore(&s_ring_lock, f);
}

uint16_t log_ring_last_seq(void) {
    return s_seq;
}

uint32_t log_ring_read(uint16_t start_seq, zx_log_record_t *out, uint32_t count) {
    irqflags_t f;
    spin_lock_irqsave(&s_ring_lock, &f);

    // Oldest record is at (head - count) mod LOG_RING_RECORDS.
    uint32_t oldest = (s_ring_head + LOG_RING_RECORDS - s_ring_count) % LOG_RING_RECORDS;
    uint32_t copied = 0;

    for (uint32_t i = 0; i < s_ring_count && copied < count; i++) {
        uint32_t idx = (oldest + i) % LOG_RING_RECORDS;
        if ((uint16_t)(s_log_ring[idx].seq - start_seq) < 0x8000U) {
            out[copied++] = s_log_ring[idx];
        }
    }

    spin_unlock_irqrestore(&s_ring_lock, f);
    return copied;
}

// ---------------------------------------------------------------------------
// Console sink
// ---------------------------------------------------------------------------

static printk_putc_sink s_sink;
static spinlock_t       s_sink_lock = SPINLOCK_INIT;

void printk_initialize(printk_putc_sink sink) {
    s_sink = sink;
    log_ring_init();
}

void printk_set_sink(printk_putc_sink sink) {
    s_sink = sink;
}

static void sink_emit(const char *buf, size_t len) {
    if (!s_sink) return;
    irqflags_t f;
    spin_lock_irqsave(&s_sink_lock, &f);
    for (size_t i = 0; i < len; i++)
        s_sink(buf[i]);
    spin_unlock_irqrestore(&s_sink_lock, f);
}

// ---------------------------------------------------------------------------
// Level helpers
// ---------------------------------------------------------------------------

// Level display characters indexed by zx_log_level_t value.
static const char s_level_char[8] = { 'X', 'A', 'C', 'E', 'W', 'N', 'I', 'D' };

/// @brief Parse the optional "\x01<N>" tag from *fmt.
///        Advances *fmt past the tag if found.
/// @return Parsed level, or ZX_LVL_INFO if no tag.
static zx_log_level_t parse_level(const char **fmt) {
    const char *p = *fmt;
    // Tag: 0x01 '<' digit '>'  — exactly 4 bytes.
    if (p[0] == '\x01' && p[1] == '<' && p[2] >= '0' && p[2] <= '7' && p[3] == '>') {
        *fmt += 4;
        return (zx_log_level_t)(p[2] - '0');
    }
    return ZX_LVL_INFO;
}

// ---------------------------------------------------------------------------
// Timestamp + prefix formatting
// ---------------------------------------------------------------------------

/// @brief Format "ZXF-SSSS.UUUUUU [cpuNN] L " into buf.
///        Returns number of bytes written (no NUL counted).
static int fmt_prefix(char *buf, size_t bufsz, ktime_t ts, zx_log_level_t level, uint8_t cpu) {
    uint64_t secs = ts / 1000000000ULL;
    uint64_t usec = (ts % 1000000000ULL) / 1000ULL;
    return snprintf(buf, bufsz, "ZXF-%04llu.%06llu [cpu%02u] %c ",
                    (unsigned long long)(secs > 9999 ? 9999 : secs),
                    (unsigned long long)usec,
                    (unsigned)cpu,
                    s_level_char[level & 7]);
}

// ---------------------------------------------------------------------------
// Core vprintk
// ---------------------------------------------------------------------------

void printk_flush(const char *buf, size_t size) {
    sink_emit(buf, size);
}

int vprintk(const char *fmt, va_list ap) {
    zx_log_level_t level = parse_level(&fmt);

    // Format the message body.
    char body[ZX_LOG_MSG_MAX];
    int  body_len = vsnprintf(body, sizeof(body), fmt, ap);
    if (body_len < 0) body_len = 0;
    if ((size_t)body_len >= sizeof(body)) body_len = (int)sizeof(body) - 1;

    // Strip trailing newline — we add our own in the output line.
    while (body_len > 0 && (body[body_len - 1] == '\n' || body[body_len - 1] == '\r'))
        body_len--;
    body[body_len] = '\0';

    ktime_t ts = ktime_get();

    // Build and store the log record.
    zx_log_record_t rec;
    rec.timestamp = ts;
    rec.level     = (uint8_t)level;
    rec.cpu       = (uint8_t)arch_smp_processor_id();
    rec.seq       = s_seq++;
    // Copy body into record (already NUL-terminated, fits in ZX_LOG_MSG_MAX).
    memcpy(rec.msg, body, (size_t)body_len + 1);
    log_ring_store(&rec);

    // Build the formatted output line: prefix + body + '\n'.
    char line[32 + ZX_LOG_MSG_MAX + 2]; // prefix(~28) + body + '\n' + NUL
    int  prefix_len = fmt_prefix(line, sizeof(line), ts, level, rec.cpu);
    memcpy(line + prefix_len, body, (size_t)body_len);
    int total = prefix_len + body_len;
    line[total++] = '\n';

    sink_emit(line, (size_t)total);
    return total;
}

int printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}
