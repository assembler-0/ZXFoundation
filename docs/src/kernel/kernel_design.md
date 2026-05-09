# ZXFoundation Kernel Design

**Document:** ZXF-KRN-DESIGN-001
**Revision:** 26h1.0
**Status:** Draft
**Date:** 2026-05-09
**Author:** ZXFoundation Core Team

---

## Document Scope

This document is the master architectural specification for the ZXFoundation
kernel. It defines the design of every major subsystem — capability system,
memory architecture, IPC, domain model, scheduler, time, trap handling,
fault recovery, and the long-term implementation roadmap.

This document does not reference source files or API signatures. Those belong
in per-subsystem reference documents. This document defines *what* the kernel
is and *why* it is designed that way. Pseudocode and diagrams are used where
precision is required.

---

## 1. Architectural Philosophy

### 1.1 Design Axioms

ZXFoundation is a **capability-based object microkernel** for IBM
z/Architecture. Six axioms govern every design decision:

1. **Minimal Trusted Computing Base.** The kernel enforces only what cannot
   be enforced elsewhere: memory isolation, capability validity, and CPU
   scheduling. Everything else is a server domain.

2. **Capability-First.** No resource may be accessed without a valid
   capability. There is no ambient authority. A thread that holds no
   capabilities can do nothing.

3. **No Implicit Trust.** Server domains are untrusted by default, including
   system-provided ones. Trust is established by capability grant, not by
   identity or position in a hierarchy.

4. **z/Architecture Native.** The kernel exploits z/Architecture hardware
   features — DAT, storage keys, SIGP, TOD clock, CPU timer, channel
   subsystem — directly. No portability layer is maintained.

5. **SysV ABI Only.** The kernel defines its own system call surface.
   No POSIX compatibility layer exists or is planned. The SysV calling
   convention (GPRs 2–7 for arguments, GPR 2 for return) is the sole ABI.

6. **Extreme Redundancy.** The kernel must not panic on a faulting server
   domain or a recoverable hardware error. Fault containment and recovery
   are first-class design requirements, not afterthoughts.

### 1.2 Threat Model

| Threat | Mitigation |
|---|---|
| Untrusted user domain reads kernel memory | Separate DAT address space per domain; kernel ASCE never loaded in user state |
| Untrusted domain forges a capability | Capabilities are kernel-managed integers; user space never constructs them |
| Faulting server domain corrupts kernel state | Server domains run in user state; a fault traps to the kernel, not into it |
| Hardware storage error corrupts a page | Machine-check recovery classifies and isolates the affected frame |
| Capability leak via IPC | Capability transfer is move-semantics; sender loses the capability atomically |
| Denial of service via busy loop | Scheduler enforces quanta; CPU timer interrupt is non-maskable by user state |

### 1.3 Kernel / User Boundary

The kernel runs exclusively in **supervisor state** (PSW problem-state bit = 0).
All server domains and user processes run in **problem state** (PSW bit 8 = 1).

The boundary is enforced by z/Architecture hardware:

- DAT translates user virtual addresses through a per-domain ASCE (CR1 is
  loaded with the domain's ASCE on context switch).
- Storage keys restrict memory access to pages owned by the domain.
- Privileged instructions (`LPSWE`, `SPX`, `SIGP`, `SSCH`, etc.) trap to
  the kernel when executed in problem state.

### 1.4 Layered Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  User Processes  (problem state, own ASCE, own capability table) │
├─────────────────────────────────────────────────────────────────┤
│  Server Domains  (problem state, own ASCE, own capability table) │
│  [ block I/O | filesystem | network | console | device mgr ]    │
├─────────────────────────────────────────────────────────────────┤
│  Kernel TCB  (supervisor state, kernel ASCE)                    │
│  ┌──────────┬──────────┬──────────┬──────────┬───────────────┐  │
│  │ Capability│  IPC     │ Scheduler│  Memory  │ Trap / Syscall│  │
│  │  System  │ Subsystem│          │  Manager │   Dispatch    │  │
│  └──────────┴──────────┴──────────┴──────────┴───────────────┘  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  KOMS · PMM · VMM · Slab · SMP · RCU · Sync Primitives  │   │
│  └──────────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  z/Architecture Hardware                                        │
│  [ DAT · Storage Keys · SIGP · TOD · CPU Timer · CSS · MCCK ]  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Capability System

### 2.1 Definition

A **capability** is an unforgeable, kernel-managed token that grants a
specific set of rights to a specific kernel object. Possession of a
capability is both necessary and sufficient to exercise the rights it encodes.
There is no access control list, no ambient authority, and no privilege
escalation path outside of explicit capability grant.

### 2.2 Capability Token Structure

A capability token is a 64-bit opaque integer. User space treats it as an
integer handle into its own capability table. The kernel interprets the
internal encoding; user space never constructs or decodes it.

```
 63      56 55      40 39      24 23       0
 ┌────────┬──────────┬──────────┬──────────┐
 │  type  │  rights  │   gen    │  index   │
 │  8 bit │  16 bit  │  16 bit  │  24 bit  │
 └────────┴──────────┴──────────┴──────────┘
```

| Field | Width | Meaning |
|---|---|---|
| `type` | 8 | Object type (maps to `kobj_type_t::type_id`) |
| `rights` | 16 | Bitmask of granted rights |
| `gen` | 16 | Generation counter; incremented on revocation |
| `index` | 24 | Index into the kernel's global object table |

The `gen` field enables **generation-based revocation**: when a capability is
revoked, the kernel increments the generation counter on the target object.
Any token whose `gen` field does not match the current object generation is
invalid, regardless of `index` or `rights`.

### 2.3 Rights Model

Rights are type-specific. The following rights are defined at the kernel
level; subsystems may define additional type-specific rights in the upper
8 bits.

| Bit | Name | Meaning |
|---|---|---|
| 0 | `CAP_READ` | Read the object's state |
| 1 | `CAP_WRITE` | Modify the object's state |
| 2 | `CAP_EXEC` | Execute / invoke the object |
| 3 | `CAP_GRANT` | Derive and transfer a capability to this object |
| 4 | `CAP_REVOKE` | Revoke derived capabilities |
| 5 | `CAP_MAP` | Map the object's memory into an address space |
| 6 | `CAP_DESTROY` | Destroy the object |
| 7–15 | reserved / type-specific | |

**Derivation rule:** A derived capability may only have a subset of the
parent's rights. Rights can never be amplified. A domain that holds
`CAP_READ | CAP_GRANT` may derive a capability with `CAP_READ` only.

### 2.4 Capability Table

Each domain owns a **capability table** — a flat, kernel-managed array of
capability slots. The table is allocated at domain creation with a fixed
capacity. User space references capabilities by their slot index (a small
integer handle).

```
Domain Capability Table
┌───────┬──────────────────────────────────────────────┐
│ Slot  │ Capability Token (64-bit, kernel-interpreted) │
├───────┼──────────────────────────────────────────────┤
│   0   │ Self capability (CAP_READ | CAP_WRITE)        │
│   1   │ IPC endpoint capability (CAP_EXEC)            │
│   2   │ Memory region capability (CAP_READ | CAP_MAP) │
│   3   │ (empty)                                       │
│  ...  │  ...                                          │
│  N-1  │ (empty)                                       │
└───────┴──────────────────────────────────────────────┘
```

The capability table is allocated from a dedicated slab cache backed by
pages with a non-zero s390x storage key. This provides hardware-enforced
isolation: a domain cannot read another domain's capability table even if
it obtains a pointer to it, because the storage key check will fault.

### 2.5 Capability Lifecycle

```
                    cap_mint(type, rights, object)
                              │
                              ▼
                    ┌─────────────────┐
                    │  CAPABILITY     │
                    │  VALID          │◄──── cap_derive(parent, subset_rights)
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
         cap_transfer    cap_revoke    object destroyed
              │              │              │
              ▼              ▼              ▼
       moved to         gen++ on        all tokens
       receiver's       object;         with this
       table            all tokens      index become
                        with old gen    invalid
                        invalid
```

### 2.6 Core Operations (Pseudocode)

```
// Mint a new capability for an existing kernel object.
// Called only from kernel context; never directly by user space.
cap_mint(object, rights):
    slot = cap_table_alloc(current_domain.cap_table)
    token.type   = object.type_id
    token.rights = rights
    token.gen    = object.cap_gen
    token.index  = object.global_index
    current_domain.cap_table[slot] = token
    return slot

// Derive a capability with reduced rights.
// Syscall: cap_derive(src_slot, new_rights) -> dst_slot
cap_derive(src_slot, new_rights):
    token = cap_lookup(current_domain, src_slot)
    assert token.rights & CAP_GRANT
    assert (new_rights & ~token.rights) == 0   // no amplification
    dst_slot = cap_table_alloc(current_domain.cap_table)
    new_token = token
    new_token.rights = new_rights
    current_domain.cap_table[dst_slot] = new_token
    return dst_slot

// Revoke all capabilities derived from an object.
// Increments the generation counter; all existing tokens become stale.
cap_revoke(object):
    atomic_inc(object.cap_gen)
    // No table scan needed: stale tokens fail at cap_lookup time.

// Look up and validate a capability slot.
// Returns the target object pointer, or fails.
cap_lookup(domain, slot):
    assert slot < domain.cap_table.capacity
    token = domain.cap_table[slot]
    assert token.type != CAP_TYPE_INVALID
    object = global_object_table[token.index]
    assert object != null
    assert object.cap_gen == token.gen    // generation check
    return object, token.rights
```

### 2.7 KOMS Integration

Every `kobject_t` is a capability target. The KOMS `type_id` field maps
directly to the capability token `type` field. The KOMS global object table
(indexed by `token.index`) is the authoritative registry of all live kernel
objects.

The capability system does not replace KOMS reference counting. A valid
capability implies the object is alive (generation check passes only while
the object is alive). When an object is destroyed, its generation is
incremented, invalidating all capabilities before the final `koms_put`.

```
┌─────────────────────────────────────────────────────┐
│  Capability System                                  │
│  token.index ──────────────────────────────────┐   │
│  token.gen   ──── generation check ────────┐   │   │
└────────────────────────────────────────────│───│───┘
                                             │   │
┌────────────────────────────────────────────│───│───┐
│  KOMS                                      │   │   │
│  global_object_table[index] ───────────────┘   │   │
│  kobject_t::cap_gen ───────────────────────────┘   │
│  kobject_t::ref (kref_t) — independent lifetime    │
└─────────────────────────────────────────────────────┘
```

---

## 3. Memory Architecture

Memory is the most critical subsystem in ZXFoundation. Every other subsystem
depends on it. This section defines strict requirements and invariants for
every memory layer. Violations of these requirements are kernel panics, not
recoverable errors.

### 3.1 Physical Memory Manager (PMM)

#### 3.1.1 Zone Model

Physical memory is partitioned into two zones at boot time. The partition is
permanent; zones are never merged or resized after `pmm_init`.

| Zone | Range | Purpose |
|---|---|---|
| `ZONE_DMA` | `[0, 16 MB)` | Channel I/O buffers (31-bit CDA constraint) |
| `ZONE_NORMAL` | `[16 MB, RAM limit)` | General kernel and domain allocations |

The 16 MB boundary is a hardware constraint: the Channel Data Address (CDA)
field in a CCW is 31 bits. All I/O buffers submitted to the channel subsystem
must reside below `0x80000000`. `ZONE_DMA` covers this range conservatively.

#### 3.1.2 Buddy Allocator

Each zone maintains a buddy allocator with orders 0 through `MAX_ORDER` (10),
covering block sizes from 4 KB (order 0) to 4 MB (order 10).

```
Zone free lists (per order):

Order 0  (4 KB):  [pfn_a] → [pfn_b] → [pfn_c] → ∅
Order 1  (8 KB):  [pfn_d] → ∅
Order 2  (16 KB): ∅
...
Order 10 (4 MB):  [pfn_e] → ∅
```

**Buddy invariants (non-negotiable):**

1. Every free block is buddy-aligned: `pfn % (1 << order) == 0`.
2. Coalescing is mandatory on every free. If a block's buddy is also free,
   they are merged into a block of order+1, recursively up to `MAX_ORDER`.
3. A block may only be freed at the same order it was allocated. Mismatched
   order corrupts the buddy tree and is a kernel panic.
4. Free blocks are poisoned with `PF_POISON`. Any allocation that returns a
   non-poisoned block indicates a double-allocation bug.

#### 3.1.3 Per-CPU Page Cache

Order-0 (4 KB) allocations are served from a per-CPU cache to avoid zone
lock contention on the hot path.

```
Per-CPU cache (one per zone per CPU):

  count = 7
  ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
  │pfn_0│pfn_1│pfn_2│pfn_3│pfn_4│pfn_5│pfn_6│  -  │  -  │
  └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
  ← count                                    PCP_HIGH=16 →

  Refill: when count == 0, acquire zone lock, pop PCP_BATCH=8 pages.
  Drain:  when count > PCP_HIGH, acquire zone lock, push PCP_BATCH pages.
```

The per-CPU cache is accessed with IRQs disabled. No spinlock is needed
because the cache is strictly per-CPU and IRQ handlers that allocate memory
must use `ZX_GFP_ATOMIC`, which bypasses the per-CPU cache and draws
directly from the zone's atomic reserve.

#### 3.1.4 Atomic Reserve

Each zone holds `PMM_ATOMIC_RESERVE = 64` pages back from the buddy
allocator. These pages are only accessible to callers that pass
`ZX_GFP_ATOMIC`. This ensures that hard-IRQ context allocations (e.g.,
channel I/O completion handlers) always succeed even under memory pressure.

**Strict requirement:** `ZX_GFP_ATOMIC` must only be used from hard-IRQ
context. Using it from process context to bypass memory pressure is
prohibited and will be detected by a context check in debug builds.

#### 3.1.5 PMM Allocation Flow

```
pmm_alloc_page(gfp):
    if gfp & ZX_GFP_ATOMIC:
        goto zone_alloc          // bypass per-CPU cache
    if order == 0:
        page = pcp_pop(current_cpu, zone)
        if page: return page
        pcp_refill(current_cpu, zone)
        return pcp_pop(current_cpu, zone)
zone_alloc:
    acquire zone.lock (irqsave)
    for order in [requested_order .. MAX_ORDER]:
        pfn = free_area_pop(zone, order)
        if pfn != INVALID:
            split down to requested_order
            release zone.lock
            if gfp & ZX_GFP_ZERO: zero_page(pfn)
            return pfn_to_page(pfn)
    if gfp & ZX_GFP_ATOMIC and zone.atomic_reserve > 0:
        // draw from reserve
        ...
    release zone.lock
    return nullptr              // OOM
```

#### 3.1.6 PMM Strict Requirements

| # | Requirement |
|---|---|
| PMM-1 | `pmm_free_page/pages` must never be called on a page not in `PF_BUDDY` state. Double-free is a kernel panic. |
| PMM-2 | The order passed to `pmm_free_pages` must match the order used at allocation. |
| PMM-3 | Allocation from hard-IRQ context requires `ZX_GFP_ATOMIC`. Any other flag in IRQ context is a kernel panic. |
| PMM-4 | `zx_mem_map[]` is allocated during `pmm_init` and never freed. It must not be modified after init except by the PMM itself. |
| PMM-5 | The per-CPU cache must be drained to the zone before a CPU goes offline. |
| PMM-6 | `ZONE_DMA` and `ZONE_NORMAL` boundaries are immutable after `pmm_init`. |

---

### 3.2 Virtual Memory Manager (VMM)

#### 3.2.1 Address Space Layout

```
Virtual Address Space (64-bit z/Architecture, 5-level DAT)

0x0000_0000_0000_0000 ┌──────────────────────────────────────┐
                      │  User / Domain space                 │
                      │  (per-domain ASCE, problem state)    │
0x0000_7FFF_FFFF_FFFF └──────────────────────────────────────┘
                        [ translation exception — unmapped ]
0xFFFF_8000_0000_0000 ┌──────────────────────────────────────┐
                      │  HHDM — Higher-Half Direct Map       │
                      │  PA 0x0 → VA 0xFFFF_8000_0000_0000   │
                      │  Mapped with EDAT-1 (1 MB pages)     │
0xFFFF_C000_0000_0000 ├──────────────────────────────────────┤
                      │  vmalloc / ioremap region            │
                      │  Virtually contiguous, phys-discontig│
0xFFFF_E000_0000_0000 ├──────────────────────────────────────┤
                      │  Kernel image + BSS + static data    │
0xFFFF_FFFF_FFFF_FFFF └──────────────────────────────────────┘
```

The HHDM offset `0xFFFF_8000_0000_0000` places the kernel in R1 entry 2047
(the topmost Region-First entry), cleanly separating kernel (R1[2047]) from
user space (R1[0..2046]) at the highest table level.

#### 3.2.2 vm_space_t and VMA Tree

Each address space is represented by a `vm_space_t`. The kernel has one
(`kernel_vm_space`). Each domain has its own, created at domain birth and
destroyed at domain death.

VMAs are indexed by an **augmented RB-tree** keyed on `vm_start`. Each node
carries `subtree_max_end` — the maximum `vm_end` in its subtree — enabling
O(log n) free-gap search for vmalloc and O(1) overlap detection.

```
VMA Tree (augmented RB-tree):

                  [0xC000, 0xE000, max_end=0xF000]
                 /                                 \
  [0xA000, 0xB000, max_end=0xB000]    [0xE000, 0xF000, max_end=0xF000]

  Each node: vm_start (key), vm_end, subtree_max_end, vm_prot, rb_node
```

**Locking model:**

- **Readers** call `vmm_find_vma` inside `rcu_read_lock()`. Fully lockless.
  The RCU-protected tree guarantees that a reader always sees a consistent
  snapshot, even while a writer is modifying the tree.
- **Writers** acquire `aug_root.lock` (spinlock, irqsave) before any
  insert, remove, or augmentation update.

A per-CPU hint cache stores the last-found VMA per CPU. On a cache hit
(the faulting address falls within the cached VMA), the tree walk is
skipped entirely — O(1) on the hot page-fault path.

#### 3.2.3 VMM Strict Requirements

| # | Requirement |
|---|---|
| VMM-1 | All VMA modifications must hold `aug_root.lock` (spinlock, irqsave). |
| VMM-2 | All VMA reads must be inside `rcu_read_lock()`. |
| VMM-3 | VMAs must not overlap. `vmm_insert_vma` rejects overlapping ranges. |
| VMM-4 | `vm_start` and `vm_end` must be page-aligned (4 KB boundary). |
| VMM-5 | A `vm_space_t` must not be destroyed while any VMA remains mapped. |
| VMM-6 | The kernel ASCE (CR1) must never be loaded into a domain's address space. |
| VMM-7 | EDAT large pages (1 MB, 2 GB) must not be used for user domain mappings without an explicit `CAP_MAP` capability granting large-page access. |
| VMM-8 | `vmm_remove_vma` must unmap all backing pages and perform a TLB invalidation (IPTE/IDTE) before returning. |

#### 3.2.4 Domain Address Space Creation

When a new domain is created, the kernel allocates a fresh `vm_space_t` and
a new R1 page table. The kernel HHDM mapping is **not** shared into domain
address spaces. Domains have no visibility into kernel virtual addresses.

```
Domain address space creation:

  alloc vm_space_t
  alloc R1 table (16 KB, order=2, ZONE_NORMAL)
  initialize all R1 entries as invalid (Z_I_BIT set)
  set vm_space.pgtbl_root = phys(R1)
  set vm_space.asce = encode_asce(phys(R1), DT=R1, TL=2048)
  // Domain's ASCE is loaded into CR1 on context switch to this domain.
  // Kernel ASCE remains in a separate register save area.
```

---

### 3.3 Slab and Object Allocator

#### 3.3.1 Magazine-Depot Model

The slab allocator uses a **magazine-depot** architecture for per-CPU
caching of fixed-size objects.

```
Per-CPU layer (no lock needed, IRQs disabled):
  ┌──────────────────────────────────────────┐
  │  Hot magazine  [obj0│obj1│obj2│...│objN] │  ← alloc/free here
  │  Cold magazine [obj0│obj1│...          ] │  ← swap with hot when full/empty
  └──────────────────────────────────────────┘
           ↕ swap (acquire depot lock)
Global depot layer (spinlock):
  ┌──────────────────────────────────────────┐
  │  Full magazines:  [mag_a][mag_b][mag_c]  │
  │  Empty magazines: [mag_d][mag_e]         │
  └──────────────────────────────────────────┘
           ↕ slab page allocation (acquire zone lock)
PMM (buddy allocator)
```

Allocation: pop from hot magazine. If empty, swap hot/cold. If cold also
empty, fetch a full magazine from the depot. If depot has none, allocate
a new slab page from PMM and populate a magazine.

Free: push to hot magazine. If full, swap hot/cold. If cold also full,
return the cold magazine to the depot as a full magazine.

#### 3.3.2 Storage Key Isolation

Each slab cache may be created with a non-zero s390x storage key. Pages
backing that cache are assigned the specified key. A domain that does not
hold the matching key in its PSW access key field will receive a protection
exception if it attempts to access those pages.

Capability table pages use a dedicated storage key (key 1 by convention).
This provides hardware-enforced isolation: even if a domain obtains a
pointer to another domain's capability table, the storage key check will
fault before any data is read.

#### 3.3.3 Slab Strict Requirements

| # | Requirement |
|---|---|
| SLAB-1 | `kmem_cache_alloc` must not be called from hard-IRQ context unless the cache was created with atomic support. Use `kmalloc(ZX_GFP_ATOMIC)` from IRQ context. |
| SLAB-2 | `kmem_cache_free` must only be called with a pointer returned by `kmem_cache_alloc` on the same cache. Cross-cache free is undefined behavior. |
| SLAB-3 | Freed objects are poisoned with a sentinel pattern. Re-use before alloc is detected in debug builds. |
| SLAB-4 | `kmem_cache_destroy` must only be called after all objects have been returned. Outstanding objects at destroy time is a kernel panic. |

---

### 3.4 Capability Memory

Capability tables are the most security-sensitive data structure in the
kernel. They receive special treatment beyond the standard slab rules.

#### 3.4.1 Allocation

Capability tables are allocated from a dedicated slab cache:
- Storage key: 1 (non-zero, distinct from general kernel data at key 0).
- GFP flags: `ZX_GFP_NORMAL` only. Capability tables are never allocated
  from the atomic reserve.
- Pages are marked `PF_PINNED` immediately after allocation. They are never
  reclaimed, swapped, or migrated.

#### 3.4.2 Lifetime

A capability table is created atomically with its domain. It is destroyed
atomically when the domain dies. The destruction sequence is:

```
domain_destroy(domain):
    // 1. Freeze the domain: no new capabilities may be minted into it.
    domain.state = DOMAIN_DYING
    // 2. Revoke all capabilities in the table.
    for slot in domain.cap_table:
        if cap_table[slot].type != CAP_TYPE_INVALID:
            cap_revoke_slot(domain, slot)
    // 3. Free the table pages.
    kmem_cache_free(cap_table_cache, domain.cap_table)
    // 4. Drop the domain kobject reference.
    koms_put(domain.kobj)
```

Step 2 increments the generation counter on every object the domain held
capabilities to. This atomically invalidates all derived capabilities that
other domains may have received from this domain.

#### 3.4.3 Capability Memory Strict Requirements

| # | Requirement |
|---|---|
| CAP-MEM-1 | Capability table pages must be `PF_PINNED`. They are never reclaimed. |
| CAP-MEM-2 | Capability table pages use storage key 1. General kernel data uses key 0. |
| CAP-MEM-3 | Capability table destruction must complete before the domain's `vm_space_t` is torn down. |
| CAP-MEM-4 | No capability token may be stored in user-accessible memory. The kernel never copies a raw token to user space. |

---

### 3.5 Memory for IPC

IPC memory is designed to minimize allocation on the critical path.

#### 3.5.1 Synchronous IPC — Zero Allocation

Small synchronous messages (up to 8 × 64-bit registers) are passed entirely
in CPU registers. The kernel performs a direct thread switch: the sender's
GPRs 2–9 become the receiver's GPRs 2–9. No kernel buffer is allocated.
No memory is touched beyond the two threads' kernel stacks.

#### 3.5.2 Asynchronous Queue — Fixed-Capacity Ring Buffer

Each IPC endpoint that supports async messaging owns a fixed-capacity ring
buffer, allocated from the slab at endpoint creation time. The capacity is
specified at creation and never changes.

```
Async message queue (ring buffer):

  head ──►  ┌──────────────────────────────────────────┐
            │  msg[0]: tag | regs[8] | caps[4]         │
            │  msg[1]: tag | regs[8] | caps[4]         │
            │  msg[2]: (empty)                         │
            │  ...                                     │
  tail ──►  │  msg[N-1]: (empty)                       │
            └──────────────────────────────────────────┘
  capacity = N (fixed at endpoint creation)
  each message slot = 136 bytes (8 + 8×8 + 4×8)
```

The ring buffer is allocated with `ZX_GFP_NORMAL` and is never reallocated.
If the queue is full, the send operation returns `ERR_QUEUE_FULL` to the
sender. The sender is responsible for retry or backpressure.

#### 3.5.3 Shared Memory — Zero-Copy Large Transfer

For bulk data transfer, the sender grants a `CAP_MAP` capability on a VMA.
The receiver maps the VMA into its own address space via `vmm_insert_vma`.
No kernel buffer is involved. The physical pages are shared between the two
address spaces via DAT table entries pointing to the same physical frames.

```
Shared memory transfer:

  Sender domain                    Receiver domain
  vm_space_t                       vm_space_t
  ┌──────────────────┐             ┌──────────────────┐
  │ VMA [A, B)       │             │ VMA [C, D)       │
  │ prot: R/W        │             │ prot: R (derived)│
  └────────┬─────────┘             └────────┬─────────┘
           │ DAT entries                    │ DAT entries
           └──────────────┬─────────────────┘
                          ▼
                  Physical frames [P0, P1, ...]
```

The receiver's mapping uses the rights from the `CAP_MAP` capability. If the
capability grants only `CAP_READ`, the receiver's DAT entries are read-only.
A write attempt generates a protection exception in the receiver's domain,
not a kernel panic.

---

## 4. IPC Subsystem

### 4.1 Design Goals

IPC is the primary communication mechanism between all domains. Because
ZXFoundation is a microkernel, IPC performance directly determines system
throughput. The design targets:

- Synchronous fastpath latency: < 1 µs on z/Architecture (single hop,
  no contention, small message).
- Async queue throughput: limited only by memory bandwidth and ring buffer
  capacity.
- Zero kernel allocation on the synchronous fastpath.
- Capability transfer atomicity: a capability moved in a message is never
  visible in both sender and receiver simultaneously.

### 4.2 IPC Endpoint

An IPC endpoint is a kernel object (`kobject_t`, type `KOBJ_TYPE_ENDPOINT`).
It is the rendezvous point for IPC. A domain that wishes to receive messages
creates an endpoint and publishes a capability to it.

```
Endpoint state:

  ENDPOINT_IDLE      — no sender or receiver waiting
  ENDPOINT_RECV_WAIT — a receiver thread is blocked, waiting for a message
  ENDPOINT_SEND_WAIT — one or more sender threads are queued (async overflow)
```

An endpoint is addressed exclusively by capability. A domain that does not
hold a capability to an endpoint cannot send to or receive from it.

### 4.3 Synchronous Fastpath

The synchronous fastpath is the primary IPC mechanism. It is used when the
receiver is already blocked on the endpoint.

```
Synchronous IPC fastpath:

  Sender                    Kernel                    Receiver
    │                          │                          │
    │  ipc_call(ep_cap,        │                          │
    │    regs[0..7])           │                          │
    ├─────────────────────────►│                          │
    │                          │  cap_lookup(ep_cap)      │
    │                          │  endpoint.state ==       │
    │                          │    RECV_WAIT?  YES       │
    │                          │                          │
    │                          │  copy regs[0..7] to      │
    │                          │  receiver kernel stack   │
    │                          │                          │
    │                          │  transfer caps (if any)  │
    │                          │  from sender table to    │
    │                          │  receiver table          │
    │                          │                          │
    │                          │  direct thread switch:   │
    │  [blocked]               │  sender → BLOCKED        │
    │                          │  receiver → RUNNING      │
    │                          ├─────────────────────────►│
    │                          │                          │  regs[0..7]
    │                          │                          │  available
    │                          │                          │
    │                          │  receiver calls          │
    │                          │  ipc_reply(regs[0..7])   │
    │                          │◄─────────────────────────┤
    │                          │  direct thread switch:   │
    │                          │  receiver → BLOCKED      │
    │◄─────────────────────────┤  sender → RUNNING        │
    │  regs[0..7] = reply      │                          │
```

The direct thread switch bypasses the scheduler run queue entirely. The
kernel saves the sender's context, restores the receiver's context, and
returns to user space in the receiver. This is the seL4-style fastpath.

**Fastpath conditions** (all must hold; any failure falls back to slow path):

1. Endpoint state is `RECV_WAIT`.
2. Message fits in 8 registers (no large payload).
3. At most 4 capability handles transferred.
4. Receiver thread is on the same CPU (avoids cross-CPU IPI on fastpath).

### 4.4 Asynchronous Queue Fallback

When the fastpath conditions are not met, the message is enqueued in the
endpoint's ring buffer and the sender continues without blocking.

```
Async send path:

  ipc_send_async(ep_cap, msg):
      endpoint = cap_lookup(ep_cap, CAP_EXEC)
      acquire endpoint.lock (spinlock, irqsave)
      if ring_buffer_full(endpoint.queue):
          release endpoint.lock
          return ERR_QUEUE_FULL
      ring_buffer_enqueue(endpoint.queue, msg)
      if endpoint.state == RECV_WAIT:
          // Wake the receiver.
          thread_wake(endpoint.waiting_receiver)
          endpoint.state = ENDPOINT_IDLE
      release endpoint.lock
      return OK

  ipc_recv(ep_cap):
      endpoint = cap_lookup(ep_cap, CAP_EXEC)
      acquire endpoint.lock
      if ring_buffer_empty(endpoint.queue):
          endpoint.state = RECV_WAIT
          endpoint.waiting_receiver = current_thread
          release endpoint.lock
          thread_block()          // deschedule; woken by sender
          // On wake: message is in thread's IPC buffer
          return OK
      msg = ring_buffer_dequeue(endpoint.queue)
      release endpoint.lock
      return msg
```

### 4.5 Message Structure

Every IPC message has the same fixed structure regardless of path:

```
IPC Message (136 bytes):

  ┌──────────────────────────────────────────────────────────┐
  │  tag      [63:0]   — message type / protocol identifier  │
  ├──────────────────────────────────────────────────────────┤
  │  regs[0]  [63:0]   ─┐                                    │
  │  regs[1]  [63:0]    │                                    │
  │  ...                │  8 × 64-bit data words             │
  │  regs[7]  [63:0]   ─┘                                    │
  ├──────────────────────────────────────────────────────────┤
  │  caps[0]  [63:0]   ─┐                                    │
  │  caps[1]  [63:0]    │  4 × capability handles            │
  │  caps[2]  [63:0]    │  (slot indices in sender's table)  │
  │  caps[3]  [63:0]   ─┘                                    │
  └──────────────────────────────────────────────────────────┘
  Total: 1 + 8 + 4 = 13 × 8 = 104 bytes of payload
         + 4 bytes padding = 136 bytes per slot
```

### 4.6 Capability Transfer

Capabilities included in a message (`caps[0..3]`) are transferred with
**move semantics**: the kernel atomically removes the capability from the
sender's table and inserts it into the receiver's table. The sender's slot
is cleared. The capability is never simultaneously visible in both tables.

```
cap_transfer(sender, receiver, sender_slot):
    acquire sender.cap_table.lock
    acquire receiver.cap_table.lock   // always in address order to avoid deadlock
    token = sender.cap_table[sender_slot]
    assert token.type != CAP_TYPE_INVALID
    dst_slot = cap_table_alloc(receiver.cap_table)
    receiver.cap_table[dst_slot] = token
    sender.cap_table[sender_slot] = CAP_INVALID
    release receiver.cap_table.lock
    release sender.cap_table.lock
    return dst_slot
```

### 4.7 IPC and KOMS

IPC endpoints are `kobject_t` instances registered in the KOMS namespace
under the owning domain's subtree. A domain may publish an endpoint by
name, allowing other domains to discover it via `koms_ns_find_get` and
then request a capability from a trusted broker.

```
KOMS namespace (IPC endpoints):

  koms_root_ns
  └── "domains"
      ├── "block-io"
      │   └── "ep.request"   ← IPC endpoint kobject
      ├── "filesystem"
      │   └── "ep.request"
      └── "console"
          └── "ep.write"
```

---

## 5. Process and Domain Model

### 5.1 Fundamental Units

ZXFoundation defines two fundamental execution units:

- **Domain:** the unit of isolation. Owns an address space (`vm_space_t`),
  a capability table, and one or more threads. Analogous to a process in a
  monolithic kernel, but the kernel makes no distinction between a "driver
  domain" and an "application domain."

- **Thread:** the unit of scheduling. Belongs to exactly one domain. Has a
  kernel stack, a saved register set (`irq_frame_t`), and a scheduling state.
  Threads within the same domain share the domain's address space and
  capability table.

### 5.2 Domain Lifecycle

```
                    domain_create()
                          │
                          ▼
                  ┌───────────────┐
                  │   CREATING    │  — address space allocated,
                  └───────┬───────┘    capability table allocated,
                          │            initial thread created
                          ▼
                  ┌───────────────┐
                  │    RUNNING    │◄──── threads scheduled normally
                  └───────┬───────┘
                          │
              ┌───────────┼───────────┐
              │           │           │
         domain_kill   unhandled   watchdog
              │         fault       timeout
              │           │           │
              ▼           ▼           │
        ┌──────────┐ ┌──────────┐    │
        │  DYING   │ │ FAULTED  │◄───┘
        └────┬─────┘ └────┬─────┘
             │            │
             │     supervisor domain
             │     decides: restart or kill
             │            │
             │     ┌──────┴──────┐
             │     │             │
             │  restart        kill
             │     │             │
             │     ▼             │
             │ ┌──────────┐      │
             │ │RESTARTING│      │
             │ └────┬─────┘      │
             │      │            │
             │      ▼            ▼
             │  ┌────────┐  ┌──────┐
             └─►│  DEAD  │  │ DEAD │
                └────────┘  └──────┘
```

### 5.3 Domain Structure

A domain is a `kobject_t` of type `KOBJ_TYPE_DOMAIN`. It embeds:

```
Domain object:

  kobject_t         kobj          — KOMS base (lifecycle, namespace, events)
  vm_space_t        space         — address space (ASCE, VMA tree)
  cap_table_t       cap_table     — capability table
  list_head_t       threads       — list of owned threads
  spinlock_t        lock          — protects state transitions
  domain_state_t    state         — CREATING/RUNNING/FAULTED/RESTARTING/DEAD
  uint32_t          domain_id     — globally unique identifier
  kobject_t        *supervisor    — domain that receives fault events (may be null)
  uint64_t          heartbeat_seq — watchdog sequence number
```

### 5.4 Thread Structure

A thread is a `kobject_t` of type `KOBJ_TYPE_THREAD`. It embeds:

```
Thread object:

  kobject_t         kobj          — KOMS base
  domain_t         *domain        — owning domain (non-null, immutable)
  irq_frame_t       saved_regs    — GPRs, FPRs, PSW (saved on context switch)
  uint64_t          kernel_stack  — kernel stack top (virtual address)
  thread_state_t    state         — RUNNABLE/RUNNING/BLOCKED/DEAD
  sched_entity_t    sched         — scheduler run queue linkage
  uint32_t          priority      — scheduling priority class
  uint64_t          cpu_mask      — CPU affinity bitmask
  uint64_t          user_timer    — accumulated user-mode CPU time (ns)
  uint64_t          sys_timer     — accumulated kernel-mode CPU time (ns)
```

### 5.5 Fault Containment

When a domain faults (unhandled program check, protection exception, or
watchdog timeout), the kernel:

1. Suspends all threads in the domain (sets state to `BLOCKED`).
2. Sets domain state to `FAULTED`.
3. Fires `KOBJ_EVENT_DOMAIN_FAULT` on the domain's kobject.
4. If the domain has a registered supervisor, delivers an IPC message to
   the supervisor's fault endpoint containing the fault code and domain ID.
5. The supervisor decides: call `domain_restart` or `domain_kill`.

If no supervisor is registered, the kernel kills the domain immediately.
The kernel itself never panics due to a domain fault.

```
Fault containment flow:

  Domain D faults
       │
       ▼
  kernel suspends D's threads
  D.state = FAULTED
  koms_event_fire(D, KOBJ_EVENT_DOMAIN_FAULT)
       │
       ├── supervisor registered?
       │         YES                        NO
       │          │                          │
       ▼          ▼                          ▼
  IPC message to supervisor          domain_kill(D)
  { fault_code, domain_id }
       │
       ├── supervisor calls domain_restart(D)
       │         │
       │         ▼
       │   D.state = RESTARTING
       │   reset address space
       │   reset capability table
       │   restart initial thread
       │   D.state = RUNNING
       │
       └── supervisor calls domain_kill(D)
                 │
                 ▼
           D.state = DEAD
           destroy address space
           destroy capability table
           koms_put(D)
```

### 5.6 Server Domains

A **server domain** is a domain that provides a service to other domains.
It is distinguished from a user domain only by convention and registration:

- It registers one or more IPC endpoints in the KOMS namespace under a
  well-known path (e.g., `"domains/block-io/ep.request"`).
- It registers a supervisor domain (typically the system manager domain)
  that will restart it on fault.
- It registers a heartbeat capability with the kernel watchdog.

The kernel has no built-in concept of "driver" or "system service." All
server domains are equal in privilege. Their authority derives entirely from
the capabilities they hold.

### 5.7 KOMS Domain Hierarchy

```
koms_root_ns
└── "domains"
    ├── "system-manager"    ← supervisor for all server domains
    │   ├── "ep.fault"      ← receives fault events
    │   └── threads/
    │       └── "main"
    ├── "block-io"
    │   ├── "ep.request"
    │   └── threads/
    │       └── "worker-0"
    ├── "filesystem"
    │   ├── "ep.request"
    │   └── threads/
    │       └── "worker-0"
    └── "user-shell"
        └── threads/
            └── "main"
```

---

## 6. Scheduler

### 6.1 Design Goals

ZXFoundation targets **throughput/batch** workloads: long-running server
domains, high CPU utilization, and minimal context-switch overhead. The
scheduler is not designed for sub-millisecond interactive latency. It is
designed to keep all CPUs busy and to minimize the overhead of scheduling
decisions on the hot path.

### 6.2 Priority Classes

The scheduler defines three priority classes, processed in strict order:

| Class | Value | Quantum | Use case |
|---|---|---|---|
| `SCHED_REALTIME` | 0 (highest) | 1 ms | Watchdog thread, IPC notification threads |
| `SCHED_BATCH` | 1 | 10 ms | Server domains, user processes |
| `SCHED_IDLE` | 2 (lowest) | unbounded | Idle loop (runs only when no other work) |

A `SCHED_REALTIME` thread always preempts a `SCHED_BATCH` or `SCHED_IDLE`
thread. A `SCHED_BATCH` thread always preempts `SCHED_IDLE`. Within a
class, scheduling is round-robin.

The 10 ms batch quantum is chosen to match the z/Architecture TOD clock
resolution and to amortize context-switch overhead over a meaningful
amount of work. Server domains that perform I/O will voluntarily yield
(block on IPC receive) long before the quantum expires.

### 6.3 Per-CPU Run Queues

Each CPU maintains three run queues, one per priority class. Run queues
are doubly-linked lists of `sched_entity_t` nodes embedded in thread objects.

```
Per-CPU scheduler state (one per CPU):

  ┌─────────────────────────────────────────────────────────┐
  │  CPU N                                                  │
  │                                                         │
  │  current_thread ──► [thread currently running]          │
  │                                                         │
  │  rq[SCHED_REALTIME]: [t_a] ↔ [t_b] ↔ ∅                │
  │  rq[SCHED_BATCH]:    [t_c] ↔ [t_d] ↔ [t_e] ↔ ∅        │
  │  rq[SCHED_IDLE]:     [idle_thread] ↔ ∅                 │
  │                                                         │
  │  rq_lock (spinlock, irqsave)                            │
  │  nr_running (total threads across all queues)           │
  └─────────────────────────────────────────────────────────┘
```

The `rq_lock` is a per-CPU spinlock. It is held only during run queue
manipulation (enqueue, dequeue, pick_next). It is never held across a
context switch.

### 6.4 Scheduling Decision

The scheduler is invoked from three points:
1. CPU timer interrupt (quantum expiry).
2. `thread_block()` — a thread voluntarily deschedules (e.g., IPC receive).
3. `thread_wake()` — a thread is made runnable (e.g., IPC send wakes receiver).

```
schedule():
    acquire rq_lock (irqsave)
    next = pick_next_thread(current_cpu)
    if next == current_thread:
        release rq_lock
        return                  // no switch needed
    prev = current_thread
    current_thread = next
    release rq_lock
    context_switch(prev, next)  // saves prev, restores next, returns in next

pick_next_thread(cpu):
    for class in [SCHED_REALTIME, SCHED_BATCH, SCHED_IDLE]:
        if rq[class] not empty:
            thread = rq[class].head
            list_rotate(rq[class])   // round-robin: move head to tail
            return thread
    return idle_thread              // always non-null
```

### 6.5 Context Switch

A context switch saves the outgoing thread's full CPU state and restores
the incoming thread's state. On z/Architecture this includes:

- 16 × 64-bit general-purpose registers (GPRs 0–15)
- 16 × 64-bit floating-point registers (FPRs 0–15)
- Program Status Word (PSW: mask + instruction address)
- 16 × 32-bit access registers (ARs 0–15)
- CPU timer value (STPTC / SPTC)

The kernel stack pointer (GPR 15) is saved in the thread's `saved_regs`
and restored on the next switch. The domain's ASCE is loaded into CR1
when switching between domains.

```
Context switch sequence:

  context_switch(prev, next):
      // Save prev state to prev.saved_regs
      STMG  R0,R15, prev.saved_regs.gprs
      STFPC prev.saved_regs.fpc
      STPTC prev.saved_regs.cpu_timer
      // Update time accounting
      prev.sys_timer += (STCK() - lowcore.sys_enter_timer)
      // Switch address space if domains differ
      if prev.domain != next.domain:
          LCTLG CR1, next.domain.space.asce
          // TLB is tagged by ASCE; no explicit flush needed on z/Arch
      // Restore next state
      LPTC  next.saved_regs.cpu_timer
      LFPC  next.saved_regs.fpc
      LMG   R0,R15, next.saved_regs.gprs
      // lowcore.current_task = next (for fault handler identification)
      lowcore.current_task = next
      lowcore.sys_enter_timer = STCK()
      // Return in next thread's context
```

### 6.6 Work Stealing

When a CPU's run queues are empty (only the idle thread is runnable), the
CPU attempts to steal work from the busiest CPU.

```
Work stealing:

  idle_loop(cpu):
      while true:
          victim = find_busiest_cpu()   // scan per-CPU nr_running
          if victim == null or victim.nr_running <= 1:
              arch_cpu_relax()          // DIAG 0x44 (z/Arch yield hint)
              continue
          acquire victim.rq_lock (irqsave)
          acquire cpu.rq_lock (irqsave)   // always in cpu_id order
          steal_half(victim, cpu)
          release cpu.rq_lock
          release victim.rq_lock
          break
```

Stealing moves half the victim's `SCHED_BATCH` threads to the idle CPU.
`SCHED_REALTIME` threads are never stolen — they are pinned to their
assigned CPU by the IPI mechanism.

### 6.7 CPU Affinity

A thread may be pinned to a subset of CPUs via its `cpu_mask` field. The
scheduler respects affinity: `pick_next_thread` skips threads whose
`cpu_mask` does not include the current CPU. Work stealing also respects
affinity: a thread is only stolen if the stealing CPU is in the thread's
`cpu_mask`.

Affinity is set at thread creation via a capability-gated syscall. The
capability must grant `CAP_WRITE` on the thread object.

---

## 7. Time Subsystem

### 7.1 Hardware Time Sources

z/Architecture provides three hardware time mechanisms, all per-CPU:

| Source | Instruction | Type | Resolution | Use |
|---|---|---|---|---|
| TOD clock | `STCK` / `STCKF` | Global, monotonic | ~0.24 ns (2^-12 µs) | Wall time, `ktime_get` |
| CPU timer | `SPTC` / `STPTC` | Per-CPU countdown | Same as TOD | Scheduler preemption |
| Clock comparator | `SCKC` / `STCKC` | Per-CPU absolute | Same as TOD | Sleep / timeout |

The TOD clock is a single hardware clock shared across all CPUs. It is
monotonic and does not wrap in any practical timeframe (64-bit, ~143 years
at full resolution). `STCKF` reads it without serialization — it is safe
from any context including hard-IRQ.

### 7.2 Kernel Time (ktime_t)

`ktime_t` is a 64-bit nanosecond count since kernel boot. It is derived
from the TOD clock with a boot-time offset computed during `pmm_init`.

```
TOD clock value (raw):
  bits 63:0 = TOD units (1 TOD unit = 2^-12 µs ≈ 0.244 ns)

ktime conversion:
  ktime_ns = (tod_raw - tod_boot_offset) * 125 / 512
           = (tod_raw - tod_boot_offset) >> 2  (approximate, 4 ns resolution)

  Exact: 1 TOD unit = 1000/4096 ns
         ktime_ns = tod_delta * 1000 / 4096
```

`ktime_get()` reads `STCKF` and applies the conversion. It is callable
from any context, holds no lock, and never sleeps.

### 7.3 CPU Timer and Scheduler Preemption

The CPU timer is a per-CPU countdown register. When it reaches zero, a
CPU timer interrupt fires (external interrupt, subclass 0x1004). The
kernel uses this to enforce scheduler quanta.

```
Quantum setup (on context switch to a new thread):
    quantum_tod = thread.priority == SCHED_REALTIME ? 1_ms_in_tod
                                                    : 10_ms_in_tod
    SPTC -quantum_tod    // load negative value; counts up to zero

CPU timer interrupt handler:
    // Fires when CPU timer reaches zero (overflows from negative to positive)
    sched_tick()         // account time, check if quantum expired
    if quantum_expired:
        schedule()       // pick next thread
    else:
        return           // spurious or early; reload timer
```

### 7.4 Clock Comparator and Timer Wheel

The clock comparator fires an external interrupt when the TOD clock reaches
a programmed absolute value. The kernel uses this for `sleep` and `timeout`
operations.

The timer wheel is a per-CPU hierarchical structure with 8 levels and 64
slots per level. Each slot covers a time range; the resolution doubles at
each level.

```
Timer wheel (per CPU):

  Level 0: 64 slots × 1 ms  = 64 ms range   (fine-grained)
  Level 1: 64 slots × 64 ms = 4 s range
  Level 2: 64 slots × 4 s   = 256 s range
  ...
  Level 7: 64 slots × ...   = years range    (coarse)

  Each slot: list of timer_t objects expiring in that window

  On clock comparator interrupt:
      advance current slot pointer
      fire all timers in the current slot
      if level 0 wraps: cascade from level 1, etc.
      program clock comparator for next non-empty slot
```

Timer callbacks execute in **softirq context** — after the hard-IRQ
handler returns, before returning to user space. They must not block,
must not acquire spinlocks held by hard-IRQ handlers, and must complete
in bounded time.

### 7.5 Time Accounting

Per-thread time accounting uses the lowcore timing fields:

```
Kernel entry (SVC, PGM, EXT, IO):
    lowcore.sys_enter_timer = STCK()

Kernel exit (return to user space):
    elapsed = STCK() - lowcore.sys_enter_timer
    current_thread.sys_timer += elapsed
    lowcore.exit_timer = STCK()

User time (updated on kernel entry):
    user_elapsed = lowcore.sys_enter_timer - lowcore.exit_timer
    current_thread.user_timer += user_elapsed
```

### 7.6 Time Strict Requirements

| # | Requirement |
|---|---|
| TIME-1 | `ktime_get()` must be callable from any context including hard-IRQ. It reads `STCKF` directly — no lock, no sleep. |
| TIME-2 | Timer callbacks execute in softirq context. They must not block or acquire locks held by hard-IRQ handlers. |
| TIME-3 | The CPU timer must be reloaded on every context switch. A thread must never run beyond its quantum without a timer interrupt. |
| TIME-4 | The clock comparator must be reprogrammed after every timer wheel advance to the next non-empty slot. |
| TIME-5 | `tod_boot_offset` is computed once during `pmm_init` and never modified. |

---

## 8. Trap and System Call Architecture

### 8.1 Interrupt Classes

z/Architecture defines six hardware interrupt classes. Each has a dedicated
new PSW slot in the lowcore and a dedicated entry point in the kernel.

| Class | Lowcore offset | Trigger | Kernel handler |
|---|---|---|---|
| `RESTART` | `0x01A0` | SIGP RESTART (AP bringup) | `restart_handler` |
| `EXTERNAL` | `0x01B0` | CPU timer, clock comparator, SIGP, service call | `ext_handler` |
| `SVC` | `0x01C0` | `SVC n` instruction (system call) | `svc_handler` |
| `PROGRAM` | `0x01D0` | Page fault, protection exception, illegal instruction | `pgm_handler` |
| `MCCK` | `0x01E0` | Machine check (hardware error) | `mcck_handler` |
| `IO` | `0x01F0` | Channel subsystem I/O completion | `io_handler` |

### 8.2 Entry Path

All interrupt classes share the same entry structure:

```
Hardware interrupt fires:
    1. Hardware saves old PSW to lowcore (e.g., svc_old_psw at 0x0140).
    2. Hardware saves interrupt parameters to lowcore
       (e.g., svc_code at 0x008A for SVC).
    3. Hardware loads new PSW from lowcore (e.g., svc_new_psw at 0x01C0).
    4. Execution begins at the kernel entry stub.

Kernel entry stub (assembly):
    STMG  R0,R15, lowcore.save_area_sync   // save all GPRs
    // Build irq_frame_t on kernel stack:
    //   gprs[16], psw (from lowcore old PSW), ilc, code
    LG    R15, lowcore.kernel_stack        // switch to kernel stack
    BRASL R14, <C handler>                 // call C dispatcher
    // On return: restore GPRs, LPSWE to return PSW
    LMG   R0,R15, frame.gprs
    LPSWE frame.psw
```

The `irq_frame_t` on the kernel stack is the canonical representation of
the interrupted context. It is used by the fault handler, the debugger,
and the context switch path.

### 8.3 SVC — System Call Dispatch

ZXFoundation defines its own system call table. There is no POSIX
compatibility layer. The SVC number is in `lowcore.svc_code` (16-bit).
Arguments follow the SysV ABI: GPRs 2–7. Return value in GPR 2.

**Every system call that operates on a kernel object takes a capability
handle as its first argument (GPR 2).** The kernel validates the capability
before performing any operation. An invalid or insufficient capability
returns `ERR_CAP_INVALID` immediately.

```
SVC dispatch:

  svc_handler(frame):
      svc_nr = lowcore.svc_code & 0xFF
      if svc_nr >= ZX_SYSCALL_MAX:
          return ERR_INVALID_SYSCALL
      cap_handle = frame.gprs[2]
      object, rights = cap_lookup(current_domain, cap_handle)
      if object == null:
          return ERR_CAP_INVALID
      return syscall_table[svc_nr](object, rights, frame)
```

**ZXFoundation v1 system call surface (~32 syscalls):**

| Number | Name | Capability type | Description |
|---|---|---|---|
| 0 | `zx_cap_derive` | any | Derive a capability with reduced rights |
| 1 | `zx_cap_transfer` | any + `CAP_GRANT` | Transfer a capability via IPC message |
| 2 | `zx_cap_revoke` | any + `CAP_REVOKE` | Revoke all derived capabilities |
| 3 | `zx_domain_create` | domain factory | Create a new domain |
| 4 | `zx_domain_kill` | domain + `CAP_DESTROY` | Kill a domain |
| 5 | `zx_domain_restart` | domain + `CAP_WRITE` | Restart a faulted domain |
| 6 | `zx_thread_create` | domain + `CAP_WRITE` | Create a thread in a domain |
| 7 | `zx_thread_start` | thread + `CAP_EXEC` | Start a thread at a given address |
| 8 | `zx_thread_exit` | — | Terminate the calling thread |
| 9 | `zx_ipc_call` | endpoint + `CAP_EXEC` | Synchronous IPC call |
| 10 | `zx_ipc_recv` | endpoint + `CAP_EXEC` | Block waiting for a message |
| 11 | `zx_ipc_reply` | — | Reply to a synchronous call |
| 12 | `zx_ipc_send` | endpoint + `CAP_EXEC` | Async send (non-blocking) |
| 13 | `zx_mem_map` | VMA + `CAP_MAP` | Map a VMA into the calling domain |
| 14 | `zx_mem_unmap` | VMA + `CAP_WRITE` | Unmap a VMA |
| 15 | `zx_mem_alloc` | domain + `CAP_WRITE` | Allocate anonymous memory |
| 16 | `zx_endpoint_create` | domain + `CAP_WRITE` | Create an IPC endpoint |
| 17 | `zx_endpoint_destroy` | endpoint + `CAP_DESTROY` | Destroy an endpoint |
| 18 | `zx_time_get` | — | Read `ktime_t` (no capability needed) |
| 19 | `zx_sleep` | — | Sleep for a duration |
| 20 | `zx_yield` | — | Voluntarily yield the CPU |
| 21 | `zx_watchdog_register` | domain + `CAP_WRITE` | Register a heartbeat capability |
| 22 | `zx_watchdog_heartbeat` | watchdog cap | Signal liveness to the watchdog |
| 23–31 | reserved | | Future use |

### 8.4 PGM — Program Check Handler

The program check handler dispatches on `lowcore.pgm_code`:

```
pgm_handler(frame):
    code = lowcore.pgm_code
    addr = lowcore.trans_exc_code   // faulting virtual address (if applicable)

    switch code:
        case PGM_TRANSLATION_EXCEPTION:   // page fault
            vma = vmm_find_vma(current_domain.space, addr)
            if vma == null:
                goto domain_fault         // no mapping → domain fault
            page = pmm_alloc_page(ZX_GFP_NORMAL)
            if page == null:
                goto domain_fault         // OOM → domain fault
            mmu_map_page(current_domain.space, addr, page, vma.vm_prot)
            return                        // retry the faulting instruction

        case PGM_PROTECTION_EXCEPTION:    // write to read-only page, or key mismatch
            goto domain_fault

        case PGM_PRIVILEGED_OPERATION:    // user tried a privileged instruction
            goto domain_fault

        case PGM_SPECIFICATION_EXCEPTION: // alignment or format error
            goto domain_fault

        default:
            goto domain_fault

domain_fault:
    domain_suspend(current_domain)
    deliver_fault_event(current_domain, code, addr)
    schedule()                            // switch to another thread
```

A program check in **kernel context** (PSW problem-state bit = 0 at the
time of the fault) is always a kernel panic. The kernel must not generate
translation exceptions or protection exceptions in its own address space.

### 8.5 EXT — External Interrupt Handler

```
ext_handler(frame):
    code = lowcore.ext_int_code

    switch code:
        case EXT_CPU_TIMER (0x1004):
            sched_tick()
            if quantum_expired: schedule()

        case EXT_CLOCK_COMPARATOR (0x1005):
            timer_wheel_advance(current_cpu)
            program_clock_comparator(next_expiry)

        case EXT_SERVICE_CALL (0x2401):
            sclp_service_call_handler()   // SCLP response (console, hardware info)

        case EXT_SIGP_EMERGENCY (0x1201):
            ipi_handler()                 // cross-CPU IPI (TLB shootdown, CPU offline)

        default:
            // Unknown external interrupt: log and ignore.
```

### 8.6 IO — Channel Subsystem Interrupt Handler

```
io_handler(frame):
    schid.sch_no = lowcore.subchannel_nr
    schid.ssid   = lowcore.subchannel_id >> 16

    // Read the Interrupt Response Block (IRB) via TSCH.
    TSCH schid, irb

    // Look up the IRQ descriptor for this subchannel.
    desc = irq_lookup_by_schid(schid)
    if desc == null:
        return                  // spurious; no handler registered

    // Dispatch to the registered handler.
    // The handler is typically the block-I/O server domain's IPC endpoint.
    desc.handler(desc, &irb)
```

The I/O handler is intentionally minimal. It reads the IRB and dispatches
to a registered handler. The handler is responsible for notifying the
appropriate server domain via IPC. The kernel does not interpret I/O
completion data.

---

## 9. Machine-Check Recovery and Watchdog

### 9.1 Machine-Check Classification

When a machine-check interrupt fires, `lowcore.mcck_interruption_code`
classifies the error. The kernel classifies each error as recoverable or
unrecoverable:

| Error class | Recoverable? | Action |
|---|---|---|
| Storage error (corrected) | Yes | Log; mark page suspect; continue |
| Storage error (uncorrected) | No | Offline affected frames; migrate domains |
| CPU malfunction | No | Offline CPU; migrate its domains |
| Timing facility error | Yes | Re-sync TOD; log |
| External damage | No | Kernel panic (hardware integrity lost) |

### 9.2 Machine-Check Recovery Flow

```
mcck_handler(frame):
    code = lowcore.mcck_interruption_code

    if code & MCCK_SD:              // system damage — unrecoverable
        goto kernel_panic

    if code & MCCK_ST:              // storage error
        addr = lowcore.failing_storage_address
        page = phys_to_page(addr)
        if code & MCCK_ST_CORRECTED:
            pmm_mark_suspect(page)  // log; keep in service
        else:
            pmm_offline_page(page)  // remove from buddy; migrate domains
            domain_migrate_from_page(page)

    if code & MCCK_CPU:             // CPU malfunction
        cpu_offline(current_cpu)    // SIGP STOP self after migration
        domain_migrate_all(current_cpu)
        SIGP STOP, current_cpu_addr

    // Recoverable: return to interrupted context.
    LPSWE frame.psw
```

### 9.3 CPU Offline and Domain Migration

When a CPU is taken offline (due to MCCK or operator request):

```
cpu_offline(cpu):
    // 1. Stop accepting new work.
    cpu.state = CPU_OFFLINE_PENDING
    // 2. Drain the run queue to other CPUs.
    acquire cpu.rq_lock
    for each thread in cpu.rq[SCHED_BATCH]:
        target = find_least_loaded_cpu(thread.cpu_mask)
        enqueue(target.rq[SCHED_BATCH], thread)
    release cpu.rq_lock
    // 3. Notify domains whose threads were migrated.
    for each migrated_thread:
        koms_event_fire(migrated_thread.domain, KOBJ_EVENT_DOMAIN_MIGRATE)
    // 4. Stop the CPU.
    cpu.state = CPU_OFFLINE
    SIGP STOP, cpu.cpu_addr
```

### 9.4 Domain Watchdog

The kernel maintains a per-CPU watchdog thread at `SCHED_REALTIME` priority.
Each server domain that registers with the watchdog receives a heartbeat
capability. The domain must call `zx_watchdog_heartbeat` within a configured
interval (default: 5 seconds).

```
Watchdog state machine (per registered domain):

  WATCHDOG_OK ──── heartbeat received ────► WATCHDOG_OK
       │
       │ interval elapsed without heartbeat
       ▼
  WATCHDOG_WARN ──── heartbeat received ──► WATCHDOG_OK
       │
       │ second interval elapsed
       ▼
  WATCHDOG_FAULT
       │
       ▼
  domain_fault(domain)   // triggers fault containment flow (Section 5.5)
```

The watchdog thread runs on a dedicated CPU (CPU 0 by convention) and is
never migrated. It is the only `SCHED_REALTIME` thread that the kernel
creates at boot time.

### 9.5 Kernel Self-Check (syschk)

The existing `zx_system_check()` infrastructure is extended with severity
levels:

| Severity | Action |
|---|---|
| `ZX_SYSCHK_WARNING` | Log to kernel ring buffer; continue |
| `ZX_SYSCHK_DEGRADED` | Disable the affected subsystem; log; continue |
| `ZX_SYSCHK_CORE_CORRUPT` | Disabled-wait PSW (kernel panic) |

`ZX_SYSCHK_CORE_CORRUPT` is reserved for conditions where kernel data
structures are known to be corrupted and continued execution would cause
silent data loss or security violations. All other conditions should use
`WARNING` or `DEGRADED` to maximize availability.

### 9.6 Storage Key Protection

Each domain is assigned a non-zero s390x storage key at creation time.
All pages mapped into the domain's address space are assigned that key.
The domain's PSW access key field is set to match.

A domain that attempts to access a page with a mismatched storage key
receives a protection exception (PGM code 0x04). This is handled as a
domain fault (Section 8.4) — the domain is suspended, not the kernel.

This provides a hardware-enforced memory isolation layer that operates
independently of DAT. Even if a bug in the kernel's page table management
accidentally maps a page from domain A into domain B's address space, the
storage key check will prevent domain B from reading or writing it.

---

## 10. Long-Term Implementation Roadmap

### 10.1 Overview

The roadmap is organized into seven phases. Each phase has a clear
prerequisite, a defined deliverable, and a set of subsystems it unlocks.
Phases are sequential within a dependency chain but may overlap where
dependencies permit.

```
Phase dependency graph:

  [Phase 1: TCB Hardening]
          │
          ▼
  [Phase 2: Capability Foundation]
          │
          ▼
  [Phase 3: Domain and IPC]
          │
     ┌────┴────┐
     ▼         ▼
[Phase 4:  [Phase 6:
 Server     Memory
 Domain     Completion]
 Infra]
     │
     ▼
[Phase 5: First Server Domains]
     │
     ▼
[Phase 7: Hardening and Observability]
```

### 10.2 Phase 1 — TCB Hardening

**Prerequisite:** Current state (PMM, VMM, slab, KOMS, IRQ, SMP, sync all
functional).

**Deliverables:**

1. **Trap/entry completion:** Full `irq_frame_t` save/restore for all six
   interrupt classes. SVC, PGM, EXT, IO, MCCK, RESTART handlers dispatch
   to C. Return path restores full CPU state via `LPSWE`.

2. **Time subsystem:** TOD clock read (`STCKF`), `ktime_t` type and
   `ktime_get()`. CPU timer setup and quantum enforcement. Clock comparator
   setup. Timer wheel (8 levels, 64 slots). `ktime_sleep()`.

3. **Scheduler — BATCH class:** Per-CPU run queues. `schedule()`,
   `thread_block()`, `thread_wake()`. Context switch (GPR/FPR/PSW save-
   restore). CPU timer interrupt → `sched_tick()`. Work stealing.
   Idle thread per CPU.

**Unlocks:** Phase 2 (capability system requires a running scheduler to
test domain creation).

### 10.3 Phase 2 — Capability Foundation

**Prerequisite:** Phase 1 complete.

**Deliverables:**

1. **Capability token:** 64-bit structure, type/rights/gen/index fields.
   `cap_mint`, `cap_derive`, `cap_revoke`, `cap_lookup`.

2. **Capability table:** Slab cache with storage key 1. Per-domain flat
   array. `cap_table_alloc`, `cap_table_free`. `PF_PINNED` pages.

3. **KOMS extension:** `kobject_t` gains `cap_gen` (generation counter)
   and `global_index` (object table index). Global object table
   (flat array, spinlock-protected). `koms_init_obj` registers in table.
   `koms_put` at zero increments `cap_gen` before freeing.

4. **Syscalls 0–2:** `zx_cap_derive`, `zx_cap_transfer`, `zx_cap_revoke`.
   SVC dispatch table. Capability validation on every syscall entry.

**Unlocks:** Phase 3 (domain creation requires capability tables).

### 10.4 Phase 3 — Domain and IPC

**Prerequisite:** Phase 2 complete.

**Deliverables:**

1. **Domain object:** `domain_t` kobject type. `vm_space_t` creation per
   domain. Capability table allocation at domain birth. Domain lifecycle
   state machine. `domain_create`, `domain_kill`.

2. **Thread object:** `thread_t` kobject type. Kernel stack allocation.
   `thread_create`, `thread_start`, `thread_exit`. Integration with
   scheduler (enqueue on `thread_start`).

3. **SVC entry — capability validation:** Every syscall validates its
   capability argument before proceeding. `ERR_CAP_INVALID` on failure.

4. **IPC sync fastpath:** `zx_ipc_call`, `zx_ipc_recv`, `zx_ipc_reply`.
   Direct thread switch. Register-passing (GPRs 2–9). Fastpath conditions
   enforced.

5. **IPC async queue:** Ring buffer slab allocation. `zx_ipc_send`.
   Enqueue/dequeue. Receiver wake on enqueue.

6. **Syscalls 3–17:** Full domain, thread, memory, and endpoint syscalls.

**Unlocks:** Phase 4 and Phase 6 (both depend on working domains and IPC).

### 10.5 Phase 4 — Server Domain Infrastructure

**Prerequisite:** Phase 3 complete.

**Deliverables:**

1. **Fault containment:** `domain_suspend`, `deliver_fault_event`. Fault
   event IPC to supervisor domain. `domain_restart`, `domain_kill` from
   supervisor.

2. **Domain watchdog:** Watchdog thread at `SCHED_REALTIME`. Heartbeat
   capability. `zx_watchdog_register`, `zx_watchdog_heartbeat`. Two-strike
   fault trigger.

3. **MCCK recovery:** Storage error classification. `pmm_offline_page`.
   CPU offline and domain migration. `KOBJ_EVENT_DOMAIN_MIGRATE`.

4. **Storage key assignment:** Per-domain key allocation. Page key
   assignment on `vmm_insert_vma`. PSW access key set on context switch.

5. **System manager domain:** The first server domain, started by the
   kernel at boot. Receives fault events for all other server domains.
   Implements restart policy.

**Unlocks:** Phase 5 (server domains require fault containment to be safe).

### 10.6 Phase 5 — First Server Domains

**Prerequisite:** Phase 4 complete.

**Deliverables:**

1. **Console server:** Wraps DIAG 0x08 / SCLP. Exposes `ep.write`
   endpoint. Accepts `zx_ipc_send` with a string payload. Replaces
   `printk` for user-visible output.

2. **Channel I/O server:** Wraps CSS interrupt dispatch. Accepts subchannel
   registration from other domains. Exposes `ep.request` for I/O submission.
   Returns I/O completion via IPC reply.

3. **Block I/O server:** Built on channel I/O server. Implements ECKD
   (DASD) read/write. Exposes `ep.request` with a block I/O protocol.

4. **Filesystem server (minimal):** Built on block I/O server. Implements
   a read-only flat filesystem (sufficient to load user programs). Exposes
   `ep.open`, `ep.read`.

**Unlocks:** Phase 7 (hardening requires a running system to test against).

### 10.7 Phase 6 — Memory Management Completion

**Prerequisite:** Phase 3 complete (can proceed in parallel with Phase 4/5).

**Deliverables:**

1. **Demand paging:** PGM translation exception → `vmm_find_vma` →
   `pmm_alloc_page` → `mmu_map_page` → retry. Anonymous and file-backed
   VMAs.

2. **Copy-on-write:** `VM_COW` flag on shared VMAs. Write protection fault
   → page copy → remap. Used for domain cloning (fork-like semantics).

3. **Page reclaim:** LRU list per zone. Reclaim under memory pressure
   (triggered when `ZONE_NORMAL.free_pages < LOW_WATERMARK`). Reclaim
   selects cold anonymous pages; writes dirty pages to swap device.

4. **Swap:** Capability-gated swap device via channel I/O server. Swap
   page table entries. `pmm_swap_out`, `pmm_swap_in`.

**Unlocks:** Phase 7 (full memory management required for production use).

### 10.8 Phase 7 — Hardening and Observability

**Prerequisite:** Phases 4, 5, and 6 complete.

**Deliverables:**

1. **KOMS attribute bus:** Expose domain/thread/memory statistics as KOMS
   attributes. Readable via `zx_attr_get` syscall with a capability.

2. **Kernel ring buffer:** Fixed-size circular log buffer. Capability-gated
   read via `ep.klog` endpoint. Replaces `printk` for kernel diagnostics.

3. **Capability audit log:** Every `cap_mint`, `cap_derive`, `cap_revoke`,
   and `cap_transfer` is logged to a dedicated ring buffer. Readable by
   the system manager domain.

4. **Syscall fuzz harness:** Host-side tool that generates random syscall
   sequences and validates that the kernel never panics (only returns
   error codes) on invalid inputs.

5. **SMP stress test:** Multi-domain IPC stress test exercising the
   fastpath, work stealing, and domain fault/restart under load.

### 10.9 Milestone Summary

| Phase | Key Deliverable | Unlocks |
|---|---|---|
| 1 | Trap, time, scheduler | Capability system |
| 2 | Capability tokens and tables | Domain creation |
| 3 | Domains, threads, IPC | Server domains, memory completion |
| 4 | Fault containment, watchdog, MCCK | First server domains |
| 5 | Console, block I/O, filesystem | Full system |
| 6 | Demand paging, CoW, reclaim, swap | Production memory management |
| 7 | Observability, audit, hardening | Production readiness |

---

*End of ZXF-KRN-DESIGN-001 Rev 26h1.0*
