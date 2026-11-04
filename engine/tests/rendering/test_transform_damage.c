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
 * an Animated.Value bound to scaleX/scaleY (er_animation_value_bind), advanced each
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

#define EXTENT_FAR (1 << 29)
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Backend op-extent recorder
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

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

/* Scenario 1: an animated scale damages only its own box, not the whole screen. */
static int check_pulse_bounded(int screen)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really fills the whole screen */
    er_node_set_props(root, &root_props);

    ERNode* logo = er_node_create(ER_NODE_VIEW);
    ERProps logo_props = props_default();
    logo_props.width = 40;
    logo_props.height = 40;
    logo_props.margin_left = 80;
    logo_props.margin_top = 80;
    logo_props.background_color = 0xFF3366FFU;
    er_node_set_props(logo, &logo_props);

    er_tree_append_child(root, logo);
    er_tree_set_root(root);
    er_commit(); /* frame 1: first commit is a full repaint by design */

    /* The starter logo pulse: one Animated.Value bound to scaleX AND scaleY. */
    ERAnimationValueHandle pulse = er_animation_value_create(1.0f);
    er_animation_value_bind(pulse, logo, ER_PROP_SCALE_X);
    er_animation_value_bind(pulse, logo, ER_PROP_SCALE_Y);

    ERAnimationConfig config = {0};
    config.type = ER_ANIMATION_TIMING;
    config.duration_ms = 800U;
    er_animation_value_animate(pulse, 1.2f, &config);

    embedded_renderer_tick(100U); /* advance ~1/8: scale ~1.025, logo marked source-dirty */

    ext_reset(); /* measure ONLY the mid-pulse commit */
    er_commit();

    const int painted_width = g_extent.max_x - g_extent.min_x;
    const int painted_height = g_extent.max_y - g_extent.min_y;
    const long screen_area = (long)screen * screen;
    const long paint_area = (g_extent.operation_count > 0) ? (long)painted_width * painted_height : 0;

    printf("mid-pulse paint: ops=%d extent=%d,%d %dx%d (%.1f%% of screen)\n",
           g_extent.operation_count,
           g_extent.min_x,
           g_extent.min_y,
           painted_width,
           painted_height,
           100.0 * (double)paint_area / (double)screen_area);

    er_animation_value_destroy(pulse);
    er_node_destroy(root); /* tears down the subtree */

    if (g_extent.operation_count == 0)
        return fail("animated scale produced no repaint at all");
    if (painted_width >= screen && painted_height >= screen)
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
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW); /* default flex column */
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    /* Sibling A: top bar whose height we grow to force the reflow. */
    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bar_props = props_default();
    bar_props.width = screen;
    bar_props.height = 20;
    bar_props.background_color = 0xFF333333U;
    er_node_set_props(bar, &bar_props);

    /* Node B: static scale-transformed box, pushed into a gap 100px below the bar. */
    ERNode* badge = er_node_create(ER_NODE_VIEW);
    ERProps badge_props = props_default();
    badge_props.width = 60;
    badge_props.height = 60;
    badge_props.margin_top = 100; /* gap → starts at y = bar.height(20) + 100 = 120 */
    badge_props.background_color = 0xFFEE5522U;
    badge_props.transform_scale_x = 1.2f; /* non-identity → exercises the complex-transform path */
    badge_props.transform_scale_y = 1.2f;
    er_node_set_props(badge, &badge_props);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, badge);
    er_tree_set_root(root);
    er_commit(); /* full first frame; records badge's painted footprint at y≈120 */

    /* Grow the bar: bar is source_dirty + a reflow shifts the badge down to y≈140. The badge itself is
     * NOT source_dirty — it only moved. */
    bar_props.height = 40;
    er_node_set_props(bar, &bar_props);

    ext_reset();
    er_commit();

    const int bottom = g_extent.max_y; /* lowest painted row + 1 */
    printf("post-reflow paint: ops=%d extent=%d,%d..%d,%d (bottom=%d, badge ~y120→140)\n",
           g_extent.operation_count,
           g_extent.min_x,
           g_extent.min_y,
           g_extent.max_x,
           g_extent.max_y,
           bottom);

    er_node_destroy(root);

    if (g_extent.operation_count == 0)
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
 * the same pathology scenarios 5 and 6 below guard for scale, reached without any transform scratch
 * being involved at all.
 *
 * The observable is where the pixels land: recolouring the spinner repaints its footprint and nothing
 * else, so a repaint that reaches back to the raw box means it was drawn there.
 *
 * @param[in] screen  Root size.
 */
static int check_activity_indicator_translate(int screen)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)screen;
    root_props.height = (int16_t)screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    /* Laid out top-left, drawn well clear of it: the offset is larger than the node, so the raw box and
     * the translated one do not touch and a repaint of either is unambiguous. */
    const int box = 40, offset = 60;
    ERNode* spinner = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    ERProps spinner_props = props_default();
    spinner_props.position = ER_POSITION_ABSOLUTE;
    spinner_props.left = 20;
    spinner_props.top = 20;
    spinner_props.width = (int16_t)box;
    spinner_props.height = (int16_t)box;
    spinner_props.color = 0xFF3366FFU;
    spinner_props.transform_translate_x = (float)offset;
    spinner_props.transform_translate_y = (float)offset;
    er_node_set_props(spinner, &spinner_props);

    er_tree_append_child(root, spinner);
    er_tree_set_root(root);

    er_commit(); /* full first frame */
    er_commit(); /* settle: the spinner has a last-painted footprint now */

    /* Recolour it — the one and only change, so the repaint is its footprint alone. */
    spinner_props.color = 0xFFFF00FFU;
    er_node_set_props(spinner, &spinner_props);
    ext_reset();
    er_commit();
    const int recolour_min_x = g_extent.min_x, recolour_min_y = g_extent.min_y,
              recolour_operations = g_extent.operation_count;

    /* Nothing changes here: a footprint the pre-pass and the paint agree on reports nothing new, so the
     * last painting commit's rects are what stays readable. */
    er_commit();
    ERRect reported = {0, 0, 0, 0};
    er_get_dirty_rect(&reported);

    printf("activity indicator translate: recolour ops=%d from %d,%d (laid out 20,20, drawn %d,%d), "
           "after idle reported %d,%d %dx%d\n",
           recolour_operations,
           recolour_min_x,
           recolour_min_y,
           20 + offset,
           20 + offset,
           reported.x,
           reported.y,
           reported.width,
           reported.height);

    er_node_destroy(root);

    /* The raw box ends at 20 + box; the drawn one starts at 20 + off. Anything at or past this cleared
     * the raw box entirely, with slack for the padding add_damage applies before inserting a rect. */
    const int clear = 20 + offset - 10;
    if (recolour_operations == 0)
        return fail("recolouring the activity indicator repainted nothing");
    if (recolour_min_x < clear || recolour_min_y < clear)
        return fail("the activity indicator was painted at its raw box — its translate was dropped");
    if (reported.x < clear || reported.y < clear)
        return fail("an idle commit re-reported the activity indicator's raw box: paint and pre-pass "
                    "disagree about where it is");

    printf("PASS: a translated activity indicator is painted at its offset\n");
    return EXIT_SUCCESS;
}

/* Scenario 4: a SPINNING ActivityIndicator must contribute only its own box to the damage set, never a
 * full-screen repaint. This is the cost half of scenario 3's correctness half, and the last member of
 * the family that bounding a scale/rotate (scenario 1) and a 3D node closed.
 *
 * er_node_create() starts a looping ER_PROP_ROTATE_Z animation the moment an indicator exists, so the
 * node is source_dirty on every commit — and tp_rotate_z is non-zero on all but the frame it wraps.
 * That angle is an internal spin that render_activity_indicator() bakes into the ring of dots; it is
 * never an affine render, and render_tree keeps the node off the capture path for exactly that reason.
 * But er_transform_is_translate_only() reads the same field, so as soon as the indicator carries ANY
 * real transform prop — has_transform true, a translate is enough — the spin made it look like a
 * rotate: node_screen_rect() refused it, node_transform_damage() excluded it by type, and with no
 * capturing ancestor to escalate to, the pre-pass gave up and repainted the whole screen once per spin
 * frame. Measured on an 800x480 panel that was commit=48 ms / present=52 ms, ~10 fps — slow enough that
 * the dots aliased against their own 45 degree spacing and appeared to rotate backwards.
 *
 * Nothing about the pixels was wrong, so the observable is the cost: the extent of every paint op in a
 * mid-spin commit. Run against both a translate and a real scale — a scaled indicator is still refused
 * the capture path by type, so it too paints untransformed at box+translate and must be bounded by the
 * same box, not by a scaled AABB it never draws.
 *
 * @param[in] screen   Root size.
 * @param[in] scale_x  Horizontal scale to put on the indicator (0 = none: the translate-only case).
 * @param[in] scale_y  Vertical scale to put on the indicator (0 = none).
 * @param[in] label    Printed name for the case.
 */
static int check_spinner_bounded(int screen, float scale_x, float scale_y, const char* label)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)screen;
    root_props.height = (int16_t)screen;
    root_props.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really fills the whole screen */
    er_node_set_props(root, &root_props);

    const int box = 40;
    ERNode* spinner = er_node_create(ER_NODE_ACTIVITY_INDICATOR);
    ERProps spinner_props = props_default();
    spinner_props.position = ER_POSITION_ABSOLUTE;
    spinner_props.left = 20;
    spinner_props.top = 20;
    spinner_props.width = (int16_t)box;
    spinner_props.height = (int16_t)box;
    spinner_props.indicator_color = 0xFF3366FFU;
    /* props_default() zeroes `animating`, and er_node_set_props() reads it as "stop": a spinner left at
     * the default is CANCELLED here, tp_rotate_z pinned at 0, and every assertion below passes without
     * the spin path ever being entered. */
    spinner_props.animating = 1U;
    /* A real transform prop, so has_transform is true — the state the spin angle used to turn into a
     * whole-screen repaint. Without one the node has no transform at all (the animation deliberately
     * skips update_has_transform for this type) and the pre-pass bounds it either way. */
    spinner_props.transform_translate_x = 30.0f;
    spinner_props.transform_translate_y = 30.0f;
    spinner_props.transform_scale_x = scale_x;
    spinner_props.transform_scale_y = scale_y;
    er_node_set_props(spinner, &spinner_props);

    er_tree_append_child(root, spinner);
    er_tree_set_root(root);

    er_commit(); /* full first frame */
    er_commit(); /* settle: the spinner has a last-painted footprint now */

    embedded_renderer_tick(100U); /* advance the spin ~36 degrees → source_dirty, tp_rotate_z non-zero */

    ext_reset(); /* measure ONLY the mid-spin commit */
    er_commit();

    const int painted_width = g_extent.max_x - g_extent.min_x;
    const int painted_height = g_extent.max_y - g_extent.min_y;
    const long screen_area = (long)screen * screen;
    const long paint_area = (g_extent.operation_count > 0) ? (long)painted_width * painted_height : 0;
    /* Asserted, not assumed: this is the whole point of the case. If it reads translate-only the node
     * took node_screen_rect()'s ordinary fast path for a reason that has nothing to do with the fix. */
    const bool spinning = !er_transform_is_translate_only(spinner) && spinner->has_transform;

    printf("%s: mid-spin paint ops=%d extent=%d,%d %dx%d (%.1f%% of screen), rotate_z=%.1f\n",
           label,
           g_extent.operation_count,
           g_extent.min_x,
           g_extent.min_y,
           painted_width,
           painted_height,
           100.0 * (double)paint_area / (double)screen_area,
           (double)spinner->transform_rotate_z);

    /* Torn down deliberately, and in this order. er_node_destroy() frees ONE node — the caller unlinks
     * and destroys the subtree (see er_scene.h) — and the damage pre-pass sweeps every tag in the pool,
     * not just the ones reachable from the root. So an indicator left orphaned but in_use goes on being
     * source_dirty and contributing its box to every LATER scenario's damage, which is exactly the cost
     * this case exists to measure. Stopping it first also retires its looping ER_PROP_ROTATE_Z: a
     * destroyed node's node-target animations outlive it, and would drive whatever node next recycles
     * its tag. */
    spinner_props.animating = 0U;
    er_node_set_props(spinner, &spinner_props);
    er_tree_remove_child(root, spinner);
    er_node_destroy(spinner);
    er_node_destroy(root);

#if ERUI_TRANSFORMS_FULL
    if (!spinning)
        return fail("the indicator is not carrying a live spin angle over a real transform — the case "
                    "that forced the full repaint was never reached");
#else
    (void)spinning; /* TRANSLATE_ONLY compiles the refusing gate out entirely */
#endif
    if (g_extent.operation_count == 0)
        return fail("a spinning activity indicator produced no repaint at all");
    if (painted_width >= screen && painted_height >= screen)
        return fail("a spinning activity indicator forced a full-screen repaint (its damage is not bounded)");
    if (paint_area > screen_area / 4)
        return fail("a spinning activity indicator's repaint region is far larger than its own box");

    printf("PASS: %s damages only its own box\n", label);
    return EXIT_SUCCESS;
}

#if ERUI_TRANSFORMS_FULL
/* Scenario 5: a transformed node too large for the transform scratch must not become a permanent damage
 * source. er_transform_source_begin() refuses a node wider/taller than ERUI_TRANSFORM_WIDTH and ERUI_TRANSFORM_HEIGHT,
 * and render_tree then degrades to painting it UNTRANSFORMED at its raw box — but the damage pre-pass used to measure
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
 *                            ERUI_TRANSFORM_WIDTH and ERUI_TRANSFORM_HEIGHT.
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

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    /* Scaled-down box in the top-left. Nothing ever changes about it after the first commit. */
    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps node_props = props_default();
    node_props.position = ER_POSITION_ABSOLUTE;
    node_props.left = 10;
    node_props.top = 10;
    node_props.width = (int16_t)side;
    node_props.height = (int16_t)side;
    node_props.background_color = 0xFF33AA55U;
    node_props.transform_scale_x = 0.3f;
    node_props.transform_scale_y = 0.3f;
    er_node_set_props(node, &node_props);

    /* The only thing that changes, parked clear of the node's raw box AND its scaled AABB. */
    const int marker_x = side + 40;
    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps marker_props = props_default();
    marker_props.position = ER_POSITION_ABSOLUTE;
    marker_props.left = (int16_t)marker_x;
    marker_props.top = (int16_t)marker_x;
    marker_props.width = 40;
    marker_props.height = 40;
    marker_props.background_color = 0xFF00AAFFU;
    er_node_set_props(marker, &marker_props);

    er_tree_append_child(root, node);
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    er_commit(); /* full first frame */
    er_commit(); /* settle: everything has a last-painted footprint now */

    /* Recolour the marker — the one and only change. */
    marker_props.background_color = 0xFFFF00FFU;
    er_node_set_props(marker, &marker_props);
    ext_reset();
    er_commit();
    const int recolour_min_x = g_extent.min_x, recolour_min_y = g_extent.min_y,
              recolour_operations = g_extent.operation_count;
    ERRect reported = {0, 0, 0, 0};
    er_get_dirty_rect(&reported);

    /* Nothing at all changes here. */
    ext_reset();
    er_commit();
    const int idle_operations = g_extent.operation_count;

    printf("%s: marker commit ops=%d from %d,%d (marker at %d,%d), reported %d,%d %dx%d; idle ops=%d\n",
           label,
           recolour_operations,
           recolour_min_x,
           recolour_min_y,
           marker_x,
           marker_x,
           reported.x,
           reported.y,
           reported.width,
           reported.height,
           idle_operations);

    er_node_destroy(root);

    /* The node's raw box ends at side + 10 = marker_x - 30 and its scaled AABB is well inside that, so a
     * repaint (or a report) that starts before this stayed clear of both. The slack absorbs the few
     * pixels add_damage pads a rect by before inserting it. */
    const int clear = marker_x - 20;
    if (recolour_operations == 0)
        return fail("the marker change repainted nothing");
    if (recolour_min_x < clear || recolour_min_y < clear)
        return fail("the marker change dragged the transformed node's region into the repaint");
    if (reported.x < clear || reported.y < clear)
        return fail("the marker change reported the transformed node's region as dirty");
    if (idle_operations != 0)
        return fail("an idle commit repainted — the transformed node damages its footprint forever");

    printf("PASS: %s contributes no damage when nothing about it changed\n", label);
    return EXIT_SUCCESS;
}

/* Scenario 6: the other half of the same defect, reached where geometry cannot predict it. Only one
 * transform capture can be active at a time, so a transformed node INSIDE another transformed node is
 * refused the scratch and painted at its raw box however small it is. Nothing about its size says so —
 * the fallback is only knowable from what the last paint actually did — and without that, the pre-pass
 * measured it by its AABB and it re-damaged the outer node's whole region on every commit.
 *
 * @param[in] screen  Root size.
 */
static int check_nested_transform_no_damage(int screen)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    /* Outer transform: comfortably within the scratch, so ITS capture succeeds and stays active for the
     * whole of the subtree below it. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps outer_props = props_default();
    outer_props.position = ER_POSITION_ABSOLUTE;
    outer_props.left = 10;
    outer_props.top = 10;
    outer_props.width = 100;
    outer_props.height = 100;
    outer_props.background_color = 0xFF33AA55U;
    outer_props.transform_rotate_z = 20.0f;
    er_node_set_props(outer, &outer_props);

    /* Inner transform: tiny, but refused the scratch because the outer capture holds it. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps inner_props = props_default();
    inner_props.position = ER_POSITION_ABSOLUTE;
    inner_props.left = 10;
    inner_props.top = 10;
    inner_props.width = 40;
    inner_props.height = 40;
    inner_props.background_color = 0xFFAA3355U;
    inner_props.transform_scale_x = 0.5f;
    inner_props.transform_scale_y = 0.5f;
    er_node_set_props(inner, &inner_props);

    const int marker_x = screen - 50;
    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps marker_props = props_default();
    marker_props.position = ER_POSITION_ABSOLUTE;
    marker_props.left = (int16_t)marker_x;
    marker_props.top = (int16_t)marker_x;
    marker_props.width = 30;
    marker_props.height = 30;
    marker_props.background_color = 0xFF00AAFFU;
    er_node_set_props(marker, &marker_props);

    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    er_commit();
    er_commit();

    marker_props.background_color = 0xFFFF00FFU;
    er_node_set_props(marker, &marker_props);
    ext_reset();
    er_commit();
    ERRect reported = {0, 0, 0, 0};
    er_get_dirty_rect(&reported);

    printf("nested transform: marker commit ops=%d from %d,%d (marker at %d,%d), reported %d,%d %dx%d\n",
           g_extent.operation_count,
           g_extent.min_x,
           g_extent.min_y,
           marker_x,
           marker_x,
           reported.x,
           reported.y,
           reported.width,
           reported.height);

    er_node_destroy(root);

    const int clear = marker_x - 20; /* the outer node ends at 110, far above this */
    if (g_extent.operation_count == 0)
        return fail("the marker change repainted nothing");
    if (g_extent.min_x < clear || g_extent.min_y < clear)
        return fail("the marker change dragged the nested transform's region into the repaint");
    if (reported.x < clear || reported.y < clear)
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
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = screen;
    root_props.height = screen;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps card_props = props_default();
    card_props.width = 40;
    card_props.height = 40;
    card_props.margin_left = 80;
    card_props.margin_top = 80;
    card_props.background_color = 0xFF22CC88U;
    card_props.transform_rotate_y = 1.0f; /* non-zero → exercises the 3D/perspective path from the first commit */
    er_node_set_props(card, &card_props);

    er_tree_append_child(root, card);
    er_tree_set_root(root);
    er_commit(); /* full first frame */

    ERAnimationValueHandle spin = er_animation_value_create(1.0f);
    er_animation_value_bind(spin, card, ER_PROP_ROTATE_Y);

    ERAnimationConfig config = {0};
    config.type = ER_ANIMATION_TIMING;
    config.duration_ms = 800U;
    er_animation_value_animate(spin, 40.0f, &config);

    embedded_renderer_tick(100U); /* rotateY advances → card source-dirty, still 3D */

    ext_reset();
    er_commit();

    const int painted_width = g_extent.max_x - g_extent.min_x;
    const int painted_height = g_extent.max_y - g_extent.min_y;
    const long screen_area = (long)screen * screen;
    const long paint_area = (g_extent.operation_count > 0) ? (long)painted_width * painted_height : 0;

    printf("mid-spin (3D) paint: ops=%d extent=%d,%d %dx%d (%.1f%% of screen)\n",
           g_extent.operation_count,
           g_extent.min_x,
           g_extent.min_y,
           painted_width,
           painted_height,
           100.0 * (double)paint_area / (double)screen_area);

    er_animation_value_destroy(spin);
    er_node_destroy(root);

    if (g_extent.operation_count == 0)
        return fail("animated 3D transform produced no repaint at all");
    if (painted_width >= screen && painted_height >= screen)
        return fail("animated 3D transform forced a full-screen repaint (3D damage not bounded)");
    if (paint_area > screen_area / 4)
        return fail("animated 3D transform repaint region far larger than the node's box");

    printf("PASS: animated 3D transform damages only its projected box\n");
    return EXIT_SUCCESS;
}
#endif /* ERUI_3D_TRANSFORMS */

int main(void)
{
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    const int screen = 200;

    int result = check_pulse_bounded(screen);
    if (result != EXIT_SUCCESS)
        return result;

    result = check_reflow_moved_no_trail(screen);
    if (result != EXIT_SUCCESS)
        return result;

    result = check_activity_indicator_translate(screen);
    if (result != EXIT_SUCCESS)
        return result;

    result = check_spinner_bounded(screen, 0.0f, 0.0f, "spinning indicator (translate)");
    if (result != EXIT_SUCCESS)
        return result;

    /* A scaled indicator is refused the capture path by type just like the plain one, so it paints —
     * and must be damaged — at the same raw box, not at a scaled AABB it never draws. */
    result = check_spinner_bounded(screen, 1.8f, 1.8f, "spinning indicator (scale)");
    if (result != EXIT_SUCCESS)
        return result;

#if ERUI_TRANSFORMS_FULL
    const int fits = (ERUI_TRANSFORM_WIDTH < ERUI_TRANSFORM_HEIGHT) ? ERUI_TRANSFORM_WIDTH : ERUI_TRANSFORM_HEIGHT;
    const int limit = (ERUI_TRANSFORM_WIDTH > ERUI_TRANSFORM_HEIGHT) ? ERUI_TRANSFORM_WIDTH : ERUI_TRANSFORM_HEIGHT;
    const int over = limit + 1;
    const int big_screen = over + 120;

    result = check_oversized_no_idle_damage(fits, big_screen, true, "node within the scratch limit");
    if (result != EXIT_SUCCESS)
        return result;

    result = check_oversized_no_idle_damage(over, big_screen, false, "node over the scratch limit");
    if (result != EXIT_SUCCESS)
        return result;

    result = check_nested_transform_no_damage(300);
    if (result != EXIT_SUCCESS)
        return result;
#endif

#if ERUI_3D_TRANSFORMS && ERUI_TRANSFORMS_FULL
    result = check_3d_rotate_bounded(screen);
    if (result != EXIT_SUCCESS)
        return result;
#endif

    return EXIT_SUCCESS;
}
