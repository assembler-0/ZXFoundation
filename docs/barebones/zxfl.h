// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl.h
//
/// @brief ZXFoundation Boot Protocol — contract between loader and kernel.

#ifndef ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
#define ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Protocol version and magic
// ---------------------------------------------------------------------------

#define STFLE_MAX_DWORDS    32U
#define ZXFL_SHA256_DIGEST_SIZE    32U


#define ZXFL_MAGIC              0x5A58464CU   // "ZXFL"
#define ZXFL_VERSION_4          0x00000004U

#define ZXFL_LOADER_MAJOR       1U
#define ZXFL_LOADER_MINOR       0U

// ---------------------------------------------------------------------------
// Loader identity seed (used to derive the binding token).
// ---------------------------------------------------------------------------
#define ZXFL_SEED               UINT64_C(0xA5F0C3E1B2D49687)

// ---------------------------------------------------------------------------
// Protocol flags — each flag indicates that a corresponding data section
// in the boot protocol has been populated and is valid.
// ---------------------------------------------------------------------------

#define ZXFL_FLAG_SMP           (1U << 0)   ///< cpu_map[] is valid
#define ZXFL_FLAG_MEM_MAP       (1U << 1)   ///< mem_map is valid
#define ZXFL_FLAG_CMDLINE       (1U << 2)   ///< cmdline_addr is valid
#define ZXFL_FLAG_LOWCORE       (1U << 3)   ///< lowcore_phys is valid
#define ZXFL_FLAG_STFLE         (1U << 4)   ///< stfle_fac[] is valid
#define ZXFL_FLAG_SYSINFO       (1U << 5)   ///< system identification is valid
#define ZXFL_FLAG_TOD           (1U << 6)   ///< tod_boot is valid

/// @brief Maximum memory regions in the static map.
#define ZXFL_MEM_MAP_MAX        16U

/// @brief Maximum CPUs in the static cpu_map.
#define ZXFL_CPU_MAP_MAX        64U

/// @brief Memory region types.
#define ZXFL_MEM_USABLE         0x01U   ///< Conventional RAM
#define ZXFL_MEM_RESERVED       0x02U   ///< Firmware/loader reserved
#define ZXFL_MEM_LOADER         0x03U   ///< Occupied by the loader itself
#define ZXFL_MEM_KERNEL         0x04U   ///< Occupied by the loaded kernel

/// @brief CPU state values in zxfl_cpu_info_t::state.
#define ZXFL_CPU_ONLINE         0x01U   ///< CPU is running (BSP)
#define ZXFL_CPU_STOPPED        0x02U   ///< CPU is stopped (AP, ready for SIGP)
#define ZXFL_CPU_UNKNOWN        0x00U   ///< State could not be determined

/// @brief CPU type values in zxfl_cpu_info_t::type.
#define ZXFL_CPU_TYPE_CP        0x00U   ///< General-purpose CP
#define ZXFL_CPU_TYPE_IFL       0x01U   ///< Integrated Facility for Linux
#define ZXFL_CPU_TYPE_UNKNOWN   0xFFU   ///< Unknown type

/// @brief Physical lowcore address (always 0 on z/Architecture).
#define ZXFL_LOWCORE_PHYS       0x0ULL

/// @brief One entry in the physical memory map.
typedef struct {
    uint64_t base;      ///< Physical base address
    uint64_t length;    ///< Length in bytes
    uint32_t type;      ///< ZXFL_MEM_* constant
    uint32_t _pad;
} zxfl_mem_region_t;

/// @brief One entry in the CPU map.
typedef struct {
    uint16_t cpu_addr;  ///< z/Arch CPU address (used with SIGP)
    uint8_t  type;      ///< ZXFL_CPU_TYPE_* constant
    uint8_t  state;     ///< ZXFL_CPU_* constant
    uint32_t _pad;
} zxfl_cpu_info_t;

/// @brief System identification — populated from STSI.
///        All strings are EBCDIC from the hardware; the loader converts
///        them to ASCII before filling this struct.
typedef struct {
    char     manufacturer[16]; ///< STSI 1.1.1 manufacturer (NUL-padded ASCII)
    char     type[4];          ///< STSI 1.1.1 machine type (e.g. "2096")
    char     model[16];        ///< STSI 1.1.1 model identifier
    char     sequence[16];     ///< STSI 1.1.1 serial number
    char     plant[4];         ///< STSI 1.1.1 manufacturing plant
    char     lpar_name[8];     ///< STSI 2.2.2 LPAR name (NUL-padded ASCII)
    uint16_t lpar_number;      ///< STSI 2.2.2 LPAR number
    uint16_t cpus_total;       ///< STSI 1.2.2 total CPUs in CEC
    uint16_t cpus_configured;  ///< STSI 1.2.2 configured CPUs
    uint16_t cpus_standby;     ///< STSI 1.2.2 standby CPUs
    uint32_t capability;       ///< STSI 1.2.2 CPU capability rating
    uint32_t _pad;
} zxfl_sysinfo_t;

typedef struct {
    // ---- HEADER (16 bytes) ----
    uint32_t magic;             ///< ZXFL_MAGIC
    uint32_t version;           ///< ZXFL_VERSION_4
    uint32_t flags;             ///< ZXFL_FLAG_* bitmask
    uint32_t _pad0;

    // ---- BINDING TOKEN (8 bytes) ----
    uint64_t binding_token;

    // ---- LOADER IDENTITY (16 bytes) ----
    uint16_t loader_major;      ///< ZXFL_LOADER_MAJOR
    uint16_t loader_minor;      ///< ZXFL_LOADER_MINOR
    uint32_t loader_timestamp;  ///< Compile-time timestamp (ZXFL_BUILD_TS macro)
    uint64_t loader_build_id;   ///< Reserved; set to 0

    // ---- DEVICE (16 bytes) ----
    uint32_t ipl_schid;         ///< Subchannel ID of the IPL device
    uint16_t ipl_dev_type;      ///< Device type (e.g. 0x3390)
    uint16_t ipl_dev_model;     ///< Device model (e.g. 0x000C for 3390-12)
    uint32_t _pad1;
    uint32_t _pad2;

    // ---- KERNEL IMAGE (24 bytes) ----
    uint64_t kernel_phys_start; ///< Physical base of the loaded kernel image
    uint64_t kernel_phys_end;   ///< Physical end (exclusive)
    uint64_t kernel_entry;      ///< Entry point (HHDM virtual address)

    // ---- MEMORY MAP (24 bytes) ----
    uint64_t mem_total_bytes;   ///< Total detected RAM (bytes)
    uint64_t mem_map_addr;      ///< HHDM virtual address of zxfl_mem_region_t[]
    uint32_t mem_map_count;     ///< Number of valid entries in mem_map
    uint32_t _pad3;

    // ---- FACILITIES (264 bytes) ----
    uint64_t stfle_fac[STFLE_MAX_DWORDS]; ///< Full STFLE output (32 dwords)
    uint32_t stfle_count;       ///< Actual dwords returned by STFLE
    uint32_t _pad4;

    // ---- LOWCORE (8 bytes) ----
    uint64_t lowcore_phys;      ///< Physical address of BSP lowcore (always 0x0)

    // ---- COMMAND LINE (16 bytes) ----
    uint64_t cmdline_addr;      ///< HHDM virtual address of NUL-terminated cmdline
    uint32_t cmdline_len;       ///< Length in bytes (excluding NUL)
    uint32_t _pad6;

    // ---- KERNEL STACK (8 bytes) ----
    uint64_t kernel_stack_top;  ///< HHDM virtual address of initial stack top

    // ---- CONTROL REGISTER SNAPSHOT (24 bytes) ----
    uint64_t cr0_snapshot;      ///< CR0 at time of kernel jump
    uint64_t cr1_snapshot;      ///< CR1 (ASCE) at time of kernel jump
    uint64_t cr14_snapshot;     ///< CR14 at time of kernel jump

    // ---- PAGE TABLE POOL (8 bytes) ----
    uint64_t pgtbl_pool_end;    ///< Physical end (exclusive) of the bootloader's
                                ///<   page-table bump pool.  The kernel PMM must
                                ///<   reserve [pool_base, pgtbl_pool_end) to avoid
                                ///<   overwriting live DAT tables.

    // ---- SMP / CPU MAP (512 + 8 bytes) ----
    zxfl_cpu_info_t cpu_map[ZXFL_CPU_MAP_MAX]; ///< CPU map (valid when FLAG_SMP)
    uint32_t cpu_count;         ///< Number of valid entries in cpu_map
    uint16_t bsp_cpu_addr;      ///< CPU address of the boot processor
    uint16_t _pad7;

    // ---- SYSTEM IDENTIFICATION (72 bytes) ----
    zxfl_sysinfo_t sysinfo;     ///< Machine info (valid when FLAG_SYSINFO)

    // ---- TOD CLOCK (8 bytes) ----
    uint64_t tod_boot;          ///< TOD clock value at boot (STCK)

    // ---- MODULES ----
    uint32_t module_count;
    uint32_t _pad8;
    struct {
        char     name[32];      ///< Module name (NUL-terminated)
        uint64_t phys_start;    ///< Physical start address
        uint64_t size_bytes;    ///< Size of the module in bytes
    } modules[16];

} zxfl_boot_protocol_t;

#endif /* ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H */
