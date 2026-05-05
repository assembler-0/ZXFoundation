# Per-CPU Data

**Document Revision:** 26h1.1  
**Source:** `include/zxfoundation/percpu.h`, `arch/s390x/cpu/percpu.c`

---

## 1. Layout

Each CPU's lowcore is a 4 KB page whose physical address is loaded into the prefix register via `SPX`. The hardware uses the first 512 bytes (0x000–0x1FF) for interrupt PSWs and status fields. The kernel extends this page with a per-CPU data block starting at offset **0x200**.

```
Physical lowcore page (4 KB)
┌──────────────────────────────┐ 0x000
│  Hardware lowcore (512 B)    │
│  PSWs, interrupt codes, etc. │
├──────────────────────────────┤ 0x200  ← PERCPU_OFFSET
│  percpu_t                    │
│  prefix_base, cpu_id,        │
│  lock_depth, MCS nodes,      │
│  RCU state, ...              │
└──────────────────────────────┘ 0x1000
```

The prefix register value is the physical base of this page. `STAP` returns the current CPU's address, which is used to locate the per-CPU block.

---

## 2. Access

| Macro | Description |
|---|---|
| `percpu_get(field)` | Read a field from the current CPU's block |
| `percpu_set(field, val)` | Write a field to the current CPU's block |
| `percpu_get_on(cpu, field)` | Read from another CPU's block (by logical ID) |
| `percpu_set_on(cpu, field, val)` | Write to another CPU's block |

All macros issue `STAP` to determine the current CPU's prefix base. The global `percpu_areas[]` array (indexed by logical CPU ID) provides cross-CPU access.

---

## 3. Initialization

| Function | When called | Effect |
|---|---|---|
| `percpu_init_bsp()` | Once, early in `main.c` | Maps BSP per-CPU block at physical 0x200 |
| `percpu_init_ap(cpu_id, cpu_addr)` | Once per AP in `smp_init()` | Allocates a 4 KB lowcore page, initializes per-CPU block |

---

## 4. Fields

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
