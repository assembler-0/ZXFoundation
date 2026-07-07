// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/init/zxfl/zxfl.h
//
/// @brief ZXFoundation Boot Protocol — contract between loader and kernel.

#ifndef ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
#define ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H

// ---------------------------------------------------------------------------
// Protocol version and magic
// ---------------------------------------------------------------------------

#define ZXFL_MAGIC              0x5A58464CU   // "ZXFL"
#define ZXFL_VERSION_4          0x00000004U

#define ZXFL_LOADER_MAJOR       1U
#define ZXFL_LOADER_MINOR       0U

// ---------------------------------------------------------------------------
// Loader identity seed (used to derive the binding token).
// ---------------------------------------------------------------------------
#define ZXFL_SEED               0xA5F0C3E1B2D49687UL

// ---------------------------------------------------------------------------
// Offsets for assembly use
// ---------------------------------------------------------------------------
#define ZXFL_OFFSET_KERNEL_STACK_TOP 400

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
#define ZXFL_FLAG_SCLP          (1U << 8)   ///< sclp_info is valid
#define ZXFL_FLAG_ECAG          (1U << 12)  ///< ecag_info is valid

/// @brief Modules limit
#define ZXFL_MAX_MODULES        16U
#define ZXFL_MAX_MODULE_NAME    32U

/// @brief Maximum memory regions in the static map.
#define ZXFL_MEM_MAP_MAX        16U

/// @brief Maximum CPUs in the static cpu_map.
#define ZXFL_CPU_MAP_MAX        CONFIG_ZX_MAX_CPUS

/// @brief Memory region types.
#define ZXFL_MEM_USABLE         0x01U   ///< Conventional RAM
#define ZXFL_MEM_RESERVED       0x02U   ///< Firmware/loader reserved
#define ZXFL_MEM_LOADER         0x03U   ///< Occupied by the loader itself
#define ZXFL_MEM_KERNEL         0x04U   ///< Occupied by the loaded kernel

#define ZXFL_MEMPROBE_UNKNOWN   0
#define ZXFL_MEMPROBE_SCLP      1U
#define ZXFL_MEMPROBE_DIAG      2U
#define ZXFL_MEMPROBE_TPROT     3U

/// @brief CPU state values in zxfl_cpu_info_t::state.
#define ZXFL_CPU_ONLINE         0x01U   ///< CPU is running (BSP)
#define ZXFL_CPU_STOPPED        0x02U   ///< CPU is stopped (AP, ready for SIGP)
#define ZXFL_CPU_UNKNOWN        0x00U   ///< State could not be determined

/// @brief CPU type values in zxfl_cpu_info_t::type (ZXFL-internal, NOT PoP hw codes).
#define ZXFL_CPU_TYPE_CP        1U   ///< General-purpose CP
#define ZXFL_CPU_TYPE_IFL       2U   ///< Integrated Facility for Linux
#define ZXFL_CPU_TYPE_ICF       3U   ///< Internal Coupling Facility
#define ZXFL_CPU_TYPE_ZIIP      4U   ///< zIIP
#define ZXFL_CPU_TYPE_ZAAP      5U   ///< zAAP (z/Arch Application Assist Processor)
#define ZXFL_CPU_TYPE_UNKNOWN   0xFFU   ///< Unknown type

/// @brief CPU topology evidence flags in zxfl_cpu_info_t::topology_evidence.
#define ZXFL_CPU_EVIDENCE_NONE      0x00U ///< No CPU topology evidence is available
#define ZXFL_CPU_EVIDENCE_SIGP      0x01U ///< CPU address was confirmed by SIGP Sense
#define ZXFL_CPU_EVIDENCE_STSI      0x02U ///< CPU topology was derived from STSI 15.1.x
#define ZXFL_CPU_EVIDENCE_FALLBACK  0x04U ///< Loader used deterministic degraded fallback

/// @brief Physical lowcore address (always 0 on z/Architecture).
#define ZXFL_LOWCORE_PHYS       0x0ULL

#ifndef __ASSEMBLER__

#include <zxfoundation/types.h>
#include <zxfoundation/zxconfig.h>
#include <arch/s390x/init/zxfl/stfle.h>

/// @brief One entry in the physical memory map.
typedef struct {
    uint64_t base;      ///< Physical base address
    uint64_t length;    ///< Length in bytes
    uint32_t type;      ///< ZXFL_MEM_* constant
    uint8_t  numa_node; ///< NUMA node ID
    uint8_t  _pad[3];
} zxfl_mem_region_t;

/// @brief One entry in the CPU map.
typedef struct {
    uint16_t cpu_addr;  ///< z/Arch CPU address (used with SIGP)
    uint8_t  type;      ///< ZXFL_CPU_TYPE_* constant
    uint8_t  state;     ///< ZXFL_CPU_* constant
    uint8_t  numa_node; ///< NUMA node ID (derived from book/socket)
    uint8_t  drawer_id; ///< Drawer ID from STSI 15.1.x
    uint8_t  book_id;   ///< Book ID from STSI 15.1.x
    uint8_t  socket_id; ///< Socket ID from STSI 15.1.x
    uint8_t  core_id;   ///< Core-group ID from STSI 15.1.x or degraded fallback
    uint8_t  smt_id;    ///< SMT thread ID within core, or zero when unproven
    uint8_t  topology_evidence; ///< ZXFL_CPU_EVIDENCE_* provenance flags
    uint8_t  capacity;  ///< Scheduler capacity percentage, 100 when unknown
} zxfl_cpu_info_t;

/// @brief System identification — populated from STSI.
///        All strings are EBCDIC from the hardware; the loader converts
///        them to ASCII before filling this struct.
typedef struct {
    // STSI 1.1.1 (machine-level)
    char     manufacturer[16]; ///< STSI 1.1.1 manufacturer (NUL-padded ASCII)
    char     type[4];          ///< STSI 1.1.1 machine type (e.g. "2096")
    char     model[16];        ///< STSI 1.1.1 model identifier
    char     sequence[16];     ///< STSI 1.1.1 serial number
    char     plant[4];         ///< STSI 1.1.1 manufacturing plant
    char     model_capacity[16];   ///< STSI 1.1.1 model-capacity string
    char     model_perm_cap[16];   ///< STSI 1.1.1 permanent capacity
    char     model_temp_cap[16];   ///< STSI 1.1.1 temporary capacity
    char     model_var_cap[16];    ///< STSI 1.1.1 variable capacity
    uint32_t model_cap_rating;     ///< STSI 1.1.1 model capacity rating
    uint32_t model_perm_cap_rating;///< STSI 1.1.1 permanent cap rating
    uint32_t model_temp_cap_rating;///< STSI 1.1.1 temporary cap rating
    uint32_t model_var_cap_rating; ///< STSI 1.1.1 variable cap rating
    uint32_t ncr;                  ///< STSI 1.1.1 nominal cap rating
    uint32_t npr;                  ///< STSI 1.1.1 nominal permanent rating
    uint32_t ntr;                  ///< STSI 1.1.1 nominal temporary rating
    uint32_t nvr;                  ///< STSI 1.1.1 nominal variable rating
    uint8_t  typepct[5];           ///< STSI 1.1.1 CPU type percentages

    // STSI 2.2.2 (LPAR-level)
    char     lpar_name[8];     ///< STSI 2.2.2 LPAR name (NUL-padded ASCII)
    uint16_t lpar_number;      ///< STSI 2.2.2 LPAR number
    uint8_t  lpar_characteristics; ///< STSI 2.2.2 flags (dedicated/shared/limited)
    uint16_t lpar_cpus_dedicated;  ///< STSI 2.2.2 dedicated CPUs
    uint16_t lpar_cpus_shared;     ///< STSI 2.2.2 shared CPUs
    uint32_t lpar_caf;             ///< STSI 2.2.2 CPU adjustment factor
    uint8_t  lpar_mt_installed;    ///< STSI 2.2.2 multi-threading installed
    uint8_t  lpar_mt_stid;         ///< STSI 2.2.2 S-MTID
    uint8_t  lpar_mt_gtid;         ///< STSI 2.2.2 G-MTID
    uint8_t  lpar_vsne;            ///< STSI 2.2.2 name encoding
    uint8_t  lpar_uuid[16];        ///< STSI 2.2.2 LPAR UUID

    // STSI 1.2.2 (machine capacity)
    uint16_t cpus_total;       ///< STSI 1.2.2 total CPUs in CEC
    uint16_t cpus_configured;  ///< STSI 1.2.2 configured CPUs
    uint16_t cpus_standby;     ///< STSI 1.2.2 standby CPUs
    uint16_t cpus_reserved;    ///< STSI 1.2.2 reserved CPUs
    uint32_t capability;       ///< STSI 1.2.2 CPU capability rating
    uint32_t nominal_cap;      ///< STSI 1.2.2 nominal capacity
    uint32_t secondary_cap;    ///< STSI 1.2.2 secondary capacity
} zxfl_sysinfo_t;

/// @brief SCLP facility flags detected by the loader.
typedef struct {
    uint64_t facilities;    ///< Raw SCLP facilities mask
    uint32_t max_cores;     ///< Maximum cores reported by SCLP
    uint32_t mtid;          ///< Maximum multi-threading ID
    uint32_t mtid_cp;       ///< MTID for CP
    uint32_t ibc;           ///< Interpretation block controls
    uint64_t rnmax;         ///< Max number of real memory slots
    uint64_t rzm;           ///< Real memory slot size (bytes)
    uint64_t hamax;         ///< Max host address mode
    uint64_t hsa_size;      ///< HSA size (bytes)
    uint64_t hmfai;         ///< Hypervisor mgmt facility attr index
    uint32_t feature_bits;  ///< Bitmask: sief2(0), siif(1), sigpif(2), gpere(3),
                            ///< ib(4), cei(5), skey(6), sclp_diag318(7), etc.
} zxfl_sclp_info_t;


/// @brief ECAG CPU attributes (cache + topology).
typedef struct {
    uint32_t cache_line_size;
    uint32_t cache_total_size;
    uint32_t cache_associativity;
    uint32_t _pad0;
    uint16_t numa_node;
    uint16_t socket_id;
    uint16_t core_id;
    uint8_t  smt_id;
    uint8_t  _pad1[9];
} zxfl_ecag_info_t;

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

    // ---- MEMORY MAP (32 bytes) ----
    uint64_t mem_total_bytes;   ///< Total detected RAM (bytes)
    uint64_t mem_map_addr;      ///< HHDM virtual address of zxfl_mem_region_t[]
    uint32_t mem_map_count;     ///< Number of valid entries in mem_map
    uint8_t  mem_detect_source; ///< 0=none, 1=SCLP, 2=DIAG260, 3=TPROT probe
    uint8_t  _pad_mem[3];
    uint64_t hhdm_phys_coverage_end; ///< Exclusive physical end covered by initial HHDM DAT

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
    uint64_t cr13_snapshot;     ///< CR13 at time of kernel jump

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
        char     name[ZXFL_MAX_MODULE_NAME];      ///< Module name (NUL-terminated)
        uint64_t phys_start;    ///< Physical start address
        uint64_t size_bytes;    ///< Size of the module in bytes
    } modules[ZXFL_MAX_MODULES];

    // ---- DYNAMIC SEGMENT ADDRESSES (discovered by p_flags scan) ----
    uint64_t cksum_table;  ///< Physical address of zxvl_checksum_table_t
    uint64_t lock_phys;         ///< Physical address of .zxfl_lock segment

    // ---- LOADER FINGERPRINT (32 bytes) ----
    uint8_t  loader_digest[32]; ///< SHA-256 of stage2 .text+.rodata+.data
                                ///< Verified by kernel to ensure ZXFL authenticity.

    // ---- OPTIONAL: SCLP INFO (pointer, valid when FLAG_SCLP) ----
    uint64_t sclp_info_addr;    ///< HHDM virt addr of zxfl_sclp_info_t

    // ---- OPTIONAL: ECAG INFO (pointer, valid when FLAG_ECAG) ----
    uint64_t ecag_info_addr;    ///< HHDM virt addr of zxfl_ecag_info_t

} zxfl_boot_protocol_t;

#endif /* !__ASSEMBLER__ */

#endif /* ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H */
