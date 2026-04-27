#pragma once

// SPDX-License-Identifier: Apache-2.0
// sclp.h - SCLP (Service Call Logical Processor) console driver

#include <zxfoundation/types.h>

// ---------------------------------------------------------------------------
// SCLP command words
// ---------------------------------------------------------------------------
#define SCLP_CMD_WRITE_EVENT_DATA   0x00760005U
#define SCLP_CMD_READ_EVENT_DATA    0x00770005U
#define SCLP_CMD_WRITE_EVENT_MASK   0x00780005U

// ---------------------------------------------------------------------------
// Event types (go in EventBufferHeader.type)
// ---------------------------------------------------------------------------
#define SCLP_EVENT_ASCII_CONSOLE    0x1aU   // ASCII console data (OpCmd)

// ---------------------------------------------------------------------------
// Event mask bits (used in Write Event Mask)
// ---------------------------------------------------------------------------
#define SCLP_EVENT_MASK_MSG_ASCII   0x00000040U

// ---------------------------------------------------------------------------
// SCCB response codes (in sccb_header_t.response_code after SERVC returns)
// ---------------------------------------------------------------------------
#define SCLP_RC_NORMAL              0x0020U // Normal completion
#define SCLP_RC_INSUFFICIENT_LEN    0x0300U // SCCB too short
#define SCLP_RC_INVALID_CMD         0x01f0U // Unrecognised command
#define SCLP_RC_INVALID_MASK_LEN    0x74f0U // Bad mask_length in event mask

// ---------------------------------------------------------------------------
// Misc constants
// ---------------------------------------------------------------------------
#define SCLP_FC_NORMAL_WRITE        0x00U   // function_code for normal writes
#define SCCB_SIZE                   4096U   // SCCBs must be 4KB-aligned

// ---------------------------------------------------------------------------
// SERVC condition codes
// ---------------------------------------------------------------------------
#define SERVC_CC_OK                 0       // Command accepted / completed
#define SERVC_CC_BUSY               2       // Interface busy, retry
#define SERVC_CC_NOT_OPERATIONAL    3       // SCLP not operational

// ---------------------------------------------------------------------------
// SCCB header - 8 bytes, must appear at the start of every SCCB.
// All fields are big-endian (s390x is big-endian natively).
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t length;            // Total SCCB length in bytes
    uint8_t  function_code;     // SCLP_FC_NORMAL_WRITE for writes
    uint8_t  control_mask[3];   // Reserved, set to 0
    uint16_t response_code;     // Filled by firmware on return
} __attribute__((packed)) sccb_header_t;

// ---------------------------------------------------------------------------
// Event Buffer Header - 6 bytes, prepended to each event payload.
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t length;    // Length of this event buffer including this header
    uint8_t  type;      // Event type: SCLP_EVENT_ASCII_CONSOLE
    uint8_t  flags;     // 0x00 on input; firmware sets 0x80 on acceptance
    uint16_t reserved;  // Must be 0
} __attribute__((packed)) sclp_evbuf_hdr_t;

// ---------------------------------------------------------------------------
// Write Event Mask SCCB
// Must be issued once at startup to tell the firmware which event types
// we intend to send. Without this, Write Event Data is silently ignored.
// ---------------------------------------------------------------------------
typedef struct {
    sccb_header_t h;
    uint16_t      _reserved;
    uint16_t      mask_length;      // Must be sizeof(uint32_t) = 4
    uint32_t      cp_receive_mask;  // Events we want to receive (0 = none)
    uint32_t      cp_send_mask;     // Events we want to send (SCLP_EVENT_MASK_MSG_ASCII)
    uint32_t      send_mask;        // Filled by firmware: what it can accept
    uint32_t      receive_mask;     // Filled by firmware: what it can send
} __attribute__((packed)) sclp_write_event_mask_t;

// ---------------------------------------------------------------------------
// Write Event Data SCCB - carries ASCII text to the console.
// The struct itself is 4KB-aligned; data[] holds the raw text payload.
// ---------------------------------------------------------------------------
#define SCCB_DATA_LEN \
    (SCCB_SIZE - sizeof(sccb_header_t) - sizeof(sclp_evbuf_hdr_t))

typedef struct {
    sccb_header_t    h;
    sclp_evbuf_hdr_t ebh;
    char             data[SCCB_DATA_LEN];
} __attribute__((packed, aligned(4096))) sclp_write_sccb_t;

// Separate 4KB-aligned buffer for the Write Event Mask command.
typedef struct {
    sclp_write_event_mask_t mask;
    char _pad[SCCB_SIZE - sizeof(sclp_write_event_mask_t)];
} __attribute__((packed, aligned(4096))) sclp_mask_sccb_t;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// sclp_setup  - must be called once before any output.
//               Registers the ASCII console event mask with the firmware.
//               Returns 0 on success, -1 if SCLP is not operational or
//               the firmware rejected the mask.
int  sclp_setup(void);

// sclp_write  - write len bytes from buf to the console.
//               Silently truncates to SCCB_DATA_LEN if len is too large.
//               Returns 0 on success, -1 on SERVC failure.
int  sclp_write(const char *buf, size_t len);

// sclp_putc   - write a single character; translates '\n' to '\r\n'.
void sclp_putc(char c);
