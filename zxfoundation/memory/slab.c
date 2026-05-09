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

static bool cache_refill_magazine(kmem_cache_t *cache, kmem_magazine_t *mag, gfp_t gfp) {
    kmem_slab_t *slab = nullptr;
    if (!list_empty(&cache->partial_slabs))
        slab = list_entry(cache->partial_slabs.next, kmem_slab_t, node);

    if (!slab) {
        slab = slab_new_page(cache, gfp);
        if (!slab) return false;
        list_add_tail(&slab->node, &cache->partial_slabs);
    }

    uint16_t *free_stack = slab_free_stack(slab);

    while (mag->count < MAG_SIZE && slab->free_count > 0) {
        uint16_t idx;
        if (slab->free_top > 0) {
            idx = free_stack[--slab->free_top];
        } else {
            idx = slab->next_free++;
        }
        mag->objects[mag->count++] =
            (char *)slab->obj_base + (uintptr_t)idx * cache->obj_size;
        slab->free_count--;
    }

    if (slab->free_count == 0) {
        list_del(&slab->node);
        list_add_tail(&slab->node, &cache->full_slabs);
    }

    return mag->count > 0;
}

static bool magazine_swap(kmem_cache_t *cache, int cpu, bool filling, gfp_t gfp) {
    irqflags_t f;
    spin_lock_irqsave(&cache->depot_lock, &f);

    if (filling) {
        if (list_empty(&cache->full_mags)) {
            if (list_empty(&cache->empty_mags)) {
                if (cache == &mag_cache) {
                    spin_unlock_irqrestore(&cache->depot_lock, f);
                    return false;
                }
                spin_unlock_irqrestore(&cache->depot_lock, f);
                kmem_magazine_t *nm = (kmem_magazine_t *)kmem_cache_alloc(&mag_cache, gfp);
                if (!nm) return false;
                nm->count = 0;
                spin_lock_irqsave(&cache->depot_lock, &f);
                list_add_tail(&nm->node, &cache->empty_mags);
            }
            kmem_magazine_t *m = list_entry(cache->empty_mags.next,
                                            kmem_magazine_t, node);
            list_del(&m->node);
            if (!cache_refill_magazine(cache, m, gfp)) {
                list_add_tail(&m->node, &cache->empty_mags);
                spin_unlock_irqrestore(&cache->depot_lock, f);
                return false;
            }
            list_add_tail(&m->node, &cache->full_mags);
        }

        list_node_t *n = cache->full_mags.next;
        list_del(n);
        if (cache->cpu_mags[cpu])
            list_add_tail(&cache->cpu_mags[cpu]->node, &cache->empty_mags);
        cache->cpu_mags[cpu] = list_entry(n, kmem_magazine_t, node);
    } else {
        list_add_tail(&cache->cpu_mags[cpu]->node, &cache->full_mags);
        if (list_empty(&cache->empty_mags)) {
            cache->cpu_mags[cpu] = nullptr;
        } else {
            list_node_t *n = cache->empty_mags.next;
            list_del(n);
            cache->cpu_mags[cpu] = list_entry(n, kmem_magazine_t, node);
        }
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
        s->page->flags &= ~PF_SLAB;
        s->page->slab_cache = nullptr;
        pmm_free_page(s->page);
    }
    list_for_each_safe(n, tmp, &cache->full_slabs) {
        kmem_slab_t *s = list_entry(n, kmem_slab_t, node);
        list_del(n);
        s->page->flags &= ~PF_SLAB;
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
        list_add_tail(&m->node, &mag_cache.empty_mags);
    }

    printk(ZX_INFO "slab: magazine-depot pools ready\n");
}
