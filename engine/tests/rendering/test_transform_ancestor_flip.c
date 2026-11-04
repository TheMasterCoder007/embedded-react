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
 * Issue #139: an ANCESTOR's transform change flips a nested transformed node's footprint.
 *
 * Only the outermost transform that fits the scratch and inverts gets to capture; a transformed node
 * inside one is refused and paints its raw box in the ancestor's source space instead. So which
 * footprint a nested node paints — its transformed AABB, on screen, or its raw box, inside someone
 * else's capture — is decided ABOVE it. An ancestor gaining or losing a transform flips it without
 * ever dirtying the node or moving its layout box.
 *
 * The damage pre-pass predicted that from last_paint_untransformed, the flag the previous paint left
 * behind, and only ever consulted it inside `if (n->source_dirty || moved)`. An ancestor-side change
 * satisfies neither: the node is clean, and the (stale) prediction still agrees with last_paint_rect,
 * so it reads as idle and contributes nothing. render_tree then repaints it anyway — the whole subtree
 * under a dirty ancestor repaints — onto the OTHER footprint, scissored to whatever damage the
 * ancestor happened to contribute, and records that footprint. From then on the prediction agrees with
 * the new rect, `moved` stays false, and the half-drawn frame never heals.
 *
 * Both directions are covered, because the staleness runs both ways:
 *
 *   - the ancestor LOSES its transform: the child stops being fallback-painted and takes the capture
 *     itself, so its pixels appear at an AABB nothing damaged;
 *   - the ancestor GAINS one: the child stops capturing and paints into the ancestor's source, so the
 *     AABB it used to blit is abandoned and nothing erases the trail.
 *
 * Each case asserts the damage-clipped commit is byte-identical to a forced full repaint of the same
 * scene state, that every changed pixel was reported through er_get_dirty_rects(), and that a
 * following idle commit neither heals it nor churns.
 *
 * Requires ERUI_TRANSFORMS=FULL — without the capture path there is no source space, nothing to
 * refuse, and no flip.
 */

#include "er_node_internal.h" /* last_paint_untransformed — asserts each case really sets up the flip */
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame */
#include "transform.h"         /* er_transform_source_fits — asserts the outer really captures */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define FRAMEBUFFER_PIXELS (SCREEN * SCREEN)
#define OPACITY_OPAQUE 255U

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

/* Everything from here down is the test proper: without a capture path there is no fallback to flip,
 * so main() skips outright and none of it is built. Only the backend above stays unconditional. */
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
 * @param[out] bbox          Receives the bounding box of the differing pixels (untouched when identical).
 *
 * @return Number of differing pixels.
 */
static int diff_frames(const uint32_t* first_frame, const uint32_t* second_frame, ERRect* bbox)
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
    if (count > 0 && bbox)
    {
        bbox->x = min_x;
        bbox->y = min_y;
        bbox->w = max_x - min_x + 1;
        bbox->h = max_y - min_y + 1;
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
                covered = (x >= rects[rect_index].x && x < rects[rect_index].x + rects[rect_index].w
                           && y >= rects[rect_index].y && y < rects[rect_index].y + rects[rect_index].h);
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
                if (x >= rects[rect_index].x && x < rects[rect_index].x + rects[rect_index].w
                    && y >= rects[rect_index].y && y < rects[rect_index].y + rects[rect_index].h)
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

/** @brief Which way the ancestor's transform flips across the commit under test. */
typedef enum
{
    FLIP_LOSE, /**< Outer starts transformed (inner is fallback-painted) and goes to identity. */
    FLIP_GAIN  /**< Outer starts plain (inner captures) and gains a transform. */
} Flip;

/** @brief One scenario: an outer/inner geometry plus the transform the outer flips in and out of. */
typedef struct
{
    int outer_x, outer_y;              /**< Outer position (absolute, in the root). */
    int inner_x, inner_y;              /**< Inner position (absolute, inside the outer). */
    float outer_rotation, outer_scale; /**< The outer's transform while it has one. */
    float inner_rotation, inner_scale; /**< The inner's transform, unchanged throughout. */
    const char* name;
} Scenario;

/** @brief Applies (or clears) the outer's transform on a props block. */
static void set_outer_transform(ERProps* props, const Scenario* scenario, bool transformed)
{
    props->transform_rotate_z = transformed ? scenario->outer_rotation : 0.0f;
    props->transform_scale_x = transformed ? scenario->outer_scale : 0.0f;
    props->transform_scale_y = transformed ? scenario->outer_scale : 0.0f;
}

/**
 * @brief Flips an ancestor's transform over a nested transformed child and demands a full-repaint match.
 *
 * @param[in] scenario  Scenario geometry.
 * @param[in] flip      Direction of the flip.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
static int check_ancestor_flip(const Scenario* scenario, Flip flip)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFFFFFFFFU; /* opaque: a full repaint really covers the screen */
    er_node_set_props(root, &root_props);

    /* Outer: fits the transform scratch comfortably, so whenever it has a transform it is the one
     * that captures — and the inner one below is refused. */
    ERNode* outer = er_node_create(ER_NODE_VIEW);
    ERProps outer_props = props_default();
    outer_props.position = ER_POS_ABSOLUTE;
    outer_props.left = (int16_t)scenario->outer_x;
    outer_props.top = (int16_t)scenario->outer_y;
    outer_props.width = 100;
    outer_props.height = 100;
    outer_props.background_color = 0xFF2244AAU;
    set_outer_transform(&outer_props, scenario, flip == FLIP_LOSE);
    er_node_set_props(outer, &outer_props);

    /* Inner: its own transform never changes. Whether it paints its AABB on screen or its raw box
     * inside the outer's source is decided entirely by the outer. */
    ERNode* inner = er_node_create(ER_NODE_VIEW);
    ERProps inner_props = props_default();
    inner_props.position = ER_POS_ABSOLUTE;
    inner_props.left = (int16_t)scenario->inner_x;
    inner_props.top = (int16_t)scenario->inner_y;
    inner_props.width = 60;
    inner_props.height = 60;
    inner_props.background_color = 0xFF33FF33U;
    inner_props.transform_rotate_z = scenario->inner_rotation;
    inner_props.transform_scale_x = scenario->inner_scale;
    inner_props.transform_scale_y = scenario->inner_scale;
    er_node_set_props(inner, &inner_props);

    er_tree_append_child(outer, inner);
    er_tree_append_child(root, outer);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: every node has a last-painted footprint and a settled fallback flag */

    /* The case only tests anything if the starting state really is the one it names. */
    const bool want_fallback = (flip == FLIP_LOSE);
    if (!inner->has_last_paint || inner->last_paint_untransformed != want_fallback)
    {
        fprintf(stderr,
                "  setup: inner has_last_paint=%d last_paint_untransformed=%d (wanted %d)\n",
                (int)inner->has_last_paint,
                (int)inner->last_paint_untransformed,
                (int)want_fallback);
        er_node_destroy(root);
        return fail("the scenario did not set up the capture state it names");
    }

    static uint32_t before[FRAMEBUFFER_PIXELS];
    memcpy(before, s_framebuffer, sizeof(before));

    set_outer_transform(&outer_props, scenario, flip == FLIP_GAIN);
    er_node_set_props(outer, &outer_props);

    er_commit(); /* the damage-clipped commit under test */

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);

    static uint32_t incremental[FRAMEBUFFER_PIXELS];
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    /* An idle commit must not be what heals it — nor damage anything of its own. */
    er_commit();
    const int idle_differ = diff_frames(s_framebuffer, incremental, NULL);

    er_force_full_repaint();
    er_commit(); /* reference: the same scene state, painted in full */

    ERRect bbox = {0, 0, 0, 0};
    const int differ = diff_frames(incremental, s_framebuffer, &bbox);
    const int missed = unreported_pixels(before, incremental, rects, rect_count);
    const int changed = diff_frames(before, s_framebuffer, NULL); /* the flip has to move pixels at all */
    const int painted = reported_area(rects, rect_count);

    printf("%s / outer %s transform: %d px differ from a full repaint%s; %d rect(s) over %d px "
           "reported, %d changed px unreported; idle commit moved %d px (%d px changed in all)\n",
           scenario->name,
           flip == FLIP_LOSE ? "loses" : "gains",
           differ,
           differ ? "" : " (identical)",
           rect_count,
           painted,
           missed,
           idle_differ,
           changed);
    if (differ)
        printf("  divergence bbox %d,%d %dx%d\n", bbox.x, bbox.y, bbox.w, bbox.h);

    er_node_destroy(root);

    if (changed == 0)
        return fail("the ancestor's transform flip changed nothing on screen — the case proves nothing");
    if (differ != 0)
        return fail("an ancestor-side transform flip diverged from a full repaint — the nested node "
                    "changed footprint without contributing damage");
    if (missed != 0)
        return fail("pixels changed outside every reported dirty rect — a partial-update host would "
                    "leave the panel stale");
    if (idle_differ != 0)
        return fail("an idle commit repainted differently — the transformed subtree is churning");
    /* Asked last, and it is the reason the three above are worth asking: widening the damage to the
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
    /* Vacuous unless the outer node really takes the capture path. */
    if (!er_transform_source_fits(100, 100) || !er_transform_source_fits(60, 60))
    {
        printf("SKIP: a 100x100 outer / 60x60 inner do not fit this build's transform scratch\n");
        return EXIT_SUCCESS;
    }

    static const Scenario k_scenarios[] = {
        /* The inner blows up to 120x120 when it captures, so its AABB reaches well past every rect
         * the outer's own damage can contribute. */
        {40, 40, 20, 20, 0.0f, 0.5f, 0.0f, 2.0f, "outer scale .5, inner scale 2"},
        /* Rotation on both: neither footprint is a scaled copy of the other, and the outer's old
         * AABB covering the inner's new one would be pure coincidence. */
        {10, 10, 20, 20, 20.0f, 0.0f, 45.0f, 1.5f, "outer rot 20, inner rot 45 scale 1.5"},
        /* The inner tucked into a corner of the source, so its AABB leaves the outer's box entirely. */
        {50, 50, 50, 50, 30.0f, 0.6f, 0.0f, 1.6f, "outer rot 30 scale .6, inner corner scale 1.6"},
    };

    for (size_t scenario_index = 0; scenario_index < sizeof(k_scenarios) / sizeof(k_scenarios[0]); scenario_index++)
    {
        const int lose_result = check_ancestor_flip(&k_scenarios[scenario_index], FLIP_LOSE);
        if (lose_result != EXIT_SUCCESS)
            return lose_result;
        const int gain_result = check_ancestor_flip(&k_scenarios[scenario_index], FLIP_GAIN);
        if (gain_result != EXIT_SUCCESS)
            return gain_result;
    }

    printf("PASS: an ancestor gaining or losing a transform over a nested transformed node matches a "
           "full repaint\n");
#else
    printf("SKIP: ERUI_TRANSFORMS=FULL required (no transform capture, no fallback to flip)\n");
#endif
    return EXIT_SUCCESS;
}
