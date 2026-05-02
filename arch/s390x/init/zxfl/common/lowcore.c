// SPDX-License-Identifier: Apache-2.0
// arch/s390x/init/zxfl/common/lowcore.c
//
// Lowcore new PSW setup is performed in entry.S (MVC + SSM) before BSS is
// zeroed, so it runs before any C code. This file is intentionally minimal.

#include <arch/s390x/init/zxfl/lowcore.h>
