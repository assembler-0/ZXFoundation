# Early Initialization

**Document Revision:** 26h1.0  
**Source:** `zxfoundation/init/main.c`

---

## 1. Initialization Sequence

`zxfoundation_global_initialize` performs early initialization in strict order before enabling interrupts or starting APs:

| Step | Action                                 | Notes                                                      |
|------|----------------------------------------|------------------------------------------------------------|
| 1    | `zxfl_lowcore_setup()`                 | Install kernel new PSWs in the BSP lowcore                 |
| 2    | `diag_setup()` + `printk_initialize()` | Enable console output                                      |
| 3    | Validate `boot->magic == ZXFL_MAGIC`   | Panic if wrong                                             |
| 4    | Validate `boot->binding_token`         | Recompute and compare; panic on mismatch                   |
| 5    | `validate_stack_frame()`               | Verify ZXVL stack canaries                                 |
| 6    | `verify_kernel_checksums()`            | Re-verify SHA-256 segment digests from HHDM                |
| 7    | Print machine/LPAR/CPU info            | If `ZXFL_FLAG_SYSINFO` / `ZXFL_FLAG_SMP` set               |
| 8    | `percpu_init_bsp()`                    | Initialize BSP per-CPU block at prefix+0x200               |
| 9    | `arch_cpu_features_init(boot)`         | Detect STFLE facilities, populate feature flags            |
| 10   | `rcu_init()`                           | Initialize RCU subsystem                                   |
| 11   | `pmm_init(boot)`                       | Register usable memory regions; reserve loader/kernel/pool |
| 12   | `mmu_init()`                           | Install 8 KB VA-0 lowcore window; scrub identity map; inherit EDAT-1/2 state. **Order is mandatory — see §4.** |
| 13   | `vmm_init()`                           | Set up vmalloc region                                      |
| 14   | `slab_init()`                          | Initialize slab caches                                     |
| 15   | `kmalloc_init()`                       | Initialize kmalloc size classes                            |
| 16   | `trap_init()`                          | Install program-check new PSW; enable trap handler         |
| 17   | `smp_init()`                           | Start all APs (SIGP sequence); each AP calls `trap_init()` |
| 18   | `sched_init()`                         | BSP becomes idle (PID 0); spawns `kernel_init` (PID 1)     |

---

## 2. Security Checks (Steps 3–6)

These checks run before any subsystem is initialized. A failure at any point calls `panic()`, which loads a disabled-wait PSW.

**Binding token** (step 4): The kernel recomputes `ZXVL_COMPUTE_TOKEN(stfle_fac[0], ipl_schid)` and compares it to `boot->binding_token`. This ties the running kernel to the specific hardware and IPL device — a protocol struct copied from another machine will fail here.

**Stack frame** (step 5): The loader writes a two-word canary at `boot->kernel_stack_top`. The kernel verifies `frame[0] == ZXVL_FRAME_MAGIC_A` and `frame[1] == ZXVL_FRAME_MAGIC_B ^ binding_token`. A mismatch indicates stack corruption or an unauthorized loader.

**Checksum re-verification** (step 6): The kernel re-reads the `zxvl_checksum_table_t` from `kernel_phys_start + ZXVL_CKSUM_TABLE_OFFSET` (via HHDM) and recomputes SHA-256 for each PT_LOAD segment. This catches any modification to the kernel image between loader verification and kernel execution.

---

## 3. PMM Reservation (Step 10)

`pmm_init` registers all `ZXFL_MEM_USABLE` regions from the boot protocol memory map, then marks the following ranges as reserved:

| Range                                           | Reason                     |
|-------------------------------------------------|----------------------------|
| `[0, 1 MB)`                                     | Lowcore + loader code      |
| `[kernel_phys_start, kernel_phys_end)`          | Kernel image               |
| `[pool_base, pgtbl_pool_end)`                   | Bootloader page table pool |
| Each module's `[phys_start, phys_start + size)` | Loaded modules             |

---

## 4. MMU Initialization Ordering Invariant (Step 12)

`mmu_init()` takes ownership of the bootloader ASCE and replaces the bootloader's
8 GB identity map with a precise 8 KB window at VA 0. This operation has a **strict,
unbreakable ordering requirement** rooted in z/Architecture hardware behavior.

### Why VA 0 Must Always Be Mapped

Every interrupt handler entry stub (`trap_pgm_entry`, `trap_ext_entry`, etc.) begins
with:

```asm
lg  %r1, LC_ASYNC_STACK(0)   // effective VA = 0x0350
```

The zero base register is not an error — it is the only way to load a value before
registers have been saved. Because DAT is active when this runs, `VA 0x350` must be
translated successfully. If the mapping is absent even for one instruction cycle while
interrupts are unmasked, a program-check fires, `SAVE_FRAME` tries to load from
`VA 0x350` again, and the CPU enters an **infinite Region-first-translation exception
(`0x0039`) death loop**.

### Required Sequence in `mmu_init()`

```
 Step 1: mmu_map_page(VA 0x0000 → PA 0x0000)   // build mapping first
 Step 2: mmu_map_page(VA 0x1000 → PA 0x1000)   // both pages of the lowcore
 Step 3: scrub r1[1..2046]                      // revoke identity map
 Step 4: mmu_flush_tlb_local()                  // make scrub visible to CPU
```

Steps 1–2 **must precede** steps 3–4. The new 8 KB mapping is committed into the
live R1 table before any identity entry is removed, so `VA 0x350` is always valid.

### Can This Be Avoided by Enabling DAT Earlier?

No. The requirement is not a consequence of *when* DAT is enabled; it comes from
*how `SAVE_FRAME` accesses the lowcore*. Even if ZXFL enabled DAT internally and
passed the kernel a fully virtual address space, the kernel's `entry.S` would still
execute `lg %r1, 0x350(0)` and still require `VA 0x350` to be mapped. This is
standard z/Architecture operating system design — Linux s390x, z/VM, and z/OS all
maintain an equivalent lowcore window at virtual address 0 for the same reason.
See `docs/src/kernel/trap.md` for the full architectural rationale.
