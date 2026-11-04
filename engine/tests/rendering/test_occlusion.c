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
 * Occlusion culling: a fully opaque node that covers the region being repainted buries everything
 * painted before it, so those layers are skipped entirely instead of being drawn and immediately
 * overwritten. The backend here logs every fill it is handed, which makes "was this layer drawn?"
 * directly observable — a buried layer's colour must simply never reach the backend.
 *
 *   - culled: an opaque full-cover child skips its parent's background and its earlier siblings,
 *   - not culled when the cover cannot prove coverage: translucent, rounded corners (the corner
 *     squares show through), or smaller than the region,
 *   - partial: only the siblings BELOW the cover are skipped, the ones above still paint,
 *   - flags retire: a buried node that changed does not re-damage the same pixels forever, and a
 *     TRANSFORMED sibling is never buried at all (it has no box-shaped footprint to record),
 *   - a translated node buried inside a clipping container still records its translated box, and a
 *     rotated one keeps the container from being buried at all,
 *   - pixel equivalence: the culled frame is byte-identical to one rendered with everything drawn.
 *
 * The assertions that a layer WAS skipped are the only ones gated on ERUI_OCCLUSION_CULLING; every
 * "must still paint" and "must look identical" check has to hold with the cull compiled out too.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define OPACITY_OPAQUE 255U

/* Distinct, easily-recognised layer colours (opaque unless a test overrides the alpha). */
#define COLOR_ROOT 0xFFFFFFFFU  /* root background      */
#define COLOR_DEEP 0xFF0000FFU  /* buried mid layer     */
#define COLOR_INNER 0xFFFF0000U /* buried leaf inside it */
#define COLOR_COVER 0xFF00FF00U /* the opaque occluder  */
#define COLOR_TOP 0xFF00FFFFU   /* sibling ABOVE it     */

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer + a log of every fill colour handed to it
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN * SCREEN];
static uint32_t s_fills[4096];
static int s_fill_count;
/* Every backend op, not just fills: a transformed node reaches the panel through copy/blend, so a
 * fill-only counter would score "painted nothing" for a subtree that painted plenty. */
static int s_operation_count;

static void log_reset(void)
{
    s_fill_count = 0;
    s_operation_count = 0;
}

/** @brief Whether any fill this frame used the given colour (alpha included). */
static bool drew(uint32_t argb)
{
    for (int fill_index = 0; fill_index < s_fill_count; fill_index++)
        if (s_fills[fill_index] == argb)
            return true;
    return false;
}

static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

static void fb_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U || width <= 0 || height <= 0)
        return;
    s_operation_count++;
    if (s_fill_count < (int)(sizeof(s_fills) / sizeof(s_fills[0])))
        s_fills[s_fill_count++] = argb;
    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= SCREEN)
                continue;
            uint32_t* destination = &s_framebuffer[row * SCREEN + column];
            if (alpha == 0xFFU)
            {
                *destination = 0xFF000000U | (argb & 0x00FFFFFFU);
            }
            else
            {
                const uint32_t inverse_alpha = ER_CHANNEL_MAX - alpha;
                const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                               destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
                const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU,
                               source_blue = argb & 0xFFU;
                *destination = 0xFF000000U
                               | ((div255(source_red * alpha) + div255(destination_red * inverse_alpha)) << 16)
                               | ((div255(source_green * alpha) + div255(destination_green * inverse_alpha)) << 8)
                               | (div255(source_blue * alpha) + div255(destination_blue * inverse_alpha));
            }
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Source-over composites premultiplied pixels, scaled by a global alpha, into the framebuffer.
 *
 * Real compositing rather than a counter, so a transformed node — which reaches the panel only through
 * copy/blend — lands in s_framebuffer and a pixel comparison can see where it was left.
 */
static void fb_composite(const void* source, int stride, uint32_t alpha, int x, int y, int width, int height)
{
    s_operation_count++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= SCREEN || destination_y < 0 || destination_y >= SCREEN)
                continue;
            const uint32_t pixel = source_row[column];
            const uint32_t source_alpha = div255(((pixel >> 24) & 0xFFU) * alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            uint32_t* destination = &s_framebuffer[destination_y * SCREEN + destination_x];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U
                           | ((div255(((pixel >> 16) & 0xFFU) * alpha) + div255(destination_red * inverse_alpha)) << 16)
                           | ((div255(((pixel >> 8) & 0xFFU) * alpha) + div255(destination_green * inverse_alpha)) << 8)
                           | (div255((pixel & 0xFFU) * alpha) + div255(destination_blue * inverse_alpha));
        }
    }
}

static void fb_copy(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    fb_composite(source, stride, 255U, x, y, width, height);
}

static void fb_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    fb_composite(source, stride, alpha, x, y, width, height);
}

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

/** @brief Absolutely positioned box, so every test scene has exact, layout-independent geometry. */
static ERProps box(int x, int y, int width, int height, uint32_t argb)
{
    ERProps props = props_default();
    props.position = ER_POS_ABSOLUTE;
    props.left = (int16_t)x;
    props.top = (int16_t)y;
    props.width = (int16_t)width;
    props.height = (int16_t)height;
    props.background_color = argb;
    return props;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/**
 * @brief Builds root → [deep → inner, cover, top].
 *
 * `deep` and `inner` sit under `cover`; `top` is painted after it. The caller supplies cover's
 * type and props so each test can vary exactly one thing about the would-be occluder.
 */
static void build_as(ERNodeType cover_type, const ERProps* cover_props, ERNode** out_inner, ERNode** out_cover)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = box(0, 0, SCREEN, SCREEN, COLOR_ROOT);
    root_props.position = ER_POS_RELATIVE;
    er_node_set_props(root, &root_props);

    ERNode* deep = er_node_create(ER_NODE_VIEW);
    ERProps deep_props = box(0, 0, SCREEN, SCREEN, COLOR_DEEP);
    er_node_set_props(deep, &deep_props);

    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps inner_props = box(20, 20, 100, 100, COLOR_INNER);
    er_node_set_props(inner, &inner_props);
    er_tree_append_child(deep, inner);

    ERNode* cover = er_node_create(cover_type);
    er_node_set_props(cover, cover_props);

    ERNode* top = er_node_create(ER_NODE_VIEW);
    ERProps top_props = box(150, 150, 40, 40, COLOR_TOP);
    er_node_set_props(top, &top_props);

    er_tree_append_child(root, deep);
    er_tree_append_child(root, cover);
    er_tree_append_child(root, top);
    er_tree_set_root(root);

    if (out_inner)
        *out_inner = inner;
    if (out_cover)
        *out_cover = cover;
}

/** @brief build_as() with a View as the would-be occluder. */
static void build(const ERProps* cover_props, ERNode** out_inner, ERNode** out_cover)
{
    build_as(ER_NODE_VIEW, cover_props, out_inner, out_cover);
}

/** @brief Renders one full frame with a clean fill log. */
static void full_frame(void)
{
    er_force_full_repaint();
    log_reset();
    er_commit();
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* The headline behaviour: an opaque full-cover child buries the root's background and its earlier
 * siblings, and none of them is drawn at all. The sibling painted AFTER it still is. */
static int check_opaque_cover_culls(void)
{
    ERProps cover_props = box(0, 0, SCREEN, SCREEN, COLOR_COVER);
    build(&cover_props, NULL, NULL);
    full_frame();

#if ERUI_OCCLUSION_CULLING
    if (drew(COLOR_ROOT))
        return fail("root background drew under a full opaque cover");
    if (drew(COLOR_DEEP) || drew(COLOR_INNER))
        return fail("buried subtree drew under a full opaque cover");
#endif
    if (!drew(COLOR_COVER))
        return fail("the occluder itself did not draw");
    if (!drew(COLOR_TOP))
        return fail("the sibling above the occluder was wrongly culled");

    /* And the result is what it should be: green everywhere except the top sibling's corner. */
    if ((s_framebuffer[100 * SCREEN + 100] & 0x00FFFFFFU) != (COLOR_COVER & 0x00FFFFFFU))
        return fail("centre pixel is not the occluder's colour");
    if ((s_framebuffer[170 * SCREEN + 170] & 0x00FFFFFFU) != (COLOR_TOP & 0x00FFFFFFU))
        return fail("top sibling did not land on screen");
    return EXIT_SUCCESS;
}

/* A translucent cover blends with what is beneath it, so nothing below may be skipped. */
static int check_translucent_cover_does_not_cull(void)
{
    ERProps cover_props = box(0, 0, SCREEN, SCREEN, 0x8000FF00U);
    build(&cover_props, NULL, NULL);
    full_frame();

    if (!drew(COLOR_DEEP) || !drew(COLOR_INNER))
        return fail("buried subtree skipped under a TRANSLUCENT cover");
    return EXIT_SUCCESS;
}

/* A shown Modal's backdrop fills the whole root, whatever the Modal's own box. When it is opaque it hides
 * everything painted before the Modal, so a sheet over a busy screen repaints only itself; the default
 * translucent dim hides nothing. */
static int check_modal_backdrop(void)
{
    for (int opaque = 1; opaque >= 0; opaque--)
    {
        ERProps modal_props = box(60, 60, 80, 80, COLOR_COVER); /* the sheet: far from full-screen */
        modal_props.modal_visible = 1;
        modal_props.backdrop_color = opaque ? 0xFF000000U : 0U; /* 0 is the default translucent dim */
        build_as(ER_NODE_MODAL, &modal_props, NULL, NULL);
        full_frame();

        if (!drew(COLOR_COVER) || !drew(COLOR_TOP))
            return fail("the modal's own box or the sibling above it did not draw");
        if (!opaque)
        {
            if (!drew(COLOR_DEEP) || !drew(COLOR_INNER))
                return fail("the screen under a TRANSLUCENT modal backdrop was skipped");
            continue;
        }
        if (!drew(0xFF000000U))
            return fail("the opaque modal backdrop did not draw");
#if ERUI_OCCLUSION_CULLING
        if (drew(COLOR_ROOT) || drew(COLOR_DEEP) || drew(COLOR_INNER))
            return fail("the screen under an opaque modal backdrop was drawn");
#endif
        if ((s_framebuffer[10 * SCREEN + 10] & 0x00FFFFFFU) != 0U)
            return fail("the backdrop does not cover the screen outside the modal's box");
        if ((s_framebuffer[100 * SCREEN + 100] & 0x00FFFFFFU) != (COLOR_COVER & 0x00FFFFFFU))
            return fail("the modal's own box did not land on screen");
    }
    return EXIT_SUCCESS;
}

/* Rounded corners leave the corner squares showing whatever is underneath. */
static int check_rounded_cover_does_not_cull(void)
{
    ERProps cover_props = box(0, 0, SCREEN, SCREEN, COLOR_COVER);
    cover_props.border_radius = 16;
    build(&cover_props, NULL, NULL);
    full_frame();

    if (!drew(COLOR_DEEP) || !drew(COLOR_INNER))
        return fail("buried subtree skipped under a ROUNDED cover");
    return EXIT_SUCCESS;
}

/* A cover smaller than the repaint region cannot bury it. */
static int check_partial_cover_does_not_cull(void)
{
    ERProps cover_props = box(0, 0, SCREEN, SCREEN - 1, COLOR_COVER);
    build(&cover_props, NULL, NULL);
    full_frame();

    if (!drew(COLOR_DEEP) || !drew(COLOR_INNER))
        return fail("buried subtree skipped under a cover one row short of the region");
    return EXIT_SUCCESS;
}

/*
 * A buried node that changes must still retire its dirty flags. If it did not, the damage pre-pass
 * would read it as changed (or moved) on every following commit and repaint the same buried pixels
 * for as long as it stayed hidden — a permanent, invisible cost.
 */
static int check_buried_change_retires(void)
{
    ERNode* inner = NULL;
    ERProps cover_props = box(0, 0, SCREEN, SCREEN, COLOR_COVER);
    build(&cover_props, &inner, NULL);
    full_frame();

    /* Recolour + move the buried leaf: source-dirty AND moved, the two damage sources. */
    ERProps inner_props = box(30, 35, 90, 80, 0xFF123456U);
    er_node_set_props(inner, &inner_props);
    log_reset();
    er_commit();
#if ERUI_OCCLUSION_CULLING
    if (drew(0xFF123456U))
        return fail("buried leaf drew its change");
#endif

    /* Nothing changed since: the commit must be a complete no-op, not a repeat of the same damage. */
    log_reset();
    er_commit();
    if (s_operation_count != 0)
        return fail("a buried change re-damaged on the following idle commit");
    return EXIT_SUCCESS;
}

#if ERUI_TRANSFORMS_FULL
/*
 * A TRANSFORMED sibling must never be buried. Burying it skips the scratch capture that gives it a
 * transformed AABB, so its last_paint_rect is recorded as the raw box; the damage pre-pass measures it
 * as an AABB, the two never agree, and it reads as "moved" on every commit from then on.
 *
 * Nothing looks wrong on screen — a move-only damage sets no dirty flag, so the region is damaged and
 * repainted by nobody. What leaks is the REPORT: er_get_dirty_rect() names that region again on every
 * idle commit, so a partial-update host re-transfers it forever. The assertion is therefore that an
 * idle commit leaves the reported rect exactly where the last real change put it.
 */
static int check_transformed_sibling_not_buried(void)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = box(0, 0, SCREEN, SCREEN, COLOR_ROOT);
    root_props.position = ER_POS_RELATIVE;
    er_node_set_props(root, &root_props);

    ERNode* spun = er_node_create(ER_NODE_VIEW);
    ERProps spun_props = box(40, 40, 80, 80, COLOR_DEEP);
    spun_props.transform_rotate_z = 30.0f; /* not translate-only: measured by its transformed AABB */
    er_node_set_props(spun, &spun_props);

    ERNode* cover = er_node_create(ER_NODE_VIEW);
    ERProps cover_props = box(0, 0, SCREEN, SCREEN, COLOR_COVER);
    er_node_set_props(cover, &cover_props);

    /* A small opaque marker on top, far from the rotated node, to give the reported rect a known
     * value that the rotated node's AABB (0,40 110x110) could never be mistaken for. */
    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps marker_props = box(160, 160, 20, 20, COLOR_TOP);
    er_node_set_props(marker, &marker_props);

    er_tree_append_child(root, spun);
    er_tree_append_child(root, cover); /* painted after the rotated node: would bury it */
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    full_frame();
    er_commit();
    er_commit(); /* settle */

    /* One real change, so the reported rect is the marker and nothing else. */
    ERProps recoloured_marker_props = box(160, 160, 20, 20, 0xFF00AAFFU);
    er_node_set_props(marker, &recoloured_marker_props);
    er_commit();

    ERRect after_change;
    if (!er_get_dirty_rect(&after_change))
        return fail("recolouring the marker reported no damage");
    if (after_change.x < 100)
        return fail("the marker's own change reported a region far from the marker");

    /* Nothing has changed since. The report must not move. */
    for (int commit_index = 0; commit_index < 3; commit_index++)
    {
        er_commit();
        ERRect idle;
        if (!er_get_dirty_rect(&idle))
            return fail("the dirty rect vanished across an idle commit");
        if (idle.x != after_change.x || idle.y != after_change.y || idle.w != after_change.w
            || idle.h != after_change.h)
            return fail("a buried TRANSFORMED sibling re-damages its footprint on every idle commit");
    }
    return EXIT_SUCCESS;
}
#endif

/**
 * @brief Slides a pill along a clipping bar under an opaque panel, then compares the frame with a full repaint.
 *
 * The scene from issues #312 and #369: a bar that clips, a pill sliding in from the right, and an opaque panel
 * at the bar's right end, painted after it, whose child changes between the moves. Every other commit repaints
 * only that child, so the panel covers the whole damage region and the cull can bury the bar.
 *
 * @param[in] rotate_z  The pill's rotation in degrees; 0 leaves it translate-only.
 * @param[in] animated  Drive every change through animated values instead of props, so no commit after the
 *                      first runs a layout pass: the pill is laid out translate-only and the rotation, the
 *                      slide and the blink (an opacity toggle) all arrive on the native driver.
 *
 * @return true when the settled frame matches a full repaint of the same scene.
 */
static bool slide_under_cover_matches_full_repaint(float rotate_z, bool animated)
{
    static uint32_t reference[SCREEN * SCREEN];
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = box(0, 0, SCREEN, SCREEN, COLOR_ROOT);
    root_props.position = ER_POS_RELATIVE;
    er_node_set_props(root, &root_props);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bar_props = box(0, 0, SCREEN, 40, 0U);
    bar_props.overflow = ER_OVERFLOW_HIDDEN;
    er_node_set_props(bar, &bar_props);

    ERNode* pill = er_node_create(ER_NODE_VIEW);
    ERProps pill_props = box(0, 10, 40, 20, COLOR_DEEP);
    pill_props.transform_translate_x = 140.0f;
    pill_props.transform_rotate_z = animated ? 0.0f : rotate_z;
    er_node_set_props(pill, &pill_props);
    er_tree_append_child(bar, pill);

    ERNode* panel = er_node_create(ER_NODE_VIEW);
    ERProps panel_props = box(150, 0, 50, 40, COLOR_COVER);
    er_node_set_props(panel, &panel_props);

    ERNode* blink = er_node_create(ER_NODE_VIEW);
    ERProps blink_props = box(20, 15, 10, 10, COLOR_TOP);
    er_node_set_props(blink, &blink_props);
    er_tree_append_child(panel, blink);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, panel); /* painted after the bar: covers the damage whenever only `blink` repaints */
    er_tree_set_root(root);

    full_frame();

    ERAnimValueHandle slide = er_anim_value_create(140.0f);
    ERAnimValueHandle spin = er_anim_value_create(0.0f);
    ERAnimValueHandle fade = er_anim_value_create(1.0f);
    if (animated)
    {
        er_anim_value_bind(slide, pill, ER_PROP_TRANSLATE_X);
        er_anim_value_bind(spin, pill, ER_PROP_ROTATE_Z);
        er_anim_value_bind(fade, blink, ER_PROP_OPACITY);
        er_anim_value_set(spin, rotate_z);
        er_commit();
    }

    bool fade_on = false;
    for (int translate_x = 140; translate_x >= 0; translate_x -= 20)
    {
        fade_on = !fade_on;
        if (animated)
            er_anim_value_set(fade, fade_on ? 0.5f : 1.0f);
        else
        {
            blink_props.background_color = fade_on ? 0xFFE0E040U : COLOR_TOP;
            er_node_set_props(blink, &blink_props);
        }
        er_commit();

        if (animated)
            er_anim_value_set(slide, (float)translate_x);
        else
        {
            pill_props.transform_translate_x = (float)translate_x;
            er_node_set_props(pill, &pill_props);
        }
        er_commit();
    }

    memcpy(reference, s_framebuffer, sizeof(reference));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    full_frame();
    return memcmp(reference, s_framebuffer, sizeof(reference)) == 0;
}

/*
 * A translated node under an opaque cover must record where its pixels are (issue #312). A clipping container
 * leaves its children's transforms out of subtree_prunable, so the cull can bury a translated child of one;
 * render_tree walks it occluded and must still apply the translate offset, or it records its untranslated box
 * and the next move erases that box instead of the pixels it painted, leaving a trail.
 */
static int check_translated_node_under_cover_leaves_no_trail(void)
{
    if (!slide_under_cover_matches_full_repaint(0.0f, false))
        return fail("a translated node walked under an opaque cover left a trail when it next moved");
    return EXIT_SUCCESS;
}

#if ERUI_TRANSFORMS_FULL
/*
 * The same slide with the pill rotated (issue #369). An occluded walk skips the capture, so it cannot record the
 * transformed footprint the node's pixels occupy, and a record at the raw box would leave the rotated corners on
 * screen after the next move. The cull must not bury the clipping bar at all while it holds such a node, even
 * though the bar's subtree_prunable leaves the clipped child's transform out.
 *
 * Run twice: with the rotation set as a prop, and with it arriving on the native driver after the last layout
 * pass, where nothing but the animation itself can tell the cached flag that the bar now holds one.
 */
static int check_rotated_node_in_clipper_not_buried(void)
{
    if (!slide_under_cover_matches_full_repaint(30.0f, false))
        return fail("a rotated node inside a clipping container left a trail after being buried by an opaque cover");
    if (!slide_under_cover_matches_full_repaint(30.0f, true))
        return fail("a node rotated by an animation inside a clipping container left a trail after being buried");
    return EXIT_SUCCESS;
}

/*
 * A commit whose only change lands off-screen paints nothing, but still walks the tree, marked occluded, to retire
 * the changed node's flags. The dirty chain reaches the root, so every node on that walk refreshes its paint
 * record. A rotated node the walk does not capture must still record the transformed footprint its pixels
 * occupy, or its next move erases the raw box instead and leaves the rotated corners on screen.
 *
 * Two nodes whose real paint falls back to the raw box ride along, so the walk has to predict the capture's
 * admission rather than assume it: a rotated child of the rotated node (a capture is already running) and a
 * rotated strip too wide for the transform source. A wrong guess for the strip over-erases rather than
 * trails, so it is caught by the reported dirty rect instead: a record the damage pre-pass does not predict
 * re-damages on every commit.
 *
 * @param[in] three_d  Tilt the three nodes with rotateX/rotateY and perspective instead of rotating them in
 *                     the plane, so the walk takes the homography path instead of the affine one.
 *
 * @return NULL when every check holds, otherwise what failed.
 */
static const char* offscreen_change_keeps_transformed_footprint(bool three_d)
{
    static uint32_t reference[SCREEN * SCREEN];
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = box(0, 0, SCREEN, SCREEN, COLOR_ROOT);
    root_props.position = ER_POS_RELATIVE;
    er_node_set_props(root, &root_props);

    ERNode* spun = er_node_create(ER_NODE_VIEW);
    ERProps spun_props = box(40, 40, 60, 30, COLOR_DEEP);
    er_node_set_props(spun, &spun_props);

    ERNode* nested = er_node_create(ER_NODE_VIEW);
    ERProps nested_props = box(10, 5, 20, 10, COLOR_INNER);
    er_node_set_props(nested, &nested_props);
    er_tree_append_child(spun, nested);

    ERNode* wide = er_node_create(ER_NODE_VIEW);
    ERProps wide_props = box(-30, 150, ERUI_XFORM_W + 10, 12, COLOR_COVER);
    er_node_set_props(wide, &wide_props);

#if ERUI_3D_TRANSFORMS
    if (three_d)
    {
        spun_props.transform_rotate_y = 50.0f;
        spun_props.transform_perspective = 150.0f;
        nested_props.transform_rotate_x = 40.0f;
        nested_props.transform_perspective = 100.0f;
        wide_props.transform_rotate_x = 30.0f;
        wide_props.transform_perspective = 200.0f;
    }
    else
#endif
    {
        (void)three_d;
        spun_props.transform_rotate_z = 30.0f;
        nested_props.transform_rotate_z = 20.0f;
        wide_props.transform_rotate_z = 3.0f;
    }
    er_node_set_props(spun, &spun_props);
    er_node_set_props(nested, &nested_props);
    er_node_set_props(wide, &wide_props);

    /* Entirely left of the screen: its damage clamps to nothing. */
    ERNode* offscreen = er_node_create(ER_NODE_VIEW);
    ERProps offscreen_props = box(-100, 0, 20, 20, COLOR_TOP);
    er_node_set_props(offscreen, &offscreen_props);

    ERNode* marker = er_node_create(ER_NODE_VIEW);
    ERProps marker_props = box(180, 180, 10, 10, COLOR_TOP);
    er_node_set_props(marker, &marker_props);

    er_tree_append_child(root, spun);
    er_tree_append_child(root, wide);
    er_tree_append_child(root, offscreen);
    er_tree_append_child(root, marker);
    er_tree_set_root(root);

    full_frame();

    offscreen_props.background_color = 0xFF123456U;
    er_node_set_props(offscreen, &offscreen_props);
    er_commit();

    /* Each record must also be the one the damage pre-pass predicts, or the node reads as moved on every
     * commit from here on. Nothing repaints a clean node, so the pixels stay right and the check below
     * would pass; what leaks is the reported dirty rect. Recolour a marker far from every transformed
     * node: the report must be the marker alone. */
    marker_props.background_color = 0xFF00AAFFU;
    er_node_set_props(marker, &marker_props);
    er_commit();
    ERRect after;
    if (!er_get_dirty_rect(&after))
        return "recolouring the marker reported no damage";
    if (after.x < 160 || after.y < 160)
        return "after an off-screen-only commit, a transformed node re-damaged a footprint it did not paint";

    /* That commit painted, so every record is a real one again. Go back off-screen, then move. */
    offscreen_props.background_color = COLOR_TOP;
    er_node_set_props(offscreen, &offscreen_props);
    er_commit();

    /* Far enough that the new footprint cannot cover what the old one left: the erase of the old record
     * alone decides whether the corners survive. */
    spun_props.transform_translate_x = 100.0f;
    spun_props.transform_translate_y = 90.0f;
    er_node_set_props(spun, &spun_props);
    wide_props.transform_translate_y = -20.0f;
    er_node_set_props(wide, &wide_props);
    er_commit();

    memcpy(reference, s_framebuffer, sizeof(reference));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    full_frame();
    if (memcmp(reference, s_framebuffer, sizeof(reference)) != 0)
        return "a transformed node left a trail after a commit whose only change was off-screen";
    return NULL;
}

static int check_rotated_node_keeps_footprint_across_offscreen_change(void)
{
    const char* error = offscreen_change_keeps_transformed_footprint(false);
    return error ? fail(error) : EXIT_SUCCESS;
}

#if ERUI_3D_TRANSFORMS
/* The same with perspective tilts: the homography path predicts admission and records the projected AABB. */
static int check_3d_node_keeps_footprint_across_offscreen_change(void)
{
    const char* error = offscreen_change_keeps_transformed_footprint(true);
    return error ? fail(error) : EXIT_SUCCESS;
}
#endif
#endif

/*
 * The catch-all. Drive an incremental, damage-clipped sequence over a scene whose occluder is only
 * SOMETIMES a valid one, then compare against the same scene rendered with a forced full repaint.
 * Any cull that skipped a layer it should not have shows up as a pixel difference.
 */
static int check_pixel_equivalence(void)
{
    static uint32_t reference[SCREEN * SCREEN];
    ERNode* inner = NULL;
    ERNode* cover = NULL;

    ERProps cover_props = box(0, 0, SCREEN, SCREEN, COLOR_COVER);
    build(&cover_props, &inner, &cover);
    full_frame();

    /* A sequence of incremental commits: shrink the cover so the buried layers reappear, recolour
     * the buried leaf while it is exposed, then make the cover opaque and full-screen again. */
    ERProps step = box(0, 0, SCREEN, 120, COLOR_COVER);
    er_node_set_props(cover, &step);
    er_commit();

    ERProps inner_props = box(20, 130, 100, 60, 0xFF884400U);
    er_node_set_props(inner, &inner_props);
    er_commit();

    step = box(0, 0, SCREEN, 120, 0xA000FF00U); /* translucent: the layers below must show through */
    er_node_set_props(cover, &step);
    er_commit();

    memcpy(reference, s_framebuffer, sizeof(reference));

    /* Same scene, everything drawn. */
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    full_frame();

    if (memcmp(reference, s_framebuffer, sizeof(reference)) != 0)
        return fail("incremental (culled) frame differs from a full repaint");
    return EXIT_SUCCESS;
}

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {.fill_rect = fb_fill, .copy_rect = fb_copy, .blend_rect = fb_blend};
    embedded_renderer_set_backend(&k_backend);

    int (*const cases[])(void) = {
        check_opaque_cover_culls,
        check_translucent_cover_does_not_cull,
        check_modal_backdrop,
        check_rounded_cover_does_not_cull,
        check_partial_cover_does_not_cull,
        check_buried_change_retires,
#if ERUI_TRANSFORMS_FULL
        check_transformed_sibling_not_buried,
#endif
        check_translated_node_under_cover_leaves_no_trail,
#if ERUI_TRANSFORMS_FULL
        check_rotated_node_in_clipper_not_buried,
        check_rotated_node_keeps_footprint_across_offscreen_change,
#if ERUI_3D_TRANSFORMS
        check_3d_node_keeps_footprint_across_offscreen_change,
#endif
#endif
        check_pixel_equivalence,
    };

    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index++)
    {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        log_reset();
        const int result = cases[case_index]();
        if (result != EXIT_SUCCESS)
            return result;
        er_reset();
    }
    return EXIT_SUCCESS;
}
