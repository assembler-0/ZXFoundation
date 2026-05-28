# Trap Handlers

**Document Revision:** 26h1.0
**Sources:** `arch/s390x/trap/entry.S`, `arch/s390x/trap/pgm.c`, `arch/s390x/trap/ext.c`,
`arch/s390x/trap/io.c`, `arch/s390x/trap/mcck.c`, `include/arch/s390x/cpu/irq_frame.h`

---

## 1. z/Architecture Interrupt Model

On z/Architecture, when any interrupt fires (program check, external, I/O, machine
check), the CPU performs the following steps **atomically and unconditionally in
hardware**, regardless of whether DAT (Dynamic Address Translation) is on or off:

1. The **current PSW** is saved into the lowcore Old PSW area at the absolute address
   corresponding to the interrupt class (e.g. `0x0150` for program checks), using the
   **prefix register** to map real address 0 to the CPU's physical lowcore.
2. The **New PSW** is loaded from the lowcore New PSW area (e.g. `0x01D0` for program
   checks). The new PSW's **address field is subject to DAT** if the DAT bit in the new
   PSW is set.
3. The CPU branches to the handler address in the new PSW.

The lowcore Old/New PSW accesses in steps 1–2 are **absolute** (prefix-relative, DAT
bypassed). Only the handler branch target is translated through DAT.

---

## 2. Entry Assembly (`entry.S`)

All four interrupt classes use a shared `SAVE_FRAME` macro:

```asm
.macro SAVE_FRAME reg, old_psw_lc, stack_lc
    lg      \reg, \stack_lc(0)          // (A) load stack pointer from lowcore
    aghi    \reg, -(160 + IRQ_FRAME_SIZE)
    stmg    %r0, %r15, (160 + IRQ_FRAME_GPRS)(\reg)
    mvc     (160 + IRQ_FRAME_PSW_MASK)(16, \reg), \old_psw_lc(0)
    lgr     %r15, \reg
    la      %r2, 160(%r15)
.endm
```

Instruction **(A)** — `lg \reg, \stack_lc(0)` — loads the async (or mcck) stack
pointer from a **virtual address formed by the literal displacement only**, with no
base register. On s390x, a zero base register means "do not add a base; the effective
address is the displacement alone." For example, `LC_ASYNC_STACK = 0x0350`, so the
effective virtual address is exactly `0x0000000000000350`.

This instruction **requires virtual address `0x350` to be mapped and readable** whenever
DAT is enabled. This is an absolute, non-negotiable architectural requirement.

---

## 3. The Virtual Address 0 Mapping: Why It Cannot Be Eliminated

### 3.1 Can DAT be enabled earlier (in ZXFL) to avoid this?

No. The requirement is not created by *when* DAT is enabled; it is created by *how
`SAVE_FRAME` accesses the lowcore*. Consider:

- When ZXFL enables DAT (via `LPSWE`), it is still physically located at a low address.
  Enabling DAT requires an identity map at the point of enablement to fetch the very
  next instruction. The identity map cannot be eliminated at the flip point — it can
  only be moved to a different stage.
- Even if ZXFL enabled DAT internally and built a "cleaner" address space, the kernel's
  `entry.S` would still execute `lg %r1, 0x350(0)`, still requiring VA `0x350` to be
  mapped. The requirement lives in the trap entry assembly, not in the boot sequence.

### 3.2 Can `entry.S` be rewritten to use HHDM addresses?

In principle, yes — the trap handler could pre-load a base register with the HHDM
lowcore address and access `LC_ASYNC_STACK` relative to it. However:

- Before `stmg` saves all GPRs, **no register can be used as a scratch base**. The
  handler has no free registers at the point it needs the stack pointer.
- This is the same constraint that originally led the IBM z/Architecture designers to
  mandate the lowcore format: the zero-base absolute load is the only way to read a
  value without first having a stack to save registers onto.
- Rewriting this would require a two-level entry (save one register to a known location,
  use it as a base to load the stack, then save the rest), adding latency to every
  interrupt on every CPU.

### 3.3 This is z/Architecture design intent

The z/Architecture Principles of Operation explicitly documents this pattern. Linux on
s390x, z/VM, and every other z/Architecture operating system maintain a mapping of the
lowcore at virtual address 0 throughout the kernel's entire lifetime. It is not a
workaround — it is the correct implementation of the architecture.

---

## 4. The 8 KB Lowcore Window

`mmu_init()` maintains a strict **8 KB identity mapping** (`VA 0x0000–0x1FFF` →
`PA 0x0000–0x1FFF`) after tearing down the bootloader's full identity map. This:

- Satisfies the `SAVE_FRAME` requirement (VA `0x350` is in the first 4 KB page).
- Covers both pages of the monolithic `zx_lowcore_t` (BSP at physical 0, APs prefixed).
- Ensures that any access **beyond `0x1FFF`** (such as a NULL-pointer struct dereference
  like `ptr->field` at VA `0x2010`) correctly triggers a DAT Translation Exception
  (program interrupt code `0x0010`), providing full NULL pointer safety.

### Critical Ordering Invariant

`mmu_init()` **must install the 8 KB mapping before invalidating `r1[0]`** (the RFX=0
identity subtree). The sequence is:

```
1. mmu_map_page(0x0000 → 0x0000)   // build the new explicit mapping
2. mmu_map_page(0x1000 → 0x1000)   // (both are committed to the live R1 table)
3. scrub r1[1..2046]               // invalidate everything except RFX=0 and HHDM
4. mmu_flush_tlb_local()           // make the scrub visible
```

Reversing steps 1–2 and 3–4 creates a window where `VA 0x350` is unmapped while
interrupts can fire. Any interrupt in that window causes `SAVE_FRAME` to fault on
`lg %r1, 0x350(0)`, which triggers another program check, which tries to save registers
via the same faulting instruction — an infinite **Region-first-translation exception
`0x0039` death loop**.

---

## 5. Interrupt Classes and Stack Selection

| Class | Old PSW Offset | New PSW Offset | Stack Used | C Handler |
|---|---|---|---|---|
| Program Check | `0x0150` | `0x01D0` | `LC_ASYNC_STACK` | `do_pgm_check()` |
| External | `0x0130` | `0x01B0` | `LC_ASYNC_STACK` | `do_ext_interrupt()` |
| I/O | `0x0170` | `0x01F0` | `LC_ASYNC_STACK` | `do_io_interrupt()` |
| Machine Check | `0x0160` | `0x01E0` | `LC_MCCK_STACK` | `do_mcck_interrupt()` |

Machine checks use a dedicated `mcck_stack` so that a machine-check interrupt arriving
while the async stack is active (during another interrupt) does not corrupt the in-flight
frame.

---

## 6. IRQ Frame Layout (`irq_frame.h`)

The `IRQ_FRAME_*` offsets define the saved-state layout pushed below the 160-byte
standard save area by `SAVE_FRAME`:

| Offset | Field | Size |
|---|---|---|
| `IRQ_FRAME_PSW_MASK` | Saved PSW (mask + address) | 16 B |
| `IRQ_FRAME_GPRS` | General-purpose registers R0–R15 | 128 B |
