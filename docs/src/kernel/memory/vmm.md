# Virtual Memory Manager (VMM)

**Document Revision:** 26h1.0  
**Source:** `zxfoundation/memory/vmm.c`

---

## 1. Address Space Regions

| Region | Base | Purpose |
|--------|------|---------|
| HHDM | `0xFFFF800000000000` | Linear physical memory map (built by loader, read-only to VMM) |
| vmalloc | `0xFFFFC00000000000` | Dynamically mapped kernel memory |

---

## 2. Virtual Memory Areas (VMAs)

Each allocated virtual range is described by a `vm_area_t`:

| Field | Description |
|-------|-------------|
| `va_start` | Start of virtual range (page-aligned) |
| `va_end` | End of virtual range (exclusive) |
| `flags` | `VM_READ`, `VM_WRITE`, `VM_EXEC` |
| `rb_node` | Red-Black Tree node for $O(\log n)$ lookup |

VMAs are indexed in a Red-Black Tree (`rbtree.h`). A one-entry MRU cache in `vm_space_t` provides an $O(1)$ fast path for sequential access patterns.

---

## 3. vmalloc

`vmm_alloc(size, flags)` reserves a contiguous virtual range in the vmalloc region and maps it with PMM-allocated frames:

```
vmm_alloc(size, flags)
  │
  ├─ Round size up to page boundary
  ├─ Bump-allocate virtual range from vmalloc region
  ├─ Insert VMA into red-black tree
  ├─ For each page in range:
  │    ├─ pmm_alloc_page(flags)
  │    └─ mmu_map_page(kernel_pgtbl, va, pa, prot)
  └─ Return va_start
```

Frames backing a vmalloc range are not required to be physically contiguous.

---

## 4. Large-Object Heap (`kheap`)

For allocations larger than 8 KB, `kheap_alloc` calls `vmm_alloc` to back the range with PMM frames. A 64-bit `HEAP_MAGIC` canary guards the allocation header against buffer underflows.

---

## 5. MMU Integration

The VMM calls `mmu_map_page` (4 KB), `mmu_map_large_page` (1 MB, if EDAT-1 available), or `mmu_map_huge_page` (2 GB, if EDAT-2 available) to install PTEs. TLB coherency is handled automatically by the `IPTE` instruction — no software IPI is required.
