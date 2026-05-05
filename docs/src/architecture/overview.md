# Architecture Overview

**Document Revision:** 26h1.0  
**Reference:** IBM z/Architecture Principles of Operation, SA22-7832

---

## 1. z/Architecture

z/Architecture (s390x) is IBM's 64-bit mainframe instruction set, introduced with the z900 in 2000. It supersedes ESA/390 (31-bit) and System/370 (24-bit). ZXFoundation targets z/Architecture exclusively; ESA/390 compatibility mode is used only during the first instruction of the IPL sequence.

Key properties that distinguish z/Architecture from commodity architectures:

- All I/O is performed through the **Channel Subsystem** (CSS). There is no memory-mapped I/O.
- The **Program Status Word** (PSW) encodes the instruction address, addressing mode, DAT enable, and all interrupt masks in a single 128-bit register.
- The **Lowcore** at physical address `0x0` is the hardware-defined interrupt vector table with a fixed layout.
- Inter-processor communication uses the **SIGP** instruction rather than memory-mapped registers or MSIs.
- The **STFLE** instruction enumerates optional hardware facilities (analogous to CPUID on x86).

---

## 2. Program Status Word (PSW)

The PSW is 128 bits wide. It is loaded atomically by `LPSWE` and saved atomically on every interrupt.

```
Bits  0–63:  Mask word
  Bit  1:    PER enable
  Bit  5:    DAT enable
  Bit  6:    I/O interrupt mask
  Bit  7:    External interrupt mask
  Bit  8:    Problem state (0=supervisor, 1=user)
  Bits 12–15: Condition Code
  Bit  31:   EA (Extended Addressing) — must be 1 for 64-bit
  Bit  32:   BA (Basic Addressing)    — must be 0 for 64-bit

Bits 64–127: Instruction address (64-bit)
```

`EA=1, BA=0` selects 64-bit addressing mode. `SAM64` sets this without altering other PSW fields.

**Disabled-wait PSW:** All interrupt masks cleared, wait bit set. The CPU halts permanently. Used as the panic state.

**New PSWs:** For each interrupt class (I/O, external, machine check, program, restart, SVC), the architecture reserves a fixed lowcore offset for a "new PSW" — the PSW loaded when that interrupt fires. The kernel must install valid new PSWs before enabling the corresponding interrupt class.

---

## 3. Lowcore (Prefix Area)

The lowcore is the 4 KB region at physical address `0x0`. Its layout is fixed by the architecture.

| Offset  | Content                     |
|---------|-----------------------------|
| `0x000` | IPL PSW                     |
| `0x008` | IPL CCW1                    |
| `0x010` | IPL CCW2                    |
| `0x068` | Restart new PSW             |
| `0x0B8` | Subchannel ID of IPL device |
| `0x1C0` | External new PSW            |
| `0x1C8` | SVC new PSW                 |
| `0x1D0` | Program new PSW             |
| `0x1D8` | Machine check new PSW       |
| `0x1E0` | I/O new PSW                 |

The **prefix register** (set by `SPX`, read by `STPX`) maps a per-CPU physical page to the logical lowcore address `0x0`. Each CPU has its own private lowcore page; the BSP uses physical page 0, APs use separately allocated pages.

---

## 4. Channel Command Words (CCW) and I/O

All device I/O is performed through the Channel Subsystem. The CPU constructs a **Channel Program** — a linked list of CCWs — and submits it via `SSCH` (Start Subchannel).

### CCW Format-1 (8 bytes)

```
Bits  0–7:   Command code  (0x02=Read, 0x01=Write, 0x08=Sense)
Bits 32–63:  Channel Data Address (CDA) — physical address of data buffer
Bit  65:     Chain Command (CC) — link to next CCW
Bits 80–95:  Byte count
```

> **Critical constraint:** The CDA field is 31 bits. All I/O data buffers must reside below physical address `0x80000000`. This is why `ZONE_DMA` covers `[0, 16 MB)`.

### I/O Sequence

```
CPU                        Channel Subsystem
 │                              │
 ├─ SSCH (schid, ORB) ────────► │  Submit channel program
 │                              ├─ Execute CCW chain, transfer data
 │◄──────── I/O interrupt ──────┤  Subchannel status available
 ├─ TSCH (schid, IRB) ────────► │  Read Interrupt Response Block
 │◄──────── IRB ────────────────┤  Device status, residual count
```

---

## 5. Initial Program Load (IPL)

When the operator issues a LOAD command, the channel subsystem performs the following automatically:

1. Reads the first physical record from the IPL device (ECKD: C=0, H=0, R=1) into physical address `0x0`.
2. The record contains an IPL PSW at `0x0` and two CCWs at `0x8`/`0x10`.
3. The CSS executes the CCW chain to load additional data.
4. The CPU loads the IPL PSW and begins execution.

For ZXFL, the IPL PSW is a 31-bit ESA/390 PSW pointing to the Stage 0 entry. The first instruction switches to z/Architecture mode via `SIGP SET ARCHITECTURE`.

---

## 6. Dynamic Address Translation (DAT)

DAT is enabled by PSW bit 5. When on, every memory access is translated through the page table hierarchy rooted at the ASCE in CR1.

### Address Space Control Element (ASCE)

The ASCE is a 64-bit value in CR1 encoding the physical address of the root table, the Designation Type (DT), and the Table Length (TL). ZXFoundation uses `DT=11` (Region-First), selecting 5-level paging.

### 5-Level Page Table Hierarchy

| Level  | Name               | Entries | Coverage per entry |
|--------|--------------------|---------|--------------------|
| ASCE → | R1 (Region-First)  | 2048    | 8 PB               |
| R1 →   | R2 (Region-Second) | 2048    | 4 TB               |
| R2 →   | R3 (Region-Third)  | 2048    | 2 GB               |
| R3 →   | Segment Table      | 2048    | 1 MB               |
| Seg →  | Page Table         | 256     | 4 KB               |

Each R1–Segment table is 16 KB (2048 × 8 bytes). Each page table is 4 KB (256 × 8 bytes).

### Virtual Address Decomposition (DT=11)

```
 63      53 52      42 41      31 30      20 19    12 11       0
 ┌────────┬──────────┬──────────┬──────────┬────────┬──────────┐
 │  RFX   │   RSX    │   RTX    │    SX    │   PX   │    BX    │
 │ 11 bit │  11 bit  │  11 bit  │  11 bit  │  8 bit │  12 bit  │
 └────────┴──────────┴──────────┴──────────┴────────┴──────────┘
   R1 idx   R2 idx    R3 idx    Seg idx    PT idx   Byte offset
```

### Large Pages (EDAT)

| Facility | STFLE bit | Page size | Mechanism                   |
|----------|-----------|-----------|-----------------------------|
| EDAT-1   | 8         | 1 MB      | FC=1 in Segment Table Entry |
| EDAT-2   | 78        | 2 GB      | FC=1 in Region-Third Entry  |

---

## 7. Virtual Address Space Layout

```
0x0000000000000000  User space (future)
        ...
0x00007FFFFFFFFFFF  User space top

        [ unmapped — translation exception ]

0xFFFF800000000000  HHDM base (CONFIG_KERNEL_VIRT_OFFSET)
                    Physical memory linearly mapped here.
                    PA 0x0 → VA 0xFFFF800000000000

0xFFFFC00000000000  vmalloc / ioremap region

0xFFFFFFFFFFFFFFFF  Top of address space
```

The HHDM offset is `0xFFFF800000000000`. The bootloader builds this mapping before transferring control; all kernel pointers in the boot protocol are HHDM virtual addresses.

---

## 8. Physical Memory Zones

| Zone          | Range                | Purpose                                     |
|---------------|----------------------|---------------------------------------------|
| `ZONE_DMA`    | `[0, 16 MB)`         | Channel I/O buffers (31-bit CDA constraint) |
| `ZONE_NORMAL` | `[16 MB, RAM limit)` | General kernel allocations                  |

---

## 9. Control Registers

| Register | Purpose                                                |
|----------|--------------------------------------------------------|
| CR0      | I/O/external interrupt subclass masks, feature enables |
| CR1      | Primary ASCE (page table root)                         |
| CR6      | I/O interrupt subclass mask (extended)                 |
| CR14     | Machine check interrupt mask                           |

The bootloader saves CR0, CR1, and CR14 snapshots in the boot protocol so the kernel can inspect the handover state.
