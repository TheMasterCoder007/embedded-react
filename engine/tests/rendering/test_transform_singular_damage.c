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
 * Issue #138: a transform too SINGULAR to invert paints untransformed, but damage measured its AABB.
 *
 * render_tree paints a scale/rotate/3D node by capturing its subtree into the transform scratch and
 * inverse-mapping it back out. Three things stop that capture: the node is too large for the source,
 * an ancestor already holds it, or — this one — the matrix cannot be inverted, so there is no inverse
 * map to blit with (|det| < 1e-6 affine, < 1e-7 for the 3D homography). On any of them it degrades to
 * painting the node untransformed at its raw layout box.
 *
 * The damage pre-pass predicted the first two and was blind to the third, so it damaged the
 * (sub-pixel) AABB while the paint laid down the full raw box, scissored to that damage — and the tear
 * never healed: the fallback paint recorded the raw box with last_paint_untransformed set, from which
 * the next commit's prediction agreed with last_paint_rect, `moved` stayed false, and the node was
 * never source-dirty again. Zero damage, forever.
 *
 * Each case crosses the invert threshold in one commit and asserts the damage-clipped frame is
 * byte-identical to a forced full repaint of the same scene state, that every changed pixel was
 * reported through er_get_dirty_rects(), and that idle commits neither heal it nor churn. Both
 * directions are covered — into the singular band and back out of it — and each singular case is
 * paired with a control just the other side of the epsilon, where the invert succeeds and the AABB
 * really is the footprint.
 *
 * er_transform_is_invertible() is asserted against every case up front, so a scenario that stopped
 * straddling the threshold (a changed epsilon, a changed matrix) fails as a broken setup rather than
 * quietly passing on both sides of a test that no longer tests anything.
 *
 * Requires ERUI_TRANSFORMS=FULL — without the capture path there is no inverse map to fail.
 */

#include "er_node_internal.h" /* last_paint_untransformed — asserts the fallback paint really happened */
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame */
#include "transform.h"         /* er_transform_is_invertible / _source_fits — asserts the path taken */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define FRAMEBUFFER_PIXELS (SCREEN * SCREEN)
#define OPACITY_OPAQUE 255U

/* Node geometry. The width is ODD so the default centre pivot lands on a half pixel: a matrix that
 * collapses the box to a line then still rounds out to a 1px-wide AABB instead of a zero-area one,
 * which would take the pre-pass's full-repaint fallback and heal the bug it is meant to expose. */
#define NODE_X 60
#define NODE_Y 60
#define NODE_WIDTH 101
#define NODE_HEIGHT 100

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[FRAMEBUFFER_PIXELS];

/** @brief Rounds a 0-65025 product back to 0-255 the way the engine's blenders do. */
static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

/** @brief Backend fill_rect: source-over composites a straight-alpha colour. */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t first_frame = (argb >> 24) & 0xFFU;
    if (first_frame == 0U || width <= 0 || height <= 0)
        return;
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - first_frame;
    const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU, source_blue = argb & 0xFFU;
    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= SCREEN)
                continue;
            uint32_t* destination = &s_framebuffer[row * SCREEN + column];
            if (first_frame == 0xFFU)
            {
                *destination = 0xFF000000U | (argb & 0x00FFFFFFU);
                continue;
            }
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U
                           | ((div255(source_red * first_frame) + div255(destination_red * inverse_alpha)) << 16)
                           | ((div255(source_green * first_frame) + div255(destination_green * inverse_alpha)) << 8)
                           | (div255(source_blue * first_frame) + div255(destination_blue * inverse_alpha));
        }
    }
}

/** @brief Backend copy_rect: source-over composites premultiplied scratch — this is how the blit lands. */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= SCREEN || destination_y < 0 || destination_y >= SCREEN)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            if (source_alpha == 0U)
                continue; /* fully transparent: leave the destination alone */
            uint32_t* destination = &s_framebuffer[destination_y * SCREEN + destination_x];
            if (source_alpha == 0xFFU)
            {
                *destination = source_pixel;
                continue;
            }
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U
                           | ((((source_pixel >> 16) & 0xFFU) + div255(destination_red * inverse_alpha)) << 16)
                           | ((((source_pixel >> 8) & 0xFFU) + div255(destination_green * inverse_alpha)) << 8)
                           | ((source_pixel & 0xFFU) + div255(destination_blue * inverse_alpha));
        }
    }
}

/** @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha. */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= SCREEN || destination_y < 0 || destination_y >= SCREEN)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = div255(((source_pixel >> 24) & 0xFFU) * (uint32_t)alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t source_red = div255(((source_pixel >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_green = div255(((source_pixel >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_blue = div255((source_pixel & 0xFFU) * (uint32_t)alpha);
            uint32_t* destination = &s_framebuffer[destination_y * SCREEN + destination_x];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                           | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                           | (source_blue + div255(destination_blue * inverse_alpha));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/* Everything from here down is the test proper: without a capture path there is no inverse map to
 * refuse, so main() skips outright and none of it is built. Only the backend above stays unconditional. */
#if ERUI_TRANSFORMS_FULL

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

/**
 * @brief Reports how two frames differ, and where.
 *
 * @param[in]  first_frame   First frame.
 * @param[in]  second_frame  Second frame.
 * @param[out] bounds        Receives the bounding box of the differing pixels (untouched when identical).
 *
 * @return Number of differing pixels.
 */
static int diff_frames(const uint32_t* first_frame, const uint32_t* second_frame, ERRect* bounds)
{
    int count = 0, min_x = SCREEN, min_y = SCREEN, max_x = -1, max_y = -1;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            if (first_frame[y * SCREEN + x] == second_frame[y * SCREEN + x])
                continue;
            count++;
            if (x < min_x)
                min_x = x;
            if (y < min_y)
                min_y = y;
            if (x > max_x)
                max_x = x;
            if (y > max_y)
                max_y = y;
        }
    }
    if (count > 0 && bounds)
    {
        bounds->x = min_x;
        bounds->y = min_y;
        bounds->width = max_x - min_x + 1;
        bounds->height = max_y - min_y + 1;
    }
    return count;
}

/**
 * @brief Counts changed pixels that no reported dirty rect covers.
 *
 * @param[in] before  Frame before the commit.
 * @param[in] after   Frame after it.
 * @param[in] rects   Rects er_get_dirty_rects() reported for that commit.
 * @param[in] count   Number of rects.
 *
 * @return Number of changed pixels outside every reported rect.
 */
static int unreported_pixels(const uint32_t* before, const uint32_t* after, const ERRect* rects, int count)
{
    int missed = 0;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            if (before[y * SCREEN + x] == after[y * SCREEN + x])
                continue;
            bool covered = false;
            for (int rect_index = 0; rect_index < count && !covered; rect_index++)
                covered = (x >= rects[rect_index].x && x < rects[rect_index].x + rects[rect_index].width
                           && y >= rects[rect_index].y && y < rects[rect_index].y + rects[rect_index].height);
            if (!covered)
                missed++;
        }
    }
    return missed;
}

/**
 * @brief Counts the screen pixels covered by at least one reported dirty rect.
 *
 * @param[in] rects  Rects er_get_dirty_rects() reported.
 * @param[in] count  Number of rects.
 *
 * @return Number of covered pixels.
 */
static int reported_area(const ERRect* rects, int count)
{
    int area = 0;
    for (int y = 0; y < SCREEN; y++)
    {
        for (int x = 0; x < SCREEN; x++)
        {
            for (int rect_index = 0; rect_index < count; rect_index++)
            {
                if (x >= rects[rect_index].x && x < rects[rect_index].x + rects[rect_index].width
                    && y >= rects[rect_index].y && y < rects[rect_index].y + rects[rect_index].height)
                {
                    area++;
                    break;
                }
            }
        }
    }
    return area;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief One transform state: what a scenario settles at, and what it commits to. */
typedef struct
{
    float scale;     /**< 0 = unset (identity scale). */
    float rotate_z;  /**< Degrees. */
    float rotate_y;  /**< Degrees (3D; ignored unless ERUI_3D_TRANSFORMS). */
    bool invertible; /**< What er_transform_is_invertible() must answer for it. */
} Xform;

/** @brief A settled state, the state one commit moves it to, and what the pair is called. */
typedef struct
{
    Xform from, to;
    const char* name;
} Scenario;

/** @brief Applies a transform state to a props block. */
static void set_transform(ERProps* props, const Xform* x)
{
    props->transform_scale_x = x->scale;
    props->transform_scale_y = x->scale;
    props->transform_rotate_z = x->rotate_z;
#if ERUI_3D_TRANSFORMS
    props->transform_rotate_y = x->rotate_y;
#else
    (void)x->rotate_y;
#endif
}

/**
 * @brief Drives one node across the invert threshold and demands a full-repaint match.
 *
 * @param[in] scenario  Scenario to run.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_singular_crossing(const Scenario* scenario)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really covers the screen */
    er_node_set_props(root, &root_props);

    ERNode* node = er_node_create(ER_NODE_VIEW);
    ERProps node_props = props_default();
    node_props.position = ER_POSITION_ABSOLUTE;
    node_props.left = NODE_X;
    node_props.top = NODE_Y;
    node_props.width = NODE_WIDTH;
    node_props.height = NODE_HEIGHT;
    node_props.background_color = 0xFF2244AAU;
    set_transform(&node_props, &scenario->from);
    er_node_set_props(node, &node_props);

    er_tree_append_child(root, node);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: a last-painted footprint and a settled fallback flag */

    /* The scenario only tests anything if both states really sit where it says they do. The node has
     * no scroll or keyboard shift above it, so its layout position IS the origin render_tree hands the
     * matrix. */
    const bool from_ok =
        er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_WIDTH, NODE_HEIGHT) == scenario->from.invertible;
    if (!from_ok || node->last_paint_untransformed == scenario->from.invertible)
    {
        fprintf(stderr,
                "  setup: settled state invertible=%d (wanted %d), last_paint_untransformed=%d\n",
                (int)er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_WIDTH, NODE_HEIGHT),
                (int)scenario->from.invertible,
                (int)node->last_paint_untransformed);
        er_node_destroy(root);
        return fail("the settled state did not take the capture path the scenario names");
    }

    static uint32_t before[FRAMEBUFFER_PIXELS];
    memcpy(before, s_framebuffer, sizeof(before));

    set_transform(&node_props, &scenario->to);
    er_node_set_props(node, &node_props);

    if (er_transform_is_invertible(node, NODE_X, NODE_Y, NODE_WIDTH, NODE_HEIGHT) != scenario->to.invertible)
    {
        er_node_destroy(root);
        return fail("the committed state did not land on the side of the epsilon the scenario names");
    }

    er_commit(); /* the damage-clipped commit under test */

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    const bool painted_raw = node->last_paint_untransformed;

    static uint32_t incremental[FRAMEBUFFER_PIXELS];
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    /* Idle commits must not be what heals it — nor damage anything of their own. The bug survived
     * every one of them, so more than a single tick is worth asking for. */
    er_commit();
    er_commit();
    er_commit();
    const int idle_differ = diff_frames(s_framebuffer, incremental, NULL);

    er_force_full_repaint();
    er_commit(); /* reference: the same scene state, painted in full */

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(incremental, s_framebuffer, &bbox);
    const int missed = unreported_pixels(before, incremental, rects, rect_count);
    const int changed = diff_frames(before, s_framebuffer, NULL); /* the crossing has to move pixels at all */
    const int painted = reported_area(rects, rect_count);

    printf("%s: %d px differ from a full repaint%s; %d rect(s) over %d px reported, %d changed px "
           "unreported; 3 idle commits moved %d px (%d px changed in all; paint used the %s box)\n",
           scenario->name,
           differ,
           differ ? "" : " (identical)",
           rect_count,
           painted,
           missed,
           idle_differ,
           changed,
           painted_raw ? "raw" : "transformed");
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.width, bbox.height);

    er_node_destroy(root);

    if (changed == 0)
        return fail("crossing the invert threshold changed nothing on screen — the case proves nothing");
    /* The paint has to have done what the pre-pass was asked to predict, or a passing frame says
     * nothing about the prediction. */
    if (painted_raw == scenario->to.invertible)
        return fail("the commit under test did not take the paint path its matrix implies");
    if (differ != 0)
        return fail("a transform that crossed the invert threshold diverged from a full repaint — "
                    "damage measured a footprint the paint never used");
    if (missed != 0)
        return fail("pixels changed outside every reported dirty rect — a partial-update host would "
                    "leave the panel stale");
    if (idle_differ != 0)
        return fail("an idle commit repainted differently — the transformed node is churning");
    /* Asked last, and it is the reason the ones above are worth asking: widening the damage to the
     * whole root satisfies every one of them and gives up exactly what the pre-pass exists for. */
    if (painted >= FRAMEBUFFER_PIXELS)
        return fail("the commit repainted the entire root — correct, but no longer damage-clipped");
    return EXIT_SUCCESS;
}

#endif /* ERUI_TRANSFORMS_FULL */

int main(void)
{
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

#if ERUI_TRANSFORMS_FULL
    /* Vacuous unless the node really takes the capture path when its matrix does invert. */
    if (!er_transform_source_fits(NODE_WIDTH, NODE_HEIGHT))
    {
        printf("SKIP: a %dx%d node does not fit this build's transform scratch\n", NODE_WIDTH, NODE_HEIGHT);
        return EXIT_SUCCESS;
    }

    static const Scenario k_scenarios[] = {
        /* det = 2.5e-7, an order of magnitude under the 1e-6 affine epsilon: the capture starts (the
         * node fits) and the inverse-map blit is what refuses, which is the hole. */
        {{0.5f, 0.0f, 0.0f, true}, {0.0005f, 0.0f, 0.0f, false}, "affine scale .5 -> .0005 (singular)"},
        /* The control, just the other side: det = 4e-6, the invert succeeds and the AABB really is
         * the footprint. Passes with or without the fix — it is here to prove the pair straddles the
         * epsilon rather than both landing on the broken side of it. */
        {{0.5f, 0.0f, 0.0f, true}, {0.002f, 0.0f, 0.0f, true}, "affine scale .5 -> .002 (invertible)"},
        /* Back out of the singular band: the node has been fallback-painting its raw box and now
         * blows up to an AABB well outside it, so the abandoned box has to be erased too. */
        {{0.0005f, 0.0f, 0.0f, false}, {1.6f, 0.0f, 0.0f, true}, "affine scale .0005 -> 1.6 (heals)"},
        /* Rotation in the mix, so the singular AABB is not simply a shrunken copy of the raw box. */
        {{1.2f, 30.0f, 0.0f, true}, {0.0008f, 30.0f, 0.0f, false}, "affine rot 30, scale 1.2 -> .0008"},
#if ERUI_3D_TRANSFORMS
        /* The same hole on the 3D path: at rotate_y 90 the plane is edge-on, the homography's det
         * (~4.4e-8 orthographic) falls under the 1e-7 3D epsilon, and er_transform_homography_invert
         * refuses. The projected AABB is a full-height 1px slice — non-degenerate, so the pre-pass
         * bounds it happily and scissors the raw-box paint down to a sliver. */
        {{0.0f, 0.0f, 45.0f, true}, {0.0f, 0.0f, 90.0f, false}, "3D rotateY 45 -> 90 (singular)"},
        /* The 3D control, 10 degrees off edge-on: det ~0.17, comfortably invertible. */
        {{0.0f, 0.0f, 45.0f, true}, {0.0f, 0.0f, 80.0f, true}, "3D rotateY 45 -> 80 (invertible)"},
        /* And back out of it. */
        {{0.0f, 0.0f, 90.0f, false}, {0.0f, 0.0f, 30.0f, true}, "3D rotateY 90 -> 30 (heals)"},
#endif
    };

    for (size_t scenario_index = 0; scenario_index < sizeof(k_scenarios) / sizeof(k_scenarios[0]); scenario_index++)
    {
        const int result = check_singular_crossing(&k_scenarios[scenario_index]);
        if (result != EXIT_SUCCESS)
            return result;
    }

    printf("PASS: a transform crossing the invert threshold matches a full repaint\n");
#else
    printf("SKIP: ERUI_TRANSFORMS=FULL required (no capture path, no inverse map to refuse)\n");
#endif
    return EXIT_SUCCESS;
}
