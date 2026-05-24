# Per-CPU Data

**Document Revision:** 26h1.2  
**Source:** `include/arch/s390x/cpu/lowcore.h`, `include/zxfoundation/percpu.h`, `arch/s390x/cpu/percpu.c`

---

## 1. Layout

Each CPU's prefix area (lowcore) is a **monolithic 8 KB block** (two contiguous pages). The physical address of this block is loaded into the prefix register via `SPX`.

The layout unifies hardware-assigned fields and software-defined per-CPU data into a single structure (`zx_lowcore_t`).

```
Physical Prefix Area (8 KB)
┌──────────────────────────────┐ 0x000
│  Hardware Lowcore            │
│  PSWs, interrupt codes, etc. │
├──────────────────────────────┤ 0x400  ← LC_PERCPU_OFFSET
│  Software Per-CPU Block      │
│  (zx_percpu_t percpu)        │
│  prefix_base, cpu_id,        │
│  lock_depth, MCS nodes,      │
│  RCU state, PCP caches...    │
├──────────────────────────────┤ 0x1200
│  Hardware Save Areas         │
│  GPRs, FPRs, CRs, ARs        │
└──────────────────────────────┘ 0x2000
```

The prefix register value is the physical base of this 8 KB block. `STAP` returns the current CPU's address, which is used to locate the monolithic block in the global `percpu_areas[]` array.

---

## 2. Access

| Macro | Description |
|---|---|
| `percpu_get(field)` | Read a field from the current CPU's `percpu` block |
| `percpu_set(field, val)` | Write a field to the current CPU's `percpu` block |
| `percpu_get_on(cpu, field)` | Read from another CPU's block (by logical ID) |
| `percpu_set_on(cpu, field, val) | Write to another CPU's block |

All macros utilize `percpu_ptr()` which resolves to the current CPU's monolithic `zx_lowcore_t` pointer.

---

## 3. Initialization

| Function | When called | Effect |
|---|---|---|
| `percpu_init_bsp()` | Once, early in `main.c` | Maps BSP monolithic lowcore at physical 0x0 |
| `percpu_init_ap(cpu_id, cpu_addr)` | Once per AP in `smp_init()` | Allocates 8 KB (2 pages), initializes monolithic lowcore |

---

## 4. Fields (zx_percpu_t)

| Field | Type | Purpose |
|---|---|---|
| `prefix_base` | `uint64_t` | Physical address of this CPU's lowcore |
| `cpu_id` | `uint16_t` | Logical CPU ID (0 = BSP) |
| `cpu_addr` | `uint16_t` | z/Arch CPU address (STAP result) |
| `lock_depth` | `uint32_t` | qspinlock nesting depth |
| `lock_nodes[4]` | `mcs_node_t[]` | MCS queue nodes for qspinlock |
| `rcu_gp_seq` | `uint64_t` | RCU grace-period sequence (written by BSP) |
| `rcu_qs_seq` | `uint64_t` | RCU quiescent-state sequence (written by this CPU) |
| `in_rcu_read_side` | `uint8_t` | 1 if inside `rcu_read_lock()` |
| `pcp[ZONE_MAX]` | `pmm_pcplist_t[]` | Per-CPU PMM page caches |
