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
 * Multi-buffer (page-flip) damage replay: er_set_display_buffer_count(n).
 *
 * A page-flipped display rotates n framebuffers, so the buffer the host renders into was last shown n
 * presents ago — it is (n-1) frames stale. Pure incremental painting (n == 1) leaves everything but the
 * current damage stale in that buffer, so a moved element ghosts and a change disjoint from other damage
 * lands in only one buffer. With n > 1 the engine repaints the union of the current frame's damage and
 * the last (n-1) frames' damage, so whichever buffer is the target ends up fully current.
 *
 * These tests drive the same op-extent recorder as test_transform_damage: the measured repaint region is
 * the union of every fill/copy/blend the backend received for a commit. The assertions turn on the
 * distinguishing behaviour vs. single-buffer: with n = 2 the frame AFTER a change must STILL repaint that
 * change (replaying it into the second buffer), whereas with n = 1 the following idle frame paints nothing.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include <stdio.h>
#include <stdlib.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Backend op-extent recorder (mirrors test_transform_damage)
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

/* One displayed frame = a commit (paint the off-screen buffer) followed by a present (page-flip). The
 * present is what advances the engine to the next rotating buffer, so tests must mark it explicitly. */
static void frame(void)
{
    er_commit();
    er_display_present();
}

/* Commit+present and report whether the whole screen was repainted (a full frame). */
static bool commit_is_full(int screen)
{
    ext_reset();
    er_commit();
    const bool full = g_ext.ops > 0 && (g_ext.x1 - g_ext.x0) >= screen && (g_ext.y1 - g_ext.y0) >= screen;
    er_display_present();
    return full;
}

/* Frame repeatedly until a commit paints nothing (the scene has settled), or bail after `limit` frames. */
static int commit_until_quiescent(int limit)
{
    for (int i = 0; i < limit; i++)
    {
        ext_reset();
        er_commit();
        const int ops = g_ext.ops;
        er_display_present();
        if (ops == 0)
            return i; /* frames it took to settle */
    }
    return -1;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* Build a scene: opaque root + a small recolourable box near the top-left. Returns the box node. */
static ERNode* build_scene(int screen, ERNode** out_root)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = screen;
    rp.height = screen;
    rp.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really fills the whole screen */
    er_node_set_props(root, &rp);

    ERNode* box = er_node_create(ER_NODE_VIEW);
    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 20;
    bp.margin_top = 20;
    bp.background_color = 0xFF3366FFU;
    er_node_set_props(box, &bp);

    er_tree_append_child(root, box);
    er_tree_set_root(root);
    *out_root = root;
    return box;
}

/* Scenario 1 (the core fix): with n = 2, the frame AFTER a change replays that change into the second
 * (stale) buffer, and then — after exactly (n-1) = 1 replay frame — idle frames paint nothing again. */
static int check_double_buffer_replays_one_frame(int screen)
{
    er_set_display_buffer_count(2);
    if (er_get_display_buffer_count() != 2)
        return fail("er_get_display_buffer_count() did not report the value just set");

    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    /* First two commits fill both rotating buffers, then the scene settles to painting nothing. */
    if (!commit_is_full(screen))
        return fail("first commit after set(2) was not a full-screen repaint");
    if (!commit_is_full(screen))
        return fail("second commit after set(2) was not a full-screen repaint (both buffers must fill)");
    if (commit_until_quiescent(6) < 0)
        return fail("scene never settled to zero paint after the initial full frames");

    /* Recolour the box: exactly one bounded region changes this frame. */
    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 20;
    bp.margin_top = 20;
    bp.background_color = 0xFFEE2222U;
    er_node_set_props(box, &bp);

    ext_reset();
    er_commit(); /* frame A: paints the box into the current buffer */
    const int aops = g_ext.ops;
    const int aw = g_ext.x1 - g_ext.x0;
    const int ah = g_ext.y1 - g_ext.y0;
    er_display_present();
    printf("n=2 change frame:  ops=%d %dx%d\n", aops, aw, ah);
    if (aops == 0)
        return fail("n=2: the changing frame painted nothing");
    if (aw >= screen && ah >= screen)
        return fail("n=2: a single bounded recolour forced a full-screen repaint");

    /* frame B: NOTHING changes, but the second buffer is one present stale, so the change must REPLAY. */
    ext_reset();
    er_commit();
    const int bops = g_ext.ops;
    const int bw = g_ext.x1 - g_ext.x0;
    const int bh = g_ext.y1 - g_ext.y0;
    /* present is deferred until after er_get_dirty_rect() below, which reports THIS commit's region. */
    printf("n=2 replay frame:  ops=%d %dx%d\n", bops, bw, bh);
    if (bops == 0)
        return fail("n=2: the frame after a change did NOT replay it into the second buffer");
    if (bw >= screen && bh >= screen)
        return fail("n=2: the replay frame ballooned to a full-screen repaint");

    /* er_get_dirty_rect() must report the actually-painted (replayed) region on the replay frame. */
    ERRect dr;
    if (!er_get_dirty_rect(&dr) || dr.w <= 0 || dr.h <= 0)
        return fail("n=2: er_get_dirty_rect() reported nothing on the replay frame");
    er_display_present(); /* end of frame B */

    /* frame C: the (n-1)=1 replay window is exhausted — both buffers now hold the change, so idle. */
    ext_reset();
    er_commit();
    const int cops = g_ext.ops;
    er_display_present();
    printf("n=2 settled frame: ops=%d\n", cops);
    if (cops != 0)
        return fail("n=2: replay did not drain after (n-1) frames — idle frame still painting");

    er_node_destroy(root);
    printf("PASS: n=2 replays a change for exactly one extra frame, then settles\n");
    return EXIT_SUCCESS;
}

/* Scenario 2 (regression): with the default n = 1, the frame after a change paints nothing (pure
 * incremental into a single persistent framebuffer) — i.e. the replay behaviour is opt-in. */
static int check_single_buffer_no_replay(int screen)
{
    er_set_display_buffer_count(1);

    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    frame(); /* full first frame */
    if (commit_until_quiescent(4) < 0)
        return fail("n=1: scene never settled after the first commit");

    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 20;
    bp.margin_top = 20;
    bp.background_color = 0xFF22CC88U;
    er_node_set_props(box, &bp);

    ext_reset();
    er_commit(); /* paints the box */
    const int changed = g_ext.ops;
    er_display_present();
    if (changed == 0)
        return fail("n=1: the changing frame painted nothing");

    ext_reset();
    er_commit(); /* n=1: nothing changed, nothing replays */
    const int after = g_ext.ops;
    er_display_present();
    printf("n=1 frame-after-change: ops=%d\n", after);
    if (after != 0)
        return fail("n=1: an idle frame after a change repainted (should be pure incremental)");

    er_node_destroy(root);
    printf("PASS: n=1 is pure incremental (no replay)\n");
    return EXIT_SUCCESS;
}

/* Scenario 3: with n = 3, a change replays for (n-1) = 2 extra frames before the ring drains. */
static int check_triple_buffer_replays_two_frames(int screen)
{
    er_set_display_buffer_count(3);

    ERNode* root;
    ERNode* box = build_scene(screen, &root);

    if (!commit_is_full(screen))
        return fail("n=3: first commit was not a full-screen repaint");
    if (commit_until_quiescent(8) < 0)
        return fail("n=3: scene never settled after the initial full frames");

    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 20;
    bp.margin_top = 20;
    bp.background_color = 0xFFDD8800U;
    er_node_set_props(box, &bp);

    ext_reset();
    er_commit(); /* change frame */
    const int changed = g_ext.ops;
    er_display_present();
    if (changed == 0)
        return fail("n=3: the changing frame painted nothing");

    ext_reset();
    er_commit(); /* replay 1 of 2 */
    const int r1 = g_ext.ops;
    er_display_present();
    ext_reset();
    er_commit(); /* replay 2 of 2 */
    const int r2 = g_ext.ops;
    er_display_present();
    ext_reset();
    er_commit(); /* debt drained */
    const int r3 = g_ext.ops;
    er_display_present();
    printf("n=3 replay ops after change: %d, %d, %d\n", r1, r2, r3);

    er_node_destroy(root);

    if (r1 == 0 || r2 == 0)
        return fail("n=3: a change must replay for (n-1)=2 extra frames");
    if (r3 != 0)
        return fail("n=3: replay did not drain after (n-1)=2 frames");

    printf("PASS: n=3 replays a change for exactly two extra frames, then settles\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    EmbeddedRenderBackend be = {0};
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    const int screen = 200;

    /* er_reset() between scenarios gives each a clean scene (and drains the damage ring). */
    int rc = check_double_buffer_replays_one_frame(screen);
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();

    rc = check_single_buffer_no_replay(screen);
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();

    rc = check_triple_buffer_replays_two_frames(screen);
    if (rc != EXIT_SUCCESS)
        return rc;

    return EXIT_SUCCESS;
}
