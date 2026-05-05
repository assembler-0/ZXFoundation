# Bootloader Overview

**Document Revision:** 26h1.0

---

## 1. What Is ZXFL?

ZXFL (ZXFoundation Loader) is the two-stage bootloader for ZXFoundation. It is the only supported mechanism for loading the kernel nucleus. Its responsibilities are:

1. Transition the CPU from ESA/390 to z/Architecture 64-bit mode.
2. Locate and load the kernel ELF64 image from DASD.
3. Verify kernel integrity (ZXVL structural lock, handshake, SHA-256 checksums).
4. Probe hardware: memory, CPUs, TOD clock, system identification.
5. Build the 5-level page tables (identity map + HHDM).
6. Populate the boot protocol structure.
7. Transfer control to the kernel entry point with DAT enabled.

---

## 2. Two-Stage Design

The split is imposed by a hard architectural constraint: the IPL mechanism loads exactly one record from the IPL device into physical address `0x0` and executes it. That record must contain the IPL PSW and enough code to load a larger second stage.

| Stage | Internal name | Dataset | Load address | Size limit |
|-------|--------------|---------|-------------|------------|
| 0 | `zxfl_stage1` | `CORE.ZXFOUNDATIONLOADER00.SYS` | `0x0` | 12 KB |
| 1 | `zxfl_stage2` | `CORE.ZXFOUNDATIONLOADER01.SYS` | `0x20000` | ~512 KB |

**Stage 0** is a minimal DASD reader. Its only job is to find Stage 1 in the VTOC, load it to `0x20000`, and jump to it.

**Stage 1** is the full loader. It performs all hardware detection, ELF loading, integrity verification, page table construction, and the final jump to the kernel.

---

## 3. IPL Flow

```
Power-on / LOAD button
  │
  ▼
Channel subsystem reads IPL record (C=0, H=0, R=1) → 0x0
  │
  ▼
Stage 0  (arch/s390x/init/zxfl/stage1/)
  ├─ SIGP SET ARCHITECTURE → z/Architecture mode
  ├─ SAM64 → 64-bit addressing
  ├─ Clear BSS
  ├─ Find CORE.ZXFOUNDATIONLOADER01.SYS in VTOC
  ├─ Read it to 0x20000
  └─ Jump to 0x20000
       │
       ▼
Stage 1  (arch/s390x/init/zxfl/stage2/)
  ├─ Install disabled-wait new PSWs (lowcore)
  ├─ Clear BSS (MVCL)
  ├─ STFLE — detect facilities
  ├─ Probe IPL device (ECKD / FBA Sense ID + RDC)
  ├─ Read parmfile (ETC.ZXFOUNDATION.PARM)
  ├─ Find CORE.ZXFOUNDATION.NUCLEUS in VTOC
  ├─ Load ELF64 PT_LOAD segments to physical memory
  ├─ ZXVL: structural lock + handshake + SHA-256 checksums
  ├─ Probe memory (write-pattern test)
  ├─ Load sysmodule= modules
  ├─ Detect SMP (SIGP Sense), STSI, TOD (STCK)
  ├─ Build 5-level page tables (identity + HHDM)
  ├─ Translate all protocol pointers to HHDM virtual
  └─ LPSWE → kernel entry point (DAT on, interrupts masked)
```

---

## 4. Dataset Names

All datasets reside on the IPL DASD volume. Names follow the IBM MVS convention (uppercase, dot-separated, max 44 characters).

| Dataset | Contents |
|---------|----------|
| `CORE.ZXFOUNDATIONLOADER00.SYS` | Stage 0 IPL record |
| `CORE.ZXFOUNDATIONLOADER01.SYS` | Stage 1 flat binary |
| `CORE.ZXFOUNDATION.NUCLEUS` | Kernel ELF64 |
| `ETC.ZXFOUNDATION.PARM` | Boot parameters (parmfile) |

Additional datasets may be listed in the parmfile via `sysmodule=` entries.

---

## 5. Parmfile

The parmfile `ETC.ZXFOUNDATION.PARM` is a plain-text file read by Stage 1. Supported keys:

| Key | Description | Default |
|-----|-------------|---------|
| `syssize=` | Memory probe limit in MB | 512 |
| `sysmodule=` | Dataset name of an additional module to load | (none) |

Multiple `sysmodule=` lines are permitted (up to 16).

---

## 6. Constraints

- All CCW channel data addresses must be 31-bit (< `0x80000000`). Static BSS buffers satisfy this automatically.
- Stage 0 must fit within 12 KB (enforced by `ASSERT` in `stage1.ld`).
- The Stage 1 stack is 32 KB. The kernel must switch to its own stack before consuming more than ~8 KB.
- The kernel entry point must be ≥ `0xFFFF800000040000` (HHDM + 256 KB). The loader enforces this.

---

## 7. Using ZXFL With Your Own Kernel

To load a custom kernel with ZXFL:

1. **Produce an ELF64 image** linked for `s390x-unknown-none-elf`. All `PT_LOAD` segments must have physical addresses in the HHDM-offset range (`p_paddr ≥ CONFIG_KERNEL_VIRT_OFFSET`).

2. **Embed the ZXVL lock section** at the correct offsets from the physical load base. See [ZXVL Verification](../security/zxvl.md).

3. **Embed a `.zxvl_checksums` section** at `load_min + 0x80000`. Run `gen_checksums` on the ELF after linking to patch in the SHA-256 digests.

4. **Implement the handshake stub** at `load_min + 0x0`. See [ZXVL Verification](../security/zxvl.md#2-handshake).

5. **Accept the boot protocol** in `%r2` at entry. Validate `proto->magic == ZXFL_MAGIC` before using any other field. See [Boot Protocol](protocol.md).

6. **Write the dataset** to the DASD volume as `CORE.ZXFOUNDATION.NUCLEUS` using `dasdload` or equivalent.
