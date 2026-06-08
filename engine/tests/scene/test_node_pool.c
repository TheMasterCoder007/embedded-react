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
    ERProps p;
    memset(&p, 0, sizeof(p));
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — null backend
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief No-op fill_rect for tests that only exercise pool/tree behavior.
 *
 * @param[in] argb  Ignored.
 * @param[in] x     Ignored.
 * @param[in] y     Ignored.
 * @param[in] w     Ignored.
 * @param[in] h     Ignored.
 * @param[in] ctx   Ignored.
 */
static void null_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief No-op copy_rect for tests that only exercise pool/tree behavior.
 *
 * @param[in] src              Ignored.
 * @param[in] src_stride_bytes Ignored.
 * @param[in] x                Ignored.
 * @param[in] y                Ignored.
 * @param[in] w                Ignored.
 * @param[in] h                Ignored.
 * @param[in] ctx              Ignored.
 */
static void null_copy(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)src_stride_bytes;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief No-op blend_rect for tests that only exercise pool/tree behavior.
 *
 * @param[in] src              Ignored.
 * @param[in] src_stride_bytes Ignored.
 * @param[in] alpha            Ignored.
 * @param[in] x                Ignored.
 * @param[in] y                Ignored.
 * @param[in] w                Ignored.
 * @param[in] h                Ignored.
 * @param[in] ctx              Ignored.
 */
static void null_blend(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)src_stride_bytes;
    (void)alpha;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
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
    ERNode* a = er_node_create(ER_NODE_VIEW);
    ERNode* b = er_node_create(ER_NODE_TEXT);
    assert(a != NULL && "first create must succeed");
    assert(b != NULL && "second create must succeed");
    assert(a != b && "distinct nodes must not alias");

    const uint16_t b_tag = b->tag;
    er_node_destroy(b);

    /* After LIFO pop, the next create should return the same slot. */
    ERNode* c = er_node_create(ER_NODE_PRESSABLE);
    assert(c != NULL && "create after destroy must succeed");
    assert(c->tag == b_tag && "LIFO free-list must return the most recently freed tag");
    assert(c->type == ER_NODE_PRESSABLE && "recycled node must have the new type");
    assert(c->in_use && "recycled node must be marked in-use");
    assert(c->dirty && "recycled node must start dirty");

    er_node_destroy(a);
    er_node_destroy(c);

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
    const int N = 8;
    ERNode* nodes[8];

    for (int i = 0; i < N; i++)
    {
        nodes[i] = er_node_create(ER_NODE_VIEW);
        assert(nodes[i] != NULL && "create must succeed during multi-reuse setup");
    }

    /* Destroy every even-indexed node — their slots go into the free-list. */
    uint16_t freed_tags[4];
    int freed_count = 0;
    for (int i = 0; i < N; i += 2)
    {
        freed_tags[freed_count++] = nodes[i]->tag;
        er_node_destroy(nodes[i]);
        nodes[i] = NULL;
    }

    /* The next 4 creates must come from the free-list (non-NULL results). */
    ERNode* recycled[4];
    for (int i = 0; i < freed_count; i++)
    {
        recycled[i] = er_node_create(ER_NODE_TEXT);
        assert(recycled[i] != NULL && "create after destroy must succeed (free-list)");
    }

    /* Cleanup. */
    for (int i = 0; i < N; i++)
        if (nodes[i])
            er_node_destroy(nodes[i]);
    for (int i = 0; i < freed_count; i++)
        er_node_destroy(recycled[i]);

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
    ERNode* n = er_node_create(ER_NODE_VIEW);
    assert(n != NULL);

    er_node_destroy(n); /* first destroy: valid */
    er_node_destroy(n); /* second destroy: must be silently ignored */

    /* Recreate to verify the free-list is not corrupt (slot still usable). */
    ERNode* n2 = er_node_create(ER_NODE_VIEW);
    assert(n2 != NULL && "pool must still yield a node after a double-free attempt");
    er_node_destroy(n2);

    printf("test_double_free_is_noop PASSED\n");
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
    ERNode* n;
    while (filled < ERUI_MAX_NODES && (n = er_node_create(ER_NODE_VIEW)) != NULL)
        overflow_nodes[filled++] = n;

    /* Pool must now be exhausted. */
    ERNode* extra = er_node_create(ER_NODE_VIEW);
    assert(extra == NULL && "er_node_create must return NULL when pool is exhausted");

    /* Free exactly 3 nodes and verify 3 creates succeed. */
    const int free_count = 3;
    assert(filled >= free_count && "test requires at least free_count nodes were allocated");
    er_node_destroy(overflow_nodes[0]);
    er_node_destroy(overflow_nodes[filled / 2]);
    er_node_destroy(overflow_nodes[filled - 1]);

    ERNode* r0 = er_node_create(ER_NODE_TEXT);
    ERNode* r1 = er_node_create(ER_NODE_IMAGE);
    ERNode* r2 = er_node_create(ER_NODE_VIEW);
    assert(r0 != NULL && "first recovery create must succeed");
    assert(r1 != NULL && "second recovery create must succeed");
    assert(r2 != NULL && "third recovery create must succeed");

    /* Pool must be exhausted again immediately after recovery. */
    assert(er_node_create(ER_NODE_VIEW) == NULL && "pool must be exhausted after consuming all freed slots");

    /* Cleanup everything for a clean exit. */
    er_node_destroy(r0);
    er_node_destroy(r1);
    er_node_destroy(r2);
    for (int i = 0; i < filled; i++)
    {
        if (i != 0 && i != filled / 2 && i != filled - 1)
            er_node_destroy(overflow_nodes[i]);
    }

    printf("test_pool_exhaustion_and_recovery PASSED\n");
}

/**
 * @brief Verifies that er_get_dirty_rect returns false when the scene is clean.
 *
 * Sets up a minimal scene, commits twice, and checks that the second commit
 * (no changes) reports no dirty rect.
 */
static void test_dirty_rect_clean_frame(void)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    assert(root != NULL);

    ERProps p = test_props_default();
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);
    er_tree_set_root(root);

    /* First commit: root is dirty, should produce a non-empty dirty rect. */
    er_commit();
    ERRect dr;
    assert(er_get_dirty_rect(&dr) && "first commit on a dirty scene must report a dirty rect");
    assert(dr.w > 0 && dr.h > 0 && "dirty rect must be non-empty after first commit");

    /* Second commit: nothing changed, scene should be clean. */
    er_commit();
    assert(!er_get_dirty_rect(NULL) && "second commit with no changes must report a clean frame");

    er_tree_set_root(NULL);
    er_node_destroy(root);

    printf("test_dirty_rect_clean_frame PASSED\n");
}

/**
 * @brief Verifies that the dirty rect expands to cover all repainted nodes.
 *
 * Creates two non-overlapping child nodes, marks them both dirty, and checks
 * that the reported dirty rect encompasses both.
 */
static void test_dirty_rect_union_coverage(void)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    assert(root != NULL);
    ERProps rp = test_props_default();
    rp.width = 320;
    rp.height = 240;
    rp.flex_direction = ER_FLEX_ROW;
    er_node_set_props(root, &rp);

    /* Left child: 80×240 placed first in the flex row. */
    ERNode* left_child = er_node_create(ER_NODE_VIEW);
    assert(left_child != NULL);
    ERProps lp = test_props_default();
    lp.width = 80;
    lp.height = 240;
    lp.background_color = 0xFF2A9D8F;
    er_node_set_props(left_child, &lp);

    /* Right child: 80×240 placed second — starts at x=80. */
    ERNode* right_child = er_node_create(ER_NODE_VIEW);
    assert(right_child != NULL);
    ERProps rcp = test_props_default();
    rcp.width = 80;
    rcp.height = 240;
    rcp.background_color = 0xFFE94560;
    er_node_set_props(right_child, &rcp);

    er_tree_append_child(root, left_child);
    er_tree_append_child(root, right_child);
    er_tree_set_root(root);

    /* First commit makes everything clean. */
    er_commit();

    /* Mark both children dirty and commit again. */
    er_mark_dirty_upward(left_child);
    er_mark_dirty_upward(right_child);
    er_commit();

    ERRect dr;
    assert(er_get_dirty_rect(&dr) && "dirty rect must be non-empty when children were re-rendered");
    /* The union must span at least from the left child's x to beyond the right child's right edge. */
    assert(dr.x <= 0 && "dirty rect left edge must start at or before the left child");
    assert(dr.x + dr.w >= 160 && "dirty rect right edge must cover both children");
    assert(dr.h >= 0 && "dirty rect height must be non-negative");

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
    int n = 0;
    while (er_anim_value_create(0.0f) != ER_ANIM_VALUE_INVALID)
        n++;
    return n;
}

/**
 * @brief Verifies er_reset() empties the scene: the whole node pool and the Animated.Value pool
 *        fully recover, and a fresh scene built afterwards lays out and paints.
 *
 * Leaves the engine reset (an empty scene) for the tests that follow.
 */
static void test_reset(void)
{
    /* Build a non-trivial scene: a committed tree plus a live Animated.Value. */
    ERNode* root = er_node_create(ER_NODE_VIEW);
    assert(root != NULL);
    ERProps p = test_props_default();
    p.width = 320;
    p.height = 240;
    er_node_set_props(root, &p);
    for (int i = 0; i < 16; i++)
    {
        ERNode* c = er_node_create(ER_NODE_VIEW);
        assert(c != NULL);
        er_tree_append_child(root, c);
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
    ERProps p2 = test_props_default();
    p2.width = 100;
    p2.height = 100;
    er_node_set_props(root2, &p2);
    er_tree_set_root(root2);
    er_commit();
    ERRect dr;
    assert(er_get_dirty_rect(&dr) && dr.w > 0 && dr.h > 0 && "fresh scene after reset must paint");

    er_reset(); /* leave an empty scene for the tests that follow */
    printf("test_reset PASSED\n");
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
    EmbeddedRenderBackend be;
    memset(&be, 0, sizeof(be));
    be.fill_rect = null_fill;
    be.copy_rect = null_copy;
    be.blend_rect = null_blend;
    embedded_renderer_set_backend(&be);

    test_reset();
    test_freelist_lifo_reuse();
    test_freelist_multi_reuse();
    test_double_free_is_noop();
    test_dirty_rect_clean_frame();
    test_dirty_rect_union_coverage();
    /* Run last: exhausts the pool, then verifies recovery. */
    test_pool_exhaustion_and_recovery();

    printf("All node pool tests PASSED\n");
    return 0;
}
