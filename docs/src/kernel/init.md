# Early Initialization

**Document Revision:** 26h1.0  
**Source:** `zxfoundation/init/main.c`

---

## 1. Initialization Sequence

`zxfoundation_global_initialize` performs early initialization in strict order before enabling interrupts or starting APs:

| Step | Action | Notes |
|------|--------|-------|
| 1 | `zxfl_lowcore_setup()` | Install kernel new PSWs in the BSP lowcore |
| 2 | `diag_setup()` + `printk_initialize()` | Enable console output |
| 3 | Validate `boot->magic == ZXFL_MAGIC` | Panic if wrong |
| 4 | Validate `boot->binding_token` | Recompute and compare; panic on mismatch |
| 5 | `validate_stack_frame()` | Verify ZXVL stack canaries |
| 6 | `verify_kernel_checksums()` | Re-verify SHA-256 segment digests from HHDM |
| 7 | Print machine/LPAR/CPU info | If `ZXFL_FLAG_SYSINFO` / `ZXFL_FLAG_SMP` set |
| 8 | `arch_cpu_features_init(boot)` | Detect STFLE facilities, populate feature flags |
| 9 | `rcu_init()` | Initialize RCU subsystem |
| 10 | `pmm_init(boot)` | Register usable memory regions; reserve loader/kernel/pool |
| 11 | `mmu_init()` | Inherit loader ASCE; detect EDAT-1/2 |
| 12 | `vmm_init()` | Set up vmalloc region |
| 13 | `slab_init()` | Initialize slab caches |
| 14 | `kmalloc_init()` | Initialize kmalloc size classes |

After step 14, the kernel halts in a `nop` loop (idle). Steps for enabling interrupts, starting APs, and launching the scheduler are not yet implemented.

---

## 2. Security Checks (Steps 3–6)

These checks run before any subsystem is initialized. A failure at any point calls `panic()`, which loads a disabled-wait PSW.

**Binding token** (step 4): The kernel recomputes `ZXVL_COMPUTE_TOKEN(stfle_fac[0], ipl_schid)` and compares it to `boot->binding_token`. This ties the running kernel to the specific hardware and IPL device — a protocol struct copied from another machine will fail here.

**Stack frame** (step 5): The loader writes a two-word canary at `boot->kernel_stack_top`. The kernel verifies `frame[0] == ZXVL_FRAME_MAGIC_A` and `frame[1] == ZXVL_FRAME_MAGIC_B ^ binding_token`. A mismatch indicates stack corruption or an unauthorized loader.

**Checksum re-verification** (step 6): The kernel re-reads the `zxvl_checksum_table_t` from `kernel_phys_start + ZXVL_CKSUM_TABLE_OFFSET` (via HHDM) and recomputes SHA-256 for each PT_LOAD segment. This catches any modification to the kernel image between loader verification and kernel execution.

---

## 3. PMM Reservation (Step 10)

`pmm_init` registers all `ZXFL_MEM_USABLE` regions from the boot protocol memory map, then marks the following ranges as reserved:

| Range | Reason |
|-------|--------|
| `[0, 1 MB)` | Lowcore + loader code |
| `[kernel_phys_start, kernel_phys_end)` | Kernel image |
| `[pool_base, pgtbl_pool_end)` | Bootloader page table pool |
| Each module's `[phys_start, phys_start + size)` | Loaded modules |
