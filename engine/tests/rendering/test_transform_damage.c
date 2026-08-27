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

/*
 * Regression: an animated 2D transform (scale/rotate) must damage only its own
 * transformed box, not force a full-screen repaint every frame.
 *
 * This guards the path that drove the starter logo pulse to ~8 fps on ESP32-S3:
 * an Animated.Value bound to scaleX/scaleY (er_anim_value_bind), advanced each
 * frame. The per-frame cost is the framebuffer repaint + panel flush region,
 * which equals the union of the backend's paint ops — NOT er_get_dirty_rect(),
 * which reports semantic change only. So this records the bounding box of every
 * fill/copy/blend op and asserts a mid-pulse commit paints only the node's box.
 * Before the compositor bounded transform damage, the opaque root background
 * repainted the whole screen on every animated frame.
 *
 * Requires the full affine path (ERUI_TRANSFORMS_FULL); under TRANSLATE_ONLY the
 * scale is ignored and the node is a plain translate, so the test trivially holds.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "transform.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Backend op-extent recorder
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    int x0, y0, x1, y1;
    int ops;
} Extent;

static Extent g_ext;

static void ext_reset(void)
{
    g_ext.x0 = g_ext.y0 = 1 << 29;
    g_ext.x1 = g_ext.y1 = -(1 << 29);
    g_ext.ops = 0;
}

static void ext_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    g_ext.ops++;
    if (x < g_ext.x0)
        g_ext.x0 = x;
    if (y < g_ext.y0)
        g_ext.y0 = y;
    if (x + w > g_ext.x1)
        g_ext.x1 = x + w;
    if (y + h > g_ext.y1)
        g_ext.y1 = y + h;
}

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)ctx;
    ext_add(x, y, w, h);
}

static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)ctx;
    ext_add(x, y, w, h);
}

static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)ctx;
    ext_add(x, y, w, h);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps p = {0};
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

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

/* Scenario 1: an animated scale damages only its own box, not the whole screen. */
static int check_pulse_bounded(int screen)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really fills the whole screen */
    er_node_set_props(root, &rp);

    ERNode* logo = er_node_create(ER_NODE_VIEW);
    ERProps lp = props_default();
    lp.width = 40;
    lp.height = 40;
    lp.margin_left = 80;
    lp.margin_top = 80;
    lp.background_color = 0xFF3366FFU;
    er_node_set_props(logo, &lp);

    er_tree_append_child(root, logo);
    er_tree_set_root(root);
    er_commit(); /* frame 1: first commit is a full repaint by design */

    /* The starter logo pulse: one Animated.Value bound to scaleX AND scaleY. */
    ERAnimValueHandle pulse = er_anim_value_create(1.0f);
    er_anim_value_bind(pulse, logo, ER_PROP_SCALE_X);
    er_anim_value_bind(pulse, logo, ER_PROP_SCALE_Y);

    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 800U;
    er_anim_value_animate(pulse, 1.2f, &cfg);

    embedded_renderer_tick(100U); /* advance ~1/8: scale ~1.025, logo marked source-dirty */

    ext_reset();   /* measure ONLY the mid-pulse commit */
    er_commit();

    const int pw = g_ext.x1 - g_ext.x0;
    const int ph = g_ext.y1 - g_ext.y0;
    const long screen_area = (long)screen * screen;
    const long paint_area = (g_ext.ops > 0) ? (long)pw * ph : 0;

    printf("mid-pulse paint: ops=%d extent=%d,%d %dx%d (%.1f%% of screen)\n",
           g_ext.ops, g_ext.x0, g_ext.y0, pw, ph, 100.0 * (double)paint_area / (double)screen_area);

    er_anim_value_destroy(pulse);
    er_node_destroy(root); /* tears down the subtree */

    if (g_ext.ops == 0)
        return fail("animated scale produced no repaint at all");
    if (pw >= screen && ph >= screen)
        return fail("animated scale forced a full-screen repaint (transform damage not bounded)");
    if (paint_area > screen_area / 4)
        return fail("animated scale repaint region far larger than the node's box");

    printf("PASS: animated scale damages only its transformed box\n");
    return EXIT_SUCCESS;
}

/* Scenario 2: a STATIC scale-transformed node that a sibling reflow pushes down — without ever being
 * source_dirty itself — must still be repainted at its new position (old footprint erased). Before the
 * damage pre-pass tracked "moved" for transformed nodes, the node was skipped and its repaint clipped
 * out, leaving a stale trail. The reflowed node sits in a gap well below the resizing sibling, so its
 * damage is spatially separable from the sibling's: if the bottom of the painted region reaches the
 * node, it was repainted; if it stops at the sibling, the node was dropped. */
static int check_reflow_moved_no_trail(int screen)
{
    ERNode* root = er_node_create(ER_NODE_VIEW); /* default flex column */
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    /* Sibling A: top bar whose height we grow to force the reflow. */
    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps ap = props_default();
    ap.width = screen;
    ap.height = 20;
    ap.background_color = 0xFF333333U;
    er_node_set_props(bar, &ap);

    /* Node B: static scale-transformed box, pushed into a gap 100px below the bar. */
    ERNode* badge = er_node_create(ER_NODE_VIEW);
    ERProps bp = props_default();
    bp.width = 60;
    bp.height = 60;
    bp.margin_top = 100; /* gap → starts at y = bar.height(20) + 100 = 120 */
    bp.background_color = 0xFFEE5522U;
    bp.transform_scale_x = 1.2f; /* non-identity → exercises the complex-transform path */
    bp.transform_scale_y = 1.2f;
    er_node_set_props(badge, &bp);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, badge);
    er_tree_set_root(root);
    er_commit(); /* full first frame; records badge's painted footprint at y≈120 */

    /* Grow the bar: bar is source_dirty + a reflow shifts the badge down to y≈140. The badge itself is
     * NOT source_dirty — it only moved. */
    ap.height = 40;
    er_node_set_props(bar, &ap);

    ext_reset();
    er_commit();

    const int bottom = g_ext.y1; /* lowest painted row + 1 */
    printf("post-reflow paint: ops=%d extent=%d,%d..%d,%d (bottom=%d, badge ~y120→140)\n",
           g_ext.ops, g_ext.x0, g_ext.y0, g_ext.x1, g_ext.y1, bottom);

    er_node_destroy(root);

    if (g_ext.ops == 0)
        return fail("reflow produced no repaint at all");
    /* The bar alone only reaches y≈40. If the badge was repainted, the region extends past the gap to
     * its position near y≈120–200. A bottom that stops short of the badge means it was left as a trail. */
    if (bottom < 110)
        return fail("reflowed transformed node was not repainted (stale trail) — moved damage missing");

    printf("PASS: reflow-moved transformed node is repainted (no trail)\n");
    return EXIT_SUCCESS;
}

/* Scenario 3: an ActivityIndicator carrying a translate must be PAINTED at the offset — the same place
 * node_screen_rect() measures it and hit-testing answers for it.
 *
 * render_tree gates its transform block on the node type, meant only to keep the spinner off the
 * capture path: tp_rotate_z is its internal spin angle, and rasterising it into the transform scratch
 * would distort it. Gating the WHOLE block dropped the translate fast path with it, so a translated
 * spinner painted at its raw layout box while the pre-pass measured it at the offset one. The two could
 * never agree, `moved` latched, and it re-damaged both boxes on every commit for the rest of the run —
 * the same pathology scenarios 4 and 5 below guard for scale, reached without any transform scratch
 * being involved at all.
 *
 * The observable is where the pixels land: recolouring the spinner repaints its footprint and nothing
 * else, so a repaint that reaches back to the raw box means it was drawn there.
 *
 * @param[in] screen  Root size.
 */
static int check_activity_indicator_translate(int screen)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = (int16_t)screen;
    rp.height = (int16_t)screen;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    /* Laid out top-left, drawn well clear of it: the offset is larger than the node, so the raw box and
     * the translated one do not touch and a repaint of either is unambiguous. */
    const int box = 40, off = 60;
    ERNode* spinner = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    ERProps sp = props_default();
    sp.position = ER_POS_ABSOLUTE;
    sp.left = 20;
    sp.top = 20;
    sp.width = (int16_t)box;
    sp.height = (int16_t)box;
    sp.color = 0xFF3366FFU;
    sp.transform_translate_x = (float)off;
    sp.transform_translate_y = (float)off;
    er_node_set_props(spinner, &sp);

    er_tree_append_child(root, spinner);
    er_tree_set_root(root);

    er_commit(); /* full first frame */
    er_commit(); /* settle: the spinner has a last-painted footprint now */

    /* Recolour it — the one and only change, so the repaint is its footprint alone. */
    sp.color = 0xFFFF00FFU;
    er_node_set_props(spinner, &sp);
    ext_reset();
    er_commit();
    const int cx0 = g_ext.x0, cy0 = g_ext.y0, cops = g_ext.ops;

    /* Nothing changes here: a footprint the pre-pass and the paint agree on reports nothing new, so the
     * last painting commit's rects are what stays readable. */
    er_commit();
    ERRect rep = {0, 0, 0, 0};
    er_get_dirty_rect(&rep);

    printf("activity indicator translate: recolour ops=%d from %d,%d (laid out 20,20, drawn %d,%d), "
           "after idle reported %d,%d %dx%d\n",
           cops,
           cx0,
           cy0,
           20 + off,
           20 + off,
           rep.x,
           rep.y,
           rep.w,
           rep.h);

    er_node_destroy(root);

    /* The raw box ends at 20 + box; the drawn one starts at 20 + off. Anything at or past this cleared
     * the raw box entirely, with slack for the padding add_damage applies before inserting a rect. */
    const int clear = 20 + off - 10;
    if (cops == 0)
        return fail("recolouring the activity indicator repainted nothing");
    if (cx0 < clear || cy0 < clear)
        return fail("the activity indicator was painted at its raw box — its translate was dropped");
    if (rep.x < clear || rep.y < clear)
        return fail("an idle commit re-reported the activity indicator's raw box: paint and pre-pass "
                    "disagree about where it is");

    printf("PASS: a translated activity indicator is painted at its offset\n");
    return EXIT_SUCCESS;
}

#if ERUI_TRANSFORMS_FULL
/* Scenario 4: a transformed node too large for the transform scratch must not become a permanent damage
 * source. er_transform_source_begin() refuses a node wider/taller than ERUI_XFORM_W/H, and render_tree
 * then degrades to painting it UNTRANSFORMED at its raw box — but the damage pre-pass used to measure
 * every transformed node by its transformed AABB regardless. last_paint_rect held the box while the
 * pre-pass computed an AABB, the two could never agree, `moved` latched true, and the node damaged and
 * re-reported its own footprint on every commit for the rest of the run.
 *
 * Nothing looked wrong (the box the pre-pass adds covers what the fallback paints), so the observable is
 * the cost: a commit where only a distant marker changed must repaint — and report — the marker and
 * nothing else. A move-only damage sets no dirty flag, so a wholly idle commit paints nothing either
 * way; it is asserted anyway so the fix cannot buy the marker commit back with a spurious idle repaint.
 * Run against BOTH sizes — the node that fits the scratch is the control, and it has to come out
 * identical.
 *
 * @param[in] side            Node size (square); the capture succeeds only when it clears BOTH
 *                            ERUI_XFORM_W and ERUI_XFORM_H.
 * @param[in] screen          Root size.
 * @param[in] expect_capture  Which path this case is meant to exercise — asserted, not assumed.
 * @param[in] label           Printed name for the case.
 */
static int check_oversized_no_idle_damage(int side, int screen, bool expect_capture, const char* label)
{
    /* er_transform_source_fits() needs BOTH dimensions inside the limits, so a square sized off one axis
     * alone can silently miss the other. Without this, a mis-sized "control" quietly takes the same
     * raw-box fallback as the oversized case, every assertion below still passes, and the captured side
     * of the pre-pass goes uncovered. */
    if (er_transform_source_fits(side, side) != expect_capture)
        return fail(expect_capture ? "control node does not fit the transform scratch — it takes the "
                                     "raw-box fallback too and covers nothing extra"
                                   : "oversized node still fits the transform scratch");

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    /* Scaled-down box in the top-left. Nothing ever changes about it after the first commit. */
    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps np = props_default();
    np.position = ER_POS_ABSOLUTE;
    np.left = 10;
    np.top = 10;
    np.width = (int16_t)side;
    np.height = (int16_t)side;
    np.background_color = 0xFF33AA55U;
    np.transform_scale_x = 0.3f;
    np.transform_scale_y = 0.3f;
    er_node_set_props(node, &np);

    /* The only thing that changes, parked clear of the node's raw box AND its scaled AABB. */
    const int mx = side + 40;
    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps mp = props_default();
    mp.position = ER_POS_ABSOLUTE;
    mp.left = (int16_t)mx;
    mp.top = (int16_t)mx;
    mp.width = 40;
    mp.height = 40;
    mp.background_color = 0xFF00AAFFU;
    er_node_set_props(marker, &mp);

    er_tree_append_child(root, node);
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    er_commit(); /* full first frame */
    er_commit(); /* settle: everything has a last-painted footprint now */

    /* Recolour the marker — the one and only change. */
    mp.background_color = 0xFFFF00FFU;
    er_node_set_props(marker, &mp);
    ext_reset();
    er_commit();
    const int cx0 = g_ext.x0, cy0 = g_ext.y0, cops = g_ext.ops;
    ERRect rep = {0, 0, 0, 0};
    er_get_dirty_rect(&rep);

    /* Nothing at all changes here. */
    ext_reset();
    er_commit();
    const int idle_ops = g_ext.ops;

    printf("%s: marker commit ops=%d from %d,%d (marker at %d,%d), reported %d,%d %dx%d; idle ops=%d\n",
           label,
           cops,
           cx0,
           cy0,
           mx,
           mx,
           rep.x,
           rep.y,
           rep.w,
           rep.h,
           idle_ops);

    er_node_destroy(root);

    /* The node's raw box ends at side + 10 = mx - 30 and its scaled AABB is well inside that, so a
     * repaint (or a report) that starts before this stayed clear of both. The slack absorbs the few
     * pixels add_damage pads a rect by before inserting it. */
    const int clear = mx - 20;
    if (cops == 0)
        return fail("the marker change repainted nothing");
    if (cx0 < clear || cy0 < clear)
        return fail("the marker change dragged the transformed node's region into the repaint");
    if (rep.x < clear || rep.y < clear)
        return fail("the marker change reported the transformed node's region as dirty");
    if (idle_ops != 0)
        return fail("an idle commit repainted — the transformed node damages its footprint forever");

    printf("PASS: %s contributes no damage when nothing about it changed\n", label);
    return EXIT_SUCCESS;
}

/* Scenario 5: the other half of the same defect, reached where geometry cannot predict it. Only one
 * transform capture can be active at a time, so a transformed node INSIDE another transformed node is
 * refused the scratch and painted at its raw box however small it is. Nothing about its size says so —
 * the fallback is only knowable from what the last paint actually did — and without that, the pre-pass
 * measured it by its AABB and it re-damaged the outer node's whole region on every commit.
 *
 * @param[in] screen  Root size.
 */
static int check_nested_transform_no_damage(int screen)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    /* Outer transform: comfortably within the scratch, so ITS capture succeeds and stays active for the
     * whole of the subtree below it. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps op = props_default();
    op.position = ER_POS_ABSOLUTE;
    op.left = 10;
    op.top = 10;
    op.width = 100;
    op.height = 100;
    op.background_color = 0xFF33AA55U;
    op.transform_rotate_z = 20.0f;
    er_node_set_props(outer, &op);

    /* Inner transform: tiny, but refused the scratch because the outer capture holds it. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps ip = props_default();
    ip.position = ER_POS_ABSOLUTE;
    ip.left = 10;
    ip.top = 10;
    ip.width = 40;
    ip.height = 40;
    ip.background_color = 0xFFAA3355U;
    ip.transform_scale_x = 0.5f;
    ip.transform_scale_y = 0.5f;
    er_node_set_props(inner, &ip);

    const int mx = screen - 50;
    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps mp = props_default();
    mp.position = ER_POS_ABSOLUTE;
    mp.left = (int16_t)mx;
    mp.top = (int16_t)mx;
    mp.width = 30;
    mp.height = 30;
    mp.background_color = 0xFF00AAFFU;
    er_node_set_props(marker, &mp);

    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    er_commit();
    er_commit();

    mp.background_color = 0xFFFF00FFU;
    er_node_set_props(marker, &mp);
    ext_reset();
    er_commit();
    ERRect rep = {0, 0, 0, 0};
    er_get_dirty_rect(&rep);

    printf("nested transform: marker commit ops=%d from %d,%d (marker at %d,%d), reported %d,%d %dx%d\n",
           g_ext.ops,
           g_ext.x0,
           g_ext.y0,
           mx,
           mx,
           rep.x,
           rep.y,
           rep.w,
           rep.h);

    er_node_destroy(root);

    const int clear = mx - 20; /* the outer node ends at 110, far above this */
    if (g_ext.ops == 0)
        return fail("the marker change repainted nothing");
    if (g_ext.x0 < clear || g_ext.y0 < clear)
        return fail("the marker change dragged the nested transform's region into the repaint");
    if (rep.x < clear || rep.y < clear)
        return fail("the marker change reported the nested transform's region as dirty");

    printf("PASS: a nested transform contributes no damage when nothing about it changed\n");
    return EXIT_SUCCESS;
}

#endif /* ERUI_TRANSFORMS_FULL */

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
/* Before the damage pre-pass projected the 3D AABB, a source_dirty 3D/perspective node fell through to
 * the full-repaint fallback on every animated frame. */
static int check_3d_rotate_bounded(int screen)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.width = 40;
    cp.height = 40;
    cp.margin_left = 80;
    cp.margin_top = 80;
    cp.background_color = 0xFF22CC88U;
    cp.transform_rotate_y = 1.0f; /* non-zero → exercises the 3D/perspective path from the first commit */
    er_node_set_props(card, &cp);

    er_tree_append_child(root, card);
    er_tree_set_root(root);
    er_commit(); /* full first frame */

    ERAnimValueHandle spin = er_anim_value_create(1.0f);
    er_anim_value_bind(spin, card, ER_PROP_ROTATE_Y);

    ERAnimConfig cfg = {0};
    cfg.type = ER_ANIM_TIMING;
    cfg.duration_ms = 800U;
    er_anim_value_animate(spin, 40.0f, &cfg);

    embedded_renderer_tick(100U); /* rotateY advances → card source-dirty, still 3D */

    ext_reset();
    er_commit();

    const int pw = g_ext.x1 - g_ext.x0;
    const int ph = g_ext.y1 - g_ext.y0;
    const long screen_area = (long)screen * screen;
    const long paint_area = (g_ext.ops > 0) ? (long)pw * ph : 0;

    printf("mid-spin (3D) paint: ops=%d extent=%d,%d %dx%d (%.1f%% of screen)\n",
           g_ext.ops, g_ext.x0, g_ext.y0, pw, ph, 100.0 * (double)paint_area / (double)screen_area);

    er_anim_value_destroy(spin);
    er_node_destroy(root);

    if (g_ext.ops == 0)
        return fail("animated 3D transform produced no repaint at all");
    if (pw >= screen && ph >= screen)
        return fail("animated 3D transform forced a full-screen repaint (3D damage not bounded)");
    if (paint_area > screen_area / 4)
        return fail("animated 3D transform repaint region far larger than the node's box");

    printf("PASS: animated 3D transform damages only its projected box\n");
    return EXIT_SUCCESS;
}
#endif /* ERUI_3D_TRANSFORMS */

int main(void)
{
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    const int screen = 200;

    int rc = check_pulse_bounded(screen);
    if (rc != EXIT_SUCCESS)
        return rc;

    rc = check_reflow_moved_no_trail(screen);
    if (rc != EXIT_SUCCESS)
        return rc;

    rc = check_activity_indicator_translate(screen);
    if (rc != EXIT_SUCCESS)
        return rc;

#if ERUI_TRANSFORMS_FULL
    const int fits = (ERUI_XFORM_W < ERUI_XFORM_H) ? ERUI_XFORM_W : ERUI_XFORM_H;
    const int limit = (ERUI_XFORM_W > ERUI_XFORM_H) ? ERUI_XFORM_W : ERUI_XFORM_H;
    const int over = limit + 1;
    const int big_screen = over + 120;

    rc = check_oversized_no_idle_damage(fits, big_screen, true, "node within the scratch limit");
    if (rc != EXIT_SUCCESS)
        return rc;

    rc = check_oversized_no_idle_damage(over, big_screen, false, "node over the scratch limit");
    if (rc != EXIT_SUCCESS)
        return rc;

    rc = check_nested_transform_no_damage(300);
    if (rc != EXIT_SUCCESS)
        return rc;
#endif

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
    rc = check_3d_rotate_bounded(screen);
    if (rc != EXIT_SUCCESS)
        return rc;
#endif

    return EXIT_SUCCESS;
}
