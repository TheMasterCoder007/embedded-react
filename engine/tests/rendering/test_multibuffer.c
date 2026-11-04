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

#define OPACITY_OPAQUE 255U
#define EXTENT_FAR (1 << 29)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend op-extent recorder (mirrors test_transform_damage)
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    int min_x, min_y, max_x, max_y;
    int operation_count;
} Extent;

static Extent g_extent;

static void ext_reset(void)
{
    g_extent.min_x = g_extent.min_y = EXTENT_FAR;
    g_extent.max_x = g_extent.max_y = -EXTENT_FAR;
    g_extent.operation_count = 0;
}

static void ext_add(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    g_extent.operation_count++;
    if (x < g_extent.min_x)
        g_extent.min_x = x;
    if (y < g_extent.min_y)
        g_extent.min_y = y;
    if (x + width > g_extent.max_x)
        g_extent.max_x = x + width;
    if (y + height > g_extent.max_y)
        g_extent.max_y = y + height;
}

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)ctx;
    ext_add(x, y, width, height);
}

static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)ctx;
    ext_add(x, y, width, height);
}

static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)ctx;
    ext_add(x, y, width, height);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps props = {0};
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

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
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
    const bool full = g_extent.operation_count > 0 && (g_extent.max_x - g_extent.min_x) >= screen
                      && (g_extent.max_y - g_extent.min_y) >= screen;
    er_display_present();
    return full;
}

/* Frame repeatedly until a commit paints nothing (the scene has settled), or bail after `limit` frames. */
static int commit_until_quiescent(int limit)
{
    for (int frame_index = 0; frame_index < limit; frame_index++)
    {
        ext_reset();
        er_commit();
        const int operation_count = g_extent.operation_count;
        er_display_present();
        if (operation_count == 0)
            return frame_index; /* frames it took to settle */
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
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really fills the whole screen */
    er_node_set_props(root, &root_props);

    ERNode* box = er_node_create(ER_NODE_VIEW);
    ERProps box_props = props_default();
    box_props.width = 30;
    box_props.height = 30;
    box_props.margin_left = 20;
    box_props.margin_top = 20;
    box_props.background_color = 0xFF3366FFU;
    er_node_set_props(box, &box_props);

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
    ERProps box_props = props_default();
    box_props.width = 30;
    box_props.height = 30;
    box_props.margin_left = 20;
    box_props.margin_top = 20;
    box_props.background_color = 0xFFEE2222U;
    er_node_set_props(box, &box_props);

    ext_reset();
    er_commit(); /* frame A: paints the box into the current buffer */
    const int change_operations = g_extent.operation_count;
    const int change_width = g_extent.max_x - g_extent.min_x;
    const int change_height = g_extent.max_y - g_extent.min_y;
    er_display_present();
    printf("n=2 change frame:  ops=%d %dx%d\n", change_operations, change_width, change_height);
    if (change_operations == 0)
        return fail("n=2: the changing frame painted nothing");
    if (change_width >= screen && change_height >= screen)
        return fail("n=2: a single bounded recolour forced a full-screen repaint");

    /* frame B: NOTHING changes, but the second buffer is one present stale, so the change must REPLAY. */
    ext_reset();
    er_commit();
    const int replay_operations = g_extent.operation_count;
    const int replay_width = g_extent.max_x - g_extent.min_x;
    const int replay_height = g_extent.max_y - g_extent.min_y;
    /* present is deferred until after er_get_dirty_rect() below, which reports THIS commit's region. */
    printf("n=2 replay frame:  ops=%d %dx%d\n", replay_operations, replay_width, replay_height);
    if (replay_operations == 0)
        return fail("n=2: the frame after a change did NOT replay it into the second buffer");
    if (replay_width >= screen && replay_height >= screen)
        return fail("n=2: the replay frame ballooned to a full-screen repaint");

    /* er_get_dirty_rect() must report the actually-painted (replayed) region on the replay frame. */
    ERRect dirty;
    if (!er_get_dirty_rect(&dirty) || dirty.width <= 0 || dirty.height <= 0)
        return fail("n=2: er_get_dirty_rect() reported nothing on the replay frame");
    er_display_present(); /* end of frame B */

    /* frame C: the (n-1)=1 replay window is exhausted — both buffers now hold the change, so idle. */
    ext_reset();
    er_commit();
    const int settled_operations = g_extent.operation_count;
    er_display_present();
    printf("n=2 settled frame: ops=%d\n", settled_operations);
    if (settled_operations != 0)
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

    ERProps box_props = props_default();
    box_props.width = 30;
    box_props.height = 30;
    box_props.margin_left = 20;
    box_props.margin_top = 20;
    box_props.background_color = 0xFF22CC88U;
    er_node_set_props(box, &box_props);

    ext_reset();
    er_commit(); /* paints the box */
    const int changed = g_extent.operation_count;
    er_display_present();
    if (changed == 0)
        return fail("n=1: the changing frame painted nothing");

    ext_reset();
    er_commit(); /* n=1: nothing changed, nothing replays */
    const int after = g_extent.operation_count;
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

    ERProps box_props = props_default();
    box_props.width = 30;
    box_props.height = 30;
    box_props.margin_left = 20;
    box_props.margin_top = 20;
    box_props.background_color = 0xFFDD8800U;
    er_node_set_props(box, &box_props);

    ext_reset();
    er_commit(); /* change frame */
    const int changed = g_extent.operation_count;
    er_display_present();
    if (changed == 0)
        return fail("n=3: the changing frame painted nothing");

    ext_reset();
    er_commit(); /* replay 1 of 2 */
    const int first_replay_operations = g_extent.operation_count;
    er_display_present();
    ext_reset();
    er_commit(); /* replay 2 of 2 */
    const int second_replay_operations = g_extent.operation_count;
    er_display_present();
    ext_reset();
    er_commit(); /* debt drained */
    const int drained_operations = g_extent.operation_count;
    er_display_present();
    printf("n=3 replay ops after change: %d, %d, %d\n",
           first_replay_operations,
           second_replay_operations,
           drained_operations);

    er_node_destroy(root);

    if (first_replay_operations == 0 || second_replay_operations == 0)
        return fail("n=3: a change must replay for (n-1)=2 extra frames");
    if (drained_operations != 0)
        return fail("n=3: replay did not drain after (n-1)=2 frames");

    printf("PASS: n=3 replays a change for exactly two extra frames, then settles\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    const int screen = 200;

    /* er_reset() between scenarios gives each a clean scene (and drains the damage ring). */
    int result = check_double_buffer_replays_one_frame(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();

    result = check_single_buffer_no_replay(screen);
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();

    result = check_triple_buffer_replays_two_frames(screen);
    if (result != EXIT_SUCCESS)
        return result;

    return EXIT_SUCCESS;
}
