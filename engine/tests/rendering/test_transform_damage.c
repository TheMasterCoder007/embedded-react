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

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
 * Before the damage pre-pass projected the 3D AABB, a source_dirty 3D/perspective node fell through to
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

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
    if (rc != EXIT_SUCCESS)
        return rc;
#endif

    return EXIT_SUCCESS;
}
