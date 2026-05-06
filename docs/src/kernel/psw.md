# PSW Manager

**Document Revision:** 26h1.0  
**Subsystem:** `arch/s390x/cpu/psw`

---

## 1. Overview

The PSW (Program Status Word) manager provides a single, authoritative
definition of all z/Architecture PSW mask constants and new-PSW lowcore
offsets.  Prior to this subsystem, constants were duplicated across
`zconfig.h` and `lowcore.h` under different names, and assembly files
hardcoded incorrect bit patterns.

All consumers — C translation units, assembly files, the ZXFL loader, and
the kernel — include a single header: `arch/s390x/cpu/psw.h`.

---

## 2. PSW Mask Word Layout

The z/Architecture PSW is 16 bytes.  The first 8 bytes are the mask word;
the second 8 bytes are the instruction address.

```
Bit  0   PER mask
Bit  5   DAT (address translation enable)
Bit  6   I/O interrupt mask
Bit  7   External interrupt mask
Bit 12   Machine-check mask
Bit 14   Wait state
Bit 15   Problem state (user mode)
Bit 31   EA — required for 64-bit addressing
Bit 32   BA — required for 64-bit addressing
```

Bits not listed above are reserved and must be zero.  Setting a reserved
bit causes a Specification Exception when the PSW is loaded via LPSWE.

---

## 3. Defined Constants

### 3.1 Bit Masks

| Constant              | Value                  | Description                        |
|-----------------------|------------------------|------------------------------------|
| `PSW_BIT_DAT`         | `0x0400000000000000`   | Address translation enable         |
| `PSW_BIT_IO`          | `0x0200000000000000`   | I/O interrupt mask                 |
| `PSW_BIT_EXT`         | `0x0100000000000000`   | External interrupt mask            |
| `PSW_BIT_MCCK`        | `0x0008000000000000`   | Machine-check mask                 |
| `PSW_BIT_WAIT`        | `0x0002000000000000`   | Wait state                         |
| `PSW_BIT_PSTATE`      | `0x0001000000000000`   | Problem state (user mode)          |
| `PSW_BIT_EA`          | `0x0000000100000000`   | Extended addressing (64-bit)       |
| `PSW_BIT_BA`          | `0x0000000080000000`   | Basic addressing (64-bit)          |

### 3.2 Composite Masks

| Constant                 | Value                | Description                                  |
|--------------------------|----------------------|----------------------------------------------|
| `PSW_ARCH_BITS`          | `0x0000000180000000` | EA\|BA — 64-bit mode, no other bits set      |
| `PSW_MASK_KERNEL`        | `0x0000000180000000` | Supervisor, DAT off, all interrupts disabled |
| `PSW_MASK_KERNEL_DAT`    | `0x0400000180000000` | Supervisor, DAT on, all interrupts disabled  |
| `PSW_MASK_DISABLED_WAIT` | `0x0002000180000000` | Wait state, DAT off, all interrupts disabled |

### 3.3 New PSW Lowcore Offsets

These are the physical offsets within the lowcore (prefix area) where the
hardware loads the PSW on each interrupt class (PoP SA22-7832 §4.3.3).

| Constant          | Offset   | Interrupt class     |
|-------------------|----------|---------------------|
| `PSW_LC_RESTART`  | `0x01A0` | Restart             |
| `PSW_LC_EXTERNAL` | `0x01B0` | External            |
| `PSW_LC_SVC`      | `0x01C0` | Supervisor call     |
| `PSW_LC_PROGRAM`  | `0x01D0` | Program check       |
| `PSW_LC_MCCK`     | `0x01E0` | Machine check       |
| `PSW_LC_IO`       | `0x01F0` | I/O                 |

> **Note:** These offsets are distinct from the old PSW save slots
> (0x0120–0x0170) and from the interrupt parameter area (0x0080–0x00C0).

---

## 4. Early Boot Initialization

`psw_install_new_psws()` is called once during early boot (before DAT is
enabled) to install disabled-wait PSWs into all six new PSW slots.  Each
slot receives a unique sentinel address encoding the interrupt class, so
that an unexpected interrupt produces an operator-visible halt code.

```
psw_install_new_psws()
  │
  ├─ PSW_LC_RESTART  ← PSW_MASK_DISABLED_WAIT, addr=0x...DEAD1A0
  ├─ PSW_LC_EXTERNAL ← PSW_MASK_DISABLED_WAIT, addr=0x...DEAD1B0
  ├─ PSW_LC_SVC      ← PSW_MASK_DISABLED_WAIT, addr=0x...DEAD1C0
  ├─ PSW_LC_PROGRAM  ← PSW_MASK_DISABLED_WAIT, addr=0x...DEAD1D0
  ├─ PSW_LC_MCCK     ← PSW_MASK_DISABLED_WAIT, addr=0x...DEAD1E0
  └─ PSW_LC_IO       ← PSW_MASK_DISABLED_WAIT, addr=0x...DEAD1F0
```

`zx_lowcore_setup_early()` (called from stage2 before DAT) delegates
directly to `psw_install_new_psws()`.

`zx_lowcore_setup_late()` (called from the kernel after DAT is on)
overwrites the sentinels with real handler PSWs via the `zx_lowcore_t`
struct pointer.