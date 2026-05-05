// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxvl_cksum.c
//
// Placeholder checksum table.

#include <arch/s390x/init/zxfl/zxvl_private.h>

__attribute__((section(".zxvl_checksums"), used))
zxvl_checksum_table_t zxvl_kernel_checksum_table = { 0 };
