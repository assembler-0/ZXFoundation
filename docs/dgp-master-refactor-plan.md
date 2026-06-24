# DGP Master Refactor Plan

## Audit Scope

This document records the kernel-wide audit matrix for the hardware-first DGP clean-core rewrite. The audit follows the manifest-discovered C++23 module graph and the loader handoff boundary rather than isolated source files. It classifies each subsystem as:

- **Compliant**: the subsystem already uses the required z/Architecture boundary or has a narrow implementation seam suitable for direct clean-core adoption.
- **Partial**: the subsystem has the intended mechanism, but policy ownership, lifecycle enforcement, SMP rules, or rollback semantics are incomplete.
- **Unsafe**: the subsystem exposes authority, hardware state, or concurrency behavior outside a single verified DGP contract.

The selected direction is **Prototype A — Integrated Hardware Isolation Contract**: a new `zxfoundation/dgp/` clean core owns the policy surface and delegates implementation to existing domain, gate, portal, memory, MMU, scheduler, and synchronization modules during migration.

## Subsystem Audit Matrix

| Subsystem | Current Files | DGP Classification | Finding | Required Migration |
| --- | --- | --- | --- | --- |
| Boot and loader handoff | `arch/s390x/init/zxfl/stage2/entry.c`, `arch/s390x/init/manifest.zxd`, `zxfoundation/init/main.cxxm` | Partial | ZXFL remains correctly isolated as pure C and provides checksummed protocol data, but NUMA and topology evidence are still heuristic and the kernel initializes DGP objects before a single DGP policy plane exists. | Keep ZXFL C-only, harden topology provenance later with `STSI`/SCLP evidence, and initialize `zxfoundation::dgp::isolation_init()` before user-visible domain, gate, or portal objects are created. |
| CPU feature and lowcore setup | `arch/s390x/cpu/features*.cxxm`, `lowcore*.cxxm`, `processor*.cxxm`, `smp*.cxxm` | Partial | CPU facility discovery and per-CPU state exist, but DGP policy does not yet consume facility state as mandatory preconditions for ASCE, storage-key, PC, AR, ASTE/ALE, and SMP invalidation features. | Add clean-core feature masks and boot-time assertions so domains cannot transition to active when mandatory hardware isolation features are unavailable. |
| Program-call linkage | `arch/s390x/cpu/pc.cxxm`, `arch/s390x/cpu/linkage.cxxm`, `zxfoundation/gate/gate.cxxm` | Partial | PC linkage and fixed nucleus linkage tables exist, but gate authority is still mostly software-local and not interpreted through a single DGP policy object. | Route all gate entry checks through `isolation_validate_gate_entry()` before dispatch, and later bind gate descriptors to explicit PC linkage metadata and capability generations. |
| Access-register portals | `arch/s390x/cpu/aste.cxxm`, `arch/s390x/cpu/aste_types.cxxm`, `arch/s390x/cpu/ar.cxxm`, `zxfoundation/portal/portal.cxxm` | Partial | Portal objects allocate `ASTE`/`ALE` resources transactionally, but portal range, target lifecycle, and requested rights are not validated by the same policy plane that owns domain isolation. | Validate portal creation through `isolation_validate_portal()` before `ASTE`/`ALE` allocation, and later add explicit `ALB` invalidation rules for portal teardown and range revocation. |
| Domain lifecycle | `zxfoundation/domain/domain.cxxm`, `zxfoundation/domain/domain_types.cxxm`, `zxfoundation/domain/strand_types.cxxm` | Partial | Domains now carry DGP isolation state for ASCE and storage-key identity, but the domain is still mostly a primary-strand container and teardown is not yet protected by `sync_contract` call-site checks. | Add multi-strand lifecycle APIs, deny-new-entry teardown phases, and real synchronization-contract validation before domain destruction or scheduler-visible state changes. |
| Dispatch authorization | `zxfoundation/gate/dispatch.cxxm`, `zxfoundation/gate/nc_*.cxxm`, `zxfoundation/sys/cap*.cxxm` | Partial | Gate dispatch and portal creation route through DGP policy, but authority lookup still does not import `sync_contract` or run inside a proven non-blocking/RCU-protected context at every call site. | Keep generation-counted `sys::cap` tokens as authority carriers and wire DGP synchronization-contract snapshots into gate, portal, signal, and I/O authority checks. |
| Capability system | `zxfoundation/sys/cap.cxxm`, `zxfoundation/sys/cap_types.cxxm` | Partial | Generation-counted tokens and typed rights exist, but enforcement coverage is inconsistent and token meaning is interpreted outside one DGP policy owner. | Keep token generation and table semantics, but make the DGP clean core the only policy interpreter for gate, portal, memory-share, signal, and I/O authority. |
| Physical memory | `zxfoundation/memory/pmm.cxxm`, `zxfoundation/memory/pmm_types.cxxm`, `zxfoundation/memory/slub*.cxxm`, `zxfoundation/memory/kmalloc.cxxm` | Partial | Buddy zones and PCP caches are now node-owned; `frame_desc::numa_node` is allocator ownership metadata for free, rollback, and local-cache placement, not an advisory drawer hint. | Add memory hotplug/offline, migration, distance tables, live reclaim feedback, and hardware failure-domain recovery before calling the NUMA implementation complete. |
| Virtual memory | `zxfoundation/memory/vm.cxxm`, `zxfoundation/memory/vm_types.cxxm`, `arch/s390x/mmu/mmu.cxxm` | Partial | VM regions now store DGP memory policy metadata for rights, storage key, NUMA node, generation, and invalidation scope, but storage-key hardware programming and full revocation are still incomplete. | Tie region protection to per-frame storage-key programming, portal sharing rights, exact `IPTE` table origins, and ALB/TLB invalidation before authority or memory reuse. |
| DAT and TLB | `arch/s390x/mmu/mmu.cxxm`, `arch/s390x/mmu/mmu_types.cxxm`, `arch/s390x/mmu/mmu_tlb.cxxm` | Partial | Typed DAT levels, EDAT-aware large-page support, rollback, `PTLB`, `IDTE`, and `IPTE` exist; global ASCE and CPU-attachment invalidation semantics need stronger policy-level rules. | Use DGP state to track active ASCE users and define invalidation scope before ASCE reuse, VM protection changes, or portal revocation. |
| Scheduler and SMP | `zxfoundation/sched/sched.cxxm`, `arch/s390x/sched/*.cxxm`, `arch/s390x/cpu/smp*.cxxm` | Partial | Per-CPU run queues, EEVDF-style scheduling, AP tick timers, ASCE switching, and DGP admission exist, but work stealing is still flat and lacks SMT/core/NUMA/domain-placement tiers. | Add topology records, run-queue load sampling, SMT/core/node balancing tiers, and strand placement policy before claiming production scheduler maturity. |
| Synchronization | `zxfoundation/locking/qspinlock.cxxm`, `zxfoundation/locking/lock.cxxm`, `zxfoundation/locking/seqlock.cxxm`, `zxfoundation/rcu/rcu.cxxm`, `zxfoundation/sync/waitqueue.cxxm` | Partial | `zxfoundation.dgp.sync_contract` exists as typestate vocabulary, but no scheduler, domain, authority, RCU, waitqueue, or qspinlock call site imports it yet. | Convert the vocabulary into enforced call-site checks for IRQ state, run-queue mutation, ASCE switching, authority lookup, portal teardown, and domain destruction phases. |
| I/O and console | `arch/s390x/css/*.cxxm`, `drivers/console/*.cxxm`, `zxfoundation/gate/nc_io.cxxm` | Unsafe | CSS and console components exist, but I/O gate operations are currently stubs or permissive paths with no enforced DGP capability interpretation. | Treat I/O as a later service-domain wave; all I/O binding, IRQ ownership, and DMA mapping must become DGP capability-governed. |
| Support library and runtime | `lib/*.cxxm`, `zxfoundation/base/*.cxxm`, `zxfoundation/base/cxxrtse/*.cxxm`, `zxfoundation/scoms/*.cxxm` | Compliant | The freestanding support layer already provides `expected`, `kernel_error`, typed utility modules, typestate support, and generation-counted SCOMS object tables. | Reuse existing `lib::expected`, `lib::kernel_error`, typestate, and SCOMS tables; avoid hosted runtime dependencies, exceptions, RTTI, and dynamic standard-library containers. |

## Migration Map

### Wave 1: Clean-Core Policy Plane

Add `zxfoundation/dgp/` with four initial modules:

- `isolation_types.cxxm`: features, rights, state, policy, and audit/state records.
- `isolation.cxxm`: lifecycle entry points for DGP initialization, domain policy creation, activation, destruction, gate checks, and portal checks.
- `memory_contract.cxxm`: adapter-level memory policy types and validation helpers for VM, PMM, storage-key, DAT, and NUMA policy.
- `sync_contract.cxxm`: lock context, IRQ context, teardown context, and SMP ordering invariants.

This wave must be manifest-discovered through a new `zxfoundation/dgp/manifest.zxd`. It must not introduce hosted dependencies or C-style object control APIs.

### Wave 2: Domain and Memory Lifecycle Adoption

Move storage-key allocation out of `zxfoundation/domain/domain.cxxm` and into DGP isolation state. Domain creation must become:

1. Allocate SCOMS domain slot.
2. Request DGP isolation state.
3. Allocate or adopt ASCE root and storage-key state according to policy.
4. Allocate kernel stack and primary strand state.
5. Activate the DGP isolation state only after all rollback-sensitive resources are valid.

Partial failures must roll back in reverse order and must return `lib::expected<..., lib::kernel_error>` without leaking storage keys, ASCE tables, stacks, or domain slots.

### Wave 3: Gate and Portal Chokepoints

Replace fragmented local checks in `zxfoundation/gate/dispatch.cxxm` with DGP validation. Portal creation must call DGP validation before hardware resources are allocated. Bootstrap/admin authority must be temporary, explicit, and documented so early `sys/bootstrap` remains possible without creating a permanent bypass.

### Wave 4: Synchronization and Scheduler Enforcement

Define DGP lock contexts for IRQ-disabled spin acquisition, RCU-protected authority lookups, domain destruction, ASCE switch, TLB invalidation, and portal teardown. The scheduler must refuse to run strands whose domains are not in an active DGP isolation state.

### Wave 5: Full Hardware-First DGP Completion

Perform the later broad rewrites that are intentionally out of the first implementation wave:

- True NUMA allocator with node-local zones, fallback tiers, watermarks, migration, and topology failure domains.
- Full VM/storage-key integration for every user mapping and portal-shared range.
- Service-domain split for I/O, persistent storage, and trusted infrastructure services.
- I/O capability enforcement for CSS devices, IRQ binding, DMA windows, and console access.
- Kernel topology self-discovery that augments ZXFL records with `STSI`/SCLP-backed evidence.

### Wave 6: NUMA/VM/DAT Memory Overhaul Slice

The second implementation wave establishes the first real hardware-first memory contract beyond the clean DGP core:

- `zxfoundation/memory/numa_types.cxxm` and `zxfoundation/memory/numa.cxxm` record boot-time NUMA nodes, CPU-to-node hints, watermarks, placement requests, and deterministic single-node fallback when explicit NUMA records are absent.
- `zxfoundation/memory/pmm.cxxm` now owns buddy state as per-node zone sets, with node-local locks, node-local free/total counters, and node-indexed PCP caches keyed by the current CPU's local node.
- `zxfoundation/memory/pmm_types.cxxm` exposes node-aware allocation request/result records while preserving compatibility wrappers for existing `pmm::alloc()` and transaction users.
- `zxfoundation/dgp/memory_contract.cxxm` authorizes VM map, protect, and unmap operations and returns storage-key, NUMA-node, rights, generation, and invalidation metadata.
- `zxfoundation/memory/vm_types.cxxm` stores DGP memory metadata in every `vm_region`, and `zxfoundation/memory/vm.cxxm` preserves split/merge only when policy metadata remains compatible.
- `arch/s390x/mmu/mmu.cxxm` provides a page-table mapping entry point that can carry PMM placement policy for newly allocated DAT tables.
- `arch/s390x/mmu/mmu_tlb.cxxm` exposes a DGP-scope invalidation wrapper over `PTLB`, `IDTE`, and `IPTE` with conservative full-ASCE fallback until CPU-ASCE attachment tracking exists.

### Wave 7: Scheduler, Revocation, I/O, and Topology Hardening Slice

The third implementation wave completes the remaining documented hardening items from the NUMA/VM slice:

- `zxfoundation/sched/sched.cxxm` now rejects ordinary domains whose DGP isolation state is not active before run-queue admission, work stealing, or ASCE switch fallback.
- `arch/s390x/cpu/percpu_types.cxxm` records the ASCE, domain ID, and DGP lifecycle state currently attached to each CPU, creating the scheduler-side evidence needed for future non-global invalidation decisions.
- `zxfoundation/dgp/isolation_types.cxxm` and `zxfoundation/dgp/isolation.cxxm` now carry storage-key revocation phase, generation, and revocation-epoch metadata so key reuse is represented as a drainable DGP state transition rather than a raw bitmap release.
- `zxfoundation/portal/portal.cxxm` destroys portals in DGP order: revoke portal authority, purge ALB state, destroy the hardware `ALE`, then release the SCOMS slot.
- `zxfoundation/dgp/io_authority.cxxm` makes CSS/device operations pass through a DGP policy surface before existing generation-counted `sys::cap` token validation and low-level CSS handling.
- `zxfoundation/memory/numa.cxxm` exposes topology provenance predicates and `zxfoundation/init/main.cxxm` emits non-fatal boot-time notices when explicit NUMA evidence is absent.

### Wave 8: Portal Regression and ZXFL Topology Verification Repair

The follow-up repair wave corrects two regressions from the topology and portal hardening slice:

- `zxfoundation/dgp/isolation.cxxm` no longer rejects zero-based portal ranges. The bootstrap test intentionally creates a portal over `[0, PAGE_SIZE)`, and a zero virtual address is a valid target-domain range when the VM contract and caller policy approve it.
- `zxfoundation/init/main.cxxm` now performs NUMA startup through one helper call, `initialize_numa_topology()`, keeping optional topology handling out of the main boot sequence.
- NUMA initialization remains non-fatal. Missing explicit NUMA records keep the allocator on deterministic bootstrap fallback instead of halting the nucleus.
- `arch/s390x/init/zxfl/stage2/entry.c` was rechecked together with `arch/s390x/init/zxfl/common/system.c`: ZXFL already performs STSI CPU topology discovery, SIGP CPU validation, SCLP memory sizing, and protocol handoff before DAT entry.
- The loader-side memory map splitter now assigns memory chunks using the discovered NUMA node count instead of rotating through a fixed four-node modulo.

#### NUMA Topology Model

The current NUMA model is intentionally deterministic and boot-time immutable:

| Record | Owner | Current Behavior | Remaining Hardening |
| --- | --- | --- | --- |
| Node table | `zxfoundation.memory.numa` | Up to 16 nodes, derived from ZXFL usable-memory and CPU records. | Replace heuristic ZXFL topology with direct `STSI`/SCLP evidence and drawer/failure-domain validation. |
| CPU placement | `g_cpu_to_node` in NUMA core | Logical CPU records map to a preferred node, falling back to node 0 when evidence is missing. | Drain or rehome PCP caches before future CPU drawer reassignment or hotplug. |
| Allocation policy | `numa::allocation_policy` | Supports local-first, strict-local, interleave placeholder, and emergency-any fallback modes. | Add distance tables and real interleave sequencing once topology provenance is stronger. |
| Watermarks | `node_topology` | Low/high watermarks are derived from boot free-page estimates. | Feed live PMM counters and reclaim pressure into watermarks. |

#### Topology Provenance Notes

The current provenance model treats ZXFL as the loader-owned boot authority and leaves room for kernel self-discovery to add direct machine topology evidence later:

| Evidence | Current Treatment | DGP Meaning |
| --- | --- | --- |
| ZXFL usable-memory node tags | Accepted as loader-provided boot evidence and recorded as `zxfl_memory_map`. | Sufficient for deterministic node-owned allocation and initial failure-domain hints. |
| ZXFL CPU drawer/node hints | Accepted as loader-provided CPU placement evidence and recorded as `zxfl_cpu_map` when no memory-map evidence is stronger. | Sufficient to seed local-node allocation and PCP indexing. |
| Synthetic boot node | Accepted as deterministic optional fallback and reported during boot. | Safe for single-node operation when no explicit NUMA evidence is present. |

The IBM PoP and machine facilities remain the source for future kernel self-discovery: a later topology wave should combine ZXFL records with direct `STSI`/SCLP evidence for drawer, book, CPU, and memory failure-domain relationships.

#### ZXFL IPL Loader Verification Notes

| Loader Area | Verification Result | Follow-up |
| --- | --- | --- |
| STSI CPU topology | `zxfl_system_detect()` queries `STSI(15, 1, 2)`, parses container and core TLEs, translates CPU type values, and normalizes topology IDs. | Broaden parser testing against non-Hercules nesting levels in the next loader test wave. |
| CPU presence | Each discovered CPU is checked with `SIGP Sense`; non-operational CPUs are skipped. | Keep this check before AP release in kernel-side SMP admission too. |
| Memory sizing | Stage2 uses SCLP memory sizing first and falls back to manual probing only when SCLP is unavailable. | Add a structured provenance flag for SCLP-vs-probe memory records. |
| Memory NUMA tags | Stage2 now derives memory chunk node IDs from the discovered node count rather than a fixed modulo-four assignment. | Replace chunk heuristics with explicit memory topology records if the platform exposes them. |
| Protocol handoff | Stage2 updates protocol pointers to HHDM addresses before jumping into the kernel DAT environment. | Keep kernel protocol validation strict for magic, binding token, stack canary, and checksum table. |

### Wave 9: IDTE Invalidation Crash Repair

The boot log in `run.log` showed a specification exception at `IDTE` during `vm_protect()` in the `mem::protect -> RO` test. The failing instruction was emitted as `IDTE 1,0,3`, leaving the option mask in the operand position that the PoP constrains for `IDTE` table/ASCE operand bits. The PoP specification-exception list states that `IDTE` raises a specification exception when bits 44-51 of general register `R2` are not zero. The Linux s390 reference pattern for ASCE flushing uses the operand order `idte 0,asce,opt`.

The repair changes `arch/s390x/mmu/mmu_tlb.cxxm` so the DGP address-space invalidation wrapper emits `idte 0,%[asce],%[opt]`, matching the operand order used by the s390 reference idea and avoiding the swapped-operand specification exception.

Runtime validation after the `IDTE` fix advanced past `mem::protect -> RO` and revealed a separate SVC-frame restore issue at `LFPC` during `sys::time_yield`. The SVC trap path was invoking scheduler preemption while the saved SVC interrupt frame was still live above the ABI call frame. `trap_svc_entry` now restores the synchronous SVC frame directly; asynchronous external/timer paths remain responsible for later preemption.

#### PMM Node-Owned Zone Audit

| Area | Completed State | Remaining Risk |
| --- | --- | --- |
| Buddy ownership | Each NUMA node owns independent `dma31` and `normal` buddy zones and locks. | Boot memory regions spanning node evidence boundaries still rely on ZXFL-provided node tags. |
| PCP caches | Per-CPU caches are indexed by node, zone, and order, so order-zero hot paths prefer local memory. | CPU migration/hotplug requires explicit PCP drain/rehome before local-node identity changes. |
| Rollback/free | Transaction rollback and `pmm::free()` return frames to the node recorded in `frame_desc::numa_node`. | Hardware memory offlining still needs a formal node-drain protocol. |
| Fallback | `local_first`, `strict_local`, and emergency fallback are explicit; strict-local exhaustion returns an error. | Interleave is represented as a policy enum but still uses deterministic fallback. |

#### VM, Storage-Key, and DAT Integration Notes

VM operations now submit full DGP memory-contract requests instead of range-only checks. Region metadata records the selected storage key, NUMA node, DGP generation, required rights, sharing scope, access mask, and invalidation scope. This prevents RB-tree region merging across policy-incompatible boundaries and gives later storage-key revocation and portal sharing work a concrete authority record.

The current storage-key value is conservative and policy-owned but not yet a full per-frame revocation engine. The next storage-key wave must connect region metadata, frame descriptors, SKey instructions, and portal teardown ordering so a key cannot be reused while stale DAT, ALB, or frame references may still exist.

#### Invalidation Policy Table

| DGP Scope | Current Wrapper Behavior | Intended Future Refinement |
| --- | --- | --- |
| `none` | No hardware invalidation. | Preserve when metadata-only changes do not affect DAT/ALB state. |
| `local_cpu` | `PTLB`. | Use for CPU-private kernel mappings only when ASCE attachment proves locality. |
| `single_page` | `IPTE` when page-table physical address is supplied; otherwise local fallback. | Pass exact page-table origin from DAT walks for precise invalidation. |
| `address_space` | `IDTE` full ASCE purge. | Limit to CPUs attached to the ASCE once scheduler tracking exists. |
| `global` | Conservative full ASCE purge when possible. | Add broadcast/attachment tracking for all active CPUs and guest/current ASCE state. |
| `alb_global` | Conservative full ASCE purge as a bridge. | Add explicit ALB invalidation for portal-shared teardown and ALET reuse. |

#### Scheduler and Revocation Hardening Notes

| Area | Completed State | Remaining Risk |
| --- | --- | --- |
| Scheduler admission | Ordinary domains must pass `isolation_validate_scheduler_admission()` before run-queue admission and user entry. | Idle/nucleus domains retain a narrow explicit exception because they are not user-visible DGP domains. |
| CPU-ASCE attachment | Per-CPU data records attached ASCE, domain ID, and DGP state at ASCE load points. | Future TLB policy must add cross-CPU ASCE attachment enumeration before replacing conservative global fallback. |
| Storage-key revocation | DGP state records key generation, revocation epoch, active/draining/reusable phase, and destruction-time draining. | Hardware storage-key reset and per-frame key revocation still require a dedicated PoP-checked storage-key engine. |
| Portal teardown | DGP portal authority is decremented before `purge_alb()` and `ALE` destruction. | Remote CPU ALB shootdown remains conservative and must become explicit before portal sharing is considered complete. |

#### I/O Capability and Service-Domain Preparation

The I/O surface now has a DGP policy contract before existing CSS and capability code:

| Operation | DGP Check | Legacy Enforcement Preserved |
| --- | --- | --- |
| Bind | Caller domain must hold DGP I/O/admin authority before an I/O binding token is created. | `sys::io` still validates CSS subchannel existence and allocates DMA state. |
| Unbind | Token must be an I/O-device token with destroy rights before device release. | `sys::io` still validates object generation and frees DMA memory. |
| Submit | Token must be an I/O-device token with execute rights before ORB copy/submission. | `cap_validate<io_binding>()` still validates generation and object state. |
| IRQ bind | Token must be an I/O-device token with write rights before IRQ gate association. | CSS subchannel owner metadata is still updated by `sys::io`. |

The service-domain split remains a future architectural boundary. This slice creates the policy seam without moving CSS, console, or storage drivers out of the nucleus yet.

## Hardware and Reference Check Notes

- **DAT/ASCE**: Changes touching ASCE construction, table origin, table-type control, or region/segment/page invalidation must be checked against the IBM z/Architecture Principles of Operation DAT and control-register chapters. Linux `arch/s390/include/asm/pgtable.h` and `arch/s390/include/asm/mmu.h` provide reference ideas for ASCE/context state only.
- **Storage keys**: Changes to access-control key allocation, fetch protection, reference/change tracking, or key reuse must be checked against the PoP storage-key and protection chapters. Key exhaustion must deny or group explicitly; silent reuse is forbidden.
- **Program Call**: Gate entry and linkage-table work must be checked against PoP `PC`, linkage-table-entry, and entry-table-entry semantics. The DGP layer must not treat PC as a plain software callback.
- **Access registers and ASTE/ALE**: Portal work must be checked against PoP access-register translation, `CR5`, `CR7`, `ASTE`, `ALE`, and ALB invalidation semantics. Portal teardown now revokes DGP authority and purges ALB before `ALE` reuse; remote CPU ALB shootdown remains a future hardening item.
- **TLB invalidation**: `PTLB`, `IDTE`, and `IPTE` scope must be checked against PoP invalidation semantics. Linux `arch/s390/include/asm/tlbflush.h` provides the key reference idea that ASCE invalidation must account for attached CPUs and guest/current context, not only a local flush.
- **SMP synchronization**: Locking and scheduler changes must preserve IRQ and preemption assumptions around run queues, DGP state transitions, and MMU/portal invalidation. Per-CPU ASCE attachment metadata now exists, but global invalidation remains conservative until all attached CPUs can be enumerated safely.

## Runtime Edge Cases Requiring First-Class Tests or Boot Checks

- Storage-key exhaustion while multiple domains are created concurrently.
- ASCE root allocation failure after a domain slot is allocated.
- Kernel-stack allocation failure after DGP state and root table state exist.
- Concurrent domain destruction while a gate entry is being validated.
- Concurrent portal teardown while an ALET or ALB translation may remain usable on another CPU.
- VM protection downgrade while another CPU is executing under the same ASCE.
- AP scheduling before DGP policy initialization completes.
- NUMA topology missing or inconsistent with allocator policy.

## Synchronization Contract Audit

The first clean-core implementation added `zxfoundation.dgp.sync_contract` as the policy vocabulary for lock context, IRQ state, authority lookup, and teardown ordering. The first enforcement slice now imports it in scheduler, domain, gate dispatch, and portal paths, while RCU, waitqueue, seqlock, qspinlock, and full authority object lifetime remain future conversion targets. The following subsystem-specific findings must govern the remaining migration waves:

| Area | Current Contract | DGP Requirement | Remaining Risk |
| --- | --- | --- | --- |
| `zxfoundation/locking/qspinlock.cxxm` | Uses queued spinning and IRQ-aware guard patterns suitable for SMP contention. | DGP state transitions that protect domain, gate, portal, and VM authority must use IRQ-disabled or explicitly documented lock contexts. | Storage-key and authority registries still need a real lock or atomic policy before AP-visible concurrent create/destroy is allowed. |
| `zxfoundation/locking/seqlock.cxxm` | Provides retry-based read-side snapshots for data where writers serialize updates. | It may be used for read-mostly DGP metadata only when stale reads cannot grant authority after generation/state changes. | Misuse for portal or capability state could permit transient stale authority unless paired with lifecycle checks. |
| `zxfoundation/rcu/rcu.cxxm` | Provides grace-period infrastructure for deferred reclamation. | Gate and portal lookup paths should become RCU read-side sections before object memory is reclaimed. | Current DGP skeleton validates structurally; it does not yet pin generation-counted objects across dispatch. |
| `zxfoundation/sync/waitqueue.cxxm` | Supplies blocking coordination for sleepers and wakeups. | Waitqueues must not be used while holding DGP spin/IRQ contexts or inside gate dispatch authorization. | Domain teardown that waits for gates/portals needs a strict phase split between deny-new-entry and wait/drain. |
| `zxfoundation/sched/sched.cxxm` | Maintains per-CPU run queues, DGP admission, work stealing, and ASCE switch paths. | Run-queue mutation and ASCE switch must validate IRQ-disabled scheduler context through `sync_contract`. | Scheduler now checks the IRQ-disabled contract, but work stealing is still topology-blind and needs placement-aware balancing. |
| `arch/s390x/mmu/mmu_tlb.cxxm` | Provides `PTLB`, corrected `IDTE`, and `IPTE` invalidation helpers. | VM, ASCE reuse, storage-key changes, and portal revocation must select local, ASCE, or global invalidation scope from DGP policy. | CPU-ASCE attachment metadata exists, but invalidation still lacks safe cross-CPU enumeration and explicit ALB shootdown. |

### Lock-Context Notes

- Gate dispatch authorization must remain non-blocking. It may validate capability generations, DGP state, and PC linkage metadata, but it must not allocate memory or wait on a queue.
- Portal creation may allocate `ASTE` and `ALE` resources only after the DGP policy validation succeeds. Portal destruction must deny reuse before stale `ALET` or `ALB` translations can survive on another CPU.
- Domain destruction must proceed in ordered phases: deny new gate entries, drain active gates, drain portals, release VM/DAT state, release storage-key identity, and finally release the SCOMS slot.
- VM protection downgrade and unmap paths must invalidate translations before any physical page, storage key, or portal authority can be reused.
- RCU-protected authority lookup must pair object state and generation checks; a pointer that remains live is not sufficient if the DGP lifecycle state has moved to draining or destroyed.

### Synchronization Enforcement Slice

The first real `sync_contract` call-site migration is complete:

- `zxfoundation/dgp/sync_contract.cxxm` now captures a per-CPU snapshot from lowcore/per-CPU state, including CPU ID, lock depth, interrupt mask state, and RCU read-side state.
- `zxfoundation/sched/sched.cxxm` validates the IRQ-disabled contract around run-queue enqueue/dequeue, `schedule()`, scheduler ticks, and ASCE switch points.
- `zxfoundation/domain/domain.cxxm` validates forward-only teardown phase progression from deny-new-entry through memory release and identity release.
- `zxfoundation/gate/dispatch.cxxm` and `zxfoundation/portal/portal.cxxm` validate authority lookup context before DGP gate/portal policy checks.
- `core.zxfoundation.nucleus` builds successfully after the slice at build number `668`.

The slice intentionally does not claim full synchronization maturity. Remaining work includes RCU pinning for generation-counted authority objects, waitqueue no-blocking checks during teardown, qspinlock lock-order validation, and topology-aware scheduler balancing.

### Topology-Aware Scheduler Slice

The first scheduler-topology slice is complete:

- `zxfoundation/sched/sched_topology_types.cxxm` defines CPU relation tiers, CPU scheduling state, per-CPU topology/load records, and DGP strand-placement policy.
- `zxfoundation/sched/sched_topology.cxxm` owns scheduler topology records, refreshes them from lowcore/per-CPU evidence, and conservatively treats unknown SMT/core data as one logical core per CPU instead of inventing sibling topology.
- `zxfoundation/sched/sched_balance.cxxm` scores steal sources by topology tier and validates that selected strands may run on the target CPU under their current DGP placement policy.
- `zxfoundation/sched/sched.cxxm` no longer picks the busiest remote run queue by flat scan alone; it now tries SMT/core, same-node, and remote-node tiers in order, then validates the stolen strand before migration.
- `core.zxfoundation.nucleus` builds successfully after the slice at build number `669`.

This is still not Linux-class scheduling. The scheduler now has a topology policy seam, but it still lacks real hardware SMT sibling IDs, core-distance tables, NUMA pressure feedback from PMM, load averages, CPU hotplug drain/rehome, and executor-driven multi-domain stress coverage.

### Classed Scheduler and Topology Evidence Slice

The first classed-scheduler surface and the hardened CPU topology evidence contract are now in place:

- `zxfoundation/sched/sched_policy_types.cxxm` defines the strict class order `deadline > rt > fair > idle`, RT/deadline policy fields, placement masks, SMT-isolation flags, admission results, and explicit evidence quality values.
- `zxfoundation/sched/sched_class.cxxm` dispatches validate/enqueue/dequeue/select/tick/preempt hooks through a class-neutral surface while preserving existing fair behavior as the only admitted non-idle backend for this slice.
- `zxfoundation/sched/sched.cxxm` routes enqueue, dequeue, pick-next, tick, and migration requeue paths through the class dispatcher; RT and deadline requests are denied with `not_implemented` until their DGP capability and bandwidth controls exist.
- `arch/s390x/init/zxfl/common/system.c`, the C protocol header, and `arch/s390x/init/zxfl/protocol.cxxm` keep CPU-map layout stable while replacing opaque padding with `core_id`, `smt_id`, `topology_evidence`, and `capacity` fields.
- `arch/s390x/cpu/lowcore.cxxm`, `percpu_types.cxxm`, `percpu.cxxm`, and `smp.cxxm` carry loader CPU evidence into per-CPU state for both BSP and APs, including single-CPU boots before AP startup returns early.
- `zxfoundation/sched/sched_topology_types.cxxm` and `sched_topology.cxxm` now record class-specific load counters, balancing interval metadata, capacity, raw evidence flags, and separate CPU/core/SMT/capacity evidence quality.
- `zxfoundation/sched/sched_balance.cxxm` refuses isolation-sensitive SMT placement when sibling evidence is degraded and includes CPU capacity in steal-source scoring.
- `core.zxfoundation.nucleus` builds successfully after the class surface at build number `682` and after the topology evidence slice at build number `684`.

Fallback modes are explicit degraded evidence, not complete topology. `STSI(15,1,2)` evidence exported by ZXFL is marked as loader-STSI evidence; SIGP-only fallback is marked degraded. Degraded core/SMT records keep one logical CPU per core group so the scheduler cannot accidentally claim SMT-sibling knowledge that the loader or kernel did not prove. Kernel-side STSI verification still remains a later refinement before topology can be called production-grade.

### User RT Admission Slice

The first full-user-RT safety gate is present, with the scheduler denying unadmitted RT work before it can reach a run queue:

- `zxfoundation/domain/strand_types.cxxm` stores per-strand scheduler policy, admission results, and RT accounting metadata after the assembly-critical context-switch fields.
- `zxfoundation/domain/domain_types.cxxm` stores per-domain RT capability token, RT period, RT budget, reserved runtime, and admitted CPU/NUMA masks.
- `zxfoundation/dgp/isolation.cxxm` validates scheduler policy against active DGP state, RT authority, priority range, runtime/period invariants, and placement-mask bounds.
- `zxfoundation/sched/sched_rt.cxxm` provides fixed-priority RT admission, FIFO queue insertion, highest-priority selection, conservative tick accounting, runtime reservation release, and preemption marking.
- `zxfoundation/sched/sched_class.cxxm` routes class dispatch through RT before fair and refuses RT enqueue unless the strand carries an RT admission record.
- `zxfoundation/sched/sched.cxxm` exports `strand_admit_policy()` so user-visible RT requests must complete DGP/class admission before any run-queue insertion.
- `core.zxfoundation.nucleus` builds successfully after the RT admission slice at build number `690`.

This is not a finished production RT scheduler. It has authority checks, denial paths, and per-domain bandwidth reservation, but it still lacks high-resolution period replenishment, per-CPU RT throttling timers, priority inheritance, user syscall plumbing, adversarial multi-domain stress, and a proven fair-class rescue policy under sustained RT load.

### Deadline and Scheduler Stress Cleanup Slice

The first conservative deadline backend is present, and the unused scheduler stress hook has been removed from build discovery:

- `zxfoundation/sched/sched_deadline.cxxm` provides DGP-authorized conservative EDF admission, runtime reservation, deadline queue insertion, earliest-deadline selection, tick accounting, and preemption marking.
- `zxfoundation/domain/domain_types.cxxm` adds a per-CPU deadline queue plus per-domain deadline period/budget/reservation metadata.
- `zxfoundation/domain/strand_types.cxxm` stores conservative deadline runtime and absolute-deadline keys alongside RT accounting metadata.
- `zxfoundation/dgp/isolation.cxxm` rejects invalid deadline tuples before scheduler admission: zero values, `runtime > deadline`, `deadline > period`, invalid masks, or missing authority.
- `zxfoundation/sched/sched_class.cxxm` selects classes in the required strict order: `deadline`, then `rt`, then `fair`, then `idle`.
- `zxfoundation/sched/sched_stress.cxxm` remains deliberately unreferenced and is no longer listed in `zxfoundation/sched/manifest.zxd`; runtime coverage is expected to come through the disposable mock user binary instead of a future-only in-kernel stress module.
- `core.zxfoundation.nucleus` builds successfully after the deadline/stress slice at build number `692`; the `dasd` image target builds successfully at build number `693`.

This is intentionally conservative. The deadline class is safe to deny or enqueue only after admission, but it does not yet implement CBS-style replenishment, overload migration, precise time accounting, or mock-user stress that actually creates and tears down adversarial multi-domain workloads.

### Multi-Strand Executor Slice

The first executor-style secondary-strand slice is complete:

- `zxfoundation/domain/strand_types.cxxm` defines `strand_entry_fn` and `strand_create_request` so callers can express owner domain, entry function, argument, stack order, CPU placement, and enqueue policy as one request.
- `zxfoundation/domain/domain.cxxm` enforces `MAX_STRANDS_PER_DOMAIN`, rejects secondary-strand creation for non-active domains, and exposes `strand_destroy()` for drained secondary strands.
- `zxfoundation/sched/sched.cxxm` exposes `strand_spawn()` to validate DGP admission, allocate a secondary strand, initialize its first-entry context through `sched_domain`, optionally enqueue it, and roll back on late admission failure.
- `core.zxfoundation.nucleus` builds successfully after the slice at build number `671`.

This is an executor foundation, not a full production thread subsystem. Missing pieces include user-facing entry ABI design, blocked-strand teardown drains, waitqueue integration, per-strand placement overrides beyond the current request fields, and boot/runtime tests that prove many strands across many domains actually balance correctly.

### DAT/HHDM Page-Table Initialization Hardening Slice

The rare post-CSS/DASD `Region-second-translation exception` in `arch::s390x::mmu::_internal::init_table()` is addressed by turning the implicit loader/direct-map assumption into an explicit checked contract:

- `arch/s390x/init/zxfl/common/mmu.c` now exports `hhdm_phys_coverage_end`, the exact exclusive physical end mapped by the loader-built HHDM DAT.
- `arch/s390x/init/zxfl/protocol.cxxm` and the C ZXFL protocol header name the existing reserved memory-map slot as `hhdm_phys_coverage_end`, preserving the protocol layout while making the contract explicit.
- `zxfoundation/init/main.cxxm` verifies the HHDM coverage is non-zero and covers both the kernel image and loader page-table pool before PMM/MMU initialization.
- `zxfoundation/memory/hhdm.cxxm` records the active coverage end and exposes `phys_range_is_mapped()` so arithmetic direct-map translation is no longer treated as proof of DAT reachability.
- `zxfoundation/memory/pmm.cxxm` validates `purpose::page_table` allocations for order/alignment, descriptor owner, node/zone consistency, usable range, and HHDM coverage before returning them to the MMU.
- `arch/s390x/mmu/mmu.cxxm` makes `init_table()` return `expected`, checks HHDM reachability before writing, uses a volatile scalar store loop for DAT table initialization, and propagates initialization errors through existing rollback/free paths.
- `core.zxfoundation.nucleus` and `dasd` build successfully after the slice at build numbers `680` and `681` respectively.

This fixes the unsafe destructive write primitive and prevents silent reuse of unmapped page-table frames, but repeated Hercules DASD boots are still required to prove the rare crash signature no longer recurs at runtime.

## Remaining Refactor Waves

- **NUMA allocator hardening**: node-local buddy zones, PCP caches, watermarks, and fallback tiers exist; remaining work is distance-aware balancing, migration policy, memory offline/drain, and hardware drawer failure-domain recovery.
- **Full VM/storage-key completion**: `vm_region` carries DGP policy metadata, but DAT permission updates, hardware storage-key programming/revocation, portal sharing rights, and required TLB/ALB invalidation still need end-to-end enforcement.
- **Topology-aware scheduler and executors**: class dispatch, topology evidence records, topology-aware steal tiers, RT/deadline admission backends, and secondary-strand creation exist, but there is no kernel-side STSI verifier, no mock-user scenario that creates adversarial workloads, no blocked-strand teardown drain, and no proven multi-domain load-balancing scenario.
- **Service-domain split**: move I/O, persistent storage, console, and trusted infrastructure services behind isolated domains once nucleus DGP enforcement is complete.
- **I/O capability enforcement**: govern CSS device binding, subchannel ownership, IRQ routing, DMA windows, and console access through generation-counted DGP capabilities.
- **Topology self-discovery hardening**: augment ZXFL NUMA evidence with `STSI`/SCLP-derived topology and failure-domain records while preserving the pure-C loader boundary.
- **Runtime boot stress**: repeatedly exercise DASD/CSS boot, multi-domain strand creation, local/remote NUMA fallback, AP balancing, ASCE attachment tracking, and teardown while preserving logs for regression comparison.

## Implementation Constraints for All Waves

- New C++ code remains C++23 freestanding modules only.
- Public APIs use `/// @brief`, `/// @param`, `/// @return`, `/// @note`, and `/// @warning` where applicable.
- All functions and methods use trailing return types.
- Error paths use `lib::expected` and `lib::kernel_error` wherever possible.
- Kernel objects use member-function semantics and typestate-style ownership; no new Linux/C-style object-control API surface is introduced.
- Manifests must stay synchronized with source additions through the `zx-discovery` DSL.