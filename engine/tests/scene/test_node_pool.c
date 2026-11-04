/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*----------------------------------------------------------------------------------------------------------------------
 - Includes
 ---------------------------------------------------------------------------------------------------------------------*/

#include "er_node_internal.h"
#include "er_scene.h"
#include "native_renderer.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Returns an ERProps with all layout fields initialised to ER_LAYOUT_AUTO.
 *
 * Mirrors the props_default() helper in the demo.  Tests must call this instead of
 * memset-zeroing an ERProps, because zero is NOT the sentinel for layout fields
 * (ER_LAYOUT_AUTO == INT16_MIN) and the layout engine would mis-interpret zero
 * flex_basis, min/max sizes, etc. as explicit zero-pixel constraints.
 *
 * @return Fully initialised ERProps ready for customisation.
 */
static ERProps test_props_default(void)
{
    ERProps props;
    memset(&props, 0, sizeof(props));
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — null backend
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief No-op fill_rect for tests that only exercise pool/tree behavior.
 *
 * @param[in] argb    Ignored.
 * @param[in] x       Ignored.
 * @param[in] y       Ignored.
 * @param[in] width   Ignored.
 * @param[in] height  Ignored.
 * @param[in] ctx     Ignored.
 */
static void null_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief No-op copy_rect for tests that only exercise pool/tree behavior.
 *
 * @param[in] source               Ignored.
 * @param[in] source_stride_bytes  Ignored.
 * @param[in] x                    Ignored.
 * @param[in] y                    Ignored.
 * @param[in] width                Ignored.
 * @param[in] height               Ignored.
 * @param[in] ctx                  Ignored.
 */
static void null_copy(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)source_stride_bytes;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief No-op blend_rect for tests that only exercise pool/tree behavior.
 *
 * @param[in] source               Ignored.
 * @param[in] source_stride_bytes  Ignored.
 * @param[in] alpha                Ignored.
 * @param[in] x                    Ignored.
 * @param[in] y                    Ignored.
 * @param[in] width                Ignored.
 * @param[in] height               Ignored.
 * @param[in] ctx                  Ignored.
 */
static void
null_blend(const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)source_stride_bytes;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — test cases
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies that a destroyed slot is immediately reused by the next create.
 *
 * Creates two nodes, destroys the second one, then creates a third node.  The
 * free-list is LIFO so the third allocation must reclaim the destroyed slot
 * (same tag, same memory address).
 */
static void test_freelist_lifo_reuse(void)
{
    ERNode* view = er_node_create(ER_NODE_VIEW);
    ERNode* text = er_node_create(ER_NODE_TEXT);
    assert(view != NULL && "first create must succeed");
    assert(text != NULL && "second create must succeed");
    assert(view != text && "distinct nodes must not alias");

    const uint16_t text_tag = text->tag;
    er_node_destroy(text);

    /* After LIFO pop, the next create should return the same slot. */
    ERNode* pressable = er_node_create(ER_NODE_PRESSABLE);
    assert(pressable != NULL && "create after destroy must succeed");
    assert(pressable->tag == text_tag && "LIFO free-list must return the most recently freed tag");
    assert(pressable->type == ER_NODE_PRESSABLE && "recycled node must have the new type");
    assert(pressable->in_use && "recycled node must be marked in-use");
    assert(pressable->dirty && "recycled node must start dirty");

    er_node_destroy(view);
    er_node_destroy(pressable);

    printf("test_freelist_lifo_reuse PASSED\n");
}

/**
 * @brief Verifies that multiple freed slots are all reused before fresh slots are taken.
 *
 * Creates N nodes, destroys alternating ones, then creates N/2 more and checks
 * that every new allocation came from the free-list (no fresh s_next_tag bump).
 */
static void test_freelist_multi_reuse(void)
{
    const int node_count = 8;
    ERNode* nodes[8];

    for (int node_index = 0; node_index < node_count; node_index++)
    {
        nodes[node_index] = er_node_create(ER_NODE_VIEW);
        assert(nodes[node_index] != NULL && "create must succeed during multi-reuse setup");
    }

    /* Destroy every even-indexed node — their slots go into the free-list. */
    uint16_t freed_tags[4];
    int freed_count = 0;
    for (int node_index = 0; node_index < node_count; node_index += 2)
    {
        freed_tags[freed_count++] = nodes[node_index]->tag;
        er_node_destroy(nodes[node_index]);
        nodes[node_index] = NULL;
    }

    /* The next 4 creates must come from the free-list (non-NULL results). */
    ERNode* recycled[4];
    for (int recycled_index = 0; recycled_index < freed_count; recycled_index++)
    {
        recycled[recycled_index] = er_node_create(ER_NODE_TEXT);
        assert(recycled[recycled_index] != NULL && "create after destroy must succeed (free-list)");
    }

    /* Cleanup. */
    for (int node_index = 0; node_index < node_count; node_index++)
        if (nodes[node_index])
            er_node_destroy(nodes[node_index]);
    for (int recycled_index = 0; recycled_index < freed_count; recycled_index++)
        er_node_destroy(recycled[recycled_index]);

    printf("test_freelist_multi_reuse PASSED\n");
}

/**
 * @brief Verifies that double-free is silently ignored and does not corrupt the free-list.
 *
 * Calls er_node_destroy twice on the same node.  The second call must be a no-op
 * (the guard in er_node_destroy checks in_use before pushing).
 */
static void test_double_free_is_noop(void)
{
    ERNode* node = er_node_create(ER_NODE_VIEW);
    assert(node != NULL);

    er_node_destroy(node); /* first destroy: valid */
    er_node_destroy(node); /* second destroy: must be silently ignored */

    /* Recreate to verify the free-list is not corrupt (slot still usable). */
    ERNode* second_node = er_node_create(ER_NODE_VIEW);
    assert(second_node != NULL && "pool must still yield a node after a double-free attempt");
    er_node_destroy(second_node);

    printf("test_double_free_is_noop PASSED\n");
}

/**
 * @brief Verifies that a reference resolves only while its own node lives.
 *
 * Destroys a node and creates another in the same slot: the old reference must not reach the new node.
 */
static void test_node_ref_outlives_node(void)
{
    ERNode* first = er_node_create(ER_NODE_VIEW);
    assert(first != NULL);
    const ERNodeRef first_ref = er_node_ref(first);
    assert(er_node_deref(first_ref) == first);

    er_node_destroy(first);
    assert(er_node_deref(first_ref) == NULL && "a destroyed node's reference must not resolve");

    ERNode* second = er_node_create(ER_NODE_VIEW);
    assert(second == first && "the free list hands the slot straight back");
    const ERNodeRef second_ref = er_node_ref(second);
    assert(er_node_deref(first_ref) == NULL && "nor must it resolve to the slot's next node");
    assert(er_node_deref(second_ref) == second);
    er_node_destroy(second);

    assert(er_node_deref(er_node_ref(NULL)) == NULL && "a reference to no node must not resolve");

    /* A zeroed reference names tag 0 with no node's serial, even with a node in that slot. */
    er_reset();
    ERNode* zero_tag = er_node_create(ER_NODE_VIEW);
    assert(zero_tag != NULL);
    const ERNodeRef zeroed = {0U, 0U};
    assert(er_node_deref(zeroed) == NULL && "a zeroed reference must not resolve");
    assert(er_node_next_serial(1U) == 2U);
    assert(er_node_next_serial(UINT32_MAX) == 1U && "a wrapping serial must skip 0");

    const ERNodeRef before_reset = er_node_ref(zero_tag);
    er_reset();
    ERNode* after_reset = er_node_create(ER_NODE_VIEW);
    assert(after_reset != NULL);
    assert(er_node_deref(before_reset) == NULL && "a reference must not survive er_reset");
    er_node_destroy(after_reset);

    printf("test_node_ref_outlives_node PASSED\n");
}

/**
 * @brief Verifies that the pool returns NULL when exhausted and recovers after frees.
 *
 * Fills all remaining pool slots, checks that the next create returns NULL,
 * destroys a few nodes, then verifies that the same number of new creates succeed.
 */
static void test_pool_exhaustion_and_recovery(void)
{
    /* Fill whatever slots remain after the earlier tests. */
    ERNode* overflow_nodes[ERUI_MAX_NODES];
    int filled = 0;
    ERNode* node;
    while (filled < ERUI_MAX_NODES && (node = er_node_create(ER_NODE_VIEW)) != NULL)
        overflow_nodes[filled++] = node;

    /* Pool must now be exhausted. */
    ERNode* extra = er_node_create(ER_NODE_VIEW);
    assert(extra == NULL && "er_node_create must return NULL when pool is exhausted");

    /* Free exactly 3 nodes and verify 3 creates succeed. */
    const int free_count = 3;
    assert(filled >= free_count && "test requires at least free_count nodes were allocated");
    er_node_destroy(overflow_nodes[0]);
    er_node_destroy(overflow_nodes[filled / 2]);
    er_node_destroy(overflow_nodes[filled - 1]);

    ERNode* recovered_text = er_node_create(ER_NODE_TEXT);
    ERNode* recovered_image = er_node_create(ER_NODE_IMAGE);
    ERNode* recovered_view = er_node_create(ER_NODE_VIEW);
    assert(recovered_text != NULL && "first recovery create must succeed");
    assert(recovered_image != NULL && "second recovery create must succeed");
    assert(recovered_view != NULL && "third recovery create must succeed");

    /* Pool must be exhausted again immediately after recovery. */
    assert(er_node_create(ER_NODE_VIEW) == NULL && "pool must be exhausted after consuming all freed slots");

    /* Cleanup everything for a clean exit. */
    er_node_destroy(recovered_text);
    er_node_destroy(recovered_image);
    er_node_destroy(recovered_view);
    for (int node_index = 0; node_index < filled; node_index++)
    {
        if (node_index != 0 && node_index != filled / 2 && node_index != filled - 1)
            er_node_destroy(overflow_nodes[node_index]);
    }

    printf("test_pool_exhaustion_and_recovery PASSED\n");
}

/**
 * @brief Verifies that a commit which paints nothing does not erase the reported dirty rect.
 *
 * Sets up a minimal scene, commits twice, and checks that the second commit (no changes) still
 * reports the first commit's rect. Flow A relies on this: React commits inside er_runtime_pump(),
 * so the host's own er_commit() is a no-op one and would otherwise clear the answer.
 */
static void test_dirty_rect_noop_commit(void)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    assert(root != NULL);

    ERProps props = test_props_default();
    props.width = 320;
    props.height = 240;
    er_node_set_props(root, &props);
    er_tree_set_root(root);

    /* First commit: root is dirty, should produce a non-empty dirty rect. */
    er_commit();
    ERRect dirty;
    assert(er_get_dirty_rect(&dirty) && "first commit on a dirty scene must report a dirty rect");
    assert(dirty.w > 0 && dirty.h > 0 && "dirty rect must be non-empty after first commit");

    /* Second commit: nothing changed, so the first commit's rect must survive unchanged. */
    er_commit();
    ERRect second_dirty;
    assert(er_get_dirty_rect(&second_dirty) && "a commit with no changes must keep the last painted rect");
    assert(second_dirty.x == dirty.x && second_dirty.y == dirty.y && second_dirty.w == dirty.w
           && second_dirty.h == dirty.h && "a commit with no changes must not alter the reported rect");

    er_tree_set_root(NULL);
    er_node_destroy(root);

    printf("test_dirty_rect_noop_commit PASSED\n");
}

/**
 * @brief Verifies that the dirty rect expands to cover all repainted nodes.
 *
 * Creates two non-overlapping child nodes, marks them both dirty, and checks
 * that the reported dirty rect encompasses both.
 */
static void test_dirty_rect_union_coverage(void)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    assert(root != NULL);
    ERProps root_props = test_props_default();
    root_props.width = 320;
    root_props.height = 240;
    root_props.flex_direction = ER_FLEX_ROW;
    er_node_set_props(root, &root_props);

    /* Left child: 80×240 placed first in the flex row. */
    ERNode* left_child = er_node_create(ER_NODE_VIEW);
    assert(left_child != NULL);
    ERProps left_child_props = test_props_default();
    left_child_props.width = 80;
    left_child_props.height = 240;
    left_child_props.background_color = 0xFF2A9D8F;
    er_node_set_props(left_child, &left_child_props);

    /* Right child: 80×240 placed second — starts at x=80. */
    ERNode* right_child = er_node_create(ER_NODE_VIEW);
    assert(right_child != NULL);
    ERProps right_child_props = test_props_default();
    right_child_props.width = 80;
    right_child_props.height = 240;
    right_child_props.background_color = 0xFFE94560;
    er_node_set_props(right_child, &right_child_props);

    er_tree_append_child(root, left_child);
    er_tree_append_child(root, right_child);
    er_tree_set_root(root);

    /* First commit makes everything clean. */
    er_commit();

    /* Mark both children dirty and commit again. */
    er_mark_dirty_upward(left_child);
    er_mark_dirty_upward(right_child);
    er_commit();

    ERRect dirty;
    assert(er_get_dirty_rect(&dirty) && "dirty rect must be non-empty when children were re-rendered");
    /* The union must span at least from the left child's x to beyond the right child's right edge. */
    assert(dirty.x <= 0 && "dirty rect left edge must start at or before the left child");
    assert(dirty.x + dirty.w >= 160 && "dirty rect right edge must cover both children");
    assert(dirty.h >= 0 && "dirty rect height must be non-negative");

    er_tree_set_root(NULL);
    er_node_destroy(left_child);
    er_node_destroy(right_child);
    er_node_destroy(root);

    printf("test_dirty_rect_union_coverage PASSED\n");
}

/**
 * @brief Drains the Animated.Value pool, returning how many values could be created before exhaustion.
 *
 * @return The pool capacity (number of successful er_anim_value_create calls).
 */
static int drain_anim_values(void)
{
    int count = 0;
    while (er_anim_value_create(0.0f) != ER_ANIM_VALUE_INVALID)
        count++;
    return count;
}

/**
 * @brief Verifies er_reset() empties the scene: the whole node pool and the Animated.Value pool
 *        fully recover, and a fresh scene built afterwards lays out and paints.
 *
 * Leaves the engine reset (an empty scene) for the tests that follow.
 */
static void test_reset(void)
{
    er_reset();

    /* Build a non-trivial scene: a committed tree plus a live Animated.Value. */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    assert(root != NULL);
    ERProps props = test_props_default();
    props.width = 320;
    props.height = 240;
    er_node_set_props(root, &props);
    for (int child_index = 0; child_index < 16; child_index++)
    {
        ERNode* child = er_node_create(ER_NODE_VIEW);
        assert(child != NULL);
        er_tree_append_child(root, child);
    }
    er_tree_set_root(root);
    er_commit();
    assert(er_anim_value_create(1.0f) != ER_ANIM_VALUE_INVALID);

    /* Reset must free every node: the whole pool is allocatable again. */
    er_reset();
    int allocated = 0;
    while (allocated < (int)ERUI_MAX_NODES && er_node_create(ER_NODE_VIEW) != NULL)
        allocated++;
    assert(allocated == (int)ERUI_MAX_NODES && "er_reset must free the entire node pool");
    assert(er_node_create(ER_NODE_VIEW) == NULL && "pool must be full after allocating ERUI_MAX_NODES");

    /* Reset must free every Animated.Value: draining twice (with a reset between) yields the same
     * capacity, and that capacity is non-zero. */
    er_reset();
    const int cap1 = drain_anim_values();
    er_reset();
    const int cap2 = drain_anim_values();
    assert(cap1 > 0 && "Animated.Value pool capacity must be non-zero");
    assert(cap1 == cap2 && "er_reset must free every Animated.Value");

    /* A fresh scene after reset lays out and paints (no stale root / dirty / force-full state). */
    er_reset();
    ERNode* root2 = er_node_create(ER_NODE_VIEW);
    assert(root2 != NULL);
    ERProps root2_props = test_props_default();
    root2_props.width = 100;
    root2_props.height = 100;
    er_node_set_props(root2, &root2_props);
    er_tree_set_root(root2);
    er_commit();
    ERRect dirty;
    assert(er_get_dirty_rect(&dirty) && dirty.w > 0 && dirty.h > 0 && "fresh scene after reset must paint");

    er_reset(); /* leave an empty scene for the tests that follow */
    printf("test_reset PASSED\n");
}

/**
 * @brief er_now_ms64() keeps counting where er_now_ms() wraps, and er_reset() leaves the clock alone.
 */
static void test_clock_64bit(void)
{
    const uint64_t start_ms = er_now_ms64();

    /* 5e9 ms is past 2^32 (~49.7 days); a tick is a 32-bit step, so it takes several. */
    for (int tick_index = 0; tick_index < 5; tick_index++)
        embedded_renderer_tick(1000000000U);
    assert(er_now_ms64() - start_ms == 5000000000ULL && "er_now_ms64 must not wrap");
    assert(er_now_ms() == (uint32_t)(start_ms + 5000000000ULL) && "er_now_ms wraps as it always has");

    const uint64_t before = er_now_ms64();
    er_reset();
    assert(er_now_ms64() == before && "er_reset must not move the clock");
    printf("test_clock_64bit PASSED\n");
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point.
 *
 * @return 0 on success; assertion failure terminates the process with a non-zero exit code.
 */
int main(void)
{
    EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = null_fill;
    backend.copy_rect = null_copy;
    backend.blend_rect = null_blend;
    embedded_renderer_set_backend(&backend);

    test_reset();
    test_clock_64bit();
    test_freelist_lifo_reuse();
    test_freelist_multi_reuse();
    test_double_free_is_noop();
    test_node_ref_outlives_node();
    test_dirty_rect_noop_commit();
    test_dirty_rect_union_coverage();
    /* Run last: exhausts the pool, then verifies recovery. */
    test_pool_exhaustion_and_recovery();

    printf("All node pool tests PASSED\n");
    return 0;
}
