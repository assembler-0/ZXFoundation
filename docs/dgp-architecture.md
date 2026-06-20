# ZXFoundation Domain-Gate-Portal (DGP) Kernel Architecture

**Version:** 1.0.0  
**Architecture:** z/Architecture (s390x)  
**Document ID:** ZX-ARCH-DGP-001  
**Status:** Final

---

## 1. Architectural Overview

The ZXFoundation kernel implements a **Domain-Gate-Portal (DGP)** architecture — a
*novel capability-based, hardware-isolated kernel design* that is neither monolithic,
microkernel, nor exokernel. The kernel is organized around three primitive abstractions:

| Primitive | Hardware Mechanism | Purpose |
|---|---|---|
| **Domain** | Region-1 ASCE (CR1), Storage Key (SK) | Unit of isolation; owns address space, strands, capabilities |
| **Gate** | Program Call (PC) instruction | Cross-domain control transfer |
| **Portal** | Access Register mode (ALET/AR) | Cross-domain zero-copy data access |
| **Capability** | Kobject ID + generation + rights | Opaque, unforgeable resource token |

### 1.1 Design Principles

1. **Minimal Trusted Computing Base (TCB):** Only domain 0 (the nucleus) is trusted.
   The nucleus exposes exactly **22 entry points** — the entire kernel API surface.
   There are no `ioctl`, `/proc`, or `sysfs` interfaces.

2. **Hardware-Enforced Isolation:** Every domain switch reloads the DAT (Dynamic
   Address Translation) tables via CR1. Storage keys (ACC field) gate every memory
   access. Cross-domain data movement requires explicit portal creation — no shared
   memory without capability delegation.

3. **Capability-Based Authorization:** All resource access is mediated through
   opaque capability tokens. A capability encodes: object type, object ID, generation
   counter (ABA prevention), and delegated rights. Capabilities are delegatable —
   a domain can pass a capability to another domain through a gate call.

4. **Channel-Program I/O:** All bulk I/O uses the z/Architecture Channel Subsystem
   (CSS). Device drivers run in user domains and submit channel command words (CCWs)
   through nucleus calls. The CSS is the *only* kernel-level I/O path.

5. **No Traditional Signals:** Exceptions and asynchronous events are delivered as
   nucleus call return codes or via a signal queue checked at domain re-entry.

### 1.2 Kernel vs User Space Boundary

```
┌──────────────────────────────────────────────────────┐
│ NUCLEUS (Domain 0)                                    │
│                                                        │
│ ┌──────────────────────────────────────────────────┐  │
│ │ Kernel Core: Sched, PMM, VM, IRQ, Timer, syschk │  │
│ │ SCOMS: Object tables, lifecycle, refcounting     │  │
│ │ Gates & Portals: Cross-domain primitives         │  │
│ │ CSS: Channel subsystem, CCW I/O                  │  │
│ │ Console: Emergency output (DIAG)                 │  │
│ └──────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────┘
         │  ▲ PC instruction (22 numbered calls)       │
         ▼  │                                           │
┌──────────────────────────────────────────────────────┐
│ USER DOMAINS (1 .. MAX_DOMAINS)                       │
│                                                        │
│ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐  │
│ │ init         │ │ vfs_server   │ │ app_domain   │  │
│ │ Bootstraps   │ │ Filesystem   │ │ Application  │  │
│ │ system       │ │ operations   │ │ logic        │  │
│ └──────────────┘ └──────────────┘ └──────────────┘  │
│ ┌──────────────┐ ┌──────────────┐                     │
│ │ driver_io    │ │ net_server   │                     │
│ │ Device I/O   │ │ Network      │                     │
│ │ mediation    │ │ stack        │                     │
│ └──────────────┘ └──────────────┘                     │
└──────────────────────────────────────────────────────┘
```

**Everything outside domain 0 runs in user mode** with the PSW problem-state bit set.
There is no "kernel-mode driver" concept. Device-specific logic runs in user domains
and accesses hardware only through the CSS channel subsystem or DIAG calls.

### 1.3 Why Not Existing Architectures

| Architecture | Why Not |
|---|---|
| **Monolithic** | Single address space: one fault brings down the entire system. No capability model. |
| **Microkernel** | IPC overhead on every service call. Does not leverage z/Architecture PC/AR hardware. |
| **Exokernel** | Exposes hardware directly; no abstraction for portability or capability-based security. |
| **Unikernel** | Single application per kernel; no multi-domain isolation. |
| **DGP (this)** | Hardware-enforced domains via DAT reload + storage keys. Zero-copy cross-domain data via AR mode. Only 22 entry points: mathematically verifiable TCB. |

---

## 2. Domain Model

### 2.1 Domain Lifecycle

```
  embryo ──┬──► active ◄──► suspended
           │       │
           │       ▼
           └──► dying ──► (slot released to free list)
```

**Transitions:**

| From | To | Trigger | Actions |
|---|---|---|---|
| embryo | active | `domain_activate()` | DAT tables loaded; domain becomes schedulable |
| active | suspended | `domain_suspend()` | Domain blocked; not on run queue |
| suspended | active | `domain_resume()` | Domain re-queued |
| active/suspended | dying | `domain_exit()` / `domain_destroy()` | Resources being freed |
| dying | (free) | `domain_reap()` | Slot returned to SCOMS free list |

### 2.2 Domain Structure

```
struct domain (offset 0):
  ├── kobject (24 bytes)    # SCOMS identity: id, type, state, refcount, generation, lock
  ├── name (8 bytes)        # Human-readable name pointer
  ├── flags (4 bytes)       # domain_flags: root, idle, need_resched
  ├── storage_key (1 byte)  # 4-bit ACC value (0=nucleus, 8-15=user)
  ├── root_phys (8 bytes)   # Physical address of region-1 root page table
  ├── primary_strand (N bytes) # Embedded execution context (or strand pointer)
  ├── asce (8 bytes)        # Region-1 ASCE value loaded into CR1 on switch
  ├── gate_count (4 bytes)  # Number of gates referencing this domain
  ├── portal_count (4 bytes)# Number of portals involving this domain
  ├── domain_list (list)    # Global domain list node
  └── vm_space (N bytes)    # Per-domain virtual memory region tree
```

### 2.3 Domain Resource Limits

| Resource | Limit |
|---|---|
| Max domains | 256 |
| Max strands per domain | 64 |
| Max gates per domain | 512 (system-wide) |
| Max portals per domain | 16 (hardware: 16 ARs) |
| Max portals system-wide | 1024 |
| Max VME regions per domain | 4096 |
| Storage keys per domain | 1 (4-bit ACC) |
| User keys available | 8 (keys 8-15) |

### 2.4 Strand — Execution Context

A strand is the unit of schedulable execution (equivalent to a thread). Each domain
has at least one strand (the `primary_strand`). Strands within a domain share the
domain's address space, gates, and portals.

**Key fields (assembly-critical offsets):**

| Offset | Field | Purpose |
|---|---|---|
| 0 | kobject | SCOMS identity |
| 24 | owner | Back-pointer to owning domain |
| 40 | sched | EEVDF scheduling parameters |
| 80 | sched_rb | Red-black tree node (scheduler) |
| 104 | kstack_base | Kernel stack base address |
| 112 | kstack_top | Kernel stack top (initial SP) |
| 120 | ksp | Current kernel stack pointer |
| 160+ | uregs | User register save area |

---

## 3. Gate — Cross-Domain Control Transfer

### 3.1 Hardware Foundation

The z/Architecture **Program Call (PC)** instruction (POP Chapter 7) performs an
inter-address-space control transfer:

1. Saves current PSW, access registers, and execution mode
2. Loads new PSW from a Program Call Parameter List (PCPL)
3. Switches to the target address space
4. Continues execution at the entry point in the target domain

PC is **not a supervisor call** — it does not require privilege.
Any domain can PC to any other domain, subject to gate authorization.

### 3.2 Gate Types

| Type | PC Number Range | Purpose |
|---|---|---|
| Nucleus | 0-31 | Kernel service calls (22 used, 10 reserved) |
| Domain | 32-511 | Cross-domain application calls |

### 3.3 Nucleus Call Numbers (Complete API)

| # | Name | Arguments | Return | Description |
|---|---|---|---|---|
| 0 | `domain_create` | name_ptr | domain_id | Create a new domain |
| 1 | `domain_destroy` | domain_id | status | Destroy an existing domain |
| 2 | `domain_exit` | — | — | Terminate current domain |
| 3 | `domain_info` | — | packed(id,cpu) | Query current domain metadata |
| 4 | `mem_alloc` | size | va | Allocate anonymous memory |
| 5 | `mem_free` | va, size | status | Free allocated memory |
| 6 | `mem_map` | va, phys, size, prot | status | Map physical pages |
| 7 | `mem_unmap` | va, size | status | Unmap pages |
| 8 | `mem_protect` | va, size, prot | status | Change page protection |
| 9 | `mem_share` | target_id, va, size, prot | cap_token | Share memory with another domain |
| 10 | `gate_create` | type, auth, target, entry, key | gate_id | Create a cross-domain gate |
| 11 | `gate_destroy` | gate_id | status | Destroy a gate |
| 12 | `portal_create` | target, va_start, va_end, prot, ar | portal_id | Create data access portal |
| 13 | `portal_destroy` | portal_id | status | Destroy a portal |
| 14 | `io_bind` | dev_id, domain_id | cap_token | Bind an I/O device to a domain |
| 15 | `io_unbind` | cap_token | status | Unbind an I/O device |
| 16 | `io_submit` | cap_token, orb_ptr | status | Submit a channel program (CCW) |
| 17 | `io_irq_bind` | cap_token, gate_id | status | Bind I/O interrupt to a gate |
| 18 | `time_yield` | — | — | Voluntarily yield the CPU |
| 19 | `time_deadline` | ns_from_now | deadline_ns | Set a timer deadline |
| 20 | `time_now` | — | monotonic_ns | Read monotonic clock |
| 21 | `console_write` | buf, len | status | Write to console |

### 3.4 Gate Authorization

| Level | Meaning |
|---|---|
| `any` | Any domain can invoke this gate |
| `keyed` | Only domains with matching storage key can invoke |
| `owner` | Only the owning domain can invoke |

---

## 4. Portal — Cross-Domain Data Access

### 4.1 Hardware Foundation

z/Architecture **Access Register (AR) mode** (POP Chapter 5) allows a program to
access another address space through ALETs (Access List Entry Tokens):

1. Load an ALET into an Access Register (AR) via `LARL` or `SAR`
2. Use a logical address with AR-specified translation
3. The hardware walks the ASTE (Address Space Table Entry) chain to translate
4. No software intervention on each access — zero-copy after setup

Each domain has 16 ARs (AR0-AR15). AR0 is reserved for the primary address space.
AR1-AR15 can hold ALETs for up to 15 simultaneous portals.

### 4.2 Portal Types

| Type | Access | Use Case |
|---|---|---|
| Read-only | `portal_prot::read` | File data, configuration |
| Write-only | `portal_prot::write` | Log output, device buffers |
| Read-write | `portal_prot::rw` | Shared memory, IPC buffers |

### 4.3 Portal Lifecycle

1. Source domain calls `portal_create(target_id, va_start, va_end, prot, ar_num)`
2. Nucleus: allocates ASTE slot, creates ALE (Access List Entry), assigns ALET
3. Source domain loads ALET into AR[ar_num]
4. Access to target domain's memory is now direct via AR-specified addressing
5. `portal_destroy()` tears down the ALE and purges the ALB (Access List Buffer)

---

## 5. Capability System

### 5.1 Capability Token Format

```
63              48 47          32 31          16 15            0
┌─────────────────┬──────────────┬──────────────┬────────────────┐
│     RIGHTS      │  GENERATION  │   OBJECT_ID  │     TYPE       │
│     (16 bits)   │   (16 bits)  │   (16 bits)  │   (16 bits)    │
└─────────────────┴──────────────┴──────────────┴────────────────┘
```

- **Type (bits 0-15):** One of `cap_type` enum
- **Object ID (bits 16-31):** Index into the relevant SCOMS table
- **Generation (bits 32-47):** Object's generation counter (ABA prevention)
- **Rights (bits 48-63):** Delegated access rights mask

### 5.2 Capability Types

| Type | Value | Object Table |
|---|---|---|
| `none` | 0 | — |
| `domain` | 1 | Domain table |
| `gate` | 2 | Gate table |
| `portal` | 3 | Portal table |
| `io_device` | 5 | I/O binding table |
| `memory` | 6 | VM region |
| `service` | 7 | Service registry entry |

### 5.3 Capability Operations

- **Acquire:** Look up object by ID + generation; increment refcount
- **Release:** Decrement refcount; free object if last reference
- **Delegate:** Create a new capability with subset rights (passed through gate)
- **Revoke:** Increment object generation; invalidate all existing capabilities

---

## 6. Service Registry

### 6.1 Protocol

A lightweight SCOMS-based registry allowing domains to publish and discover services.

| Operation | Gate Call | Description |
|---|---|---|
| Register | service_register | Register(name, cap) → service_id |
| Lookup | service_lookup | Lookup(name) → cap |
| Unregister | service_unregister | Unregister(service_id) |

### 6.2 Well-Known Services

| Service Name | Description | Responsible Domain |
|---|---|---|
| `zx.vfs` | Virtual file system server | init (or dedicated) |
| `zx.console` | Console I/O (virtual) | nucleus (domain 0) |
| `zx.io.binder` | I/O device binding | nucleus |
| `zx.diag` | Diagnostic output | nucleus |

---

## 7. Signal & Exception Delivery

### 7.1 Design

The DGP kernel does not use Unix-style signals. Instead:

1. **Synchronous exceptions** (protection fault, addressing exception):
   The nucleus returns an error code from the nucleus call. No signal delivery occurs.

2. **Asynchronous events** (timer expiry, I/O completion):
   The nucleus sets `signal_pending` flag on the target domain's kobject and requests
   reschedule. On the next return-to-user transition, the domain's trampoline checks
   `signal_pending`. If set, the nucleus delivers the signal by modifying the
   domain's register state to call a registered signal handler.

3. **Domain exit** is always voluntary — the nucleus never forcefully terminates
   a domain. If a domain is stuck, the watchdog halts the entire system.

### 7.2 Signal Queue

Per-domain signal queue (embedded in domain struct):

```c
struct signal_queue {
    u32                 pending_mask;  // Bitmask of pending signal types
    u32                 _pad;
    signal_handler      handlers[32];  // Registered handlers
    u64                 data[32];      // Per-signal opaque data
};
```

### 7.3 Signal Types

| # | Name | Source | Default Action |
|---|---|---|---|
| 0 | SIG_TIMER | Timer expiry | Ignore |
| 1 | SIG_IO | I/O completion | Ignore |
| 2 | SIG_FAULT | Domain page fault | Exit |
| 3 | SIG_TERM | Graceful termination request | Exit |
| 4 | SIG_USER0 | User-defined | Ignore |
| 5 | SIG_USER1 | User-defined | Ignore |

---

## 8. I/O Model

### 8.1 Channel Subsystem (CSS)

All bulk I/O uses the z/Architecture CSS. The kernel provides:

- Device discovery (probe subchannels)
- I/O interrupt routing
- Channel program submission
- DMA buffer management

### 8.2 Device Driver Model

Device drivers are **user domains** that:

1. Call `io_bind(dev_id)` to obtain a device capability
2. Construct CCW (Channel Command Word) programs in DMA-able memory
3. Call `io_submit(cap, orb)` to execute the channel program
4. Receive I/O completion via signal or gate call

### 8.3 DMA Memory

DMA buffers are allocated from the `zone::dma31` zone (< 2 GiB physical) and
mapped into the driver domain's address space via `mem_map_fixed()`.

---

## 9. Boot Protocol

### 9.1 Boot Sequence

```
ZXFL (stage1) → ZXFL (stage2) → Kernel ELF → zxfoundation_global_initialize()
                                ↓
1. Protocol integrity verification
2. Checksum verification (SHA-256)
3. Feature detection (STFLE)
4. Lowcore initialization
5. syschk_init, time_init
6. PMM init, MMU init
7. SLUB init, kmalloc init, VM init
8. IRQ subsystem init
9. Timer init
10. RCU init
11. Domain subsystem init
12. Gate subsystem init, Portal init
13. Scheduler init
14. SMP init
15. CSS probe
16. Bootstrap domain creation
17. Scheduler idle loop
```

### 9.2 Domain Hierarchy

```
Domain 0 (nucleus)
 └── Domain 1 (sys/bootstrap)
      └── Domain 2 (user/init)
           ├── Domain 3 (vfs_server)
           ├── Domain 4 (net_server)
           └── Domain 5+ (applications)
```

---

## 10. Security Model

### 10.1 Storage Key Capabilities

Each domain has exactly one 4-bit **Access Control Code (ACC)**:

- **Key 0:** Nucleus (master key — unrestricted access to all memory)
- **Keys 1-7:** Reserved for future use (system services)
- **Keys 8-15:** User-assignable (one per domain)

Memory access requires:
```
ACC(domain) == ACC(page)  OR  Fetch-Protection-Bit(domain) == 0
```

### 10.2 Capability Delegation

A capability can be delegated by passing it as an argument to a gate call.
The receiver can then use the capability to access the resource. Revocation
is achieved by incrementing the object's generation counter, which invalidates
all outstanding capabilities.

### 10.3 Audit Trail

The nucleus can log all gate invocations via the printk subsystem.
Each nucleus call is dispatched through a single function (`nucleus_dispatch`),
providing a natural audit point.

---

## 11. Module Layout

### 11.1 Source Tree Structure

```
zxfoundation/          # Kernel modules
├── base/              # Types, config, typestate, endian, cxxrtse
├── domain/            # Domain + strand types and lifecycle
├── gate/              # Gate types, lifecycle, dispatch, handlers
├── portal/            # Portal types and lifecycle
├── scoms/             # SCOMS object management
├── memory/            # PMM, VM, SLUB, kmalloc, HHDM, fault
├── sched/             # Scheduler, context, fair, idle
├── sys/               # printk, time, timer, syschk, irq, cap, service, signal
├── exec/              # ELF loader
├── init/              # Bootstrap, main entry
├── rcu/               # Read-copy-update
├── sync/              # Wait queues
├── locking/           # Spinlocks, seqlocks, MCS
arch/s390x/            # Architecture-specific
├── cpu/               # Lowcore, PSW, IRQ, SMP, SIGP, IPI, etc.
├── mmu/               # DAT page tables, TLB management
├── css/               # Channel subsystem, CCW I/O
├── trap/              # Interrupt/trap entry stubs
├── sched/             # Context switch assembly
├── init/              # Boot head, ZXFL protocol, linker script
├── lib/               # Atomic operations
drivers/               # Device drivers
├── console/           # Console driver (DIAG-based)
crypto/                # Cryptographic services
├── sha256/            # SHA-256 implementation
lib/                   # Kernel library
├── expected, error, format, string, span, rb_tree, list, ...
```

### 11.2 Kernel Memory Layout (Virtual Address Space)

```
0x0000.0000.0000.0000 - 0x0000.0000.0000.7FFF:  Lowcore (per-CPU)
0x0000.0000.0040.0000 - 0x0000.0080.0000.0000:  User space (domains)
0x0000.0080.0000.0000 - 0x0000.00FF.FFFF.FFFF:  Reserved
0x0000.0X00.0000.0000 - 0x0000.0XFF.FFFF.FFFF:  HHDM (identity-mapped physical)
```

---

## 12. Drawbacks and Risk Mitigation

### 12.1 Known Drawbacks

| Issue | Impact | Mitigation |
|---|---|---|
| **Context switch cost** | DAT reload + storage key switch per domain switch | z/Architecture ASN (Address Space Number) facility reduces cost; kernel supports ASN reuse |
| **Limited portals** | Only 15 portals per domain (16 ARs minus AR0) | Portal chaining: one portal can map large VA ranges; domains can request multiple portal slots |
| **Storage key exhaustion** | Only 8 user keys (8-15) | Key reuse on domain destruction; future: key aliasing via ASTE |
| **No shared libraries** | Each domain has separate address space | Portal-based shared library mechanism: code pages mapped read-only via portal |
| **No live migration** | Domain state is tied to physical storage key | Storage key virtualization in progress |
| **I/O latency** | Channel program setup requires nucleus call | Batch submission; asynchronous I/O completion via signals |

### 12.2 Edge Cases

1. **Domain destruction with active portals:** `portal_destroy_for_domain()` tears down
   all portals referencing the dying domain. The ALB (Access List Buffer) is purged
   to prevent stale ALET usage.

2. **Storage key conflict:** Two domains request the same key. The allocator assigns
   the first available key. On exhaustion, domain creation fails with
   `memory_error::out_of_memory`.

3. **Gate invocation during domain teardown:** Gates pointing to a dying domain are
   destroyed by `gate_destroy_for_domain()` before the domain slot is freed.

4. **Nucleus call from interrupt context:** The dispatch function runs with IRQs
   disabled. Some calls (domain_create, mem_alloc) may sleep — these return
   `generic_error::not_supported` if called from interrupt context.

5. **Out-of-memory during domain creation:** The transactional pattern
   (`domain_create_txn`) ensures all partial allocations are rolled back if any step
   fails. No resources leak.

6. **Capability revocation race:** A domain holds a capability that is revoked
   (generation incremented). Next access via that capability returns
   `generic_error::not_found`. The kernel ensures all dispatch paths check
   generation before granting access.

---

## 13. API Reference (Nucleus Calls)

Each nucleus call follows the convention:

```
auto nc_<name>_handler(
    u64 a0,   // Arg 0 (r3)
    u64 a1,   // Arg 1 (r4)
    u64 a2,   // Arg 2 (r5)
    u64 a3,   // Arg 3 (r6)
    u64 a4,   // Arg 4 (r7)
    u64 a5    // Arg 5 (r8, currently unused)
) noexcept -> u64;
```

Return value `~0ULL` signals an error. All other values are success indicators.

---

*Document version 1.0.0 — June 2026*  
*Copyright (C) 2026 assembler-0. Apache License, Version 2.0.*
