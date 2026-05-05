// SPDX-License-Identifier: Apache-2.0
// lib/rbtree.c
//
/// @brief Iterative red-black tree — insert, erase, traversal.
///
///        INVARIANTS (maintained after every public operation)
///        =====================================================
///        1. Every node is either RED or BLACK.
///        2. The root is always BLACK.
///        3. Every RED node has two BLACK children (no double-red).
///        4. Every path from a node to any leaf descendant has the same
///           number of BLACK nodes ("black-height" invariant).
///
///        COLOUR STORAGE
///        ==============
///        The colour bit lives in bit 0 of parent_and_color.  Because all
///        rb_node_t pointers on s390x are 8-byte aligned, bits [2:0] of
///        a valid node pointer are always zero — bit 0 is free for colour.
///        This saves 8 bytes per node vs. a separate colour field.
///
///        ROTATIONS
///        =========
///        Left-rotate and right-rotate are the only structural operations.
///        They update parent/child links and preserve BST ordering.
///
///        INSERT FIXUP
///        ============
///        After insertion as a RED leaf, three cases resolve double-red:
///          Case 1: Uncle is RED  → recolour parent, uncle, grandparent.
///          Case 2: Uncle is BLACK, node is "inner" child → rotate parent.
///          Case 3: Uncle is BLACK, node is "outer" child → rotate grandparent.
///
///        DELETE FIXUP
///        ============
///        Splice out the node (or its in-order successor when it has two
///        children).  If the spliced node was BLACK, propagate a "double-
///        black" upward through four cases until the root is reached or the
///        double-black is resolved.

#include <lib/rbtree.h>

// ---------------------------------------------------------------------------
// Internal helpers: rotate
// ---------------------------------------------------------------------------

/// @brief Rotate the subtree rooted at 'x' left.
///        y = x->right becomes the new subtree root.
///        x->right ← y->left; y->left ← x; update parent links.
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

/// @brief Rotate the subtree rooted at 'x' right.
///        y = x->left becomes the new subtree root.
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
// rb_link_node
// ---------------------------------------------------------------------------

void rb_link_node(rb_node_t *node, rb_node_t *parent, rb_node_t **link) {
    node->left             = nullptr;
    node->right            = nullptr;
    rb_set_parent_color(node, parent, RB_RED);
    *link = node;
}

// ---------------------------------------------------------------------------
// rb_insert_fixup
// ---------------------------------------------------------------------------

void rb_insert_fixup(rb_root_t *tree, rb_node_t *node) {
    rb_node_t *parent, *gparent, *uncle;

    while (true) {
        parent = rb_parent(node);

        // If node is the root, paint it black and we're done.
        if (!parent) {
            rb_set_color(node, RB_BLACK);
            tree->root = node;
            return;
        }

        // If parent is black, the invariant holds — done.
        if (rb_is_black(parent))
            return;

        // Parent is red — grandparent must exist (root is always black).
        gparent = rb_parent(parent);

        if (parent == gparent->left) {
            uncle = gparent->right;

            if (uncle && rb_is_red(uncle)) {
                // Case 1: Uncle is RED.  Recolour and push problem upward.
                rb_set_color(parent,  RB_BLACK);
                rb_set_color(uncle,   RB_BLACK);
                rb_set_color(gparent, RB_RED);
                node = gparent;
                continue;
            }

            if (node == parent->right) {
                // Case 2: Triangle (inner child).  Rotate parent left to
                // convert to Case 3.
                rotate_left(tree, parent);
                // After rotation, old parent is now node's left child.
                rb_node_t *tmp = node;
                node   = parent;
                parent = tmp;
            }

            // Case 3: Line (outer child).  Rotate grandparent right and
            // recolour.
            rb_set_color(parent,  RB_BLACK);
            rb_set_color(gparent, RB_RED);
            rotate_right(tree, gparent);
            return;
        } else {
            // Mirror: parent is gparent->right.
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
                rb_node_t *tmp = node;
                node   = parent;
                parent = tmp;
            }

            rb_set_color(parent,  RB_BLACK);
            rb_set_color(gparent, RB_RED);
            rotate_left(tree, gparent);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// rb_erase helpers
// ---------------------------------------------------------------------------

/// @brief Replace 'u' in the tree with 'v' (v may be nullptr).
///        Used during erase to splice out a node.
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

/// @brief Fix double-black after erase spliced out a BLACK node.
///        'x' is the child that replaced the spliced node (may be nullptr,
///        in which case 'xp' is its parent).
static void erase_fixup(rb_root_t *tree, rb_node_t *x, rb_node_t *xp) {
    rb_node_t *sibling;

    while (x != tree->root && (!x || rb_is_black(x))) {
        if (x == (xp ? xp->left : nullptr)) {
            sibling = xp ? xp->right : nullptr;

            if (sibling && rb_is_red(sibling)) {
                // Case 1: Sibling is red — rotate to convert to case 2-4.
                rb_set_color(sibling, RB_BLACK);
                rb_set_color(xp,     RB_RED);
                rotate_left(tree, xp);
                sibling = xp->right;
            }

            if ((!sibling->left  || rb_is_black(sibling->left)) &&
                (!sibling->right || rb_is_black(sibling->right))) {
                // Case 2: Sibling's children are both black — push up.
                rb_set_color(sibling, RB_RED);
                x  = xp;
                xp = rb_parent(x);
            } else {
                if (!sibling->right || rb_is_black(sibling->right)) {
                    // Case 3: Sibling's right child is black — rotate sibling.
                    if (sibling->left)
                        rb_set_color(sibling->left, RB_BLACK);
                    rb_set_color(sibling, RB_RED);
                    rotate_right(tree, sibling);
                    sibling = xp->right;
                }
                // Case 4: Sibling's right child is red — rotate xp.
                rb_set_parent_color(sibling, rb_parent(xp), rb_color(xp));
                rb_set_color(xp, RB_BLACK);
                if (sibling->right)
                    rb_set_color(sibling->right, RB_BLACK);
                rotate_left(tree, xp);
                x = tree->root; // Done.
            }
        } else {
            // Mirror.
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
                x  = xp;
                xp = rb_parent(x);
            } else {
                if (!sibling->left || rb_is_black(sibling->left)) {
                    if (sibling->right)
                        rb_set_color(sibling->right, RB_BLACK);
                    rb_set_color(sibling, RB_RED);
                    rotate_left(tree, sibling);
                    sibling = xp->left;
                }
                rb_set_parent_color(sibling, rb_parent(xp), rb_color(xp));
                rb_set_color(xp, RB_BLACK);
                if (sibling->left)
                    rb_set_color(sibling->left, RB_BLACK);
                rotate_right(tree, xp);
                x = tree->root;
            }
        }
    }
    if (x) rb_set_color(x, RB_BLACK);
}

// ---------------------------------------------------------------------------
// rb_erase
// ---------------------------------------------------------------------------

void rb_erase(rb_root_t *tree, rb_node_t *node) {
    rb_node_t *child, *parent;
    unsigned   orig_color;

    if (!node->left) {
        // Node has no left child: replace with right child (may be nullptr).
        orig_color = rb_color(node);
        child      = node->right;
        parent     = rb_parent(node);
        transplant(tree, node, child);
    } else if (!node->right) {
        // Node has no right child.
        orig_color = rb_color(node);
        child      = node->left;
        parent     = rb_parent(node);
        transplant(tree, node, child);
    } else {
        // Node has two children: find in-order successor (leftmost in right subtree).
        rb_node_t *successor = node->right;
        while (successor->left)
            successor = successor->left;

        orig_color = rb_color(successor);
        child      = successor->right;

        if (rb_parent(successor) == node) {
            // Successor is the direct right child.
            parent = successor;
        } else {
            // Splice out the successor.
            parent = rb_parent(successor);
            transplant(tree, successor, child);
            successor->right = node->right;
            rb_set_parent(successor->right, successor);
        }

        // Replace node with successor.
        transplant(tree, node, successor);
        successor->left = node->left;
        rb_set_parent(successor->left, successor);
        rb_set_parent_color(successor, rb_parent(successor), rb_color(node));
    }

    // If we removed a BLACK node, fixup the double-black violation.
    if (orig_color == RB_BLACK)
        erase_fixup(tree, child, parent);
}

// ---------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------

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
    while (p && node == p->right) {
        node = p;
        p    = rb_parent(p);
    }
    return p;
}

rb_node_t *rb_prev(const rb_node_t *node) {
    if (node->left) {
        rb_node_t *n = node->left;
        while (n->right) n = n->right;
        return n;
    }
    rb_node_t *p = rb_parent(node);
    while (p && node == p->left) {
        node = p;
        p    = rb_parent(p);
    }
    return p;
}
