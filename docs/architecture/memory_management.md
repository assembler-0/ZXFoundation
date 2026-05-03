# ZXFoundation Memory Management Architecture

## 1. Overview
The ZXFoundation kernel implements an industrial-grade, SMP-aware, highly concurrent 5-level paging memory architecture optimized for z/Architecture (s390x).

The architecture is layered hierarchically:
1.  **PMM (Physical Memory Manager):** Zone-aware buddy allocator managing frames via `zx_page_t`.
2.  **Slab & Kmalloc:** Per-CPU lockless-magazine object allocators.
3.  **VMM (Virtual Memory Manager):** RB-tree-based VMA management and `vmalloc` bump allocation.
4.  **MMU:** Hardware DAT abstraction utilizing IPTE for SMP TLB invalidation and EDAT-1 large pages.

---

## 2. PMM (Physical Memory Manager)
The physical memory is tracked using a flat `zx_mem_map` array mapped contiguously in the HHDM.
*   **Zones:**
    *   `ZONE_DMA`: [0 – 16 MB) for legacy channel I/O.
    *   `ZONE_NORMAL`: [16 MB – max physical RAM).
*   **Buddy Allocator:**
    *   Supports block orders 0 (4 KB) to 10 (4 MB).
    *   Uses intrusive, PFN-based single-linked lists (`buddy_next`) to survive HHDM translations.
*   **Page Descriptor (`zx_page_t`):**
    *   Carefully packed to exactly 32 bytes to ensure cache-line efficiency (128 descriptors per 4 KB frame).
    *   Contains reference counting (`atomic_t`), buddy order tracking, and UAF-poison bits.
*   **SMP Safety:** Per-zone ticket spinlocks with `irqsave` / `irqrestore` cycles prevent deadlocks during concurrent multi-processor allocation and IRQ re-entrancy.

---

## 3. Virtual Memory Manager (VMM) & Heap
The VMM abstracts the address space layout, separating the Higher-Half Direct Map (HHDM) from the dynamically allocated `vmalloc` region.
*   **Address Space Layout:**
    *   `0xFFFF800000000000`: HHDM Start.
    *   `0xFFFFC00000000000`: Vmalloc / Ioremap Start.
*   **VMA Indexing (RB-Tree):**
    *   `vm_area_t` structures are organized in a lock-free, iterative Red-Black Tree (`rbtree.h`), yielding $O(\log n)$ insertion/lookup performance.
    *   A 1-entry MRU (Most Recently Used) cache within `vm_space_t` optimizes sequential access patterns down to $O(1)$ fast-paths.
*   **Large-Object Heap (`kheap`):**
    *   For allocations larger than 8 KB, the `kheap_alloc` system relies on `vmm_alloc()` to map contiguous virtual memory backed by discontiguous PMM frames.
    *   Metadata is protected via a 64-bit `HEAP_MAGIC` canary to catch buffer-under-writes.

---

## 4. Hardware MMU Integration (s390x)
*   **5-Level Paging:** Standard 5-level z/Architecture Dynamic Address Translation (DAT).
*   **EDAT-1 Support:** Transparent integration with Facility 78 (Enhanced DAT 1), capable of 1 MB Large Page (`FC=1`) segment table entries.
*   **TLB Shootdowns:** Completely hardware-offloaded. The `IPTE` instruction atomically clears PTEs and issues hardware-broadcast TLB purges across all CPUs.

---

## 5. Subsystem Integration & Status

### 5.1 Dynamically Sized Pages
The lower-level MMU (`mmu_map_large_page`) and PMM (`pmm_alloc_pages`) strictly support allocating and mapping $2^n$ large blocks and 1 MB compound folios. However, the upper-level `vmm_insert_vma` and `kheap` currently fallback to looping over 4 KB pages (`mmu_map_page`) to ensure minimal internal fragmentation. Opportunistic large-page promotion inside the VMM will be enabled in a future refinement.

### 5.2 KObject Integration
*   `zx_page_t` **is explicitly excluded** from the `kobject` inheritance tree. Embedding a full kobject into the 32-byte page descriptor would cause an unacceptable memory overhead. Instead, it uses a lightweight `atomic_t` for raw reference counting.
*   `vm_space_t` currently operates as a static kernel singleton (`kernel_vm_space`). When user processes are introduced, dynamically allocated `vm_space_t` instances will inherit `kobject` for standard reference-counted lifecycle tracking.
