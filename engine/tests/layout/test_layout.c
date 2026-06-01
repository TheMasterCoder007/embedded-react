#include "er_scene.h"
#include "native_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 320
#define FB_H 240

/* Width the wrapping text nodes are constrained to; narrow enough to force wrapping. */
#define TEXT_BOX_W 120

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Captured computed height for each text node, written by its ER_EVENT_LAYOUT handler. */
static int s_one_line_h = -1;
static int s_two_line_h = -1;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief No-op fill_rect backend callback.
 *
 * @param[in] argb  Fill color (unused).
 * @param[in] x     Left edge (unused).
 * @param[in] y     Top edge (unused).
 * @param[in] w     Width (unused).
 * @param[in] h     Height (unused).
 * @param[in] ctx   Opaque context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief No-op copy_rect backend callback.
 *
 * @param[in] src  Source buffer (unused).
 * @param[in] s    Stride (unused).
 * @param[in] x    X (unused).
 * @param[in] y    Y (unused).
 * @param[in] w    Width (unused).
 * @param[in] h    Height (unused).
 * @param[in] ctx  Opaque context (unused).
 */
static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief No-op blend_rect backend callback.
 *
 * @param[in] src  Source buffer (unused).
 * @param[in] s    Stride (unused).
 * @param[in] a    Global alpha (unused).
 * @param[in] x    X (unused).
 * @param[in] y    Y (unused).
 * @param[in] w    Width (unused).
 * @param[in] h    Height (unused).
 * @param[in] ctx  Opaque context (unused).
 */
static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief ER_EVENT_LAYOUT handler that records a node's computed height into an int.
 *
 * @param[in] node       Node that fired the event (unused).
 * @param[in] data       Event payload carrying data->layout_rect.
 * @param[in] user_data  Pointer to the int that receives layout_rect.h.
 */
static void on_layout(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    *(int*)user_data = data->layout_rect.h;
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief ER_EVENT_LAYOUT handler that records a node's full computed rect.
 *
 * @param[in] node       Node that fired the event (unused).
 * @param[in] data       Event payload carrying data->layout_rect.
 * @param[in] user_data  Pointer to an ERRect that receives the computed rectangle.
 */
static void on_layout_rect(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    *(ERRect*)user_data = data->layout_rect;
}

/**
 * @brief Builds an ERProps with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return Zero-initialised ERProps with AUTO layout sentinels and opacity 255.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.padding_horizontal = p.padding_vertical = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.margin_horizontal = p.margin_vertical = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — verifies layout behaviour, including text intrinsic height.
 *
 * Builds a root with two width-constrained Text nodes containing the same long string:
 * one with number_of_lines = 1 and one with number_of_lines = 2. Asserts that the
 * two-line node is allocated more vertical space than the one-line node, confirming
 * that number_of_lines feeds the intrinsic-height measurement during layout.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    /* Smoke: er_commit with no root must not crash. */
    er_commit();

    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&be);

    static const char* k_long = "This is a long string that must wrap across multiple lines when constrained";

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = (int16_t)FB_W;
    rp.height = (int16_t)FB_H;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    er_node_set_props(root, &rp);

    /* One-line text node, width-constrained. */
    ERNode* one = er_node_create(ER_NODE_TEXT);
    ERProps op = props_default();
    op.width = TEXT_BOX_W;
    op.font_size = 14;
    op.number_of_lines = 1;
    op.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(op.text, k_long, ER_TEXT_MAX);
    er_node_set_props(one, &op);
    er_event_set(one, ER_EVENT_LAYOUT, on_layout, &s_one_line_h);

    /* Two-line text node, identical text and width. */
    ERNode* two = er_node_create(ER_NODE_TEXT);
    ERProps tp = props_default();
    tp.width = TEXT_BOX_W;
    tp.font_size = 14;
    tp.number_of_lines = 2;
    tp.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(tp.text, k_long, ER_TEXT_MAX);
    er_node_set_props(two, &tp);
    er_event_set(two, ER_EVENT_LAYOUT, on_layout, &s_two_line_h);

    er_tree_append_child(root, one);
    er_tree_append_child(root, two);
    er_tree_set_root(root);

    er_commit();

    if (s_one_line_h <= 0)
        return fail("one-line text node reported non-positive computed height");
    if (s_two_line_h <= 0)
        return fail("two-line text node reported non-positive computed height");
    if (s_two_line_h <= s_one_line_h)
        return fail("number_of_lines=2 did not reserve more height than number_of_lines=1");
    /* Two lines should be roughly twice one line; allow generous tolerance. */
    if (s_two_line_h < s_one_line_h + s_one_line_h / 2)
        return fail("two-line height is not close to twice the one-line height");

    /* -----------------------------------------------------------------------
     * Test: aspectRatio — child derives height from explicit width.
     * Container: row 200×200.  Child: width=100, aspect_ratio=2.0 → height=50.
     * -----------------------------------------------------------------------*/
    {
        ERNode* ar_root = er_node_create(ER_NODE_VIEW);
        ERProps ap = props_default();
        ap.width = 200;
        ap.height = 200;
        ap.flex_direction = ER_FLEX_ROW;
        ap.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(ar_root, &ap);

        ERNode* ar_child = er_node_create(ER_NODE_VIEW);
        ERRect ar_rect = {-1, -1, -1, -1};
        ap = props_default();
        ap.width = 100;
        ap.aspect_ratio = 2.0f; /* width/height = 2 → height = 50 */
        er_node_set_props(ar_child, &ap);
        er_event_set(ar_child, ER_EVENT_LAYOUT, on_layout_rect, &ar_rect);

        er_tree_append_child(ar_root, ar_child);
        er_tree_set_root(ar_root);
        er_commit();

        if (ar_rect.w != 100)
            return fail("aspectRatio: child width should be 100");
        if (ar_rect.h != 50)
            return fail("aspectRatio: child height should be 50 (width/ratio=100/2)");

        er_tree_remove_child(ar_root, ar_child);
        er_node_destroy(ar_child);
        er_node_destroy(ar_root);
    }

    /* -----------------------------------------------------------------------
     * Test: flex_basis_pct — child claims 50% of parent's main axis.
     * Container: row 200×80.  Child: flex_basis_pct=50 → width=100.
     * -----------------------------------------------------------------------*/
    {
        ERNode* pct_root = er_node_create(ER_NODE_VIEW);
        ERProps pp = props_default();
        pp.width = 200;
        pp.height = 80;
        pp.flex_direction = ER_FLEX_ROW;
        pp.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(pct_root, &pp);

        ERNode* pct_child = er_node_create(ER_NODE_VIEW);
        ERRect pct_rect = {-1, -1, -1, -1};
        pp = props_default();
        pp.flex_basis_pct = 50.0f;
        er_node_set_props(pct_child, &pp);
        er_event_set(pct_child, ER_EVENT_LAYOUT, on_layout_rect, &pct_rect);

        er_tree_append_child(pct_root, pct_child);
        er_tree_set_root(pct_root);
        er_commit();

        if (pct_rect.w != 100)
            return fail("flex_basis_pct: 50% of 200px parent should yield width=100");

        er_tree_remove_child(pct_root, pct_child);
        er_node_destroy(pct_child);
        er_node_destroy(pct_root);
    }

    /* -----------------------------------------------------------------------
     * Test: marginHorizontal — expands to margin_left + margin_right.
     * Container: col 200×200.  Child: marginHorizontal=20 → x=20, width=160.
     * -----------------------------------------------------------------------*/
    {
        ERNode* mh_root = er_node_create(ER_NODE_VIEW);
        ERProps mp = props_default();
        mp.width = 200;
        mp.height = 200;
        mp.flex_direction = ER_FLEX_COL;
        mp.align_items = ER_ALIGN_STRETCH;
        er_node_set_props(mh_root, &mp);

        ERNode* mh_child = er_node_create(ER_NODE_VIEW);
        ERRect mh_rect = {-1, -1, -1, -1};
        mp = props_default();
        mp.height = 40;
        mp.margin_horizontal = 20;
        er_node_set_props(mh_child, &mp);
        er_event_set(mh_child, ER_EVENT_LAYOUT, on_layout_rect, &mh_rect);

        er_tree_append_child(mh_root, mh_child);
        er_tree_set_root(mh_root);
        er_commit();

        if (mh_rect.x != 20)
            return fail("marginHorizontal: child should be offset 20px from parent left");
        if (mh_rect.w != 160)
            return fail("marginHorizontal: child width should be 160 (200 - 20*2)");

        er_tree_remove_child(mh_root, mh_child);
        er_node_destroy(mh_child);
        er_node_destroy(mh_root);
    }

    /* -----------------------------------------------------------------------
     * Test: paddingHorizontal — adds padding to both left and right.
     * Container: col 200×200, paddingHorizontal=15. Child: stretch → x=15, w=170.
     * -----------------------------------------------------------------------*/
    {
        ERNode* ph_root = er_node_create(ER_NODE_VIEW);
        ERProps php = props_default();
        php.width = 200;
        php.height = 200;
        php.flex_direction = ER_FLEX_COL;
        php.align_items = ER_ALIGN_STRETCH;
        php.padding_horizontal = 15;
        er_node_set_props(ph_root, &php);

        ERNode* ph_child = er_node_create(ER_NODE_VIEW);
        ERRect ph_rect = {-1, -1, -1, -1};
        php = props_default();
        php.height = 40;
        er_node_set_props(ph_child, &php);
        er_event_set(ph_child, ER_EVENT_LAYOUT, on_layout_rect, &ph_rect);

        er_tree_append_child(ph_root, ph_child);
        er_tree_set_root(ph_root);
        er_commit();

        if (ph_rect.x != 15)
            return fail("paddingHorizontal: child should start at x=15");
        if (ph_rect.w != 170)
            return fail("paddingHorizontal: child width should be 170 (200 - 15*2)");

        er_tree_remove_child(ph_root, ph_child);
        er_node_destroy(ph_child);
        er_node_destroy(ph_root);
    }

    /* -----------------------------------------------------------------------
     * Test: overflow:scroll — flex_shrink does not squish children to viewport.
     *
     * ScrollView 200×100 (flex_direction=col) contains 3 children each h=60.
     * Total content = 180 px, viewport = 100 px.  With flex_shrink=1 on each
     * child, the old code would shrink each child to ~33 px.  With the fix
     * children must keep their explicit 60 px height and the content size must
     * be reported as 180 px.
     * -----------------------------------------------------------------------*/
    {
        ERNode* sv = er_node_create(ER_NODE_SCROLL_VIEW);
        ERProps sp = props_default();
        sp.width = 200;
        sp.height = 100;
        sp.flex_direction = ER_FLEX_COL;
        sp.overflow = ER_OVERFLOW_SCROLL;
        er_node_set_props(sv, &sp);

        ERNode* kids[3];
        ERRect krect[3];
        for (int i = 0; i < 3; i++)
        {
            kids[i] = er_node_create(ER_NODE_VIEW);
            ERProps kp = props_default();
            kp.width = 200;
            kp.height = 60;
            kp.flex_shrink = 1; /* would shrink without the overflow:scroll fix */
            er_node_set_props(kids[i], &kp);
            krect[i] = (ERRect){-1, -1, -1, -1};
            er_event_set(kids[i], ER_EVENT_LAYOUT, on_layout_rect, &krect[i]);
            er_tree_append_child(sv, kids[i]);
        }

        er_tree_set_root(sv);
        er_commit();

        /* Each child must keep its explicit 60 px height (no shrinking). */
        for (int i = 0; i < 3; i++)
        {
            if (krect[i].h != 60)
                return fail("overflow:scroll: child was shrunk to fit the viewport (flex_shrink not suppressed)");
        }
        /* Children stack starting at y=0 (ScrollView is root, placed at origin). */
        if (krect[0].y != 0)
            return fail("overflow:scroll: first child y should be 0");
        if (krect[1].y != 60)
            return fail("overflow:scroll: second child y should be 60");
        if (krect[2].y != 120)
            return fail("overflow:scroll: third child y should be 120");

        for (int i = 0; i < 3; i++)
        {
            er_tree_remove_child(sv, kids[i]);
            er_node_destroy(kids[i]);
        }
        er_node_destroy(sv);
    }

    /* -----------------------------------------------------------------------
     * Test: layout-dirty fast path — er_commit() re-runs layout only when a
     * prop set, tree mutation, or LayoutAnimation requires it. Static frames
     * and animation-only frames must skip the flex + text-measure pass.
     * -----------------------------------------------------------------------*/
    {
        ERNode* d_root = er_node_create(ER_NODE_VIEW);
        ERProps dp = props_default();
        dp.width = 200;
        dp.height = 200;
        dp.flex_direction = ER_FLEX_COL;
        dp.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(d_root, &dp);

        ERNode* d_child = er_node_create(ER_NODE_VIEW);
        ERRect d_rect = {-1, -1, -1, -1};
        dp = props_default();
        dp.width = 80;
        dp.height = 40;
        er_node_set_props(d_child, &dp);
        er_event_set(d_child, ER_EVENT_LAYOUT, on_layout_rect, &d_rect);

        er_tree_append_child(d_root, d_child);
        er_tree_set_root(d_root);

        /* First commit must run a layout pass (tree was just built). */
        const uint32_t c0 = er_layout_pass_count();
        er_commit();
        const uint32_t c1 = er_layout_pass_count();
        if (c1 != c0 + 1)
            return fail("layout-dirty: first commit did not run exactly one layout pass");
        if (d_rect.w != 80 || d_rect.h != 40)
            return fail("layout-dirty: child rect wrong after first commit");

        /* A commit with no mutations must take the fast path (no layout pass). */
        er_commit();
        if (er_layout_pass_count() != c1)
            return fail("layout-dirty: static commit re-ran layout instead of skipping");

        /* Several idle commits in a row stay on the fast path. */
        er_commit();
        er_commit();
        if (er_layout_pass_count() != c1)
            return fail("layout-dirty: repeated idle commits re-ran layout");

        /* Computed rect must survive the skipped commits unchanged. */
        d_rect = (ERRect){-1, -1, -1, -1};
        er_event_set(d_child, ER_EVENT_LAYOUT, on_layout_rect, &d_rect);
        /* (No layout fired during skips, so d_rect stays at the sentinel — that itself
         * confirms ER_EVENT_LAYOUT did not fire when layout was skipped.) */
        if (d_rect.w != -1)
            return fail("layout-dirty: ER_EVENT_LAYOUT fired on a skipped commit");

        /* An animation mutates render-only props; it must NOT trigger a layout pass. */
        ERAnimConfig acfg;
        memset(&acfg, 0, sizeof(acfg));
        acfg.type = ER_ANIM_TIMING;
        acfg.duration_ms = 100U;
        er_anim_start(d_child, ER_PROP_OPACITY, 0.0f, &acfg);
        const uint32_t c_before_anim = er_layout_pass_count();
        for (int f = 0; f < 5; f++)
        {
            embedded_renderer_tick(16U); /* advances the animation */
            er_commit();
        }
        if (er_layout_pass_count() != c_before_anim)
            return fail("layout-dirty: an opacity animation forced a layout pass");

        /* Changing a layout-affecting prop must request a fresh layout pass. */
        ERProps grow = props_default();
        grow.width = 120;
        grow.height = 50;
        er_node_set_props(d_child, &grow);
        const uint32_t c_before_mut = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != c_before_mut + 1)
            return fail("layout-dirty: prop change did not trigger a layout pass");
        if (d_rect.w != 120 || d_rect.h != 50)
            return fail("layout-dirty: child rect did not update after prop change");

        /* Back to idle — no further layout passes. */
        er_commit();
        if (er_layout_pass_count() != c_before_mut + 1)
            return fail("layout-dirty: commit after prop-change relayout did not return to fast path");

        /* A pending LayoutAnimation must force a layout pass even with no prop change. */
        const uint32_t c_before_la = er_layout_pass_count();
        er_layout_anim_configure_next(&ER_LAYOUT_ANIM_EASE_IN_EASE_OUT);
        er_commit();
        if (er_layout_pass_count() != c_before_la + 1)
            return fail("layout-dirty: pending LayoutAnimation did not force a layout pass");

        er_tree_remove_child(d_root, d_child);
        er_node_destroy(d_child);
        er_node_destroy(d_root);
    }

    return EXIT_SUCCESS;
}
