// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/sclp.h

#ifndef ZXFOUNDATION_S390X_SCLP_H
#define ZXFOUNDATION_S390X_SCLP_H

#include <zxfoundation/types.h>

/// @brief SCLP Command Codes
#define SCLP_CMDW_READ_SCP_INFO             0x00020001
#define SCLP_CMDW_READ_SCP_INFO_FORCED      0x00120001
#define SCLP_CMDW_READ_STORAGE_INFO         0x00040001
#define SCLP_CMDW_READ_CPU_INFO             0x00010001
#define SCLP_CMDW_CONFIGURE_CORE            0x00110001
#define SCLP_CMDW_DECONFIGURE_CORE          0x00100001

/// @brief SCLP Response Codes
#define SCLP_RC_NORMAL_COMPLETION           0x0010
#define SCLP_RC_SCCB_BOUNDARY_VIOLATION     0x0100
#define SCLP_RC_INVALID_SCLP_COMMAND        0x01f0
#define SCLP_RC_CONTAINED_SCCB_ERROR        0x0300
#define SCLP_RC_INSUFFICIENT_SCCB_LENGTH    0x03f0

#define SCLP_MAX_CORES                      512
#define SCLP_SERVC_MAX_RETRIES       100
#define SCLP_SERVC_BUSY_DELAY        100000

#ifndef __ASSEMBLER__

typedef uint16_t sclp_response_t;

struct sccb_header {
    uint16_t length;
    uint8_t  function_code;
    uint8_t  control_mask[3];
    sclp_response_t response_code;
} __attribute__((packed, aligned(8)));

struct read_info_sccb {
    struct sccb_header header;
    uint16_t rnmax;
    uint8_t  rnsize;
    uint8_t  _reserved0[23];
    uint16_t max_cores;
    uint8_t  _reserved1[94];
    uint64_t facilities;
    uint8_t  _reserved2[24];
} __attribute__((packed, aligned(4096)));

struct sclp_core_entry {
    uint8_t  core_id;
    uint8_t  _reserved0;
    uint8_t  _flags0;
    uint8_t  _flags1;
    uint8_t  _reserved1[3];
    uint8_t  _flags2;
    uint8_t  _reserved2[6];
    uint8_t  type;
    uint8_t  _reserved3;
} __attribute__((packed));

struct read_cpu_info_sccb {
    struct sccb_header header;
    uint16_t configured;
    uint16_t standby;
    uint16_t combined;
    uint8_t  _reserved[2];
    struct sclp_core_entry entries[SCLP_MAX_CORES];
} __attribute__((packed, aligned(4096)));

struct storage_info_entry {
    uint16_t rn;
    uint8_t  _reserved;
    uint8_t  type;
} __attribute__((packed));

struct read_storage_info_sccb {
    struct sccb_header header;
    uint16_t max_rn;
    uint16_t rnsize;
    uint16_t assigned_rn;
    uint8_t  _reserved[2];
    struct storage_info_entry entries[];
} __attribute__((packed, aligned(4096)));

/// @brief Low-level SCLP service call.
/// @param command SCLP command code.
/// @param sccb 4KB-aligned SCCB virtual address.
/// @return 0 on success, 1 if busy, or negative on error.
int arch_sclp_service_call(uint32_t command, void *sccb);

/// @brief Early memory detection via SCLP.
/// @param mem_size_out Pointer to store detected memory size.
/// @return 0 on success, or negative on error.
int arch_sclp_early_get_memsize(uint64_t *mem_size_out);

#endif /* __ASSEMBLER__ */

#endif /* ZXFOUNDATION_S390X_SCLP_H */
