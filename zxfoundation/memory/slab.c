// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/slab.c
//
/// @brief Magazine-depot slab allocator — updated for page-descriptor PMM API.

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/memory/page.h>
#include <zxfoundation/memory/slab.h>
#include <zxfoundation/sync/spinlock.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sys/syschk.h>
#include <zxfoundation/percpu.h>
#include <zxfoundation/zconfig.h>
#include <arch/s390x/cpu/processor.h>
#include <lib/list.h>
#include <lib/string.h>

#define MAG_SIZE        31      ///< Objects per magazine.

typedef struct kmem_magazine {
    list_node_t  node;
    uint32_t     count;
    void        *objects[MAG_SIZE];
} kmem_magazine_t;

/// @brief Free-object index stack embedded in the slab page.
typedef struct kmem_slab {
    list_node_t  node;
    uint16_t     free_count;   ///< Remaining allocatable objects.
    uint16_t     total;        ///< Total objects sliced from this slab.
    uint16_t     next_free;    ///< Next unallocated slot (bump pointer).
    uint16_t     free_top;     ///< Top of the recycled-index stack.
    zx_page_t   *page;         ///< Back-pointer to the owning PMM page.
    void        *obj_base;     ///< Pointer to the first object in the slab.
} kmem_slab_t;

struct kmem_cache {
    const char     *name;
    size_t          obj_size;    ///< Aligned object size.
    uint8_t         storage_key;

    spinlock_t      depot_lock;
    list_node_t     full_mags;
    list_node_t     empty_mags;
    list_node_t     partial_slabs;
    list_node_t     full_slabs;

    kmem_magazine_t *cpu_mags[MAX_CPUS];
};

static kmem_cache_t cache_cache;    ///< Allocates kmem_cache_t objects.
static kmem_cache_t mag_cache;      ///< Allocates kmem_magazine_t objects.

/// @brief Given a slab header at 'slab' (start of a PMM page), compute
///        object layout for a cache with obj_size.
static void slab_compute_layout(kmem_slab_t *slab, size_t obj_size) {
    uintptr_t hdr_end = (uintptr_t)slab + sizeof(kmem_slab_t);
    uint16_t capacity = 0;

    for (;;) {
        uint16_t try_cap = capacity + 1;
        uintptr_t idx_end = hdr_end + (uintptr_t)try_cap * sizeof(uint16_t);
        uintptr_t obj_start = (idx_end + 7UL) & ~7UL; // 8-byte align
        uintptr_t obj_end = obj_start + (uintptr_t)try_cap * obj_size;
        if (obj_end > (uintptr_t)slab + PAGE_SIZE) break;
        capacity = try_cap;
    }
    if (capacity == 0)
        zx_system_check(ZX_SYSCHK_CORE_INTERNAL_ERROR, "slab: object size %zu > PAGE_SIZE", obj_size);

    uintptr_t idx_end  = hdr_end + (uintptr_t)capacity * sizeof(uint16_t);
    uintptr_t obj_start = (idx_end + 7UL) & ~7UL;

    slab->total      = capacity;
    slab->free_count = capacity;
    slab->next_free  = 0;
    slab->free_top   = 0;
    slab->obj_base   = (void *)obj_start;
}

/// @brief Free-index stack array within a slab page.
static inline uint16_t *slab_free_stack(kmem_slab_t *slab) {
    return (uint16_t *)((uintptr_t)slab + sizeof(kmem_slab_t));
}

static kmem_slab_t *slab_new_page(kmem_cache_t *cache, gfp_t gfp) {
    zx_page_t *page = pmm_alloc_page(gfp | ZX_GFP_ZERO);
    if (!page) return nullptr;

    arch_set_storage_key(pmm_page_to_phys(page), cache->storage_key | 0x10);

    kmem_slab_t *slab = (kmem_slab_t *)(uintptr_t)
        hhdm_phys_to_virt(pmm_page_to_phys(page));

    slab->page = page;
    page->slab_cache = cache;
    page->flags     |= PF_SLAB;

    slab_compute_layout(slab, cache->obj_size);
    return slab;
}

/// @brief Fill @mag with objects from the cache's slab pool.
///
///        Called with @cache->depot_lock held.  The lock is dropped around
///        the pmm_alloc_page() call inside slab_new_page() so that the PMM
///        zone lock and the depot lock are never held simultaneously — this
///        avoids the lock-inversion hazard that causes deadlocks on SMP and
///        prevents the PMM's per-CPU path from recursing into the slab.
///
///        After the PMM allocation the lock is re-acquired and the list state
///        is re-validated before committing the new slab, because another CPU
///        may have populated partial_slabs in the window.
///
/// @param cache  Owning cache (depot_lock must be held on entry and exit).
/// @param mag    Empty magazine to fill.
/// @param gfp    GFP flags forwarded to the PMM if a new slab is needed.
/// @param f      Saved IRQ flags for the spin_unlock_irqrestore on the
///               lock-drop path.
/// @return true if at least one object was placed in @mag.
static bool cache_refill_magazine(kmem_cache_t *cache, kmem_magazine_t *mag,
                                  gfp_t gfp, irqflags_t f) {
    // Fast path: serve from an existing partial slab without dropping the lock.
    if (!list_empty(&cache->partial_slabs)) {
        kmem_slab_t *slab = list_entry(cache->partial_slabs.next,
                                       kmem_slab_t, node);
        uint16_t *free_stack = slab_free_stack(slab);

        while (mag->count < MAG_SIZE && slab->free_count > 0) {
            uint16_t idx;
            if (slab->free_top > 0)
                idx = free_stack[--slab->free_top];
            else
                idx = slab->next_free++;

            mag->objects[mag->count++] =
                (char *)slab->obj_base + (uintptr_t)idx * cache->obj_size;
            slab->free_count--;
        }

        if (slab->free_count == 0) {
            list_del_init(&slab->node);
            list_add_tail(&slab->node, &cache->full_slabs);
        }

        return mag->count > 0;
    }

    // Slow path: no partial slab — must allocate a new page from the PMM.
    // Drop depot_lock before calling into the PMM to avoid holding two
    // unrelated locks simultaneously (PMM zone lock + depot_lock).
    spin_unlock_irqrestore(&cache->depot_lock, f);
    kmem_slab_t *new_slab = slab_new_page(cache, gfp);
    spin_lock_irqsave(&cache->depot_lock, &f);

    if (!new_slab)
        return false;

    // Re-check partial_slabs: another CPU may have added one in the window.
    // If so, discard our freshly allocated slab to avoid leaking it — note
    // this is a tiny waste of one page that is acceptable for correctness.
    // In a production path a free list for surplus slabs would be warranted,
    // but the boot-time frequency is too low to justify the complexity.
    if (!list_empty(&cache->partial_slabs)) {
        // Another CPU beat us; release the new page and serve from theirs.
        new_slab->page->flags    &= ~PF_SLAB;
        new_slab->page->slab_cache = nullptr;
        pmm_free_page(new_slab->page);

        kmem_slab_t *slab = list_entry(cache->partial_slabs.next,
                                       kmem_slab_t, node);
        uint16_t *free_stack = slab_free_stack(slab);

        while (mag->count < MAG_SIZE && slab->free_count > 0) {
            uint16_t idx;
            if (slab->free_top > 0)
                idx = free_stack[--slab->free_top];
            else
                idx = slab->next_free++;

            mag->objects[mag->count++] =
                (char *)slab->obj_base + (uintptr_t)idx * cache->obj_size;
            slab->free_count--;
        }

        if (slab->free_count == 0) {
            list_del_init(&slab->node);
            list_add_tail(&slab->node, &cache->full_slabs);
        }

        return mag->count > 0;
    }

    // We own the new slab exclusively — link it and fill.
    list_add_tail(&new_slab->node, &cache->partial_slabs);

    uint16_t *free_stack = slab_free_stack(new_slab);
    while (mag->count < MAG_SIZE && new_slab->free_count > 0) {
        uint16_t idx;
        if (new_slab->free_top > 0)
            idx = free_stack[--new_slab->free_top];
        else
            idx = new_slab->next_free++;

        mag->objects[mag->count++] =
            (char *)new_slab->obj_base + (uintptr_t)idx * cache->obj_size;
        new_slab->free_count--;
    }

    if (new_slab->free_count == 0) {
        list_del_init(&new_slab->node);
        list_add_tail(&new_slab->node, &cache->full_slabs);
    }

    return mag->count > 0;
}

/// @brief Swap the per-CPU magazine with one from the depot.
///
///        Two distinct code paths exist:
///
///        Filling (caller needs objects):
///          1. Try to promote a full depot magazine to the CPU slot.
///          2. If none, obtain an empty magazine from the depot (allocating
///             one from mag_cache if needed), fill it via cache_refill_magazine,
///             then promote it.
///
///        Draining (caller has a full CPU magazine):
///          1. Push the full CPU magazine to the full-mag depot list.
///          2. Pull an empty magazine from the depot (if any) into the CPU slot.
///
///        Locking discipline: depot_lock is acquired once at the top.
///        cache_refill_magazine may transiently drop and re-acquire it around
///        the pmm_alloc_page() call; the saved flags @f are threaded through
///        so the IRQ state is correctly maintained across that window.
///
/// @param cache    Cache to operate on.
/// @param cpu      Logical CPU ID of the calling CPU.
/// @param filling  true = want objects; false = returning a full magazine.
/// @param gfp      GFP flags for potential PMM / mag_cache allocation.
/// @return true on success.
static bool magazine_swap(kmem_cache_t *cache, int cpu, bool filling, gfp_t gfp) {
    irqflags_t f;
    spin_lock_irqsave(&cache->depot_lock, &f);

    if (filling) {
        // --- Fast fill: a full depot magazine is immediately available. ---
        if (!list_empty(&cache->full_mags)) {
            list_node_t *n = cache->full_mags.next;
            list_del_init(n);
            if (cache->cpu_mags[cpu])
                list_add_tail(&cache->cpu_mags[cpu]->node, &cache->empty_mags);
            cache->cpu_mags[cpu] = list_entry(n, kmem_magazine_t, node);
            spin_unlock_irqrestore(&cache->depot_lock, f);
            return true;
        }

        // --- Slow fill: no full depot magazine — must populate an empty one. ---

        // Obtain an empty magazine shell.
        kmem_magazine_t *nm = nullptr;

        if (!list_empty(&cache->empty_mags)) {
            list_node_t *en = cache->empty_mags.next;
            list_del_init(en);
            nm = list_entry(en, kmem_magazine_t, node);
        } else {
            // No empty shell in the depot either.  Bootstrap guard: mag_cache
            // cannot recursively allocate from itself.
            if (cache == &mag_cache) {
                spin_unlock_irqrestore(&cache->depot_lock, f);
                return false;
            }

            // Drop the depot lock while calling into mag_cache to avoid
            // holding depot_lock across kmem_cache_alloc's IRQ-disable
            // section (which itself would call magazine_swap on mag_cache,
            // a different lock, so no deadlock — but cleaner to drop here).
            spin_unlock_irqrestore(&cache->depot_lock, f);
            nm = (kmem_magazine_t *)kmem_cache_alloc(&mag_cache, gfp);
            spin_lock_irqsave(&cache->depot_lock, &f);

            if (!nm) {
                spin_unlock_irqrestore(&cache->depot_lock, f);
                return false;
            }
            nm->count = 0;
            // Initialize the node so list_del_init is safe later.
            list_init(&nm->node);
        }

        // nm is now an empty, unlinked magazine shell.  Fill it.
        // cache_refill_magazine may drop and re-acquire depot_lock around
        // pmm_alloc_page; @f is passed by value for that inner unlock and
        // is not re-used by this frame after the call.
        bool ok = cache_refill_magazine(cache, nm, gfp, f);
        // depot_lock is held again here after cache_refill_magazine returns.

        if (!ok) {
            // Refill failed (OOM).  Return the empty shell to the depot.
            list_add_tail(&nm->node, &cache->empty_mags);
            spin_unlock_irqrestore(&cache->depot_lock, f);
            return false;
        }

        // Move the now-full magazine into the full-mag list, then immediately
        // promote it to the CPU slot via the normal fast path.
        list_add_tail(&nm->node, &cache->full_mags);

        list_node_t *n = cache->full_mags.next;
        list_del_init(n);
        if (cache->cpu_mags[cpu])
            list_add_tail(&cache->cpu_mags[cpu]->node, &cache->empty_mags);
        cache->cpu_mags[cpu] = list_entry(n, kmem_magazine_t, node);

        spin_unlock_irqrestore(&cache->depot_lock, f);
        return true;
    }

    // --- Drain path: push the full CPU magazine to the depot. ---
    list_add_tail(&cache->cpu_mags[cpu]->node, &cache->full_mags);
    if (list_empty(&cache->empty_mags)) {
        cache->cpu_mags[cpu] = nullptr;
    } else {
        list_node_t *n = cache->empty_mags.next;
        list_del_init(n);
        cache->cpu_mags[cpu] = list_entry(n, kmem_magazine_t, node);
    }

    spin_unlock_irqrestore(&cache->depot_lock, f);
    return true;
}

void *kmem_cache_alloc(kmem_cache_t *cache, gfp_t gfp) {
    irqflags_t f = arch_local_save_flags();
    arch_local_irq_disable();

    int cpu = arch_smp_processor_id();
    kmem_magazine_t *mag = cache->cpu_mags[cpu];

    if (mag && mag->count > 0) {
        void *obj = mag->objects[--mag->count];
        arch_local_irq_restore(f);
        return obj;
    }

    // magazine_swap re-enables IRQs internally during lock operations;
    // on return IRQs are disabled again (depot_lock is released before return).
    if (magazine_swap(cache, cpu, true, gfp)) {
        mag = cache->cpu_mags[cpu];
        void *obj = mag->objects[--mag->count];
        arch_local_irq_restore(f);
        return obj;
    }

    arch_local_irq_restore(f);
    return nullptr;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    if (!obj) return;

    irqflags_t f = arch_local_save_flags();
    arch_local_irq_disable();

    int cpu = arch_smp_processor_id();
    kmem_magazine_t *mag = cache->cpu_mags[cpu];

    if (mag && mag->count < MAG_SIZE) {
        mag->objects[mag->count++] = obj;
        arch_local_irq_restore(f);
        return;
    }

    magazine_swap(cache, cpu, false, ZX_GFP_NORMAL);
    mag = cache->cpu_mags[cpu];
    if (mag)
        mag->objects[mag->count++] = obj;

    arch_local_irq_restore(f);
}

kmem_cache_t *kmem_cache_create(const char *name, size_t size, uint8_t storage_key) {
    kmem_cache_t *cp = (kmem_cache_t *)kmem_cache_alloc(&cache_cache, ZX_GFP_NORMAL);
    if (!cp) return nullptr;

    cp->name        = name;
    cp->obj_size    = (size + 7UL) & ~7UL;  // 8-byte align
    cp->storage_key = storage_key;
    spin_lock_init(&cp->depot_lock);
    list_init(&cp->full_mags);
    list_init(&cp->empty_mags);
    list_init(&cp->partial_slabs);
    list_init(&cp->full_slabs);
    memset(cp->cpu_mags, 0, sizeof(cp->cpu_mags));

    // Seed with a few empty magazines.
    for (int i = 0; i < 4; i++) {
        kmem_magazine_t *m = (kmem_magazine_t *)kmem_cache_alloc(&mag_cache, ZX_GFP_NORMAL);
        if (m) { m->count = 0; list_add_tail(&m->node, &cp->empty_mags); }
    }

    return cp;
}

void kmem_cache_destroy(kmem_cache_t *cache) {
    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        kmem_magazine_t *mag = cache->cpu_mags[cpu];
        if (!mag) continue;

        irqflags_t f = arch_local_save_flags();
        arch_local_irq_disable();

        while (mag->count > 0) {
            void *obj = mag->objects[--mag->count];
            // Walk partial_slabs to find the owning slab and return the object.
            zx_page_t *pg = virt_to_page(obj);
            if (pg->flags & PF_SLAB) {
                kmem_slab_t *slab = (kmem_slab_t *)(uintptr_t)
                    hhdm_phys_to_virt(pmm_page_to_phys(pg));
                uint16_t idx = (uint16_t)(
                    ((uintptr_t)obj - (uintptr_t)slab->obj_base) / cache->obj_size);
                uint16_t *free_stack = slab_free_stack(slab);
                free_stack[slab->free_top++] = idx;
                slab->free_count++;
            }
        }
        arch_local_irq_restore(f);
        cache->cpu_mags[cpu] = nullptr;
    }

    // Return all slab pages to the PMM.
    list_node_t *n, *tmp;
    list_for_each_safe(n, tmp, &cache->partial_slabs) {
        kmem_slab_t *s = list_entry(n, kmem_slab_t, node);
        list_del(n);
        s->page->flags     &= ~PF_SLAB;
        s->page->slab_cache = nullptr;
        pmm_free_page(s->page);
    }
    list_for_each_safe(n, tmp, &cache->full_slabs) {
        kmem_slab_t *s = list_entry(n, kmem_slab_t, node);
        list_del(n);
        s->page->flags     &= ~PF_SLAB;
        s->page->slab_cache = nullptr;
        pmm_free_page(s->page);
    }
    kmem_cache_free(&cache_cache, cache);
}

static void init_static_cache(kmem_cache_t *c, const char *name, size_t obj_size) {
    c->name        = name;
    c->obj_size    = (obj_size + 7UL) & ~7UL;
    c->storage_key = 0x0;
    spin_lock_init(&c->depot_lock);
    list_init(&c->full_mags);
    list_init(&c->empty_mags);
    list_init(&c->partial_slabs);
    list_init(&c->full_slabs);
    memset(c->cpu_mags, 0, sizeof(c->cpu_mags));
}

void slab_init(void) {
    init_static_cache(&cache_cache, "kmem_cache_t",    sizeof(kmem_cache_t));
    init_static_cache(&mag_cache,   "kmem_magazine_t", sizeof(kmem_magazine_t));

    zx_page_t *boot_page = pmm_alloc_page(ZX_GFP_NORMAL | ZX_GFP_ZERO);
    if (!boot_page) zx_system_check(ZX_SYSCHK_MEM_OOM, "slab_init: cannot allocate bootstrap page");

    char *raw = (char *)(uintptr_t)hhdm_phys_to_virt(pmm_page_to_phys(boot_page));
    size_t mag_sz = (sizeof(kmem_magazine_t) + 7UL) & ~7UL;
    for (size_t off = 0; off + mag_sz <= PAGE_SIZE; off += mag_sz) {
        kmem_magazine_t *m = (kmem_magazine_t *)(raw + off);
        m->count = 0;
        list_init(&m->node);
        list_add_tail(&m->node, &mag_cache.empty_mags);
    }

    printk(ZX_INFO "slab: magazine-depot pools ready\n");
}
