// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Intrusive red-black interval tree for inclusive intervals [start, end].
 * Duplicate intervals are allowed. The caller owns nodes and must serialize
 * access to the tree; callbacks must not mutate it while visiting overlaps.
 */
enum interval_tree_color {
    INTERVAL_TREE_RED,
    INTERVAL_TREE_BLACK
};

struct interval_tree_node {
    struct interval_tree_node *left;
    struct interval_tree_node *right;
    struct interval_tree_node *parent;
    uint64_t                   start;
    uint64_t                   end;
    uint64_t                   max_end;
    uint64_t                   sequence;
    enum interval_tree_color color;
};

struct interval_tree {
    struct interval_tree_node *root;
    struct interval_tree_node  nil;
    uint64_t                   next_sequence;
};

struct interval_tree_overlap_iterator {
    uint64_t start;
    uint64_t end;
    uint64_t max_sequence;
    uint64_t last_start;
    uint64_t last_end;
    uint64_t last_sequence;
    size_t   nodes_visited;
    bool     valid;
};

typedef bool (*interval_tree_visit_fn)(
    struct interval_tree_node *node,
    void                      *private_data);

static inline void
interval_tree_init(struct interval_tree *tree)
{
    tree->nil.left      = &tree->nil;
    tree->nil.right     = &tree->nil;
    tree->nil.parent    = &tree->nil;
    tree->nil.start     = 0;
    tree->nil.end       = 0;
    tree->nil.max_end   = 0;
    tree->nil.color     = INTERVAL_TREE_BLACK;
    tree->root          = &tree->nil;
    tree->next_sequence = 1;
} /* interval_tree_init */

static inline bool
interval_tree_empty(const struct interval_tree *tree)
{
    return tree->root == &tree->nil;
} /* interval_tree_empty */

/* Returns false for an inverted interval and leaves the node untouched. */
static inline bool
interval_tree_node_init(
    struct interval_tree_node *node,
    uint64_t                   start,
    uint64_t                   end)
{
    if (start > end) {
        return false;
    }

    node->left     = NULL;
    node->right    = NULL;
    node->parent   = NULL;
    node->start    = start;
    node->end      = end;
    node->max_end  = end;
    node->sequence = 0;
    node->color    = INTERVAL_TREE_RED;
    return true;
} /* interval_tree_node_init */

static inline int
interval_tree_compare(
    const struct interval_tree_node *a,
    const struct interval_tree_node *b)
{
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    if (a->end != b->end) {
        return a->end < b->end ? -1 : 1;
    }
    if (a->sequence != b->sequence) {
        return a->sequence < b->sequence ? -1 : 1;
    }
    return 0;
} /* interval_tree_compare */

static inline void
interval_tree_recompute(
    struct interval_tree      *tree,
    struct interval_tree_node *node)
{
    uint64_t max_end = node->end;

    if (node->left != &tree->nil && node->left->max_end > max_end) {
        max_end = node->left->max_end;
    }
    if (node->right != &tree->nil && node->right->max_end > max_end) {
        max_end = node->right->max_end;
    }
    node->max_end = max_end;
} /* interval_tree_recompute */

static inline void
interval_tree_update_upwards(
    struct interval_tree      *tree,
    struct interval_tree_node *node)
{
    while (node != &tree->nil) {
        interval_tree_recompute(tree, node);
        node = node->parent;
    }
} /* interval_tree_update_upwards */

static inline void
interval_tree_left_rotate(
    struct interval_tree      *tree,
    struct interval_tree_node *x)
{
    struct interval_tree_node *y = x->right;

    x->right = y->left;
    if (y->left != &tree->nil) {
        y->left->parent = x;
    }

    y->parent = x->parent;
    if (x->parent == &tree->nil) {
        tree->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }

    y->left   = x;
    x->parent = y;
    interval_tree_recompute(tree, x);
    interval_tree_recompute(tree, y);
} /* interval_tree_left_rotate */

static inline void
interval_tree_right_rotate(
    struct interval_tree      *tree,
    struct interval_tree_node *y)
{
    struct interval_tree_node *x = y->left;

    y->left = x->right;
    if (x->right != &tree->nil) {
        x->right->parent = y;
    }

    x->parent = y->parent;
    if (y->parent == &tree->nil) {
        tree->root = x;
    } else if (y == y->parent->right) {
        y->parent->right = x;
    } else {
        y->parent->left = x;
    }

    x->right  = y;
    y->parent = x;
    interval_tree_recompute(tree, y);
    interval_tree_recompute(tree, x);
} /* interval_tree_right_rotate */

static inline void
interval_tree_insert_fixup(
    struct interval_tree      *tree,
    struct interval_tree_node *node)
{
    while (node->parent->color == INTERVAL_TREE_RED) {
        if (node->parent == node->parent->parent->left) {
            struct interval_tree_node *uncle = node->parent->parent->right;

            if (uncle->color == INTERVAL_TREE_RED) {
                node->parent->color         = INTERVAL_TREE_BLACK;
                uncle->color                = INTERVAL_TREE_BLACK;
                node->parent->parent->color = INTERVAL_TREE_RED;
                node                        = node->parent->parent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    interval_tree_left_rotate(tree, node);
                }
                node->parent->color         = INTERVAL_TREE_BLACK;
                node->parent->parent->color = INTERVAL_TREE_RED;
                interval_tree_right_rotate(tree, node->parent->parent);
            }
        } else {
            struct interval_tree_node *uncle = node->parent->parent->left;

            if (uncle->color == INTERVAL_TREE_RED) {
                node->parent->color         = INTERVAL_TREE_BLACK;
                uncle->color                = INTERVAL_TREE_BLACK;
                node->parent->parent->color = INTERVAL_TREE_RED;
                node                        = node->parent->parent;
            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    interval_tree_right_rotate(tree, node);
                }
                node->parent->color         = INTERVAL_TREE_BLACK;
                node->parent->parent->color = INTERVAL_TREE_RED;
                interval_tree_left_rotate(tree, node->parent->parent);
            }
        }
    }
    tree->root->color = INTERVAL_TREE_BLACK;
} /* interval_tree_insert_fixup */

/* Returns false if the node is not detached or its interval is inverted. */
static inline bool
interval_tree_insert(
    struct interval_tree      *tree,
    struct interval_tree_node *node)
{
    struct interval_tree_node *parent = &tree->nil;
    struct interval_tree_node *cursor = tree->root;

    if (node->start > node->end || node->left != NULL ||
        node->right != NULL || node->parent != NULL) {
        return false;
    }
    if (tree->next_sequence == 0) {
        return false;
    }

    node->sequence = tree->next_sequence++;

    while (cursor != &tree->nil) {
        parent = cursor;
        if (interval_tree_compare(node, cursor) < 0) {
            cursor = cursor->left;
        } else {
            cursor = cursor->right;
        }
    }

    node->parent  = parent;
    node->left    = &tree->nil;
    node->right   = &tree->nil;
    node->max_end = node->end;
    node->color   = INTERVAL_TREE_RED;

    if (parent == &tree->nil) {
        tree->root = node;
    } else if (interval_tree_compare(node, parent) < 0) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    interval_tree_update_upwards(tree, parent);
    interval_tree_insert_fixup(tree, node);
    return true;
} /* interval_tree_insert */

static inline void
interval_tree_transplant(
    struct interval_tree      *tree,
    struct interval_tree_node *old_node,
    struct interval_tree_node *new_node)
{
    if (old_node->parent == &tree->nil) {
        tree->root = new_node;
    } else if (old_node == old_node->parent->left) {
        old_node->parent->left = new_node;
    } else {
        old_node->parent->right = new_node;
    }
    new_node->parent = old_node->parent;
} /* interval_tree_transplant */

static inline void
interval_tree_delete_fixup(
    struct interval_tree      *tree,
    struct interval_tree_node *node,
    struct interval_tree_node *parent)
{
    while (node != tree->root && node->color == INTERVAL_TREE_BLACK) {
        struct interval_tree_node *sibling;

        if (parent == &tree->nil) {
            break;
        }

        if (node == parent->left) {
            sibling = parent->right;
            if (sibling == &tree->nil) {
                node   = parent;
                parent = node->parent;
                continue;
            }
            if (sibling->color == INTERVAL_TREE_RED) {
                sibling->color = INTERVAL_TREE_BLACK;
                parent->color  = INTERVAL_TREE_RED;
                interval_tree_left_rotate(tree, parent);
                sibling = parent->right;
                if (sibling == &tree->nil) {
                    node   = parent;
                    parent = node->parent;
                    continue;
                }
            }
            if ((sibling->left == &tree->nil || sibling->left->color == INTERVAL_TREE_BLACK) &&
                (sibling->right == &tree->nil || sibling->right->color == INTERVAL_TREE_BLACK)) {
                sibling->color = INTERVAL_TREE_RED;
                node           = parent;
                parent         = node->parent;
            } else {
                if (sibling->right == &tree->nil || sibling->right->color == INTERVAL_TREE_BLACK) {
                    if (sibling->left != &tree->nil) {
                        sibling->left->color = INTERVAL_TREE_BLACK;
                    }
                    sibling->color = INTERVAL_TREE_RED;
                    interval_tree_right_rotate(tree, sibling);
                    sibling = parent->right;
                }
                sibling->color = parent->color;
                parent->color  = INTERVAL_TREE_BLACK;
                if (sibling->right != &tree->nil) {
                    sibling->right->color = INTERVAL_TREE_BLACK;
                }
                interval_tree_left_rotate(tree, parent);
                node   = tree->root;
                parent = &tree->nil;
            }
        } else {
            sibling = parent->left;
            if (sibling == &tree->nil) {
                node   = parent;
                parent = node->parent;
                continue;
            }
            if (sibling->color == INTERVAL_TREE_RED) {
                sibling->color = INTERVAL_TREE_BLACK;
                parent->color  = INTERVAL_TREE_RED;
                interval_tree_right_rotate(tree, parent);
                sibling = parent->left;
                if (sibling == &tree->nil) {
                    node   = parent;
                    parent = node->parent;
                    continue;
                }
            }
            if ((sibling->right == &tree->nil || sibling->right->color == INTERVAL_TREE_BLACK) &&
                (sibling->left == &tree->nil || sibling->left->color == INTERVAL_TREE_BLACK)) {
                sibling->color = INTERVAL_TREE_RED;
                node           = parent;
                parent         = node->parent;
            } else {
                if (sibling->left == &tree->nil || sibling->left->color == INTERVAL_TREE_BLACK) {
                    if (sibling->right != &tree->nil) {
                        sibling->right->color = INTERVAL_TREE_BLACK;
                    }
                    sibling->color = INTERVAL_TREE_RED;
                    interval_tree_left_rotate(tree, sibling);
                    sibling = parent->left;
                }
                sibling->color = parent->color;
                parent->color  = INTERVAL_TREE_BLACK;
                if (sibling->left != &tree->nil) {
                    sibling->left->color = INTERVAL_TREE_BLACK;
                }
                interval_tree_right_rotate(tree, parent);
                node   = tree->root;
                parent = &tree->nil;
            }
        }
    }

    if (node != &tree->nil) {
        node->color = INTERVAL_TREE_BLACK;
    }
} /* interval_tree_delete_fixup */

/* Removes a linked node and returns it to the detached state. */
static inline void
interval_tree_remove(
    struct interval_tree      *tree,
    struct interval_tree_node *node)
{
    struct interval_tree_node *moved = node;
    struct interval_tree_node *child;
    struct interval_tree_node *child_parent;
    enum interval_tree_color   moved_color = moved->color;

    if (node->left == &tree->nil) {
        child        = node->right;
        child_parent = node->parent;
        interval_tree_transplant(tree, node, node->right);
        interval_tree_update_upwards(tree, child_parent);
    } else if (node->right == &tree->nil) {
        child        = node->left;
        child_parent = node->parent;
        interval_tree_transplant(tree, node, node->left);
        interval_tree_update_upwards(tree, child_parent);
    } else {
        moved = node->right;
        while (moved->left != &tree->nil) {
            moved = moved->left;
        }
        moved_color = moved->color;
        child       = moved->right;

        if (moved->parent == node) {
            child_parent = moved;
        } else {
            child_parent = moved->parent;
            interval_tree_transplant(tree, moved, moved->right);
            interval_tree_update_upwards(tree, child_parent);
            moved->right         = node->right;
            moved->right->parent = moved;
        }

        interval_tree_transplant(tree, node, moved);
        moved->left         = node->left;
        moved->left->parent = moved;
        moved->color        = node->color;
        interval_tree_recompute(tree, moved);
        interval_tree_update_upwards(tree, moved->parent);
    }

    if (moved_color == INTERVAL_TREE_BLACK) {
        interval_tree_delete_fixup(tree, child, child_parent);
    }

    tree->nil.parent = &tree->nil;
    tree->nil.color  = INTERVAL_TREE_BLACK;
    node->left       = NULL;
    node->right      = NULL;
    node->parent     = NULL;
    node->max_end    = node->end;
    node->sequence   = 0;
    node->color      = INTERVAL_TREE_RED;
} /* interval_tree_remove */

static inline bool
interval_tree_overlaps(
    const struct interval_tree_node *node,
    uint64_t                         start,
    uint64_t                         end)
{
    return node->start <= end && start <= node->end;
} /* interval_tree_overlaps */

/* Finds one overlapping interval in O(log N); endpoints are inclusive. */
static inline struct interval_tree_node *
interval_tree_find_overlap(
    struct interval_tree *tree,
    uint64_t              start,
    uint64_t              end)
{
    struct interval_tree_node *node = tree->root;

    if (start > end) {
        return NULL;
    }

    while (node != &tree->nil) {
        if (interval_tree_overlaps(node, start, end)) {
            return node;
        }
        if (node->left != &tree->nil && node->left->max_end >= start) {
            node = node->left;
        } else {
            node = node->right;
        }
    }
    return NULL;
} /* interval_tree_find_overlap */

static inline struct interval_tree_node *
interval_tree_first(struct interval_tree *tree)
{
    struct interval_tree_node *node = tree->root;

    if (node == &tree->nil) {
        return NULL;
    }
    while (node->left != &tree->nil) {
        node = node->left;
    }
    return node;
} /* interval_tree_first */

static inline struct interval_tree_node *
interval_tree_next(
    struct interval_tree      *tree,
    struct interval_tree_node *node)
{
    struct interval_tree_node *parent;

    if (node->right != &tree->nil) {
        node = node->right;
        while (node->left != &tree->nil) {
            node = node->left;
        }
        return node;
    }

    parent = node->parent;
    while (parent != &tree->nil && node == parent->right) {
        node   = parent;
        parent = parent->parent;
    }
    return parent == &tree->nil ? NULL : parent;
} /* interval_tree_next */

static inline struct interval_tree_node *
interval_tree_first_overlap_node(
    struct interval_tree      *tree,
    struct interval_tree_node *node,
    uint64_t                   start,
    uint64_t                   end)
{
    struct interval_tree_node *found;

    if (node == &tree->nil || node->max_end < start) {
        return NULL;
    }

    found = interval_tree_first_overlap_node(tree, node->left, start, end);
    if (found) {
        return found;
    }
    if (node->start > end) {
        return NULL;
    }
    if (node->end >= start) {
        return node;
    }
    return interval_tree_first_overlap_node(tree, node->right, start, end);
} /* interval_tree_first_overlap_node */

/* Finds the first overlap in start/end order; endpoints are inclusive. */
static inline struct interval_tree_node *
interval_tree_first_overlap(
    struct interval_tree *tree,
    uint64_t              start,
    uint64_t              end)
{
    if (start > end) {
        return NULL;
    }
    return interval_tree_first_overlap_node(tree, tree->root, start, end);
} /* interval_tree_first_overlap */

static inline bool
interval_tree_key_after_iterator(
    const struct interval_tree_node             *node,
    const struct interval_tree_overlap_iterator *iterator)
{
    if (!iterator->valid) {
        return true;
    }
    if (node->start != iterator->last_start) {
        return node->start > iterator->last_start;
    }
    if (node->end != iterator->last_end) {
        return node->end > iterator->last_end;
    }
    return node->sequence > iterator->last_sequence;
} /* interval_tree_key_after_iterator */

static inline struct interval_tree_node *
interval_tree_first_overlap_after_node(
    struct interval_tree                  *tree,
    struct interval_tree_node             *node,
    struct interval_tree_overlap_iterator *iterator)
{
    struct interval_tree_node *found;

    if (node == &tree->nil || node->max_end < iterator->start) {
        return NULL;
    }
    iterator->nodes_visited++;

    if (node->start > iterator->end) {
        return interval_tree_first_overlap_after_node(tree, node->left,
                                                      iterator);
    }

    if (!interval_tree_key_after_iterator(node, iterator)) {
        return interval_tree_first_overlap_after_node(tree, node->right,
                                                      iterator);
    }

    found = interval_tree_first_overlap_after_node(tree, node->left, iterator);
    if (found) {
        return found;
    }
    if (node->sequence <= iterator->max_sequence &&
        interval_tree_overlaps(node, iterator->start, iterator->end)) {
        return node;
    }
    return interval_tree_first_overlap_after_node(tree, node->right, iterator);
} /* interval_tree_first_overlap_after_node */

/* Snapshot the tree's insertion sequence, then return each original overlap
 * in key order.  The caller may remove the returned node and insert new nodes
 * between calls: removed-node links are never retained, and nodes inserted
 * after initialization are excluded.  `nodes_visited` supports complexity
 * assertions in tests and diagnostics. */
static inline void
interval_tree_overlap_iterator_init(
    struct interval_tree                  *tree,
    struct interval_tree_overlap_iterator *iterator,
    uint64_t                               start,
    uint64_t                               end)
{
    iterator->start        = start;
    iterator->end          = end;
    iterator->max_sequence = tree->next_sequence == 0
        ? UINT64_MAX : tree->next_sequence - 1;
    iterator->last_start    = 0;
    iterator->last_end      = 0;
    iterator->last_sequence = 0;
    iterator->nodes_visited = 0;
    iterator->valid         = false;
} /* interval_tree_overlap_iterator_init */

static inline struct interval_tree_node *
interval_tree_overlap_iterator_next(
    struct interval_tree                  *tree,
    struct interval_tree_overlap_iterator *iterator)
{
    struct interval_tree_node *node;

    if (iterator->start > iterator->end) {
        return NULL;
    }
    node = interval_tree_first_overlap_after_node(tree, tree->root, iterator);
    if (node) {
        iterator->last_start    = node->start;
        iterator->last_end      = node->end;
        iterator->last_sequence = node->sequence;
        iterator->valid         = true;
    }
    return node;
} /* interval_tree_overlap_iterator_next */

static inline bool
interval_tree_visit_node(
    struct interval_tree      *tree,
    struct interval_tree_node *node,
    uint64_t                   start,
    uint64_t                   end,
    interval_tree_visit_fn     visit,
    void                      *private_data)
{
    if (node == &tree->nil || node->max_end < start) {
        return true;
    }

    if (!interval_tree_visit_node(tree, node->left, start, end, visit, private_data)) {
        return false;
    }
    if (interval_tree_overlaps(node, start, end) && !visit(node, private_data)) {
        return false;
    }
    if (node->start > end) {
        return true;
    }
    return interval_tree_visit_node(tree, node->right, start, end, visit, private_data);
} /* interval_tree_visit_node */

/*
 * Visits overlaps in start/end order. Returns false when the callback stops
 * traversal; an inverted query is empty and returns true. The callback must
 * not modify the tree.
 */
static inline bool
interval_tree_visit_overlaps(
    struct interval_tree  *tree,
    uint64_t               start,
    uint64_t               end,
    interval_tree_visit_fn visit,
    void                  *private_data)
{
    if (start > end) {
        return true;
    }
    if (visit == NULL) {
        return false;
    }
    return interval_tree_visit_node(tree, tree->root, start, end, visit, private_data);
} /* interval_tree_visit_overlaps */