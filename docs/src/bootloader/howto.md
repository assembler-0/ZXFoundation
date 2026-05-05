# How to Load Your Kernel with ZXFL

**Document Revision:** 26h1.0

This guide walks through every step required to produce a kernel image that ZXFL will accept and execute. Read the [Boot Protocol](protocol.md) and [ZXVL Verification](zxvl.md) pages first for background.

---

## Overview

ZXFL imposes five requirements on the kernel image before it will execute it:

1. Valid ELF64 for s390x, `ET_EXEC`, all `PT_LOAD` segments in the HHDM range.
2. Structural lock section at fixed offsets.
3. Handshake stub at the physical load base.
4. SHA-256 checksum table at `load_min + 0x80000`, patched by `gen_checksums`.
5. Boot protocol validation on entry.

---

## Step 1 — Link for the HHDM

All `PT_LOAD` segments must have virtual addresses at or above `CONFIG_KERNEL_VIRT_OFFSET` (`0xFFFF800000000000`). ZXFL computes the physical load address by subtracting this offset from `p_paddr`:

```
pa = p_paddr - 0xFFFF800000000000
```

No `AT()` override is needed. Because there is no LMA override in the linker script, `p_paddr` equals `p_vaddr`, and the loader strips the HHDM offset to get the physical address.

A minimal linker script skeleton (modelled on `arch/s390x/init/link.ld`):

```ld
ENTRY(my_kernel_entry)

PHDRS {
    nucleus       PT_LOAD FLAGS(7);
    checksums_seg PT_LOAD FLAGS(4);
}

SECTIONS {
    /* Handshake stub — must be the first code at the physical load base */
    .zxfl_hs 0xFFFF800000100000 : {
        KEEP(*(.zxfl_hs))
    } :nucleus

    .text 0xFFFF800000100400 : {
        KEEP(*(.text.my_kernel_entry))
        *(.text .text.*)
    } :nucleus

    .rodata : ALIGN(8) { *(.rodata .rodata.*) } :nucleus
    .data   : ALIGN(8) { *(.data   .data.*)   } :nucleus

    /* Structural lock — fixed virtual offsets from load base */
    .zxfl_lock 0xFFFF800000170000 : {
        KEEP(*(.zxfl_lock))
    } :nucleus

    .bss : ALIGN(4096) {
        *(.bss .bss.*) *(COMMON)
    } :nucleus

    /* Checksum table — fixed virtual offset from load base */
    .zxvl_checksums 0xFFFF800000180000 : {
        KEEP(*(.zxvl_checksums))
    } :checksums_seg
}
```

> The entry point (`e_entry`) must be at or above `0xFFFF800000040000` (HHDM + 256 KB). ZXFL rejects images with a lower entry point.

---

## Step 2 — Embed the Structural Lock

The lock constants can be placed directly in the linker script (as ZXFoundation does), or in a C translation unit:

```ld
/* In the linker script — simplest approach */
.zxfl_lock 0xFFFF800000170000 : {
    LONG(0xCCBBCC35)   /* hi */
    LONG(0x5A58464C)   /* sentinel "ZXFL" */
    . = . + 0x1000 - 8;
    LONG(0xE5664311)   /* lo */
} :nucleus
```

The loader verifies: `((hi << 32 | lo) ^ 0x3C1E0F8704B2D596) == 0xF0A5C3B2E1D49687`.

---

## Step 3 — Implement the Handshake Stub

The stub must be the very first code at the physical load base. It receives a nonce in `%r2` and must return the response in `%r2`. `ZXVL_HS_RESPONSE = 0xDEADBEEF0BADF00D`.

```asm
    .machinemode zarch
    .section .text.handshake, "ax"
    .globl __zxfl_handshake_stub
.equ ZXFL_SEED_HI, 0xA5F0C3E1
.equ ZXFL_SEED_LO, 0xB2D49687
.equ HS_RESPONSE_HI,  0xDEADBEEF
.equ HS_RESPONSE_LO,  0x0BADF00D

__zxfl_handshake_stub:
    llihf   %r0, ZXFL_SEED_HI
    iilf    %r0, ZXFL_SEED_LO
    xgr     %r2, %r0
    lgr     %r0, %r2
    sllg    %r0, %r0, 17
    srlg    %r1, %r2, 47
    ogr     %r0, %r1
    llihf   %r1, HS_RESPONSE_HI
    iilf    %r1, HS_RESPONSE_LO
    lgr     %r2, %r0
    agr     %r2, %r1
    br      %r14
```

The stub must not clobber `%r14` (return address) or `%r15` (stack pointer). It must be callable with `BRASL` and return via `BR %r14`.

---

## Step 4 — Reserve the Checksum Table

Declare the checksum table section. It is zero at link time; `gen_checksums` patches it after linking:

```c
__attribute__((section(".zxvl_checksums"), used))
static volatile zxvl_checksum_table_t zxvl_cksum_table = { 0 };
```

---

## Step 5 — Run `gen_checksums`

After linking, run the host tool on the ELF:

```sh
gen_checksums my_kernel.elf
```

This computes SHA-256 for each `PT_LOAD` segment (excluding `.zxvl_checksums` itself) and patches the table in-place. The ELF is now ready for DASD.

---

## Step 6 — Write to DASD

Write the kernel ELF to the DASD volume as dataset `CORE.ZXFOUNDATION.NUCLEUS`. In `sysres.conf`:

```
DATASET CORE.ZXFOUNDATION.NUCLEUS  my_kernel.elf
```

See [Build Targets](../build/targets.md#dasd--sysres3390) for the full `dasdload` invocation.

---

## Step 7 — Handle the Boot Protocol on Entry

Your kernel entry point receives `zxfl_boot_protocol_t *boot` in `%r2`. Minimum required validation:

```c
[[noreturn]] void my_kernel_entry(zxfl_boot_protocol_t *boot) {
    if (!boot || boot->magic != ZXFL_MAGIC)
        for (;;) __asm__("nop");

    uint64_t expected = ZXVL_COMPUTE_TOKEN(boot->stfle_fac[0], boot->ipl_schid);
    if (boot->binding_token != expected)
        for (;;) __asm__("nop");

    if (boot->version != ZXFL_VERSION_4)
        for (;;) __asm__("nop");

    /* proceed */
}
```

All pointer fields in the protocol are HHDM virtual addresses. Do not treat them as physical addresses.

---

## Checklist

| # | Requirement | Enforced by |
|---|-------------|-------------|
| 1 | ELF64, `ET_EXEC`, `e_machine = 0x16` (EM_S390) | Loader ELF validation |
| 2 | All `PT_LOAD` `p_vaddr >= 0xFFFF800000000000` | Loader address check |
| 3 | `e_entry >= 0xFFFF800000040000` | Loader entry check |
| 4 | Structural lock at `load_min + 0x70000` | `zxvl_verify` |
| 5 | Handshake stub at `load_min + 0x0` | `zxvl_verify` |
| 6 | Checksum table at `load_min + 0x80000`, patched by `gen_checksums` | `zxvl_verify` |
| 7 | `boot->magic` validated on entry | Kernel |
| 8 | `boot->binding_token` validated on entry | Kernel |
