# SMP

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/cpu/`

---

## 1. CPU Detection

The bootloader detects CPUs by issuing `SIGP Sense` (order `0x01`) to each address in `[0, ZXFL_CPU_MAP_MAX)`. A condition code of 3 means "not operational" — the address is unoccupied. CC 0, 1, or 2 means the CPU exists and is recorded in `proto->cpu_map[]`.

The BSP address is read with `STAP` (Store CPU Address).

At kernel entry, `proto->cpu_count` contains the number of detected CPUs and `proto->bsp_cpu_addr` identifies the boot processor.

---

## 2. AP State at Handover

All APs are in the **stopped** state when the kernel receives control. The bootloader never starts APs. The kernel BSP is responsible for starting each AP:

| Step | Action                                                                    |
|------|---------------------------------------------------------------------------|
| 1    | Allocate a private prefix area (4 KB, page-aligned) for the AP            |
| 2    | Allocate a private stack for the AP                                       |
| 3    | Install interrupt new PSWs in the AP's prefix area                        |
| 4    | `SIGP Initial CPU Reset` — clear the AP's state                           |
| 5    | `SIGP Set Prefix` — point the AP's prefix register at its private lowcore |
| 6    | `SIGP Restart` — start the AP at the restart new PSW in its prefix area   |

> **Note:** AP startup is not yet implemented. The current kernel halts after BSP initialization.

---

## 3. Per-CPU Data

Each CPU requires its own:

- **Prefix area** (4 KB) — private lowcore with correct new PSWs. Set via `SPX`.
- **Stack** — the AP must not use the BSP stack or the loader stack.
- **Per-CPU variables** — accessed via the prefix register offset (analogous to `%gs` on x86).

---

## 4. TLB Coherency

z/Architecture hardware handles TLB coherency automatically via the `IPTE` (Invalidate Page Table Entry) instruction. `IPTE` atomically clears a PTE and broadcasts a TLB purge to all CPUs that have the affected ASCE loaded. No software IPI is required for TLB shootdowns.

```
mmu_ipte(va):
    ipte %r0, va    ← serialising, hardware-broadcast
```

`PTLB` (Purge TLB) flushes the entire local TLB and should only be used during address-space teardown. For single-page invalidation in a running SMP kernel, always use `IPTE`.

---

## 5. SIGP Reference

| Order             | Code                 | Use                               |
|-------------------|----------------------|-----------------------------------|
| Sense             | `0x01`               | Query CPU state                   |
| External Call     | `0x02`               | Send external interrupt to CPU    |
| Emergency Signal  | `0x03`               | Send emergency signal             |
| Initial CPU Reset | `0x06`               | Clear CPU state before restart    |
| Set Prefix        | `0x0D`               | Set prefix register on target CPU |
| Store Status      | `0x0E`               | Save CPU registers to prefix area |
| Set Architecture  | `0x12`               | Switch to z/Architecture mode     |
| Restart           | `0x06` + Restart PSW | Start AP at restart new PSW       |
