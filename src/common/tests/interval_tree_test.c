// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/interval_tree.h"
#include "common/macros.h"

#define RANDOM_NODE_COUNT   128
#define MAX_TEST_NODE_COUNT 128

struct test_interval {
    size_t                    id;
    struct interval_tree_node node;
};

struct query_result {
    bool   seen[MAX_TEST_NODE_COUNT];
    size_t limit;
    size_t count;
    bool   stop_after_first;
};

static uint32_t
next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
} /* next_random */

static int
compare_intervals(
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
} /* compare_intervals */

static int
validate_node(
    const struct interval_tree      *tree,
    const struct interval_tree_node *node,
    const struct interval_tree_node *parent,
    const struct interval_tree_node *lower,
    const struct interval_tree_node *upper,
    size_t                          *count,
    uint64_t                        *max_end)
{
    uint64_t left_max;
    uint64_t right_max;
    int      left_black_height;
    int      right_black_height;

    if (node == &tree->nil) {
        *max_end = 0;
        return 1;
    }

    assert(node->parent == parent);
    assert(node->start <= node->end);
    assert(lower == NULL || compare_intervals(lower, node) <= 0);
    assert(upper == NULL || compare_intervals(node, upper) <= 0);
    if (node->left != &tree->nil) {
        assert(node->left->parent == node);
    }
    if (node->right != &tree->nil) {
        assert(node->right->parent == node);
    }
    if (node->color == INTERVAL_TREE_RED) {
        assert(node->left->color == INTERVAL_TREE_BLACK);
        assert(node->right->color == INTERVAL_TREE_BLACK);
    }

    left_black_height = validate_node(tree, node->left, node, lower, node,
                                      count, &left_max);
    right_black_height = validate_node(tree, node->right, node, node, upper,
                                       count, &right_max);
    assert(left_black_height == right_black_height);

    *max_end = node->end;
    if (node->left != &tree->nil && left_max > *max_end) {
        *max_end = left_max;
    }
    if (node->right != &tree->nil && right_max > *max_end) {
        *max_end = right_max;
    }
    assert(node->max_end == *max_end);

    (*count)++;
    return left_black_height + (node->color == INTERVAL_TREE_BLACK);
} /* validate_node */

static void
verify_tree(
    const struct interval_tree *tree,
    size_t                      expected_count)
{
    size_t   count = 0;
    uint64_t max_end;

    assert(tree->nil.color == INTERVAL_TREE_BLACK);
    assert(tree->nil.left == &tree->nil);
    assert(tree->nil.right == &tree->nil);
    assert(tree->root == &tree->nil || tree->root->parent == &tree->nil);
    assert(tree->root == &tree->nil || tree->root->color == INTERVAL_TREE_BLACK);
    validate_node(tree, tree->root, &tree->nil, NULL, NULL, &count, &max_end);
    assert(count == expected_count);
    assert(interval_tree_empty(tree) == (expected_count == 0));
} /* verify_tree */

static bool
collect_overlap(
    struct interval_tree_node *node,
    void                      *private_data)
{
    struct query_result  *result = private_data;
    struct test_interval *entry  = container_of(node, struct test_interval, node);

    assert(entry->id < result->limit);
    assert(!result->seen[entry->id]);
    result->seen[entry->id] = true;
    result->count++;
    return !result->stop_after_first;
} /* collect_overlap */

static void
verify_query(
    struct interval_tree       *tree,
    const struct test_interval *nodes,
    const bool                 *active,
    size_t                      count,
    uint64_t                    start,
    uint64_t                    end)
{
    struct interval_tree_node *found;
    struct query_result        result       = { .limit = count };
    bool                       expected_any = false;

    assert(interval_tree_visit_overlaps(tree, start, end, collect_overlap, &result));
    found = interval_tree_find_overlap(tree, start, end);

    for (size_t i = 0; i < count; i++) {
        bool expected = active[i] && start <= end &&
            nodes[i].node.start <= end && start <= nodes[i].node.end;

        assert(result.seen[i] == expected);
        expected_any |= expected;
    }
    assert((found != NULL) == expected_any);
    if (found != NULL) {
        assert(found->start <= end && start <= found->end);
    }

    {
        struct query_result first_result = { .limit = count, .stop_after_first = true };
        bool                completed;

        completed = interval_tree_visit_overlaps(tree, start, end, collect_overlap,
                                                 &first_result);
        assert(completed == !expected_any);
        assert(first_result.count == (expected_any ? 1 : 0));
    }
} /* verify_query */

static void
test_basic_queries_and_edges(void)
{
    struct interval_tree      tree;
    struct test_interval      nodes[] = {
        { .id = 0 }, { .id = 1 }, { .id = 2 }, { .id = 3 },
        { .id = 4 }, { .id = 5 }, { .id = 6 }, { .id = 7 }
    };
    bool                      active[sizeof(nodes) / sizeof(nodes[0])];
    const uint64_t            starts[] = { 10, 40, 70, 5, 17, 25, 10, UINT64_MAX - 1 };
    const uint64_t            ends[]   = { 30, 60, 90, 12, 19, 25, 30, UINT64_MAX };
    struct interval_tree_node invalid;

    interval_tree_init(&tree);
    assert(interval_tree_empty(&tree));
    assert(!interval_tree_node_init(&invalid, 2, 1));

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        active[i] = true;
        assert(interval_tree_node_init(&nodes[i].node, starts[i], ends[i]));
        assert(interval_tree_insert(&tree, &nodes[i].node));
        verify_tree(&tree, i + 1);
    }
    assert(!interval_tree_insert(&tree, &nodes[0].node));

    verify_query(&tree, nodes, active, 8, 25, 75);
    verify_query(&tree, nodes, active, 8, 30, 40);
    verify_query(&tree, nodes, active, 8, 31, 39);
    verify_query(&tree, nodes, active, 8, UINT64_MAX, UINT64_MAX);
    verify_query(&tree, nodes, active, 8, 80, 79);

    assert(interval_tree_first(&tree)->start == 5);
    assert(interval_tree_next(&tree, interval_tree_first(&tree))->start == 10);
    assert(interval_tree_first_overlap(&tree, 25, 30)->start == 10);
    assert(interval_tree_first_overlap(&tree, 31, 39) == NULL);
    assert(interval_tree_first_overlap(&tree, UINT64_MAX, UINT64_MAX)->start ==
           UINT64_MAX - 1);

    interval_tree_remove(&tree, &nodes[0].node);
    active[0] = false;
    verify_tree(&tree, 7);
    verify_query(&tree, nodes, active, 8, 30, 30);
    assert(interval_tree_insert(&tree, &nodes[0].node));
    active[0] = true;
    verify_tree(&tree, 8);
    interval_tree_remove(&tree, &nodes[0].node);
    active[0] = false;
    verify_tree(&tree, 7);

    interval_tree_remove(&tree, &nodes[7].node);
    active[7] = false;
    verify_tree(&tree, 6);
    verify_query(&tree, nodes, active, 8, UINT64_MAX, UINT64_MAX);
} /* test_basic_queries_and_edges */

static void
test_randomized_operations(void)
{
    struct interval_tree tree;
    struct test_interval nodes[RANDOM_NODE_COUNT]  = { 0 };
    bool                 active[RANDOM_NODE_COUNT] = { false };
    size_t               order[RANDOM_NODE_COUNT];
    uint32_t             random_state = 0x9e3779b9U;
    size_t               inserted     = 0;

    interval_tree_init(&tree);
    for (size_t i = 0; i < RANDOM_NODE_COUNT; i++) {
        uint64_t start = next_random(&random_state) % 512;
        uint64_t end   = start + next_random(&random_state) % 64;

        nodes[i].id = i;
        order[i]    = i;
        assert(interval_tree_node_init(&nodes[i].node, start, end));
    }

    for (size_t i = RANDOM_NODE_COUNT - 1; i > 0; i--) {
        size_t j   = next_random(&random_state) % (i + 1);
        size_t tmp = order[i];

        order[i] = order[j];
        order[j] = tmp;
    }

    for (size_t i = 0; i < RANDOM_NODE_COUNT; i++) {
        size_t index = order[i];

        assert(interval_tree_insert(&tree, &nodes[index].node));
        active[index] = true;
        inserted++;
        verify_tree(&tree, inserted);
    }

    for (size_t i = 0; i < 2048; i++) {
        uint64_t start = next_random(&random_state) % 600;
        uint64_t end   = start + next_random(&random_state) % 80;

        verify_query(&tree, nodes, active, RANDOM_NODE_COUNT, start, end);
    }

    for (size_t i = RANDOM_NODE_COUNT - 1; i > 0; i--) {
        size_t j   = next_random(&random_state) % (i + 1);
        size_t tmp = order[i];

        order[i] = order[j];
        order[j] = tmp;
    }

    for (size_t i = 0; i < RANDOM_NODE_COUNT; i++) {
        size_t index = order[i];

        interval_tree_remove(&tree, &nodes[index].node);
        active[index] = false;
        inserted--;
        verify_tree(&tree, inserted);
        assert(nodes[index].node.left == NULL);
        assert(nodes[index].node.right == NULL);
        assert(nodes[index].node.parent == NULL);

        if (i % 8 == 0) {
            uint64_t start = next_random(&random_state) % 600;
            uint64_t end   = start + next_random(&random_state) % 80;

            verify_query(&tree, nodes, active, RANDOM_NODE_COUNT, start, end);
        }
    }

    verify_query(&tree, nodes, active, RANDOM_NODE_COUNT, 0, UINT64_MAX);
} /* test_randomized_operations */

static void
test_overlap_iterator_prunes_and_survives_mutation(void)
{
    enum { NODE_COUNT = 10000 };
    struct interval_tree                  tree;
    struct interval_tree_overlap_iterator iterator;
    struct test_interval                 *nodes = calloc(NODE_COUNT + 1,
                                                         sizeof(*nodes));
    struct interval_tree_node            *node;
    struct test_interval                  duplicates[4] = { 0 };
    size_t                                i;

    assert(nodes != NULL);
    interval_tree_init(&tree);
    for (i = 0; i < NODE_COUNT; i++) {
        nodes[i].id = i;
        assert(interval_tree_node_init(&nodes[i].node, i * 4, i * 4 + 1));
        assert(interval_tree_insert(&tree, &nodes[i].node));
    }

    interval_tree_overlap_iterator_init(&tree, &iterator,
                                        NODE_COUNT * 2, NODE_COUNT * 2 + 1);
    node = interval_tree_overlap_iterator_next(&tree, &iterator);
    assert(node == &nodes[NODE_COUNT / 2].node);
    assert(interval_tree_overlap_iterator_next(&tree, &iterator) == NULL);
    assert(iterator.nodes_visited < 100);

    interval_tree_init(&tree);
    for (i = 0; i < 3; i++) {
        assert(interval_tree_node_init(&duplicates[i].node, 0, 100));
        assert(interval_tree_insert(&tree, &duplicates[i].node));
    }
    interval_tree_overlap_iterator_init(&tree, &iterator, 50, 50);
    node = interval_tree_overlap_iterator_next(&tree, &iterator);
    assert(node == &duplicates[0].node);
    interval_tree_remove(&tree, node);
    assert(interval_tree_node_init(&duplicates[3].node, 0, 100));
    assert(interval_tree_insert(&tree, &duplicates[3].node));
    assert(interval_tree_overlap_iterator_next(&tree, &iterator) ==
           &duplicates[1].node);
    assert(interval_tree_overlap_iterator_next(&tree, &iterator) ==
           &duplicates[2].node);
    assert(interval_tree_overlap_iterator_next(&tree, &iterator) == NULL);
    verify_tree(&tree, 3);

    for (i = 0; i < 3; i++) {
        interval_tree_remove(&tree, &duplicates[i + 1].node);
    }
    free(nodes);
} /* test_overlap_iterator_prunes_and_survives_mutation */

int
main(void)
{
    test_basic_queries_and_edges();
    test_randomized_operations();
    test_overlap_iterator_prunes_and_survives_mutation();
    printf("Interval tree tests passed.\n");
    return 0;
} /* main */