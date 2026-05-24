# Physical Memory Manager (PMM)

**Document Revision:** 26h1.0  
**Source:** `zxfoundation/memory/pmm.c`

---

## 1. Zones

| Zone | Physical range | Purpose |
|------|---------------|---------|
| `ZONE_DMA` | `[0, 16 MB)` | Channel I/O buffers (31-bit CDA constraint) |
| `ZONE_NORMAL` | `[16 MB, RAM limit)` | General kernel allocations |

Allocations without `ZX_GFP_DMA` are served from `ZONE_NORMAL` first. If `ZONE_NORMAL` is exhausted and `ZX_GFP_DMA_FALLBACK` is set, the PMM falls back to `ZONE_DMA`.

---

## 2. Buddy Allocator

Free physical frames are managed in a buddy system. Block sizes are powers of two, from order 0 (4 KB) to order 10 (4 MB). Each order has a free list of blocks.

**Allocation** — walk the free list at the requested order. If empty, split a block from the next higher order. Repeat until a block is found or all orders are exhausted.

**Deallocation** — compute the buddy PFN (`pfn ^ (1 << order)`). If the buddy is free at the same order, coalesce and recurse upward.

Free list links use PFN-based intrusive fields (`buddy_next`) rather than virtual pointers, ensuring correctness across HHDM translations.

---

## 3. Page Descriptor (`zx_page_t`)

Each physical frame has a 32-byte descriptor. The descriptor array is mapped contiguously in the HHDM. 32 bytes places 128 descriptors per 4 KB frame — a deliberate cache-line optimization.

| Field | Description |
|-------|-------------|
| `refcount` | Atomic reference count; 0 = free |
| `order` | Current buddy order of this block |
| `flags` | Zone membership, compound page markers |
| `buddy_next` | PFN of next free block in the buddy list |

---

## 4. GFP Flags

| Flag | Meaning |
|------|---------|
| `ZX_GFP_NORMAL` | Standard allocation from `ZONE_NORMAL` |
| `ZX_GFP_DMA` | Must allocate from `ZONE_DMA` |
| `ZX_GFP_DMA_FALLBACK` | Try `ZONE_NORMAL`, fall back to `ZONE_DMA` |
| `ZX_GFP_ZERO` | Zero-fill the allocated pages |

---

## 5. SMP Safety & Per-CPU Lists (PCP)

Each zone has a dedicated ticket spinlock. To reduce contention, order-0 pages are cached in **Per-CPU Lists (PCP)**.
* **Allocation**: CPUs pull from local PCP first without locking (IRQs disabled).
* **Drain**: Global operations (like `pmm_reserve_range`) trigger a **global PCP drainage** via SIGP Emergency Signals (IPI) to all other CPUs. This ensures no CPU holds a 'stale' cached page that should be reserved.

---

## 6. HHDM Side Reinforcement

The Direct Physical Mapping (HHDM) is validated during initialization:
1. **Validation**: `pmm_verify_hhdm()` checks translation consistency against the loader's memory map. It verifies that every usable physical page is correctly mapped to its HHDM virtual counterpart.
2. **EDAT Compliance**: Verifies Enhanced-DAT (EDAT-1/2) 1 MB and 2 GB page usage to optimize memory performance and reduce TLB pressure.
3. **Consistency**: The loader must ensure that the mapping covers the entire physical memory range described in the boot protocol, rounding up to the nearest Region-3 or Segment boundary as required by the z/Architecture DAT structure.

---

## 7. Initialization

`pmm_init(boot)` is called once during early init:

1. Walk `boot->mem_map[]` and register all `ZXFL_MEM_USABLE` regions.
2. Mark reserved ranges via **Surgical Reservation**:
   * **Lowcore/Artifacts**: `[0, 1 MB)` is always reserved to protect lowcore and loader leftovers.
   * **Kernel Image**: `[kernel_phys_start, kernel_phys_end)` is marked as critical.
   * **Page Table Pool**: `[kernel_phys_end, pgtbl_pool_end)` is reserved to protect active DAT tables.
   * **PMM Metadata**: The `zx_mem_map` descriptor array itself.
3. Insert all non-reserved `USABLE` frames into the buddy free lists.

> [!IMPORTANT]
> **Surgical Reservation** prevents "Zone Exhaustion" bugs where a large bootloader page pool could otherwise wipe out all available frames in `ZONE_DMA` (under 16 MB).
