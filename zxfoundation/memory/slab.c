// SPDX-License-Identifier: Apache-2.0
// zxfoundation/memory/slab.c

#include <zxfoundation/memory/pmm.h>
#include <zxfoundation/spinlock.h>
#include <zxfoundation/sys/printk.h>
#include <zxfoundation/sys/panic.h>
#include <arch/s390x/cpu/processor.h>
#include <lib/list.h>
#include <lib/string.h>

#define MAG_SIZE 31

typedef struct kmem_magazine {
    list_node_t node;
    uint32_t    count;
    void       *objects[MAG_SIZE];
} kmem_magazine_t;

/// @brief A raw slab of memory (usually 1 page).
typedef struct kmem_slab {
    list_node_t node;
    uint16_t    free_count;
    uint16_t    next_free_idx;
    void       *data;
} kmem_slab_t;

typedef struct kmem_cache {
    const char *name;
    size_t      obj_size;
    uint8_t     storage_key;
    
    spinlock_t  depot_lock;
    list_node_t full_mags;
    list_node_t empty_mags;
    list_node_t partial_slabs;
    list_node_t full_slabs;
    
    /// @brief Per-CPU magazines.
    kmem_magazine_t *cpu_mags[64]; 
} kmem_cache_t;

// Bootstrap caches
static kmem_cache_t cache_cache;
static kmem_cache_t mag_cache;

/// @brief Internal: Fill a magazine by slicing a new slab or using partials.
static bool kmem_cache_refill_magazine(kmem_cache_t *cache, kmem_magazine_t *mag) {
    kmem_slab_t *slab = nullptr;
    if (!list_empty(&cache->partial_slabs)) {
        slab = list_entry(cache->partial_slabs.next, kmem_slab_t, node);
    } else {
        uint64_t pfn = pmm_alloc_page();
        if (pfn == PMM_INVALID_PFN) return false;
        
        // Apply Storage Key protection immediately.
        // Bit 4 set (0x10) means fetch protection is enabled.
        arch_set_storage_key(pmm_pfn_to_phys(pfn), cache->storage_key | 0x10);

        slab = (kmem_slab_t*)pmm_pfn_to_phys(pfn);
        slab->data = (void*)((uintptr_t)slab + sizeof(kmem_slab_t));
        slab->data = (void*)(((uintptr_t)slab->data + 7) & ~7UL);
        
        size_t available = PAGE_SIZE - ((uintptr_t)slab->data - (uintptr_t)slab);
        slab->free_count = available / cache->obj_size;
        slab->next_free_idx = 0;
        
        list_add_tail(&slab->node, &cache->partial_slabs);
    }

    while (mag->count < MAG_SIZE && slab->free_count > 0) {
        void *obj = (char*)slab->data + (slab->next_free_idx * cache->obj_size);
        mag->objects[mag->count++] = obj;
        slab->next_free_idx++;
        slab->free_count--;
    }

    if (slab->free_count == 0) {
        list_del(&slab->node);
        list_add_tail(&slab->node, &cache->full_slabs);
    }

    return mag->count > 0;
}

static bool kmem_magazine_swap(kmem_cache_t *cache, int cpu_id, bool filling) {
    irqflags_t flags;
    spin_lock_irqsave(&cache->depot_lock, &flags);

    if (filling) {
        if (list_empty(&cache->full_mags)) {
            // Need an empty magazine to refill
            if (list_empty(&cache->empty_mags)) {
                spin_unlock_irqrestore(&cache->depot_lock, flags);
                return false;
            }
            kmem_magazine_t *m = list_entry(cache->empty_mags.next, kmem_magazine_t, node);
            list_del(&m->node);
            if (!kmem_cache_refill_magazine(cache, m)) {
                list_add_tail(&m->node, &cache->empty_mags);
                spin_unlock_irqrestore(&cache->depot_lock, flags);
                return false;
            }
            list_add_tail(&m->node, &cache->full_mags);
        }
        
        list_node_t *node = cache->full_mags.next;
        list_del(node);
        if (cache->cpu_mags[cpu_id]) {
            list_add_tail(&cache->cpu_mags[cpu_id]->node, &cache->empty_mags);
        }
        cache->cpu_mags[cpu_id] = list_entry(node, kmem_magazine_t, node);
    } else {
        list_add_tail(&cache->cpu_mags[cpu_id]->node, &cache->full_mags);
        if (list_empty(&cache->empty_mags)) {
            cache->cpu_mags[cpu_id] = nullptr; 
        } else {
            list_node_t *node = cache->empty_mags.next;
            list_del(node);
            cache->cpu_mags[cpu_id] = list_entry(node, kmem_magazine_t, node);
        }
    }

    spin_unlock_irqrestore(&cache->depot_lock, flags);
    return true;
}

void* kmem_cache_alloc(kmem_cache_t *cache) {
    int cpu = 0; 
    kmem_magazine_t *mag = cache->cpu_mags[cpu];

    if (mag && mag->count > 0) {
        return mag->objects[--mag->count];
    }

    if (kmem_magazine_swap(cache, cpu, true)) {
        mag = cache->cpu_mags[cpu];
        return mag->objects[--mag->count];
    }

    return nullptr; 
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    int cpu = 0;
    kmem_magazine_t *mag = cache->cpu_mags[cpu];

    if (mag && mag->count < MAG_SIZE) {
        mag->objects[mag->count++] = obj;
        return;
    }

    kmem_magazine_swap(cache, cpu, false);
    mag = cache->cpu_mags[cpu];
    if (mag) {
        mag->objects[mag->count++] = obj;
    }
}

kmem_cache_t* kmem_cache_create(const char *name, size_t size, uint8_t storage_key) {
    kmem_cache_t *cp = (kmem_cache_t*)kmem_cache_alloc(&cache_cache);
    if (!cp) return nullptr;

    cp->name = name;
    cp->obj_size = (size + 7) & ~7UL;
    cp->storage_key = storage_key;
    spin_lock_init(&cp->depot_lock);
    list_init(&cp->full_mags);
    list_init(&cp->empty_mags);
    list_init(&cp->partial_slabs);
    list_init(&cp->full_slabs);
    memset(cp->cpu_mags, 0, sizeof(cp->cpu_mags));

    for (int i = 0; i < 4; i++) {
        kmem_magazine_t *m = (kmem_magazine_t*)kmem_cache_alloc(&mag_cache);
        if (m) {
            m->count = 0;
            list_add_tail(&m->node, &cp->empty_mags);
        }
    }

    return cp;
}

void slab_init(void) {
    cache_cache.name = "kmem_cache_t";
    cache_cache.obj_size = sizeof(kmem_cache_t);
    cache_cache.storage_key = 0x0;
    spin_lock_init(&cache_cache.depot_lock);
    list_init(&cache_cache.full_mags);
    list_init(&cache_cache.empty_mags);
    list_init(&cache_cache.partial_slabs);
    list_init(&cache_cache.full_slabs);

    mag_cache.name = "kmem_magazine_t";
    mag_cache.obj_size = sizeof(kmem_magazine_t);
    mag_cache.storage_key = 0x0;
    spin_lock_init(&mag_cache.depot_lock);
    list_init(&mag_cache.full_mags);
    list_init(&mag_cache.empty_mags);
    list_init(&mag_cache.partial_slabs);
    list_init(&mag_cache.full_slabs);

    printk("slab: magazine-slab pools populated\n");
}
