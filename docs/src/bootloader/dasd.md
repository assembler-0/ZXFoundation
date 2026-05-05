# DASD Subsystem

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/common/dasd_*.c`

---

## 1. Overview

ZXFL supports three DASD device types. The correct driver is selected automatically by probing the IPL device with Sense ID and Read Device Characteristics (RDC) CCWs.

| Type | Driver | Typical device |
|------|--------|---------------|
| ECKD | `dasd_eckd.c` | 3390 (most common) |
| FBA | `dasd_fba.c` | 9336 |
| Tape | `dasd_tape.c` | 3480, 3490, 3590 |

---

## 2. Low-Level I/O (`dasd_io.c`)

All device access goes through a single CCW submission layer:

```
dasd_do_io(schid, ccw_chain, sense_buf)
  │
  ├─ Build ORB pointing to ccw_chain
  ├─ SSCH(schid, ORB)
  ├─ Wait for I/O interrupt (disabled-wait loop on TSCH)
  ├─ TSCH(schid, IRB) → check device end status
  └─ Return status or panic on unrecoverable error
```

All CCW data buffers are static BSS arrays, ensuring they reside below `0x80000000` (31-bit CDA constraint).

---

## 3. ECKD Driver (`dasd_eckd.c`)

ECKD (Extended Count Key Data) is the standard format for IBM 3390 DASD. Addressing is by cylinder, head, and record number (C/H/R).

Key operations:

| Operation | CCW command | Description |
|-----------|-------------|-------------|
| Sense ID | `0xE4` | Identify device type and model |
| Read Device Characteristics | `0x64` | Obtain geometry (cylinders, heads, sectors) |
| Seek | `0x07` | Position to cylinder/head |
| Search ID Equal | `0x31` | Find record by C/H/R |
| Read Count Key Data | `0x86` | Read a full record |

Track reads use a Seek → Search → Read CCW chain. The search CCW loops (via TIC — Transfer in Channel) until the target record is found.

---

## 4. FBA Driver (`dasd_fba.c`)

FBA (Fixed Block Architecture) devices use linear block addressing. Each block is 512 bytes.

Key operations:

| Operation | CCW command | Description |
|-----------|-------------|-------------|
| Sense ID | `0xE4` | Identify device |
| Define Extent | `0x63` | Set the block range for the following operation |
| Locate Record | `0x43` | Specify starting block and count |
| Read | `0x42` | Transfer data |

---

## 5. Tape Driver (`dasd_tape.c`)

Tape support is provided for environments where the kernel is stored on a 3480/3490/3590 tape cartridge. Tape is read sequentially; there is no random access.

Key operations: Sense ID, Rewind, Read Block, Forward Space File.

---

## 6. Device Selection

At Stage 1 startup, `probe_ipl_device()` issues a Sense ID CCW to the IPL subchannel. The returned device type code selects the driver:

```
device_type == 0x3390  →  ECKD
device_type == 0x9336  →  FBA
device_type == 0x3480
              0x3490
              0x3590   →  Tape
otherwise              →  panic("unsupported IPL device")
```
