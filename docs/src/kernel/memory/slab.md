# Slab Allocator & kmalloc

**Document Revision:** 26h1.1
**Source:** `zxfoundation/memory/slab.c`, `zxfoundation/memory/kmalloc.c`

---

## 1. Slab Allocator

The slab allocator provides fixed-size object caches to amortize the cost of frequent small allocations (VMAs, sync primitives, capability tables, etc.).
It uses a **magazine-depot** architecture for lock-free per-CPU fast paths and SMP-safe bulk operations through the depot.

### 1.1 Architecture

```
kmem_cache_t
  ├─ obj_size          (8-byte aligned)
  ├─ storage_key       (s390x storage key for all backing pages)
  ├─ depot_lock        (spinlock protecting the depot lists)
  ├─ full_mags         (depot: magazines with MAG_SIZE objects ready)
  ├─ empty_mags        (depot: magazines ready to be refilled)
  ├─ partial_slabs     (slab pages with free objects remaining)
  ├─ full_slabs        (slab pages fully allocated)
  └─ cpu_mags[MAX_CPUS] (per-CPU active magazine pointer)
```

Each **magazine** holds up to `MAG_SIZE` (31) object pointers.
Each **slab** is one PMM page; the slab header, free-index stack, and object array are all embedded within that page.

### 1.2 Fast Path (per-CPU, no lock)

```
alloc:
  IRQs disabled
  if cpu_mag.count > 0 → pop and return
  else → magazine_swap(fill) → pop and return

free:
  IRQs disabled
  if cpu_mag.count < MAG_SIZE → push and return
  else → magazine_swap(drain) → push and return
```

IRQs are disabled for the duration of the fast path.
No lock is taken; the per-CPU magazine is accessed exclusively.

### 1.3 Slow Path (depot, with lock)

`magazine_swap` acquires `depot_lock`.  Two sub-paths:

**Fill (need objects):**

```
1. full_mags non-empty?
      yes → promote to CPU slot immediately (fast fill)
       no → obtain empty shell from empty_mags (or alloc from mag_cache)
            → cache_refill_magazine (may drop+reacquire depot_lock for PMM)
            → move filled shell to full_mags → promote to CPU slot
```

**Drain (returning a full CPU magazine):**

```
1. Push CPU magazine to full_mags
2. Pull empty shell from empty_mags into CPU slot (or set to nullptr)
```

### 1.4 Slab Refill & Lock Discipline

`cache_refill_magazine` is called with `depot_lock` held.
When a new slab page must be allocated from the PMM:

```
drop depot_lock
  pmm_alloc_page()      ← PMM zone lock acquired/released here
reacquire depot_lock
re-validate partial_slabs (another CPU may have added one in the window)
```

This ensures the PMM zone lock and `depot_lock` are **never held simultaneously**, eliminating the lock-inversion hazard present in earlier revisions.

### 1.5 Node Lifecycle

Magazine nodes cycle between:

```
empty_mags ──fill──▶ (detached, being filled) ──▶ full_mags ──promote──▶ cpu_mag
cpu_mag ──drain──▶ full_mags   empty_mags ◀── (pulled empty shell)
```

`list_del_init` is used for all magazine-node removals so nodes are always in a self-pointing state when not on a list, making re-insertion safe without re-initialization.

---

## 2. kmalloc

`kmalloc(size)` routes requests to the appropriate slab cache based on size class.

| Size range | Backing                  |
|------------|--------------------------|
| ≤ 8 KB     | Slab cache (power-of-two class) |
| > 8 KB     | `vmalloc` → `vmm_alloc`  |

`kfree(ptr)` returns the object to its originating cache.
A header embedded before each allocation records the cache pointer and a canary for use-after-free detection.

---

## 3. Initialization Order

```
pmm_init()      ← must run first; slab needs PMM pages
slab_init()     ← bootstraps cache_cache and mag_cache from a single PMM page
kmalloc_init()  ← registers size-class caches via kmem_cache_create
vmm_notify_slab_ready() ← switches VMM early allocator to kmalloc
```

---

## 4. Strict Requirements

| ID     | Requirement |
|--------|-------------|
| SLAB-1 | `kmem_cache_alloc` must not be called from hard-IRQ context unless the cache was created with atomic support.  Use `kmalloc(ZX_GFP_ATOMIC)` from IRQ context. |
| SLAB-2 | `kmem_cache_free` must only be called with a pointer returned by `kmem_cache_alloc` on the **same** cache.  Cross-cache free is undefined behavior. |
| SLAB-3 | `kmem_cache_destroy` must only be called after all objects have been returned.  Outstanding objects at destroy time trigger a kernel panic. |
| SLAB-4 | `depot_lock` must never be held when calling into the PMM or any allocator that may itself acquire a zone lock.  Use the lock-drop protocol in `cache_refill_magazine`. |
