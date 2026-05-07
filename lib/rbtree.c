// SPDX-License-Identifier: Apache-2.0
// lib/rbtree.c
//
/// @brief Red-black tree — core, augmented, RCU-protected, RCU-augmented,
///        and per-CPU cached implementations.

#include <arch/s390x/cpu/processor.h>
#include <zxfoundation/percpu.h>
#include <lib/rbtree.h>

static void rotate_left(rb_root_t *tree, rb_node_t *x) {
    rb_node_t *y  = x->right;
    rb_node_t *px = rb_parent(x);

    x->right = y->left;
    if (y->left)
        rb_set_parent(y->left, x);

    rb_set_parent(y, px);
    if (!px)
        tree->root = y;
    else if (x == px->left)
        px->left = y;
    else
        px->right = y;

    y->left = x;
    rb_set_parent(x, y);
}

static void rotate_right(rb_root_t *tree, rb_node_t *x) {
    rb_node_t *y  = x->left;
    rb_node_t *px = rb_parent(x);

    x->left = y->right;
    if (y->right)
        rb_set_parent(y->right, x);

    rb_set_parent(y, px);
    if (!px)
        tree->root = y;
    else if (x == px->right)
        px->right = y;
    else
        px->left = y;

    y->right = x;
    rb_set_parent(x, y);
}

// ---------------------------------------------------------------------------
// Layer 0 — Core
// ---------------------------------------------------------------------------

void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **link) {
    node->left  = nullptr;
    node->right = nullptr;
    rb_set_parent_color(node, parent, RB_RED);
    *link = node;
}

void rb_insert_fixup(rb_root_t *tree, rb_node_t *node) {
    rb_node_t *parent, *gparent, *uncle;

    while (true) {
        parent = rb_parent(node);

        if (!parent) {
            rb_set_color(node, RB_BLACK);
            tree->root = node;
            return;
        }
        if (rb_is_black(parent))
            return;

        gparent = rb_parent(parent);

        if (parent == gparent->left) {
            uncle = gparent->right;
            if (uncle && rb_is_red(uncle)) {
                // Case 1: uncle RED — recolour, push problem upward.
                rb_set_color(parent,  RB_BLACK);
                rb_set_color(uncle,   RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }
            if (node == parent->right) {
                // Case 2: triangle — rotate to convert to Case 3.
                rotate_left(tree, parent);
                rb_node_t *tmp = node; node = parent; parent = tmp;
            }
            // Case 3: line — rotate grandparent and recolour.
            rb_set_color(parent,  RB_BLACK);
            rb_set_color(gparent, RB_RED);
            rotate_right(tree, gparent);
            return;
        } else {
            uncle = gparent->left;
            if (uncle && rb_is_red(uncle)) {
                rb_set_color(parent,  RB_BLACK);
                rb_set_color(uncle,   RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }
            if (node == parent->left) {
                rotate_right(tree, parent);
                rb_node_t *tmp = node; node = parent; parent = tmp;
            }
            rb_set_color(parent,  RB_BLACK);
            rb_set_color(gparent, RB_RED);
            rotate_left(tree, gparent);
            return;
        }
    }
}

/// @brief Replace u with v in the tree (v may be nullptr).
static void transplant(rb_root_t *tree, rb_node_t *u, rb_node_t *v) {
    rb_node_t *pu = rb_parent(u);
    if (!pu)
        tree->root = v;
    else if (u == pu->left)
        pu->left = v;
    else
        pu->right = v;
    if (v)
        rb_set_parent(v, pu);
}

/// @brief Restore RB invariants after splicing out a BLACK node.
static void erase_fixup(rb_root_t *tree, rb_node_t *x, rb_node_t *xp) {
    rb_node_t *sibling;

    while (x != tree->root && (!x || rb_is_black(x))) {
        if (x == (xp ? xp->left : nullptr)) {
            sibling = xp ? xp->right : nullptr;
            if (sibling && rb_is_red(sibling)) {
                rb_set_color(sibling, RB_BLACK);
                rb_set_color(xp,     RB_RED);
                rotate_left(tree, xp);
                sibling = xp->right;
            }
            if ((!sibling->left  || rb_is_black(sibling->left)) &&
                (!sibling->right || rb_is_black(sibling->right))) {
                rb_set_color(sibling, RB_RED);
                x = xp; xp = rb_parent(x);
            } else {
                if (!sibling->right || rb_is_black(sibling->right)) {
                    if (sibling->left) rb_set_color(sibling->left, RB_BLACK);
                    rb_set_color(sibling, RB_RED);
                    rotate_right(tree, sibling);
                    sibling = xp->right;
                }
                rb_set_parent_color(sibling, rb_parent(xp), rb_color(xp));
                rb_set_color(xp, RB_BLACK);
                if (sibling->right) rb_set_color(sibling->right, RB_BLACK);
                rotate_left(tree, xp);
                x = tree->root;
            }
        } else {
            sibling = xp ? xp->left : nullptr;
            if (sibling && rb_is_red(sibling)) {
                rb_set_color(sibling, RB_BLACK);
                rb_set_color(xp,     RB_RED);
                rotate_right(tree, xp);
                sibling = xp->left;
            }
            if ((!sibling->right || rb_is_black(sibling->right)) &&
                (!sibling->left  || rb_is_black(sibling->left))) {
                rb_set_color(sibling, RB_RED);
                x = xp; xp = rb_parent(x);
            } else {
                if (!sibling->left || rb_is_black(sibling->left)) {
                    if (sibling->right) rb_set_color(sibling->right, RB_BLACK);
                    rb_set_color(sibling, RB_RED);
                    rotate_left(tree, sibling);
                    sibling = xp->left;
                }
                rb_set_parent_color(sibling, rb_parent(xp), rb_color(xp));
                rb_set_color(xp, RB_BLACK);
                if (sibling->left) rb_set_color(sibling->left, RB_BLACK);
                rotate_right(tree, xp);
                x = tree->root;
            }
        }
    }
    if (x) rb_set_color(x, RB_BLACK);
}

void rb_erase(rb_root_t *tree, rb_node_t *node) {
    rb_node_t *child, *parent;
    unsigned   orig_color;

    if (!node->left) {
        orig_color = rb_color(node);
        child      = node->right;
        parent     = rb_parent(node);
        transplant(tree, node, child);
    } else if (!node->right) {
        orig_color = rb_color(node);
        child      = node->left;
        parent     = rb_parent(node);
        transplant(tree, node, child);
    } else {
        rb_node_t *successor = node->right;
        while (successor->left)
            successor = successor->left;

        orig_color = rb_color(successor);
        child      = successor->right;

        if (rb_parent(successor) == node) {
            parent = successor;
        } else {
            parent = rb_parent(successor);
            transplant(tree, successor, child);
            successor->right = node->right;
            rb_set_parent(successor->right, successor);
        }
        transplant(tree, node, successor);
        successor->left = node->left;
        rb_set_parent(successor->left, successor);
        rb_set_parent_color(successor, rb_parent(successor), rb_color(node));
    }

    if (orig_color == RB_BLACK)
        erase_fixup(tree, child, parent);
}

rb_node_t *rb_first(const rb_root_t *tree) {
    rb_node_t *n = tree->root;
    if (!n) return nullptr;
    while (n->left) n = n->left;
    return n;
}

rb_node_t *rb_last(const rb_root_t *tree) {
    rb_node_t *n = tree->root;
    if (!n) return nullptr;
    while (n->right) n = n->right;
    return n;
}

rb_node_t *rb_next(const rb_node_t *node) {
    if (node->right) {
        rb_node_t *n = node->right;
        while (n->left) n = n->left;
        return n;
    }
    rb_node_t *p = rb_parent(node);
    while (p && node == p->right) { node = p; p = rb_parent(p); }
    return p;
}

rb_node_t *rb_prev(const rb_node_t *node) {
    if (node->left) {
        rb_node_t *n = node->left;
        while (n->right) n = n->right;
        return n;
    }
    rb_node_t *p = rb_parent(node);
    while (p && node == p->left) { node = p; p = rb_parent(p); }
    return p;
}

// ---------------------------------------------------------------------------
// Layer 1 — Augmented
// ---------------------------------------------------------------------------

/// @brief Propagate aggregates from node up to the root.
static void aug_propagate_up(const rb_aug_callbacks_t *cb, rb_node_t *node) {
    while (node) {
        cb->propagate(node);
        node = rb_parent(node);
    }
}

/// @brief Augmented rotate_left: rotate then re-propagate the two affected nodes.
///        After rotating x left, x moves down (its subtree changed) and y
///        moves up (it gained x as a child).  x must be propagated first.
static void aug_rotate_left(rb_root_aug_t *root, rb_node_t *x) {
    rotate_left(&root->base, x);
    root->cb->propagate(x);
    root->cb->propagate(rb_parent(x));
}

/// @brief Augmented rotate_right: symmetric to aug_rotate_left.
static void aug_rotate_right(rb_root_aug_t *root, rb_node_t *x) {
    rotate_right(&root->base, x);
    root->cb->propagate(x);
    root->cb->propagate(rb_parent(x));
}

void rb_insert_aug(rb_root_aug_t *root, rb_node_t *node,
                   rb_node_t *parent, rb_node_t **link) {
    rb_link_node(node, parent, link);
    root->cb->propagate(node);
    aug_propagate_up(root->cb, parent);

    rb_node_t *gparent, *uncle;
    while (true) {
        parent = rb_parent(node);
        if (!parent) {
            rb_set_color(node, RB_BLACK);
            root->base.root = node;
            return;
        }
        if (rb_is_black(parent))
            return;
        gparent = rb_parent(parent);

        if (parent == gparent->left) {
            uncle = gparent->right;
            if (uncle && rb_is_red(uncle)) {
                // Recolour only — no pointer changes, no propagation needed.
                rb_set_color(parent,  RB_BLACK);
                rb_set_color(uncle,   RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }
            if (node == parent->right) {
                aug_rotate_left(root, parent);
                rb_node_t *tmp = node; node = parent; parent = tmp;
            }
            rb_set_color(parent,  RB_BLACK);
            rb_set_color(gparent, RB_RED);
            aug_rotate_right(root, gparent);
            return;
        } else {
            uncle = gparent->left;
            if (uncle && rb_is_red(uncle)) {
                rb_set_color(parent,  RB_BLACK);
                rb_set_color(uncle,   RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }
            if (node == parent->left) {
                aug_rotate_right(root, parent);
                rb_node_t *tmp = node; node = parent; parent = tmp;
            }
            rb_set_color(parent,  RB_BLACK);
            rb_set_color(gparent, RB_RED);
            aug_rotate_left(root, gparent);
            return;
        }
    }
}

void rb_erase_aug(rb_root_aug_t *root, rb_node_t *node) {
    rb_node_t *fixup_start;
    if (!node->left || !node->right) {
        fixup_start = rb_parent(node);
    } else {
        rb_node_t *s = node->right;
        while (s->left) s = s->left;
        fixup_start = (rb_parent(s) == node) ? s : rb_parent(s);

        if (root->cb->copy)
            root->cb->copy(s, node);
    }

    rb_erase(&root->base, node);
    aug_propagate_up(root->cb, fixup_start);
}

// ---------------------------------------------------------------------------
// Layer 2 — RCU-protected (plain, non-augmented)
// ---------------------------------------------------------------------------

rb_node_t *rcu_rb_find(const rcu_rb_root_t *root,
                        int (*cmp)(const rb_node_t *, const void *),
                        const void *arg) {
    const rb_node_t *n = rcu_dereference(root->base.root);
    while (n) {
        int c = cmp(n, arg);
        if      (c > 0) n = rcu_dereference(n->left);
        else if (c < 0) n = rcu_dereference(n->right);
        else            return (rb_node_t *)n;
    }
    return nullptr;
}

void rcu_rb_insert(rcu_rb_root_t *root, rb_node_t *node,
                   rb_node_t *parent, rb_node_t **link) {
    irqflags_t flags;
    spin_lock_irqsave(&root->lock, &flags);

    rb_link_node(node, parent, link);
    rb_insert_fixup(&root->base, node);
    rcu_assign_pointer(root->base.root, root->base.root);

    spin_unlock_irqrestore(&root->lock, flags);
}

void rcu_rb_erase(rcu_rb_root_t *root, rb_node_t *node,
                  rcu_head_t *head, void (*free_fn)(rcu_head_t *)) {
    irqflags_t flags;
    spin_lock_irqsave(&root->lock, &flags);
    rb_erase(&root->base, node);
    rcu_assign_pointer(root->base.root, root->base.root);
    spin_unlock_irqrestore(&root->lock, flags);

    // Defer the actual free until all pre-existing RCU readers have finished.
    call_rcu(head, free_fn);
}

// ---------------------------------------------------------------------------
// Layer 2A — RCU-augmented
// ---------------------------------------------------------------------------

rb_node_t *rcu_rb_aug_find(const rcu_rb_root_aug_t *root,
                            int (*cmp)(const rb_node_t *, const void *),
                            const void *arg) {
    const rb_node_t *n = rcu_dereference(root->aug.base.root);
    while (n) {
        int c = cmp(n, arg);
        if      (c > 0) n = rcu_dereference(n->left);
        else if (c < 0) n = rcu_dereference(n->right);
        else            return (rb_node_t *)n;
    }
    return nullptr;
}

void rcu_rb_aug_insert(rcu_rb_root_aug_t *root, rb_node_t *node,
                       rb_node_t *parent, rb_node_t **link) {
    irqflags_t flags;
    spin_lock_irqsave(&root->lock, &flags);

    rb_insert_aug(&root->aug, node, parent, link);
    rcu_assign_pointer(root->aug.base.root, root->aug.base.root);

    spin_unlock_irqrestore(&root->lock, flags);
}

void rcu_rb_aug_erase(rcu_rb_root_aug_t *root, rb_node_t *node,
                      rcu_head_t *head, void (*free_fn)(rcu_head_t *)) {
    irqflags_t flags;
    spin_lock_irqsave(&root->lock, &flags);

    rb_erase_aug(&root->aug, node);
    rcu_assign_pointer(root->aug.base.root, root->aug.base.root);

    spin_unlock_irqrestore(&root->lock, flags);
    call_rcu(head, free_fn);
}

uint64_t rcu_rb_aug_find_gap(const rcu_rb_root_aug_t *root,
                              uint64_t size, uint64_t align,
                              uint64_t lo,   uint64_t hi,
                              uint64_t (*node_start)(const rb_node_t *),
                              uint64_t (*node_end  )(const rb_node_t *)) {
    const rb_node_t *n = rcu_dereference(root->aug.base.root);
    if (!n) {
        uint64_t aligned = (lo + align - 1) & ~(align - 1);
        return (aligned + size <= hi) ? aligned : 0;
    }

    uint64_t cursor = lo;

    // Phase 1: descend to the first node we need to check.
    while (true) {
        if (n->left) {
            const rb_node_aug_t *la = (const rb_node_aug_t *)n->left;
            if (la->subtree_max_end > cursor) {
                n = rcu_dereference(n->left);
                continue;
            }
        }
        break; // left subtree entirely behind cursor, check n
    }

    // Phase 2: in-order walk from n.
    while (n) {
        uint64_t ns = node_start(n);
        if (ns >= hi) break;

        uint64_t aligned = (cursor + align - 1) & ~(align - 1);
        if (aligned + size <= ns && aligned + size <= hi)
            return aligned;

        uint64_t ne = node_end(n);
        if (ne > cursor) cursor = ne;

        n = rb_next(n);
    }

    uint64_t aligned = (cursor + align - 1) & ~(align - 1);
    return (aligned + size <= hi) ? aligned : 0;
}

// ---------------------------------------------------------------------------
// Layer 3 — Per-CPU hint cache with generation-counter invalidation
// ---------------------------------------------------------------------------

rb_node_t *rb_find_cached(const rb_root_t *root,
                           rb_cache_gen_t  *gen,
                           rb_pcpu_cache_t  cache[],
                           int (*cmp)(const rb_node_t *, const void *),
                           const void *arg) {
    uint32_t cpu     = arch_smp_processor_id();
    uint64_t cur_gen = (uint64_t)atomic64_read(&gen->val);

    if (cache[cpu].hint && cache[cpu].gen == cur_gen &&
        cmp(cache[cpu].hint, arg) == 0)
        return cache[cpu].hint;

    rb_node_t *n = root->root;
    while (n) {
        int c = cmp(n, arg);
        if      (c > 0) n = n->left;
        else if (c < 0) n = n->right;
        else {
            cache[cpu].hint = n;
            cache[cpu].gen  = cur_gen;
            return n;
        }
    }
    cache[cpu].hint = nullptr;
    return nullptr;
}

rb_node_t *rcu_rb_aug_find_cached(const rcu_rb_root_aug_t *root,
                                   rb_cache_gen_t          *gen,
                                   rb_pcpu_cache_t          cache[],
                                   int (*cmp)(const rb_node_t *, const void *),
                                   const void *arg) {
    uint32_t cpu     = arch_smp_processor_id();
    uint64_t cur_gen = (uint64_t)atomic64_read(&gen->val);

    if (cache[cpu].hint && cache[cpu].gen == cur_gen &&
        cmp(cache[cpu].hint, arg) == 0)
        return cache[cpu].hint;

    const rb_node_t *n = rcu_dereference(root->aug.base.root);
    while (n) {
        int c = cmp(n, arg);
        if      (c > 0) n = rcu_dereference(n->left);
        else if (c < 0) n = rcu_dereference(n->right);
        else {
            cache[cpu].hint = (rb_node_t *)n;
            cache[cpu].gen  = cur_gen;
            return (rb_node_t *)n;
        }
    }
    cache[cpu].hint = nullptr;
    return nullptr;
}

void rb_cache_invalidate_local(rb_pcpu_cache_t cache[]) {
    cache[arch_smp_processor_id()].hint = nullptr;
}
