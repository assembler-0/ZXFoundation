#ifndef ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
#define ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H

#include <zxfoundation/types.h>

/// @brief ZXFL Magic signature ("ZXFL")
#define ZXFL_MAGIC 0x5A58464C
#define ZXFL_VERSION_1 0x00000001

/// @brief Memory region types
#define ZXFL_MEM_AVAILABLE 1
#define ZXFL_MEM_RESERVED  2
#define ZXFL_MEM_KERNEL    3
#define ZXFL_MEM_RAMDISK   4
#define ZXFL_MEM_LOADER    5

/// @brief Memory map entry descriptor
typedef struct {
    uint64_t start;
    uint64_t size;
    uint32_t type;
} zxfl_mmap_entry_t;

/// @brief The ZXFL boot protocol passed to the kernel
typedef struct {
    uint32_t magic;           ///< Magic signature (ZXFL_MAGIC)
    uint32_t version;         ///< Protocol version
    
    // I/O & Device Info
    uint32_t ipl_schid;       ///< Subchannel ID of the IPL device
    uint32_t ipl_dev_type;    ///< Device type (e.g., 3390)
    
    // Memory & Regions
    uint64_t kernel_start;    ///< Physical start address of the kernel
    uint64_t kernel_size;     ///< Total size of the loaded kernel
    uint64_t ramdisk_start;   ///< Physical start address of initrd (if any, 0 otherwise)
    uint64_t ramdisk_size;    ///< Size of initrd
    uint64_t loader_start;    ///< Physical start address of this loader (for reclamation)
    uint64_t loader_size;     ///< Size of the loader footprint
    
    // Machine State / Facilities (gathered by loader)
    uint64_t stfle_fac[4];    ///< 256 bits of hardware facilities (STFLE output)
    
    // Dynamic Structures
    uint32_t mmap_count;      ///< Number of memory map entries
    zxfl_mmap_entry_t* mmap;  ///< Pointer to the memory map array
    
    // Boot Configuration
    char* cmdline;            ///< Pointer to the command line string
} zxfl_boot_protocol_t;

#endif // ZXFOUNDATION_ZXFL_BOOT_PROTOCOL_H
