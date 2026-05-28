# Per-CPU Data

**Document Revision:** 26h1.3
**Sources:** `include/arch/s390x/cpu/lowcore.h`, `include/zxfoundation/percpu.h`,
`arch/s390x/cpu/percpu.c`

---

## 1. Layout

Each CPU's prefix area (lowcore) is a **monolithic 8 KB block** (two contiguous
physical pages). The physical address of this block is loaded into the hardware
prefix register via `SPX`. The prefix register transparently remaps absolute address
`0x0000–0x1FFF` to the CPU's own physical lowcore for all absolute-mode accesses.

The layout unifies hardware-assigned fields and software-defined per-CPU data into a
single structure (`zx_lowcore_t`):

```
Physical Prefix Area (8 KB)
┌──────────────────────────────┐ 0x000
│  Hardware Lowcore            │   PSWs, interrupt codes, save areas (PoP §4)
├──────────────────────────────┤ 0x400  ← LC_PERCPU_OFFSET
│  Software Per-CPU Block      │   prefix_base, cpu_id, lock_depth,
│  (zx_percpu_t percpu)        │   MCS nodes, RCU state, PCP caches
├──────────────────────────────┤ 0x1200
│  Hardware Save Areas         │   GPRs, FPRs, CRs, ARs
└──────────────────────────────┘ 0x2000
```

---

## 2. Access — Current CPU

To access the **current CPU's** own per-CPU data, the kernel uses `zx_lowcore()`,
which returns the HHDM-mapped pointer to the active lowcore. Because the prefix
register already routes absolute-address-0 to this CPU's physical lowcore, and the
HHDM maps physical 0 to `CONFIG_KERNEL_VIRT_OFFSET`, `zx_lowcore()` always resolves
to the correct CPU without needing the prefix register value at all.

| Macro | Description |
|---|---|
| `percpu_get(field)` | Read a field from the current CPU's `percpu` block |
| `percpu_set(field, val)` | Write a field to the current CPU's `percpu` block |
| `percpu_inc(field)` | Increment a field in place |
| `percpu_dec(field)` | Decrement a field in place |
| `percpu_ptr_to(field)` | Pointer to a field in the current CPU's block |

---

## 3. Access — Other CPUs (`zx_lowcore_cpu`)

### 3.1 The Hardware Prefix Aliasing Bug

Accessing another CPU's lowcore by index into a global pointer array is deceptively
dangerous on s390x. Consider the global array `__percpu_areas_raw[]` where:

- `__percpu_areas_raw[0]` = HHDM pointer to BSP lowcore = `CONFIG_KERNEL_VIRT_OFFSET + 0`
- `__percpu_areas_raw[1]` = HHDM pointer to AP-1 lowcore = `CONFIG_KERNEL_VIRT_OFFSET + P`

When **AP-1** (whose prefix register is `P`) reads a value from address
`CONFIG_KERNEL_VIRT_OFFSET + 0` (i.e., the BSP's HHDM lowcore), the MMU translates
it to physical address `0`. The prefix register then remaps physical `0` to physical
`P` — so AP-1 silently reads **its own** lowcore, not the BSP's.

Symmetrically, when AP-1 reads from `CONFIG_KERNEL_VIRT_OFFSET + P`, the MMU
translates it to physical `P`. The prefix register remaps physical `P` to physical `0`
— so AP-1 silently reads the **BSP's** lowcore.

The result: every AP's cross-CPU lowcore lookup is silently swapped with the BSP's.
IPI delivery, RCU quiescent-state tracking, and PMM per-CPU page caches all operated
on the wrong CPU's data. The system "mostly worked" because the perfect symmetry of the
swap caused IPIs to still reach all CPUs, masking the corruption.

### 3.2 The Safe Accessor: `zx_lowcore_cpu(cpu)`

**`__percpu_areas_raw[]` must never be accessed directly.** Use `zx_lowcore_cpu(cpu)`
defined in `include/zxfoundation/percpu.h`, which applies an inverse prefix swap in
software:

```c
#define zx_lowcore_cpu(cpu)                                                    \
    ({                                                                          \
        zx_lowcore_t *__lc = __percpu_areas_raw[(cpu)];                        \
        zx_lowcore_t *__res = __lc;                                             \
        if (__lc) {                                                             \
            uint64_t __target_real = (uint64_t)__lc - CONFIG_KERNEL_VIRT_OFFSET;\
            uint64_t __my_prefix   = zx_lowcore()->percpu.prefix_base;         \
            if (__target_real == __my_prefix)                                   \
                __res = (zx_lowcore_t *)CONFIG_KERNEL_VIRT_OFFSET;             \
            else if (__target_real == 0)                                        \
                __res = (zx_lowcore_t *)(CONFIG_KERNEL_VIRT_OFFSET + __my_prefix);\
        }                                                                       \
        __res;                                                                  \
    })
```

**How it works:** if the target's physical address matches `my_prefix`, the hardware
would have swapped it to 0, so we manually redirect to `HHDM + 0` (the BSP). If the
target's physical address is 0, the hardware would have swapped it to `my_prefix`, so
we redirect to `HHDM + my_prefix`. Any other CPU is unaffected (no swap applies).

The cross-CPU access macros all go through this accessor:

| Macro | Description |
|---|---|
| `percpu_get_on(cpu, field)` | Read from another CPU's `percpu` block |
| `percpu_set_on(cpu, field, val)` | Write to another CPU's `percpu` block |
| `percpu_ptr_on(cpu, field)` | Pointer to a field in another CPU's block |

---

## 4. Initialization

| Function | When Called | Effect |
|---|---|---|
| `percpu_init_bsp()` | Once, early in `main.c` | Registers BSP lowcore (physical `0x0`) in `__percpu_areas_raw[0]` |
| `percpu_init_ap(cpu_id, cpu_addr, node)` | Once per AP in `smp_init()` | Allocates 8 KB (order-1), installs prefix via `SPX`, registers in `__percpu_areas_raw[cpu_id]` |

---

## 5. Fields (`zx_percpu_t`)

| Field | Type | Purpose |
|---|---|---|
| `prefix_base` | `uint64_t` | Physical address of this CPU's lowcore (used by `zx_lowcore_cpu`) |
| `cpu_id` | `uint16_t` | Logical CPU ID (0 = BSP) |
| `cpu_addr` | `uint16_t` | z/Arch CPU address (`STAP` result); used for `SIGP` |
| `lock_depth` | `uint32_t` | qspinlock nesting depth |
| `lock_nodes[MAX_LOCK_DEPTH]` | `mcs_node_t[]` | MCS queue nodes for qspinlock |
| `rcu_gp_seq` | `uint64_t` | RCU grace-period sequence (written by BSP) |
| `rcu_qs_seq` | `uint64_t` | RCU quiescent-state sequence (written by this CPU) |
| `in_rcu_read_side` | `uint8_t` | 1 if inside `rcu_read_lock()` |
| `ipi_pending_count` | `uint32_t` | Pending IPI completion counter |
| `ap_stack_top` | `uint64_t` | Initial AP stack pointer (physical, set before SIGP Restart) |
| `pcp[ZONE_MAX]` | `pmm_pcplist_t[]` | Per-CPU PMM order-0 page caches, one per memory zone |

---

## 6. Assembly Offsets

Key lowcore offsets used by `entry.S` and `head64.S` are defined as named constants in
`include/arch/s390x/cpu/lowcore.h` and verified at compile time by `_Static_assert`:

| Constant | Value | Field |
|---|---|---|
| `LC_ASYNC_STACK` | `0x0350` | `zx_lowcore_t::async_stack` |
| `LC_MCCK_STACK` | `0x0368` | `zx_lowcore_t::mcck_stack` |
| `LC_KERNEL_STACK` | `0x0348` | `zx_lowcore_t::kernel_stack` |
| `LC_RESTART_STACK` | `0x0360` | `zx_lowcore_t::restart_stack` |
| `LC_KERNEL_ASCE` | `0x0388` | `zx_lowcore_t::kernel_asce` |
| `LC_PERCPU_OFFSET` | `0x0400` | `zx_lowcore_t::percpu` |
| `LC_CPU_ID_OFFSET` | `0x0408` | `zx_percpu_t::cpu_id` (within percpu block) |
