# Red-Black Tree

**Document Revision:** 26h1.1  
**Source:** `lib/rbtree.c`, `include/lib/rbtree.h`

---

## 1. Overview

ZXFoundation provides a layered intrusive red-black tree library. Each layer is a strict superset of the one below it; callers of lower layers require no modification when higher layers are added.

| Layer | Type | Concurrency |
|-------|------|-------------|
| 0 — Core | `rb_root_t` | None (caller-managed) |
| 1 — Augmented | `rb_root_aug_t` | None (caller-managed) |
| 2 — RCU-protected | `rcu_rb_root_t` | Lockless readers, serialised writers |
| 2A — RCU-augmented | `rcu_rb_root_aug_t` | Lockless readers, serialised writers + propagation |
| 3 — Per-CPU cached | `rb_pcpu_cache_t` | O(1) fast path per CPU |

The tree is **intrusive**: the caller embeds `rb_node_t` (or `rb_node_aug_t`) inside its own struct and recovers the container with `rb_entry()`. The colour bit is packed into bit 0 of the parent pointer, keeping `rb_node_t` at exactly 24 bytes.

---

## 2. Node Layout

```
rb_node_t (24 bytes)
┌──────────────────────────┐
│ left             (8 B)   │  pointer to left child
│ right            (8 B)   │  pointer to right child
│ parent_and_color (8 B)   │  parent ptr | colour bit (bit 0)
└──────────────────────────┘

rb_node_aug_t (32 bytes)
┌──────────────────────────┐
│ node  (rb_node_t, 24 B)  │  must be at offset 0 — cast-compatible
│ subtree_max_gap  (8 B)   │  maintained by propagate callback
└──────────────────────────┘
```

All `rb_node_t` pointers are 8-byte aligned on s390x, so bit 0 of any valid pointer is always zero and is free for colour storage.

---

## 3. Layer 0 — Core

The core layer provides O(log n) insert, erase, and traversal with no locking. All operations are iterative (bounded stack depth).

### Insert Protocol

```
walk tree → find (parent, link)
rb_link_node(node, parent, link)
rb_insert_fixup(tree, node)
```

### Erase

```
rb_erase(tree, node)
```

### Traversal

```
rb_first(tree)   →  minimum node
rb_last(tree)    →  maximum node
rb_next(node)    →  in-order successor
rb_prev(node)    →  in-order predecessor

rb_for_each(pos, tree)
rb_for_each_entry(pos, tree, member)
```

### Container Recovery

```
rb_entry(ptr, type, member)
rb_entry_safe(ptr, type, member)   ← null-safe variant
```

---

## 4. Layer 1 — Augmented

The augmented layer adds a `rb_aug_callbacks_t` to `rb_root_aug_t`. After every structural change (insert, erase, rotation), `propagate` is invoked bottom-up from the affected node to the root.

Callers embed `rb_node_aug_t` instead of `rb_node_t` and maintain a per-node subtree aggregate in `subtree_max_gap`.

### Callbacks

```
propagate(node)          recompute node->subtree_max_gap from children
copy(dst, src)           copy aggregate when successor replaces deleted node
```

`copy` is required when the two-child erase case physically moves the successor into the deleted node's position. Without it the successor would carry a stale aggregate into its new location.

### Propagation Order

```
structural change at node L
        │
        ▼
propagate(L)          ← children already up-to-date
        │
        ▼
propagate(parent(L))
        │
        ▼
        …  (up to root)
```

### API

```
rb_root_aug_t root = RB_ROOT_AUG_INIT(&my_callbacks);

rb_insert_aug(&root, node, parent, link);
rb_erase_aug(&root, node);
```

---

## 5. Layer 2 — RCU-Protected

`rcu_rb_root_t` wraps `rb_root_t` with a write-side spinlock. Readers use the RCU lockless path; writers serialise through the lock and publish pointer updates via `rcu_assign_pointer()`.

### Concurrency Model

```
Reader                          Writer
──────────────────────          ──────────────────────────────
rcu_read_lock()                 spin_lock_irqsave(&root->lock)
  node = rcu_rb_find(...)         rb_erase(...)
  // use node safely              rcu_assign_pointer(root, ...)
rcu_read_unlock()               spin_unlock_irqrestore(...)
                                call_rcu(head, free_fn)
```

`rcu_assign_pointer()` issues `smp_mb()` before the store. `rcu_dereference()` issues a compiler barrier after each pointer load, preventing the compiler from collapsing multiple loads of the same pointer.

### Erase and Grace Period

```
rcu_rb_erase(root, node, head, free_fn)
    ├─ unlink node under lock
    ├─ rcu_assign_pointer(...)   ← publish updated tree
    └─ call_rcu(head, free_fn)   ← free after grace period
```

---

## 6. Layer 2A — RCU-Augmented

`rcu_rb_root_aug_t` composes Layer 1 and Layer 2 under a single write lock. The lock covers both rebalancing and aggregate propagation atomically.

**Key invariant:** readers always observe a tree where `subtree_max_gap` is consistent with the pointer structure they see, because both are updated under the same lock before `rcu_assign_pointer()` publishes the result.

### Gap Search

`rcu_rb_aug_find_gap()` performs an O(log n) free-gap search by pruning subtrees whose `subtree_max_gap` is smaller than the requested size:

```
find_gap(root, size, align, lo, hi):

  cursor = lo
  n = root

  while n:
    if n.left.subtree_max_gap >= size:
      descend left            ← prune right subtree entirely
      continue

    aligned = align_up(cursor, align)
    if aligned + size <= n.start:
      return aligned          ← gap found left of n

    cursor = max(cursor, n.end)
    n = n.right               ← no gap left of n; try right

  aligned = align_up(cursor, align)
  if aligned + size <= hi:
    return aligned            ← gap after last node

  return 0                    ← no gap found
```

This replaces the former O(n) linear scan. The caller supplies `node_start` and `node_end` accessors, making the search generic over any interval type.

### API

```
rcu_rb_root_aug_t root = RCU_RB_ROOT_AUG_INIT(&my_callbacks);

rcu_rb_aug_insert(&root, node, parent, link);
rcu_rb_aug_erase(&root, node, head, free_fn);

// Under lock or rcu_read_lock():
uint64_t addr = rcu_rb_aug_find_gap(&root, size, align, lo, hi,
                                    node_start_fn, node_end_fn);
```

---

## 7. Layer 3 — Per-CPU Cached

`rb_pcpu_cache_t` is a per-CPU array of `(hint, hint_key)` pairs. On a cache hit the search returns in O(1) without touching the tree.

```
rb_find_cached(root, cache, cmp, arg):

  cpu  = current_cpu()
  hint = cache[cpu].hint

  if hint != NULL && cmp(hint, arg) == 0:
    return hint               ← O(1) fast path

  // full O(log n) walk
  result = tree_walk(root, cmp, arg)
  cache[cpu].hint = result
  return result
```

The hint is **opportunistic** — it may be stale. The comparator validates it before the result is returned.

### Invalidation

```
rb_cache_invalidate(cache, node)        O(MAX_CPUS) — call before erase
rb_cache_invalidate_local(cache)        O(1)        — current CPU only
```

`rb_cache_invalidate()` must be called before `rb_erase()` or `rcu_rb_aug_erase()` on any node in a cached tree to prevent dangling hint pointers.

---

## 8. RB-Tree Invariants

The implementation maintains the four standard invariants after every operation:

1. Every node is RED or BLACK.
2. The root is BLACK.
3. Every RED node has two BLACK children.
4. Every path from a node to a null leaf contains the same number of BLACK nodes.

Insert fixup resolves double-red violations with at most 2 rotations and O(log n) recolourings. Erase fixup resolves double-black violations with at most 3 rotations and O(log n) recolourings. Recolourings do not change pointer structure and are invisible to RCU readers.

---

## 9. Constraints

- `rb_node_aug_t::node` must be at offset 0. The `_Static_assert` in the header enforces this.
- `rb_aug_callbacks_t::copy` may be `nullptr` only if the caller guarantees no two-child erase will occur. For general use it must be provided.
- `rb_cache_invalidate()` must be called **before** erasing a node from any cached tree.
- `rcu_rb_aug_find_gap()` may be called under `rcu_read_lock()` for a best-effort result, or under the write lock for a guaranteed-current result.
- `synchronize_rcu()` may block indefinitely if a CPU never reports a quiescent state. Callers of `rcu_rb_aug_erase()` must ensure `rcu_report_qs()` is called from the idle loop and scheduler tick.
