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
 * Reflow damage: changing a node's LAYOUT props does not, by itself, damage that node's box. The
 * node still looks the same; what the repaint has to cover is whatever the layout pass actually
 * moved, which the damage pre-pass already finds by comparing each node's new screen rect against
 * where it was last painted.
 *
 *   - a layout prop change that moves nothing repaints nothing, on that commit and the next,
 *   - a layout prop change that moves one child repaints that child's old and new spots, not the
 *     whole container that was re-measured,
 *   - an appearance change still damages the full box (the split must not lose real changes),
 *   - and the moved-child frame is pixel-identical to a full repaint of the same scene.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define BAR_HEIGHT 60
#define KID_SIZE 20
#define OPACITY_OPAQUE 255U

static uint32_t s_framebuffer[SCREEN * SCREEN];
static int s_fill_count;

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
    s_fill_count++;
    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= SCREEN)
                continue;
            uint32_t* destination = &s_framebuffer[row * SCREEN + column];
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - alpha;
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU,
                           source_blue = argb & 0xFFU;
            *destination = 0xFF000000U | ((div255(source_red * alpha) + div255(destination_red * inverse_alpha)) << 16)
                           | ((div255(source_green * alpha) + div255(destination_green * inverse_alpha)) << 8)
                           | (div255(source_blue * alpha) + div255(destination_blue * inverse_alpha));
        }
    }
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

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/** @brief The bar's props at a given left padding — the one thing the tests vary. */
static ERProps bar_props(int16_t padding_left, uint32_t background)
{
    ERProps props = props_default();
    props.width = SCREEN;
    props.height = BAR_HEIGHT;
    props.flex_direction = ER_FLEX_ROW;
    props.padding_left = padding_left;
    props.background_color = background;
    return props;
}

/** @brief root(white, 200x200) → bar(grey, full-width strip) → kid(orange 20x20). */
static void build(ERNode** out_bar)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bar_node_props = bar_props(0, 0xFF303030U);
    er_node_set_props(bar, &bar_node_props);

    ERNode* kid = er_node_create(ER_NODE_VIEW);
    ERProps kid_props = props_default();
    kid_props.width = KID_SIZE;
    kid_props.height = KID_SIZE;
    kid_props.background_color = 0xFFFF8800U;
    er_node_set_props(kid, &kid_props);

    er_tree_append_child(bar, kid);
    er_tree_append_child(root, bar);
    er_tree_set_root(root);
    *out_bar = bar;
}

/** @brief Renders a settled starting frame (full repaint, then one idle commit). */
static void settle(void)
{
    er_force_full_repaint();
    er_commit();
    s_fill_count = 0;
    er_commit();
}

/* A layout prop that changes nothing on screen must cost nothing — this commit or any after it. */
static int check_no_move_no_damage(void)
{
    ERNode* bar = NULL;
    build(&bar);
    settle();
    if (s_fill_count != 0)
        return fail("the settled scene was not idle to begin with");

    /* max_width above the bar's actual width: a real prop change, zero geometric effect. */
    ERProps bar_node_props = bar_props(0, 0xFF303030U);
    bar_node_props.max_width = SCREEN + 100;
    er_node_set_props(bar, &bar_node_props);

    s_fill_count = 0;
    er_commit();
    if (s_fill_count != 0)
        return fail("a layout change that moved nothing still repainted");

    s_fill_count = 0;
    er_commit();
    if (s_fill_count != 0)
        return fail("the reflow re-damaged on the following idle commit");
    return EXIT_SUCCESS;
}

/* Shifting one child inside a full-width bar repaints the child's trail, not the whole bar. */
static int check_moved_child_only(void)
{
    ERNode* bar = NULL;
    build(&bar);
    settle();

    ERProps bar_node_props = bar_props(40, 0xFF303030U);
    er_node_set_props(bar, &bar_node_props);
    er_commit();

    ERRect dirty;
    if (!er_get_dirty_rect(&dirty))
        return fail("moving a child reported no damage at all");
    /* Both footprints, or the contract is broken in the other direction: the reported rect has to
     * COVER what was repainted, and a move repaints the trail as well as the destination. Old spot
     * is x[0,20), new spot x[40,60). */
    if (dirty.x > 0 || dirty.x + dirty.width < 40 + KID_SIZE)
        return fail("reported damage does not cover both the old and new position of a moved child");
    /* ...and no wider than that, or nothing was saved over damaging the whole 200px bar. */
    if (dirty.width > 100)
        return fail("a moved child damaged the whole re-measured container");
    if (dirty.height > BAR_HEIGHT + 8)
        return fail("damage spilled outside the bar's own strip");
    return EXIT_SUCCESS;
}

/* The split must not swallow real appearance changes: a recolour still damages the full box. */
static int check_visual_change_still_damages(void)
{
    ERNode* bar = NULL;
    build(&bar);
    settle();

    ERProps bar_node_props = bar_props(0, 0xFF00AA00U);
    er_node_set_props(bar, &bar_node_props);
    er_commit();

    ERRect dirty;
    if (!er_get_dirty_rect(&dirty))
        return fail("a recolour reported no damage");
    if (dirty.width < SCREEN || dirty.height < BAR_HEIGHT)
        return fail("a recolour damaged less than the node's box");
    if ((s_framebuffer[10 * SCREEN + 150] & 0x00FFFFFFU) != 0x0000AA00U)
        return fail("the new colour did not reach the far end of the bar");
    return EXIT_SUCCESS;
}

/* Catch-all: the incrementally repainted frame equals a full repaint of the same scene. */
static int check_pixel_equivalence(void)
{
    static uint32_t reference[SCREEN * SCREEN];
    ERNode* bar = NULL;
    build(&bar);
    settle();

    ERProps bar_node_props = bar_props(40, 0xFF303030U);
    er_node_set_props(bar, &bar_node_props);
    er_commit();
    bar_node_props.max_width = SCREEN + 100; /* a second, movement-free reflow on top */
    er_node_set_props(bar, &bar_node_props);
    er_commit();
    memcpy(reference, s_framebuffer, sizeof(reference));

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();

    if (memcmp(reference, s_framebuffer, sizeof(reference)) != 0)
        return fail("incremental reflow frame differs from a full repaint");
    return EXIT_SUCCESS;
}

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {.fill_rect = fb_fill};
    embedded_renderer_set_backend(&k_backend);

    int (*const cases[])(void) = {
        check_no_move_no_damage,
        check_moved_child_only,
        check_visual_change_still_damages,
        check_pixel_equivalence,
    };

    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index++)
    {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        s_fill_count = 0;
        const int result = cases[case_index]();
        if (result != EXIT_SUCCESS)
            return result;
        er_reset();
    }
    return EXIT_SUCCESS;
}
