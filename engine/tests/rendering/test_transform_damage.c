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

#include "er_node_internal.h"
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

int main(void)
{
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    const int screen = 200;

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

    if (g_ext.ops == 0)
        return fail("animated scale produced no repaint at all");
    if (pw >= screen && ph >= screen)
        return fail("animated scale forced a full-screen repaint (transform damage not bounded)");
    if (paint_area > screen_area / 4)
        return fail("animated scale repaint region far larger than the node's box");

    printf("PASS: animated scale damages only its transformed box\n");
    return EXIT_SUCCESS;
}
