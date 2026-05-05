# Memory Management

**Document Revision:** 26h1.0

---

ZXFoundation's memory management is organized in four layers:

```
┌──────────────────────────────────────────┐
│  kmalloc / kfree  (general-purpose)      │
├──────────────────────────────────────────┤
│  Slab allocator   (fixed-size caches)    │
├──────────────────────────────────────────┤
│  VMM              (virtual address space)│
├──────────────────────────────────────────┤
│  PMM              (physical frames)      │
├──────────────────────────────────────────┤
│  MMU              (hardware DAT tables)  │
└──────────────────────────────────────────┘
```

| Page | Contents |
|------|----------|
| [PMM](pmm.md) | Zone-aware buddy allocator, page descriptors |
| [VMM](vmm.md) | Virtual address space, VMA red-black tree, vmalloc |
| [Slab & Kmalloc](slab.md) | Fixed-size object caches, general allocator |
