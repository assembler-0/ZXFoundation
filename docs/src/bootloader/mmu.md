# Bootloader MMU & HHDM

**Document Revision:** 26h1.0  
**Source:** `arch/s390x/init/zxfl/common/mmu.c`

---

## 1. Purpose

Before transferring control to the kernel, Stage 1 must enable DAT (Dynamic Address Translation) and establish the virtual address space the kernel expects. This involves building a 5-level page table hierarchy with two mappings:

| Mapping | Virtual range | Physical range | Purpose |
|---------|--------------|----------------|---------|
| Identity | `[0x0, RAM)` | `[0x0, RAM)` | Allows the loader itself to continue executing after DAT is enabled |
| HHDM | `[HHDM_BASE, HHDM_BASE + RAM)` | `[0x0, RAM)` | The kernel's primary view of physical memory |

`HHDM_BASE = 0xFFFF800000000000` (`CONFIG_KERNEL_VIRT_OFFSET`).

---

## 2. Page Table Allocation

The bootloader allocates page tables from a **bump allocator** backed by a contiguous physical region immediately after the kernel image. The region base is the first 1 MB-aligned address after `kernel_phys_end`, floored at 32 MB. The end of this region is recorded in `proto->pgtbl_pool_end`.

The kernel PMM must mark `[pool_base, pgtbl_pool_end)` as reserved during initialization.

---

## 3. Build Sequence

```
zxfl_mmu_setup_and_jump(proto, entry_point)
  │
  ├─ Allocate R1 table (16 KB, zero-filled)
  ├─ For each 4 KB page in [0, RAM):
  │    ├─ Map VA = PA         (identity)
  │    └─ Map VA = PA + HHDM  (HHDM)
  ├─ Build ASCE: R1_phys | DT=11 | TL=2048
  ├─ Load ASCE into CR1 (LCTL)
  ├─ Translate all proto pointer fields to HHDM virtual
  ├─ Set PSW.DAT = 1 in the new PSW
  └─ LPSWE → entry_point (DAT on, interrupts masked)
```

Large pages (EDAT-1 / EDAT-2) are used if the corresponding STFLE facility is present, reducing the number of page table entries required.

---

## 4. Pointer Translation

All pointer fields in `zxfl_boot_protocol_t` that reference physical memory are translated to HHDM virtual addresses before the jump:

$$va = pa + \texttt{CONFIG\_KERNEL\_VIRT\_OFFSET}$$

This includes `mem_map_addr`, `kernel_entry`, `kernel_stack_top`, `cmdline_addr`, and `lowcore_phys`. The kernel must not attempt to dereference any protocol pointer as a physical address.

---

## 5. State at Kernel Entry

| Resource | State |
|----------|-------|
| DAT | **On** — CR1 holds the ASCE built by the loader |
| Interrupts | **Masked** — all interrupt classes disabled |
| `%r2` | HHDM virtual address of `zxfl_boot_protocol_t` |
| `%r15` | HHDM virtual address of initial stack top (32 KB) |
| All other GPRs | Undefined |
