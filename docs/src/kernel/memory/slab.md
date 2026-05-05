# Slab Allocator & kmalloc

**Document Revision:** 26h1.0  
**Source:** `zxfoundation/memory/slab.c`, `zxfoundation/memory/kmalloc.c`

---

## 1. Slab Allocator

The slab allocator provides fixed-size object caches to amortize the overhead of frequent small allocations (VMAs, kobjects, sync primitives, etc.).

### Structure

Each **slab cache** (`slab_cache_t`) manages a pool of equal-sized objects. Objects are carved from PMM-allocated slabs (one or more contiguous pages). Free objects within a slab are linked in a free list embedded in the object itself.

```
slab_cache_t
  ├─ object_size
  ├─ slab_order       (PMM order for each slab backing page)
  ├─ free_list        (head of free object list)
  └─ lock             (spinlock)
```

### Allocation

`slab_alloc(cache)`:
1. If `free_list` is non-empty, pop and return the head object.
2. Otherwise, allocate a new slab from the PMM, carve it into objects, link them into `free_list`, then pop one.

### Deallocation

`slab_free(cache, obj)`: push `obj` onto `free_list`.

---

## 2. kmalloc

`kmalloc(size)` routes requests to the appropriate slab cache based on size class, falling back to `kheap_alloc` for large requests.

| Size range | Backing |
|------------|---------|
| ≤ 8 KB | Slab cache for the nearest power-of-two size class |
| > 8 KB | `kheap_alloc` → `vmm_alloc` |

`kfree(ptr)` returns the object to its originating cache. The allocator embeds a header before each allocation to record the cache pointer and a magic canary for use-after-free detection.

---

## 3. Initialization Order

```
pmm_init()   ← must run first (slab needs PMM)
vmm_init()   ← must run before kheap
slab_init()  ← registers size-class caches
kmalloc_init() ← sets up kmalloc routing table
```
