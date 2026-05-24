// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/slab.c
//
/// @brief Magazine-depot slab allocator.

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

/// Objects per magazine.  31 keeps sizeof(kmem_magazine_t) under a cache line
/// boundary on z/Architecture (256-byte L1 cache lines) while giving enough
/// depth to absorb burst alloc/free sequences without depot contention.
#define MAG_SIZE        31U

typedef struct kmem_magazine {
    list_head_t  node;
    uint32_t     count;
    void        *objects[MAG_SIZE];
} kmem_magazine_t;

/// @brief Free-object index stack embedded at the start of a slab allocation.
typedef struct kmem_slab {
    list_head_t  node;
    uint16_t     free_count;   ///< Remaining allocatable objects.
    uint16_t     total;        ///< Total objects sliced from this slab.
    uint16_t     next_free;    ///< Bump pointer for virgin slots.
    uint16_t     free_top;     ///< Top of the recycled-index stack.
    uint8_t      slab_order;   ///< PMM order used to allocate this slab.
    uint8_t      _pad[7];
    zx_page_t   *page;         ///< Back-pointer to the owning PMM page.
    void        *obj_base;     ///< Pointer to the first object in the slab.
} kmem_slab_t;

/// @brief Per-CPU magazine pair.  Accessed with IRQs disabled; no spinlock.
typedef struct {
    kmem_magazine_t *loaded;   ///< Active magazine (alloc/free here first).
    kmem_magazine_t *prev;     ///< Previously active magazine (swap target).
} cpu_mag_pair_t;

struct kmem_cache {
    const char     *name;
    size_t          obj_size;    ///< Aligned object size.
    uint8_t         storage_key;

    spinlock_t      depot_lock;
    list_head_t     full_mags;
    list_head_t     empty_mags;
    list_head_t     partial_slabs;
    list_head_t     full_slabs;

    cpu_mag_pair_t  cpu_mags[MAX_CPUS];
};

static kmem_cache_t cache_cache;    ///< Allocates kmem_cache_t objects.
static kmem_cache_t mag_cache;      ///< Allocates kmem_magazine_t objects.

// ---------------------------------------------------------------------------
// Slab layout
// ---------------------------------------------------------------------------

static void slab_compute_layout(kmem_slab_t *slab, size_t obj_size, uint32_t ord) {
    size_t slab_bytes = PAGE_SIZE << ord;
    uintptr_t hdr_end = (uintptr_t)slab + sizeof(kmem_slab_t);
    uint16_t capacity = 0;

    for (;;) {
        uint16_t try_cap = capacity + 1;
        uintptr_t idx_end   = hdr_end + (uintptr_t)try_cap * sizeof(uint16_t);
        uintptr_t obj_start = (idx_end + 7UL) & ~7UL;
        uintptr_t obj_end   = obj_start + (uintptr_t)try_cap * obj_size;
        if (obj_end > (uintptr_t)slab + slab_bytes) break;
        capacity = try_cap;
    }
    if (capacity == 0)
        zx_system_check(ZX_SYSCHK_CORE_INTERNAL_ERROR,
                        "slab: object size %zu > slab size %zu", obj_size, slab_bytes);

    uintptr_t idx_end   = hdr_end + (uintptr_t)capacity * sizeof(uint16_t);
    uintptr_t obj_start = (idx_end + 7UL) & ~7UL;

    slab->total      = capacity;
    slab->free_count = capacity;
    slab->next_free  = 0;
    slab->free_top   = 0;
    slab->obj_base   = (void *)obj_start;
}

static inline uint16_t *slab_free_stack(kmem_slab_t *slab) {
    return (uint16_t *)((uintptr_t)slab + sizeof(kmem_slab_t));
}

/// @brief Minimum PMM order so at least one object fits after the slab header.
static uint32_t slab_order_for(size_t obj_size) {
    for (uint32_t ord = 0; ord <= MAX_ORDER; ord++) {
        size_t slab_bytes = PAGE_SIZE << ord;
        uintptr_t hdr_end   = sizeof(kmem_slab_t) + sizeof(uint16_t);
        uintptr_t obj_start = (hdr_end + 7UL) & ~7UL;
        if (obj_start + obj_size <= slab_bytes)
            return ord;
    }
    return MAX_ORDER;
}

static kmem_slab_t *slab_new_page(kmem_cache_t *cache, gfp_t gfp) {
    uint32_t ord = slab_order_for(cache->obj_size);
    zx_page_t *page = pmm_alloc_pages(ord, gfp | ZX_GFP_ZERO);
    if (!page) return nullptr;

    arch_set_storage_key(pmm_page_to_phys(page), cache->storage_key | 0x10);

    kmem_slab_t *slab = (kmem_slab_t *)(uintptr_t)
        hhdm_phys_to_virt(pmm_page_to_phys(page));

    slab->page       = page;
    slab->slab_order = ord;
    page->slab_cache = cache;
    page->flags     |= PF_SLAB;

    slab_compute_layout(slab, cache->obj_size, ord);
    return slab;
}

// ---------------------------------------------------------------------------
// Depot operations  (all called with depot_lock held unless noted)
// ---------------------------------------------------------------------------

/// @brief Pop up to MAG_SIZE objects from the cache's slab pool into @mag.
///        Drops and re-acquires depot_lock around the PMM call if a new slab
///        page is needed.  @f carries the saved IRQ flags across that window.
static bool depot_fill_magazine(kmem_cache_t *cache, kmem_magazine_t *mag,
                                gfp_t gfp, irqflags_t f) {
    // Fast path: partial slab already available.
    if (!list_empty(&cache->partial_slabs)) {
        kmem_slab_t *slab = list_entry(cache->partial_slabs.next, kmem_slab_t, node);
        uint16_t *fs = slab_free_stack(slab);

        while (mag->count < MAG_SIZE && slab->free_count > 0) {
            uint16_t idx = (slab->free_top > 0) ? fs[--slab->free_top] : slab->next_free++;
            mag->objects[mag->count++] = (char *)slab->obj_base + (uintptr_t)idx * cache->obj_size;
            slab->free_count--;
        }
        if (slab->free_count == 0) {
            list_del_init(&slab->node);
            list_add_tail(&slab->node, &cache->full_slabs);
        }
        return mag->count > 0;
    }

    // Slow path: allocate a new slab page from the PMM.
    // Drop depot_lock to avoid holding it across the PMM zone lock.
    spin_unlock_irqrestore(&cache->depot_lock, f);
    kmem_slab_t *ns = slab_new_page(cache, gfp);
    spin_lock_irqsave(&cache->depot_lock, &f);

    if (!ns) return false;

    // Another CPU may have added a partial slab in the window — discard ours.
    if (!list_empty(&cache->partial_slabs)) {
        ns->page->flags    &= ~PF_SLAB;
        ns->page->slab_cache = nullptr;
        pmm_free_pages(ns->page, ns->slab_order);

        kmem_slab_t *slab = list_entry(cache->partial_slabs.next, kmem_slab_t, node);
        uint16_t *fs = slab_free_stack(slab);
        while (mag->count < MAG_SIZE && slab->free_count > 0) {
            uint16_t idx = (slab->free_top > 0) ? fs[--slab->free_top] : slab->next_free++;
            mag->objects[mag->count++] = (char *)slab->obj_base + (uintptr_t)idx * cache->obj_size;
            slab->free_count--;
        }
        if (slab->free_count == 0) {
            list_del_init(&slab->node);
            list_add_tail(&slab->node, &cache->full_slabs);
        }
        return mag->count > 0;
    }

    list_add_tail(&ns->node, &cache->partial_slabs);
    uint16_t *fs = slab_free_stack(ns);
    while (mag->count < MAG_SIZE && ns->free_count > 0) {
        uint16_t idx = (ns->free_top > 0) ? fs[--ns->free_top] : ns->next_free++;
        mag->objects[mag->count++] = (char *)ns->obj_base + (uintptr_t)idx * cache->obj_size;
        ns->free_count--;
    }
    if (ns->free_count == 0) {
        list_del_init(&ns->node);
        list_add_tail(&ns->node, &cache->full_slabs);
    }
    return mag->count > 0;
}

/// @brief Return all objects in @mag directly to their slab pages.
///        Called with depot_lock held.
static void depot_drain_magazine(kmem_cache_t *cache, kmem_magazine_t *mag) {
    while (mag->count > 0) {
        void *obj = mag->objects[--mag->count];
        zx_page_t *pg = virt_to_page(obj);
        if (!(pg->flags & PF_SLAB) || !pg->slab_cache) continue;
        kmem_slab_t *slab = (kmem_slab_t *)(uintptr_t)hhdm_phys_to_virt(pmm_page_to_phys(pg));
        uint16_t idx = (uint16_t)(((uintptr_t)obj - (uintptr_t)slab->obj_base) / cache->obj_size);
        uint16_t *fs = slab_free_stack(slab);
        fs[slab->free_top++] = idx;
        if (++slab->free_count == 1) {
            // Was full — move back to partial.
            list_del_init(&slab->node);
            list_add_tail(&slab->node, &cache->partial_slabs);
        }
    }
}

/// @brief Obtain an empty magazine shell from the depot, allocating one if needed.
///        Returns nullptr only on OOM or bootstrap recursion guard.
///        Called with depot_lock held; may drop and re-acquire it.
static kmem_magazine_t *depot_get_empty(kmem_cache_t *cache, gfp_t gfp, irqflags_t *f) {
    if (!list_empty(&cache->empty_mags)) {
        list_head_t *n = cache->empty_mags.next;
        list_del_init(n);
        return list_entry(n, kmem_magazine_t, node);
    }
    // Bootstrap guard: mag_cache cannot allocate from itself.
    if (cache == &mag_cache) return nullptr;

    spin_unlock_irqrestore(&cache->depot_lock, *f);
    kmem_magazine_t *m = (kmem_magazine_t *)kmem_cache_alloc(&mag_cache, gfp);
    spin_lock_irqsave(&cache->depot_lock, f);

    if (!m) return nullptr;
    m->count = 0;
    list_init(&m->node);
    return m;
}

// ---------------------------------------------------------------------------
// Alloc / free — two-magazine fast path
// ---------------------------------------------------------------------------

void *kmem_cache_alloc(kmem_cache_t *cache, gfp_t gfp) {
    irqflags_t f = arch_local_save_flags();
    arch_local_irq_disable();

    int cpu = arch_smp_processor_id();
    cpu_mag_pair_t *pair = &cache->cpu_mags[cpu];

    // Fast path 1: pop from loaded magazine.
    if (pair->loaded && pair->loaded->count > 0) {
        void *obj = pair->loaded->objects[--pair->loaded->count];
        arch_local_irq_restore(f);
        return obj;
    }

    // Fast path 2: swap loaded ↔ prev, then pop.
    if (pair->prev && pair->prev->count > 0) {
        kmem_magazine_t *tmp = pair->loaded;
        pair->loaded = pair->prev;
        pair->prev   = tmp;
        void *obj = pair->loaded->objects[--pair->loaded->count];
        arch_local_irq_restore(f);
        return obj;
    }

    // Slow path: both magazines empty — go to depot.
    arch_local_irq_restore(f);

    irqflags_t df;
    spin_lock_irqsave(&cache->depot_lock, &df);

    // Try to pull a full magazine from the depot.
    if (!list_empty(&cache->full_mags)) {
        list_head_t *n = cache->full_mags.next;
        list_del_init(n);
        kmem_magazine_t *full = list_entry(n, kmem_magazine_t, node);

        // Stash the old loaded (empty) into empty_mags if we have one.
        arch_local_irq_disable();
        cpu_mag_pair_t *p = &cache->cpu_mags[arch_smp_processor_id()];
        if (p->loaded) list_add_tail(&p->loaded->node, &cache->empty_mags);
        p->loaded = full;
        void *obj = p->loaded->objects[--p->loaded->count];
        arch_local_irq_restore(f);
        spin_unlock_irqrestore(&cache->depot_lock, df);
        return obj;
    }

    // No full magazine — get an empty shell and fill it.
    kmem_magazine_t *nm = depot_get_empty(cache, gfp, &df);
    if (!nm) {
        spin_unlock_irqrestore(&cache->depot_lock, df);
        return nullptr;
    }

    if (!depot_fill_magazine(cache, nm, gfp, df)) {
        list_add_tail(&nm->node, &cache->empty_mags);
        spin_unlock_irqrestore(&cache->depot_lock, df);
        return nullptr;
    }

    // Install the filled magazine as loaded.
    arch_local_irq_disable();
    cpu_mag_pair_t *p = &cache->cpu_mags[arch_smp_processor_id()];
    if (p->loaded) list_add_tail(&p->loaded->node, &cache->empty_mags);
    p->loaded = nm;
    void *obj = p->loaded->objects[--p->loaded->count];
    arch_local_irq_restore(f);
    spin_unlock_irqrestore(&cache->depot_lock, df);
    return obj;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    if (!obj) return;

    irqflags_t f = arch_local_save_flags();
    arch_local_irq_disable();

    int cpu = arch_smp_processor_id();
    cpu_mag_pair_t *pair = &cache->cpu_mags[cpu];

    // Fast path 1: push to loaded magazine.
    if (pair->loaded && pair->loaded->count < MAG_SIZE) {
        pair->loaded->objects[pair->loaded->count++] = obj;
        arch_local_irq_restore(f);
        return;
    }

    // Fast path 2: swap loaded ↔ prev, then push.
    if (pair->prev && pair->prev->count < MAG_SIZE) {
        kmem_magazine_t *tmp = pair->loaded;
        pair->loaded = pair->prev;
        pair->prev   = tmp;
        pair->loaded->objects[pair->loaded->count++] = obj;
        arch_local_irq_restore(f);
        return;
    }

    // Slow path: both magazines full — push loaded to depot, get empty shell.
    arch_local_irq_restore(f);

    irqflags_t df;
    spin_lock_irqsave(&cache->depot_lock, &df);

    arch_local_irq_disable();
    cpu_mag_pair_t *p = &cache->cpu_mags[arch_smp_processor_id()];

    if (p->loaded) {
        list_add_tail(&p->loaded->node, &cache->full_mags);
        p->loaded = nullptr;
    }

    kmem_magazine_t *em = depot_get_empty(cache, ZX_GFP_NORMAL, &df);
    if (em) {
        p->loaded = em;
        p->loaded->objects[p->loaded->count++] = obj;
        arch_local_irq_restore(f);
        spin_unlock_irqrestore(&cache->depot_lock, df);
        return;
    }

    arch_local_irq_restore(f);

    // No magazine shell available — return directly to the slab.
    depot_drain_magazine(cache, &(kmem_magazine_t){ .count = 1, .objects = { obj } });
    spin_unlock_irqrestore(&cache->depot_lock, df);
}

kmem_cache_t *kmem_cache_create(const char *name, size_t size, uint8_t storage_key) {
    kmem_cache_t *cp = (kmem_cache_t *)kmem_cache_alloc(&cache_cache, ZX_GFP_NORMAL);
    if (!cp) return nullptr;

    cp->name        = name;
    cp->obj_size    = (size + 7UL) & ~7UL;
    cp->storage_key = storage_key;
    spin_lock_init(&cp->depot_lock);
    list_init(&cp->full_mags);
    list_init(&cp->empty_mags);
    list_init(&cp->partial_slabs);
    list_init(&cp->full_slabs);
    memset(cp->cpu_mags, 0, sizeof(cp->cpu_mags));

    // Seed with a few empty magazine shells so the first alloc doesn't have
    // to allocate a shell from mag_cache under depot_lock.
    for (int i = 0; i < 4; i++) {
        kmem_magazine_t *m = (kmem_magazine_t *)kmem_cache_alloc(&mag_cache, ZX_GFP_NORMAL);
        if (m) { m->count = 0; list_add_tail(&m->node, &cp->empty_mags); }
    }
    return cp;
}

void kmem_cache_destroy(kmem_cache_t *cache) {
    irqflags_t df;
    spin_lock_irqsave(&cache->depot_lock, &df);

    // Drain all per-CPU magazines back to slabs.
    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_mag_pair_t *p = &cache->cpu_mags[cpu];
        if (p->loaded) {
            depot_drain_magazine(cache, p->loaded);
            list_add_tail(&p->loaded->node, &cache->empty_mags);
            p->loaded = nullptr;
        }
        if (p->prev) {
            depot_drain_magazine(cache, p->prev);
            list_add_tail(&p->prev->node, &cache->empty_mags);
            p->prev = nullptr;
        }
    }

    // Drain all full depot magazines back to slabs.
    list_head_t *n, *tmp;
    list_for_each_safe(n, tmp, &cache->full_mags) {
        kmem_magazine_t *m = list_entry(n, kmem_magazine_t, node);
        list_del_init(n);
        depot_drain_magazine(cache, m);
        list_add_tail(&m->node, &cache->empty_mags);
    }

    spin_unlock_irqrestore(&cache->depot_lock, df);

    // Return all slab pages to the PMM.
    spin_lock_irqsave(&cache->depot_lock, &df);
    list_for_each_safe(n, tmp, &cache->partial_slabs) {
        kmem_slab_t *s = list_entry(n, kmem_slab_t, node);
        list_del(n);
        s->page->flags     &= ~PF_SLAB;
        s->page->slab_cache = nullptr;
        pmm_free_pages(s->page, s->slab_order);
    }
    list_for_each_safe(n, tmp, &cache->full_slabs) {
        kmem_slab_t *s = list_entry(n, kmem_slab_t, node);
        list_del(n);
        s->page->flags     &= ~PF_SLAB;
        s->page->slab_cache = nullptr;
        pmm_free_pages(s->page, s->slab_order);
    }
    spin_unlock_irqrestore(&cache->depot_lock, df);

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
    if (!boot_page)
        zx_system_check(ZX_SYSCHK_MEM_OOM, "slab_init: cannot allocate bootstrap page");

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
