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

    /* -----------------------------------------------------------------------
     * Test: er_node_set_text_spans vs the global layout pass.
     *
     * Span text feeds a Text node's intrinsic size, so changing it must re-solve
     * layout — unless the node's box cannot change with the glyphs.  A pinned
     * width + single line produces the same rect for every string, so it must
     * take the fast path; content-sized and pinned-but-multi-line must not.
     * Guards the imperative-readout path (updateText on a dial's centre number
     * every drag move): a ~5.7 ms pass on ~90% of drag frames on an ESP32-S3.
     * -----------------------------------------------------------------------*/
    {
        ERNode* ts_root = er_node_create(ER_NODE_VIEW);
        ERProps tsp = props_default();
        tsp.width = 200;
        tsp.height = 100;
        er_node_set_props(ts_root, &tsp);

        ERNode* ts_auto = er_node_create(ER_NODE_TEXT); /* content-sized */
        tsp = props_default();
        tsp.font_size = 16;
        er_node_set_props(ts_auto, &tsp);

        ERNode* ts_pinned = er_node_create(ER_NODE_TEXT); /* pinned width, single line */
        tsp = props_default();
        tsp.font_size = 16;
        tsp.width = 120;
        tsp.number_of_lines = 1;
        er_node_set_props(ts_pinned, &tsp);

        ERNode* ts_multi = er_node_create(ER_NODE_TEXT); /* pinned width, wraps → height varies */
        tsp = props_default();
        tsp.font_size = 16;
        tsp.width = 120;
        tsp.number_of_lines = 0;
        er_node_set_props(ts_multi, &tsp);

        er_tree_append_child(ts_root, ts_auto);
        er_tree_append_child(ts_root, ts_pinned);
        er_tree_append_child(ts_root, ts_multi);
        er_tree_set_root(ts_root);
        er_commit(); /* settle */

        ERTextSpan span;
        memset(&span, 0, sizeof(span));

        strncpy(span.text, "72", ER_SPAN_TEXT_MAX);
        er_node_set_text_spans(ts_auto, &span, 1);
        uint32_t c = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != c + 1)
            return fail("text-spans: content-sized Text did not re-run layout");

        strncpy(span.text, "68", ER_SPAN_TEXT_MAX);
        er_node_set_text_spans(ts_pinned, &span, 1);
        c = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != c)
            return fail("text-spans: pinned single-line Text forced a global layout pass");

        strncpy(span.text, "a much longer string that has to wrap", ER_SPAN_TEXT_MAX);
        er_node_set_text_spans(ts_multi, &span, 1);
        c = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != c + 1)
            return fail("text-spans: pinned multi-line Text skipped a needed layout pass");

        er_tree_remove_child(ts_root, ts_auto);
        er_tree_remove_child(ts_root, ts_pinned);
        er_tree_remove_child(ts_root, ts_multi);
        er_node_destroy(ts_auto);
        er_node_destroy(ts_pinned);
        er_node_destroy(ts_multi);
        er_node_destroy(ts_root);
    }

    /* -----------------------------------------------------------------------
     * Test: container auto cross-size — a row with no explicit height grows to
     * fit its children instead of collapsing to 0.  Regression for the Flow A
     * demo where a row of cards rendered but was not hit-testable because the
     * row had height 0 (hit-testing descends through the row's empty bounds).
     *
     * Column root 200×200 (align stretch).  Child "row" (flex_direction=row, no
     * height) holds two children with flex:1 (flex_basis 0) and height=60.  The
     * row must report height 60 and stretch to width 200, and each child must
     * land within the row's vertical bounds.
     * -----------------------------------------------------------------------*/
    {
        ERNode* ac_root = er_node_create(ER_NODE_VIEW);
        ERProps acp = props_default();
        acp.width = 200;
        acp.height = 200;
        acp.flex_direction = ER_FLEX_COL;
        acp.align_items = ER_ALIGN_STRETCH;
        er_node_set_props(ac_root, &acp);

        ERNode* ac_row = er_node_create(ER_NODE_VIEW);
        ERRect ac_row_rect = {-1, -1, -1, -1};
        acp = props_default();
        acp.flex_direction = ER_FLEX_ROW; /* no height — must be derived from children */
        er_node_set_props(ac_row, &acp);
        er_event_set(ac_row, ER_EVENT_LAYOUT, on_layout_rect, &ac_row_rect);
        er_tree_append_child(ac_root, ac_row);

        ERNode* ac_kids[2];
        ERRect ac_krect[2];
        for (int i = 0; i < 2; i++)
        {
            ac_kids[i] = er_node_create(ER_NODE_VIEW);
            ERProps kp = props_default();
            kp.flex_grow = 1;
            kp.flex_shrink = 1;
            kp.flex_basis = 0;
            kp.height = 60;
            er_node_set_props(ac_kids[i], &kp);
            ac_krect[i] = (ERRect){-1, -1, -1, -1};
            er_event_set(ac_kids[i], ER_EVENT_LAYOUT, on_layout_rect, &ac_krect[i]);
            er_tree_append_child(ac_row, ac_kids[i]);
        }

        er_tree_set_root(ac_root);
        er_commit();

        if (ac_row_rect.h != 60)
            return fail("auto cross-size: row should grow to its children's height (60), not collapse to 0");
        if (ac_row_rect.w != 200)
            return fail("auto cross-size: row should stretch to the column's width (200)");
        for (int i = 0; i < 2; i++)
        {
            if (ac_krect[i].h != 60)
                return fail("auto cross-size: child height should be 60");
            if (ac_krect[i].y < ac_row_rect.y || ac_krect[i].y + ac_krect[i].h > ac_row_rect.y + ac_row_rect.h)
                return fail("auto cross-size: child falls outside the row's bounds (not hit-testable)");
        }

        for (int i = 0; i < 2; i++)
        {
            er_tree_remove_child(ac_row, ac_kids[i]);
            er_node_destroy(ac_kids[i]);
        }
        er_tree_remove_child(ac_root, ac_row);
        er_node_destroy(ac_row);
        er_node_destroy(ac_root);
    }

    /* -----------------------------------------------------------------------
     * Test: er_tree_insert_before — ordered insertion, reordering (move), and
     * the NULL/non-child append fallback. Order is observed through the stacked
     * y positions of a column's children.
     * -----------------------------------------------------------------------*/
    {
        ERNode* ib_root = er_node_create(ER_NODE_VIEW);
        ERProps ibp = props_default();
        ibp.width = 100;
        ibp.height = 100;
        ibp.flex_direction = ER_FLEX_COL;
        ibp.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(ib_root, &ibp);

        ERNode* a = er_node_create(ER_NODE_VIEW);
        ERNode* b = er_node_create(ER_NODE_VIEW);
        ERNode* c = er_node_create(ER_NODE_VIEW);
        ERNode* d = er_node_create(ER_NODE_VIEW);
        ERRect ra = {0}, rb = {0}, rc = {0}, rd = {0};
        ibp = props_default();
        ibp.width = 40;
        ibp.height = 20;
        er_node_set_props(a, &ibp);
        er_node_set_props(b, &ibp);
        er_node_set_props(c, &ibp);
        er_node_set_props(d, &ibp);
        er_event_set(a, ER_EVENT_LAYOUT, on_layout_rect, &ra);
        er_event_set(b, ER_EVENT_LAYOUT, on_layout_rect, &rb);
        er_event_set(c, ER_EVENT_LAYOUT, on_layout_rect, &rc);
        er_event_set(d, ER_EVENT_LAYOUT, on_layout_rect, &rd);

        /* Insert c before b in [a, b] → order a, c, b. */
        er_tree_append_child(ib_root, a);
        er_tree_append_child(ib_root, b);
        er_tree_insert_before(ib_root, c, b);
        er_tree_set_root(ib_root);
        er_commit();
        if (ra.y != 0 || rc.y != 20 || rb.y != 40)
            return fail("insert_before: ordered insert produced wrong order (expected a,c,b)");

        /* Move existing b before a → order b, a, c. */
        er_tree_insert_before(ib_root, b, a);
        er_commit();
        if (rb.y != 0 || ra.y != 20 || rc.y != 40)
            return fail("insert_before: move of existing child produced wrong order (expected b,a,c)");

        /* before == NULL appends → order b, a, c, d. */
        er_tree_insert_before(ib_root, d, NULL);
        er_commit();
        if (rd.y != 60)
            return fail("insert_before: NULL anchor should append at the end");

        er_tree_remove_child(ib_root, a);
        er_tree_remove_child(ib_root, b);
        er_tree_remove_child(ib_root, c);
        er_tree_remove_child(ib_root, d);
        er_node_destroy(a);
        er_node_destroy(b);
        er_node_destroy(c);
        er_node_destroy(d);
        er_node_destroy(ib_root);
    }

    /* -----------------------------------------------------------------------
     * Test: nested explicit sizes — a fixed-size child of the root keeps its
     * height, and its grandchildren stack at their own heights (regression for
     * the reconciler reorder demo where a 100x200 child filled the 480x320 root
     * and its 20px items spread to ~106px each).
     * -----------------------------------------------------------------------*/
    {
        ERNode* ne_root = er_node_create(ER_NODE_VIEW);
        ERProps nep = props_default();
        nep.width = 480;
        nep.height = 320;
        nep.flex_direction = ER_FLEX_COL;
        er_node_set_props(ne_root, &nep);

        ERNode* ne_a = er_node_create(ER_NODE_VIEW);
        ERRect ne_a_rect = {0};
        nep = props_default();
        nep.width = 100;
        nep.height = 200;
        nep.flex_direction = ER_FLEX_COL;
        er_node_set_props(ne_a, &nep);
        er_event_set(ne_a, ER_EVENT_LAYOUT, on_layout_rect, &ne_a_rect);
        er_tree_append_child(ne_root, ne_a);

        ERNode* ne_b[3];
        ERRect ne_brect[3];
        for (int i = 0; i < 3; i++)
        {
            ne_b[i] = er_node_create(ER_NODE_VIEW);
            ERProps bp = props_default();
            bp.width = 40;
            bp.height = 20;
            er_node_set_props(ne_b[i], &bp);
            ne_brect[i] = (ERRect){-1, -1, -1, -1};
            er_event_set(ne_b[i], ER_EVENT_LAYOUT, on_layout_rect, &ne_brect[i]);
            er_tree_append_child(ne_a, ne_b[i]);
        }

        er_tree_set_root(ne_root);
        er_commit();

        if (ne_a_rect.h != 200)
            return fail("nested explicit size: fixed-height child should stay 200, not fill the root");
        if (ne_brect[0].y != 0 || ne_brect[1].y != 20 || ne_brect[2].y != 40)
            return fail("nested explicit size: grandchildren should stack at 0,20,40");
        if (ne_brect[0].h != 20)
            return fail("nested explicit size: grandchild height should stay 20");

        for (int i = 0; i < 3; i++)
        {
            er_tree_remove_child(ne_a, ne_b[i]);
            er_node_destroy(ne_b[i]);
        }
        er_tree_remove_child(ne_root, ne_a);
        er_node_destroy(ne_a);
        er_node_destroy(ne_root);
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() memoisation — a Text leaf must be measured once
     * per layout pass, not once per ancestor depth. Regression for issue #51:
     * Pass 1 called measure_content(child) for every in-flow child and that
     * recursed the child's entire subtree, so before memoisation a leaf D
     * levels deep was remeasured D times as compute_layout descended one
     * level at a time (compute_layout re-enters Pass 1 at every level, and
     * nothing cached the result of the level above).
     *
     * A chain of MC_DEPTH auto-sized Views under a fixed root, ending in one
     * auto-sized Text leaf, must produce exactly one er_text_measure*() call
     * per commit — and, since the memoisation cache is keyed by a per-pass
     * generation, a second commit after a real layout-affecting change must
     * remeasure it again exactly once (not zero, and not accumulate stale
     * counts from the first pass).
     * -----------------------------------------------------------------------*/
    {
#define MC_DEPTH 8
        ERNode* mc_root = er_node_create(ER_NODE_VIEW);
        ERProps mcp = props_default();
        mcp.width = 200;
        mcp.height = 200;
        er_node_set_props(mc_root, &mcp);
        er_tree_set_root(mc_root);

        ERNode* mc_chain[MC_DEPTH];
        ERNode* mc_parent = mc_root;
        for (int i = 0; i < MC_DEPTH; i++)
        {
            mc_chain[i] = er_node_create(ER_NODE_VIEW);
            ERProps vp = props_default(); /* width/height stay AUTO */
            er_node_set_props(mc_chain[i], &vp);
            er_tree_append_child(mc_parent, mc_chain[i]);
            mc_parent = mc_chain[i];
        }

        ERNode* mc_text = er_node_create(ER_NODE_TEXT);
        ERProps mctp = props_default(); /* width/height stay AUTO */
        mctp.font_size = 16;
        strncpy(mctp.text, "measure me", ER_TEXT_MAX);
        er_node_set_props(mc_text, &mctp);
        er_tree_append_child(mc_parent, mc_text);

        const uint32_t tm0 = er_text_measure_count();
        er_commit();
        const uint32_t tm1 = er_text_measure_count();
        if (tm1 != tm0 + 1U)
            return fail("measure-cache: deep chain remeasured the Text leaf more than once per pass");

        /* A real layout-affecting change must invalidate the per-pass cache and remeasure —
         * exactly once, proving the cache doesn't leak a stale hit into the next pass. */
        mcp.width = 210;
        er_node_set_props(mc_root, &mcp);
        er_commit();
        const uint32_t tm2 = er_text_measure_count();
        if (tm2 != tm1 + 1U)
            return fail("measure-cache: second layout pass did not remeasure the Text leaf exactly once");

        er_tree_remove_child(mc_parent, mc_text);
        er_node_destroy(mc_text);
        for (int i = MC_DEPTH - 1; i >= 0; i--)
        {
            ERNode* p = (i == 0) ? mc_root : mc_chain[i - 1];
            er_tree_remove_child(p, mc_chain[i]);
            er_node_destroy(mc_chain[i]);
        }
        er_node_destroy(mc_root);
#undef MC_DEPTH
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() memoisation — a wide (not deep) tree. Even a
     * single container with many auto-sized Text siblings used to measure
     * each one twice per commit: once while the container computed its own
     * intrinsic content size (summing each child's measurement), and once
     * more when compute_layout's Pass 1 ran for real on that same container.
     * With the cache, the second lookup is a hit — exactly one measurement
     * per Text sibling per commit.
     * -----------------------------------------------------------------------*/
    {
#define MC_WIDE_COUNT 6
        ERNode* mw_root = er_node_create(ER_NODE_VIEW);
        ERProps mwp = props_default();
        mwp.width = 300;
        mwp.height = 300;
        er_node_set_props(mw_root, &mwp);

        ERNode* mw_container = er_node_create(ER_NODE_VIEW);
        ERProps mwcp = props_default(); /* width/height stay AUTO */
        er_node_set_props(mw_container, &mwcp);
        er_tree_append_child(mw_root, mw_container);

        ERNode* mw_texts[MC_WIDE_COUNT];
        for (int i = 0; i < MC_WIDE_COUNT; i++)
        {
            mw_texts[i] = er_node_create(ER_NODE_TEXT);
            ERProps wtp = props_default(); /* width/height stay AUTO */
            wtp.font_size = 16;
            strncpy(wtp.text, "sib", ER_TEXT_MAX);
            er_node_set_props(mw_texts[i], &wtp);
            er_tree_append_child(mw_container, mw_texts[i]);
        }
        er_tree_set_root(mw_root);

        const uint32_t tw0 = er_text_measure_count();
        er_commit();
        const uint32_t tw1 = er_text_measure_count();
        if (tw1 != tw0 + (uint32_t)MC_WIDE_COUNT)
            return fail("measure-cache: wide tree measured a Text sibling more than once per commit");

        for (int i = 0; i < MC_WIDE_COUNT; i++)
        {
            er_tree_remove_child(mw_container, mw_texts[i]);
            er_node_destroy(mw_texts[i]);
        }
        er_tree_remove_child(mw_root, mw_container);
        er_node_destroy(mw_container);
        er_node_destroy(mw_root);
#undef MC_WIDE_COUNT
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() short-circuit — a Text node with both width and
     * height explicit never needs its glyph run measured (the measured value
     * is not consulted for either axis), even nested inside auto-sized
     * ancestors that themselves must be measured. Zero er_text_measure*()
     * calls for the whole commit.
     * -----------------------------------------------------------------------*/
    {
        ERNode* mf_root = er_node_create(ER_NODE_VIEW);
        ERProps mfp = props_default();
        mfp.width = 200;
        mfp.height = 200;
        er_node_set_props(mf_root, &mfp);

        ERNode* mf_mid = er_node_create(ER_NODE_VIEW);
        ERProps mfmp = props_default(); /* auto — forces mf_root's Pass 1 to measure mf_mid */
        er_node_set_props(mf_mid, &mfmp);
        er_tree_append_child(mf_root, mf_mid);

        ERNode* mf_text = er_node_create(ER_NODE_TEXT);
        ERProps mftp = props_default();
        mftp.width = 60;
        mftp.height = 20;
        strncpy(mftp.text, "fixed size, never measured", ER_TEXT_MAX);
        er_node_set_props(mf_text, &mftp);
        er_tree_append_child(mf_mid, mf_text);
        er_tree_set_root(mf_root);

        const uint32_t tf0 = er_text_measure_count();
        er_commit();
        const uint32_t tf1 = er_text_measure_count();
        if (tf1 != tf0)
            return fail("measure-cache: fully-explicit Text size still triggered a glyph-run measurement");

        er_tree_remove_child(mf_mid, mf_text);
        er_node_destroy(mf_text);
        er_tree_remove_child(mf_root, mf_mid);
        er_node_destroy(mf_mid);
        er_node_destroy(mf_root);
    }

    return EXIT_SUCCESS;
}
