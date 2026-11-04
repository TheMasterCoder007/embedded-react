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
 * Disjoint dirty-rect damage: er_commit() tracks a SET of disjoint repaint rects instead of one
 * bounding box, so a change in the top-left corner plus one in the bottom-right repaints two small
 * areas — not the whole span between them. These tests drive a backend that both records which
 * pixels each op touches AND source-over composites fills into a real framebuffer:
 *
 *   - corner isolation: recolouring two opposite-corner boxes in one commit leaves the screen
 *     centre untouched, and every op lands inside one of the er_get_dirty_rects() rects,
 *   - pixel equivalence: the multi-pass incremental result is byte-identical to a forced full
 *     repaint — the catch-all for any multi-pass compositing bug (double blends, missed
 *     backgrounds, clip errors), asserted with TRANSLUCENT content where such bugs show,
 *   - multi-buffer replay: with 2 page-flip buffers, the frame after a two-corner change replays
 *     both corners into the stale buffer and still leaves the centre untouched,
 *   - er_get_dirty_rects() contract: disjoint, covering, bbox-collapse when the caller's buffer is
 *     too small, count query with NULL,
 *   - scattered grid: a dozen small widgets updating at once stay a dozen small rects instead of
 *     cascade-merging toward the whole-grid bounding box (the saturation the rect budget bounds).
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame for pixel equivalence */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer + per-pixel touch tracking
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN * SCREEN];
static uint8_t s_touched[SCREEN * SCREEN]; /* pixels written by any op since touched_reset() */

static void touched_reset(void)
{
    memset(s_touched, 0, sizeof(s_touched));
}

static void mark_touched(int x, int y, int width, int height)
{
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            if (row >= 0 && row < SCREEN && column >= 0 && column < SCREEN)
                s_touched[row * SCREEN + column] = 1U;
}

static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

/** @brief Straight-alpha source-over fill (mirrors backends/software), plus touch tracking. */
static void fb_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U || width <= 0 || height <= 0)
        return;
    mark_touched(x, y, width, height);
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
                const uint32_t red = div255(source_red * alpha) + div255(destination_red * inverse_alpha);
                const uint32_t green = div255(source_green * alpha) + div255(destination_green * inverse_alpha);
                const uint32_t blue = div255(source_blue * alpha) + div255(destination_blue * inverse_alpha);
                *destination = 0xFF000000U | (red << 16) | (green << 8) | blue;
            }
        }
    }
}

static void fb_copy(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)ctx;
    mark_touched(x, y, width, height); /* content irrelevant to these scenes; the footprint is what matters */
}

static void fb_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)ctx;
    mark_touched(x, y, width, height);
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

static void frame(void)
{
    er_commit();
    er_display_present();
}

/**
 * @brief Builds the two-corner scene: opaque white root, box A top-left, box B bottom-right.
 *
 * Column layout: A occupies y 10..40 at x 10; B's margins place it at (160,160)..(190,190).
 * @p alpha sets the boxes' background alpha (255 = opaque; less exercises translucent compositing).
 */
static void build_corner_scene(uint8_t alpha, ERNode** out_root, ERNode** out_box_a, ERNode** out_box_b)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    ERNode* box_a = er_node_create(ER_NODE_VIEW);
    ERProps box_a_props = props_default();
    box_a_props.width = 30;
    box_a_props.height = 30;
    box_a_props.margin_left = 10;
    box_a_props.margin_top = 10;
    box_a_props.background_color = ((uint32_t)alpha << 24) | 0x003366FFU;
    er_node_set_props(box_a, &box_a_props);

    ERNode* box_b = er_node_create(ER_NODE_VIEW);
    ERProps box_b_props = props_default();
    box_b_props.width = 30;
    box_b_props.height = 30;
    box_b_props.margin_left = 160;
    box_b_props.margin_top = 120; /* below A's bottom edge (y=40): 40+120 = 160 */
    box_b_props.background_color = ((uint32_t)alpha << 24) | 0x00CC3344U;
    er_node_set_props(box_b, &box_b_props);

    er_tree_append_child(root, box_a);
    er_tree_append_child(root, box_b);
    er_tree_set_root(root);
    *out_root = root;
    *out_box_a = box_a;
    *out_box_b = box_b;
}

/** @brief Recolours a 30x30 corner box in place (same geometry, new colour). */
static void recolour(ERNode* box, int margin_left, int margin_top, uint32_t argb)
{
    ERProps props = props_default();
    props.width = 30;
    props.height = 30;
    props.margin_left = (int16_t)margin_left;
    props.margin_top = (int16_t)margin_top;
    props.background_color = argb;
    er_node_set_props(box, &props);
}

static bool point_in(const ERRect* rect, int x, int y)
{
    return x >= rect->x && x < rect->x + rect->width && y >= rect->y && y < rect->y + rect->height;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* The headline behaviour: two opposite-corner changes repaint two small areas, centre untouched. */
static int check_corner_isolation(void)
{
    ERNode *root, *box_a, *box_b;
    build_corner_scene(0xFFU, &root, &box_a, &box_b);
    frame(); /* mount: full repaint */
    frame(); /* settle */

    /* Recolour BOTH corners in one commit. */
    recolour(box_a, 10, 10, 0xFF22CC88U);
    recolour(box_b, 160, 120, 0xFF8822CCU);
    touched_reset();
    frame();

    /* The reported repaint: exactly two disjoint rects, one per corner. */
    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    printf("corner recolour: %d dirty rects\n", rect_count);
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
        printf("  rect[%d] = (%d,%d %dx%d)\n",
               rect_index,
               rects[rect_index].x,
               rects[rect_index].y,
               rects[rect_index].width,
               rects[rect_index].height);
    if (rect_count != 2)
        return fail("two far-apart changes did not report two dirty rects");
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
        for (int other_index = rect_index + 1; other_index < rect_count; other_index++)
            if (!(rects[rect_index].x + rects[rect_index].width < rects[other_index].x
                  || rects[other_index].x + rects[other_index].width < rects[rect_index].x
                  || rects[rect_index].y + rects[rect_index].height < rects[other_index].y
                  || rects[other_index].y + rects[other_index].height < rects[rect_index].y))
                return fail("reported rects overlap or abut");

    /* The rects must cover the two boxes... */
    bool box_a_covered = false, box_b_covered = false;
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
    {
        if (point_in(&rects[rect_index], 10, 10) && point_in(&rects[rect_index], 39, 39))
            box_a_covered = true;
        if (point_in(&rects[rect_index], 160, 160) && point_in(&rects[rect_index], 189, 189))
            box_b_covered = true;
    }
    if (!box_a_covered || !box_b_covered)
        return fail("the dirty rects do not cover the changed boxes");

    /* ...every touched pixel must lie inside a reported rect (the scissors really held)... */
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
        {
            if (!s_touched[y * SCREEN + x])
                continue;
            bool inside = false;
            for (int rect_index = 0; rect_index < rect_count; rect_index++)
                if (point_in(&rects[rect_index], x, y))
                    inside = true;
            if (!inside)
                return fail("an op touched pixels outside every reported dirty rect");
        }

    /* ...and in particular the screen centre — the old union box's territory — stays untouched. */
    for (int y = 60; y < 140; y++)
        for (int x = 60; x < 140; x++)
            if (s_touched[y * SCREEN + x])
                return fail("the centre band was repainted (union-box behaviour is back)");

    er_node_destroy(root);
    printf("PASS: corner isolation — 2 disjoint rects, centre untouched\n");
    return EXIT_SUCCESS;
}

/* Catch-all for multi-pass compositing bugs: the incremental multi-rect result must be
 * byte-identical to a forced full repaint — with TRANSLUCENT boxes, where a double blend or a
 * missed background repaint would change pixel values. */
static int check_pixel_equivalence(void)
{
    ERNode *root, *box_a, *box_b;
    build_corner_scene(0x80U, &root, &box_a, &box_b); /* translucent corners over the white root */
    frame();
    frame();

    /* Also park a translucent box NEAR box A (4 px gap: the 2 px pads make the damage rects touch,
     * so the set must coalesce them — stored-overlap would double-composite the seam). */
    ERNode* box_c = er_node_create(ER_NODE_VIEW);
    ERProps box_c_props = props_default();
    box_c_props.width = 30;
    box_c_props.height = 30;
    box_c_props.position = ER_POSITION_ABSOLUTE;
    box_c_props.left = 44;
    box_c_props.top = 10;
    box_c_props.background_color = 0x8000AA55U;
    er_node_set_props(box_c, &box_c_props);
    er_tree_append_child(root, box_c);
    frame(); /* paint C */
    frame(); /* settle */

    /* Recolour all three translucent boxes in one commit (A and C damage-merge; B stays separate). */
    recolour(box_a, 10, 10, 0x80CC8833U);
    box_c_props.background_color = 0x803355AAU;
    er_node_set_props(box_c, &box_c_props);
    recolour(box_b, 160, 120, 0x8044BB66U);
    frame();

    uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    /* Reference: force a full repaint of the identical scene and compare every pixel. */
    er_force_full_repaint();
    frame();
    if (memcmp(incremental, s_framebuffer, sizeof(incremental)) != 0)
        return fail("multi-rect incremental result differs from a full repaint (compositing bug)");

    er_node_destroy(root);
    printf("PASS: pixel equivalence — multi-rect passes match a full repaint exactly\n");
    return EXIT_SUCCESS;
}

/* Multi-buffer: the replay frame after a two-corner change must paint both corners into the stale
 * buffer — as two rects, still leaving the centre untouched. */
static int check_multibuffer_replay(void)
{
    er_set_display_buffer_count(2);

    ERNode *root, *box_a, *box_b;
    build_corner_scene(0xFFU, &root, &box_a, &box_b);
    frame(); /* fill buffer 0 */
    frame(); /* fill buffer 1 */
    frame(); /* settle */
    frame();

    recolour(box_a, 10, 10, 0xFFDD8800U);
    recolour(box_b, 160, 120, 0xFF0088DDU);
    frame(); /* change frame: paints into one buffer */

    /* Replay frame: the OTHER buffer owes both corners. */
    touched_reset();
    frame();
    bool box_a_touched = false, box_b_touched = false;
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
            if (s_touched[y * SCREEN + x])
            {
                if (x < 60 && y < 60)
                    box_a_touched = true;
                else if (x >= 140 && y >= 140)
                    box_b_touched = true;
            }
    if (!box_a_touched || !box_b_touched)
        return fail("the replay frame did not repaint both corners into the stale buffer");
    for (int y = 60; y < 140; y++)
        for (int x = 60; x < 140; x++)
            if (s_touched[y * SCREEN + x])
                return fail("the replay frame repainted the centre band (debt ballooned to a bbox)");

    /* Debt drained: the next frames paint nothing. */
    touched_reset();
    frame();
    frame();
    for (int pixel_index = 0; pixel_index < SCREEN * SCREEN; pixel_index++)
        if (s_touched[pixel_index])
            return fail("frames after the replay still repainted");

    er_node_destroy(root);
    er_set_display_buffer_count(1);
    printf("PASS: multi-buffer replay — both corners replayed as rects, centre untouched\n");
    return EXIT_SUCCESS;
}

/*
 * Scattered updates: a GRID of small widgets all changing in one commit — the case that saturated the
 * old 4-rect budget and cascade-merged into (near) the whole-grid bounding box. Each cell must keep
 * its own rect, the lanes between them must stay untouched, and the pixels must still match a full
 * repaint (translucent cells, so a double blend would show).
 */
#define GRID_COLUMNS 4
#define GRID_ROWS 3
#define GRID_CELL 20
static const int k_grid_x[GRID_COLUMNS] = {5, 55, 105, 155};
static const int k_grid_y[GRID_ROWS] = {5, 90, 175};

static int check_scattered_grid(void)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    ERNode* cell[GRID_ROWS * GRID_COLUMNS];
    for (int grid_row = 0; grid_row < GRID_ROWS; grid_row++)
        for (int grid_column = 0; grid_column < GRID_COLUMNS; grid_column++)
        {
            ERProps cell_node_props = props_default();
            cell_node_props.width = GRID_CELL;
            cell_node_props.height = GRID_CELL;
            cell_node_props.position = ER_POSITION_ABSOLUTE;
            cell_node_props.left = (float)k_grid_x[grid_column];
            cell_node_props.top = (float)k_grid_y[grid_row];
            cell_node_props.background_color = 0x80336699U;
            ERNode* cell_node = er_node_create(ER_NODE_VIEW);
            er_node_set_props(cell_node, &cell_node_props);
            er_tree_append_child(root, cell_node);
            cell[grid_row * GRID_COLUMNS + grid_column] = cell_node;
        }
    er_tree_set_root(root);
    frame(); /* mount: full repaint */
    frame(); /* settle */

    /* Recolour every cell in ONE commit — the scattered-update frame. */
    for (int grid_row = 0; grid_row < GRID_ROWS; grid_row++)
        for (int grid_column = 0; grid_column < GRID_COLUMNS; grid_column++)
        {
            ERProps cell_props = props_default();
            cell_props.width = GRID_CELL;
            cell_props.height = GRID_CELL;
            cell_props.position = ER_POSITION_ABSOLUTE;
            cell_props.left = (float)k_grid_x[grid_column];
            cell_props.top = (float)k_grid_y[grid_row];
            /* Every cell gets a colour distinct from the 0x80336699 it mounted with (a cell whose
             * props do not actually change is not dirty, and would contribute no rect). */
            const uint32_t cell_color =
                ((uint32_t)(grid_row * GRID_COLUMNS + grid_column) * 0x1F3A5CU + 0x040506U) & 0x00FFFFFFU;
            cell_props.background_color = 0x80000000U | cell_color;
            er_node_set_props(cell[grid_row * GRID_COLUMNS + grid_column], &cell_props);
        }
    touched_reset();
    frame();

    /* ER_DAMAGE_RECTS_MAX is a build-time budget that constrained targets legitimately lower (the CYD
     * and RP2040 examples pin it to 4). Below one rect per cell the engine is SUPPOSED to saturate and
     * merge, so only the tightness claims are gated on a full budget — every correctness check below
     * (coverage, containment, full-repaint equivalence) runs at any budget. */
    const int cells = GRID_ROWS * GRID_COLUMNS;
    const bool full_budget = (ER_DAMAGE_RECTS_MAX >= cells);

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int rect_count = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    printf("scattered grid: %d dirty rects for %d cells (budget %d)\n", rect_count, cells, ER_DAMAGE_RECTS_MAX);
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
        printf("  rect[%d] = (%d,%d %dx%d)\n",
               rect_index,
               rects[rect_index].x,
               rects[rect_index].y,
               rects[rect_index].width,
               rects[rect_index].height);
    if (full_budget)
    {
        if (rect_count != cells)
            return fail("the scattered grid did not report one rect per cell");
    }
    else if (rect_count < 1 || rect_count > ER_DAMAGE_RECTS_MAX)
    {
        /* Saturated: merging is expected, but never past the budget and never down to nothing. */
        return fail("the saturated grid reported a rect count outside the configured budget");
    }

    uint32_t area = 0U;
    for (int rect_index = 0; rect_index < rect_count; rect_index++)
        area += (uint32_t)rects[rect_index].width * (uint32_t)rects[rect_index].height;
    ERRect bbox;
    if (!er_get_dirty_rect(&bbox))
        return fail("no covering dirty rect after the grid update");
    const uint32_t bbox_area = (uint32_t)bbox.width * (uint32_t)bbox.height;
    printf("  damage %u px vs bbox %u px (%u%%)\n", area, bbox_area, 100U * area / bbox_area);
    /* The damage must stay a small fraction of the bounding box it used to collapse into — the whole
     * point of the budget, and therefore only claimable when the budget covers the cells. */
    if (full_budget && area * 4U >= bbox_area)
        return fail("the grid damage ballooned past a quarter of its bounding box");

    /* The lanes between the cells are clean pixels: nothing may write there. */
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
        {
            if (!s_touched[y * SCREEN + x])
                continue;
            bool inside = false;
            for (int rect_index = 0; rect_index < rect_count; rect_index++)
                if (point_in(&rects[rect_index], x, y))
                    inside = true;
            if (!inside)
                return fail("a grid op touched pixels outside every reported dirty rect");
        }
    if (full_budget && (s_touched[45 * SCREEN + 40] || s_touched[140 * SCREEN + 90]))
        return fail("the lanes between the grid cells were repainted (cascade-merge is back)");

    /* Twelve clipped passes must still composite exactly like one unclipped repaint. */
    uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    er_force_full_repaint();
    frame();
    if (memcmp(incremental, s_framebuffer, sizeof(incremental)) != 0)
        return fail("the grid's incremental result differs from a full repaint (compositing bug)");

    er_node_destroy(root);
    printf("PASS: scattered grid — %s, pixels match full repaint\n",
           full_budget ? "one rect per cell, lanes untouched" : "saturated within budget, coverage held");
    return EXIT_SUCCESS;
}

/* er_get_dirty_rects() contract edges: count query, bbox collapse on a too-small buffer. */
static int check_api_contract(void)
{
    ERNode *root, *box_a, *box_b;
    build_corner_scene(0xFFU, &root, &box_a, &box_b);
    frame();

    /* Mount was a full repaint: one root-sized rect. */
    ERRect single_rect;
    if (er_get_dirty_rects(&single_rect, 1) != 1 || single_rect.width != SCREEN || single_rect.height != SCREEN)
        return fail("a full repaint did not report one root-sized rect");

    /* A commit that paints nothing is non-destructive: the last commit that DID paint stays readable.
     * (Flow A commits inside er_runtime_pump(), so the host's own er_commit() is the no-op one — it
     * must not be the one that answers.) */
    frame();
    if (er_get_dirty_rects(&single_rect, 1) != 1 || single_rect.width != SCREEN || single_rect.height != SCREEN)
        return fail("a clean frame erased the last painting commit's rect");

    recolour(box_a, 10, 10, 0xFF112233U);
    recolour(box_b, 160, 120, 0xFF445566U);
    frame();
    if (er_get_dirty_rects(NULL, 0) != 2)
        return fail("NULL/0 did not query the rect count");

    frame(); /* still two: the no-op commit leaves the multi-rect set alone as well */
    if (er_get_dirty_rects(NULL, 0) != 2)
        return fail("a clean frame erased the last painting commit's rect set");

    /* Capacity 1 for 2 rects: collapse to the covering bbox (coverage must hold at any capacity). */
    if (er_get_dirty_rects(&single_rect, 1) != 1)
        return fail("a too-small buffer did not collapse to one rect");
    if (!point_in(&single_rect, 10, 10) || !point_in(&single_rect, 189, 189))
        return fail("the collapsed bbox does not cover both corners");

    /* A forced full repaint on an otherwise-settled scene (a host re-installing its framebuffer, say)
     * paints every pixel with NO node source-dirty, so nothing feeds the render walk's accumulator.
     * Both accessors must still report the truth — and the same truth. */
    er_force_full_repaint();
    frame();
    ERRect dirty_bounds;
    if (!er_get_dirty_rect(&dirty_bounds))
        return fail("a forced full repaint reported no dirty rect");
    if (dirty_bounds.width != SCREEN || dirty_bounds.height != SCREEN)
        return fail("a forced full repaint did not report the whole root as dirty");
    if (er_get_dirty_rects(&single_rect, 1) != 1 || single_rect.x != dirty_bounds.x || single_rect.y != dirty_bounds.y
        || single_rect.width != dirty_bounds.width || single_rect.height != dirty_bounds.height)
        return fail("er_get_dirty_rect() and er_get_dirty_rects() disagree on a forced full repaint");

    er_node_destroy(root);
    printf("PASS: er_get_dirty_rects contract — count query + bbox collapse\n");
    return EXIT_SUCCESS;
}

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {
        .fill_rect = fb_fill,
        .copy_rect = fb_copy,
        .blend_rect = fb_blend,
    };
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    touched_reset();
    embedded_renderer_set_backend(&k_backend);

    int result = check_corner_isolation();
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();

    result = check_pixel_equivalence();
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();

    result = check_multibuffer_replay();
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();

    result = check_api_contract();
    if (result != EXIT_SUCCESS)
        return result;
    er_reset();

    result = check_scattered_grid();
    if (result != EXIT_SUCCESS)
        return result;

    return EXIT_SUCCESS;
}
