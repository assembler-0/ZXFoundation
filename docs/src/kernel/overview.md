# Kernel Overview

**Document Revision:** 26h1.0

---

## 1. Entry Contract

The kernel receives control from ZXFL with the following guaranteed state:

| Resource | State |
|----------|-------|
| DAT | **On** — CR1 holds the ASCE built by the loader |
| Interrupts | **Masked** — all interrupt classes disabled |
| `%r2` | HHDM virtual address of `zxfl_boot_protocol_t` |
| `%r15` | HHDM virtual address of initial stack top (32 KB loader stack) |
| All other GPRs | Undefined |

The kernel entry point is `zxfoundation_global_initialize(zxfl_boot_protocol_t *boot)`. The first action must be to validate `boot->magic == ZXFL_MAGIC`. Any other use of the protocol before this check is undefined behavior.

---

## 2. Subsystem Table

| Subsystem | Source location | Status |
|-----------|----------------|--------|
| Early init | `zxfoundation/init/` | Active |
| PMM | `zxfoundation/memory/pmm.c` | Active |
| VMM | `zxfoundation/memory/vmm.c` | Active |
| Slab | `zxfoundation/memory/slab.c` | Active |
| kmalloc | `zxfoundation/memory/kmalloc.c` | Active |
| Heap | `zxfoundation/memory/heap.c` | Active |
| MMU | `arch/s390x/mmu/mmu.c` | Active |
| Spinlock | `include/zxfoundation/spinlock.h` | Active |
| Mutex | `zxfoundation/sync/mutex.c` | Active |
| RW Lock | `zxfoundation/sync/rwlock.c` | Active |
| Semaphore | `zxfoundation/sync/semaphore.c` | Active |
| Wait queue | `zxfoundation/sync/waitqueue.c` | Active |
| RCU | `zxfoundation/sync/rcu.c` | Stub |
| kobject | `zxfoundation/object/kobject.c` | Active |
| printk | `zxfoundation/sys/printk.c` | Active |
| panic | `zxfoundation/sys/panic.c` | Active |
| IRQ | `arch/s390x/irq/` | Stub |
| Trap | `arch/s390x/trap/` | Stub |
| Time | `arch/s390x/time/` | Stub |
| Scheduler | `zxfoundation/sched/` | Stub |
| SMP | `arch/s390x/cpu/` | Partial |

---

## 3. Source Organization

```
zxfoundation/
  init/       — kernel entry point and early initialization
  memory/     — PMM, VMM, slab, kmalloc, heap
  sync/       — mutex, rwlock, semaphore, waitqueue, RCU
  object/     — kobject reference-counted object model
  sys/        — printk, panic
  sched/      — scheduler (stub)
  irq/        — IRQ framework (stub)

arch/s390x/
  init/       — lowcore setup, linker script
  mmu/        — 5-level DAT page table manager
  cpu/        — CPU feature detection (STFLE, STSI)
  trap/       — program interrupt handler (stub)
  irq/        — I/O interrupt handler (stub)
  time/       — TOD clock (stub)

drivers/
  console/    — DIAG 8 console driver

lib/
              — string, vsprintf, cmdline, rbtree, bitmap, ebcdic
```
