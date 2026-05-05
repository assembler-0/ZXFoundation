[Introduction](introduction.md)

---

# Architecture

- [Overview](architecture/overview.md)

---

# Bootloader (ZXFL)

- [Overview](bootloader/overview.md)
- [Stage 0](bootloader/stage1.md)
- [Stage 1](bootloader/stage2.md)
- [DASD Drivers](bootloader/dasd.md)
- [VTOC](bootloader/vtoc.md)
- [ELF Loader](bootloader/elfload.md)
- [MMU & HHDM](bootloader/mmu.md)
- [Boot Protocol](bootloader/protocol.md)
- [ZXVL Verification](bootloader/zxvl.md)
- [Checksum Protocol](bootloader/checksums.md)
- [How to Load Your Kernel](bootloader/howto.md)

---

# Kernel

- [Overview](kernel/overview.md)
- [Early Init](kernel/init.md)
- [Per-CPU Data](kernel/percpu.md)
- [Trap Handler](kernel/trap.md)
- [Memory](kernel/memory/index.md)
  - [PMM](kernel/memory/pmm.md)
  - [VMM](kernel/memory/vmm.md)
  - [Slab & Kmalloc](kernel/memory/slab.md)
- [SMP](kernel/smp.md)
- [Synchronization](kernel/sync.md)
- [RCU & SRCU](kernel/rcu.md)
- [Scheduler](kernel/sched.md)
- [Subsystem Stubs](kernel/subsystems.md)

---

# Build System

- [Overview](build/overview.md)
- [Toolchains](build/toolchains.md)
- [Targets](build/targets.md)

---

# Host Tools

- [bin2rec](tools/bin2rec.md)
- [gen_checksums](tools/gen_checksums.md)