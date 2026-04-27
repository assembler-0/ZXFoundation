#ifndef ZXFOUNDATION_ZCONFIG_H
#define ZXFOUNDATION_ZCONFIG_H

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------
#define CONFIG_ULTRASPARK_RELEASE           "26h1"

// ---------------------------------------------------------------------------
// Debugging
// ---------------------------------------------------------------------------
/* #undef CONFIG_DEBUG */
#define CONFIG_EARLY_PRINTK                 1

// ---------------------------------------------------------------------------
// Console drivers
// ---------------------------------------------------------------------------
#define CONFIG_SCLP_CONSOLE                 1
#define CONFIG_SCLP_SERVC_MAX_RETRIES       3
#define CONFIG_SCLP_SERVC_BUSY_DELAY        100000

// ---------------------------------------------------------------------------
// s390x boot
// ---------------------------------------------------------------------------
#define CONFIG_KERNEL_LOAD_ADDRESS          0x10000
#define CONFIG_BOOT_STACK_SIZE              16384
#define CONFIG_S390X_SAVE_AREA              160

// ---------------------------------------------------------------------------
// s390x PSW constants
// (64-bit values — use ULL suffix so they are safe in both C and assembly)
// ---------------------------------------------------------------------------
#define CONFIG_PSW_ARCH_BITS                0x0000000180000000ULL
#define CONFIG_PSW_DISABLED_WAIT            0x0002000180000000ULL

// ---------------------------------------------------------------------------
// Computed / derived
// ---------------------------------------------------------------------------
#define CONFIG_HAVE_CONSOLE                 1

#endif /* ZXFOUNDATION_ZCONFIG_H */
