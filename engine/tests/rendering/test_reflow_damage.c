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
#define BAR_H 60
#define KID 20

static uint32_t s_fb[SCREEN * SCREEN];
static int s_fill_count;

static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

static void fb_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    s_fill_count++;
    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || col >= SCREEN)
                continue;
            uint32_t* d = &s_fb[row * SCREEN + col];
            const uint32_t inv = 255U - a;
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;
            *d = 0xFF000000U | ((div255(sr * a) + div255(dr * inv)) << 16) | ((div255(sg * a) + div255(dg * inv)) << 8)
                 | (div255(sb * a) + div255(db * inv));
        }
    }
}

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

/** @brief The bar's props at a given left padding — the one thing the tests vary. */
static ERProps bar_props(int16_t padding_left, uint32_t bg)
{
    ERProps p = props_default();
    p.width = SCREEN;
    p.height = BAR_H;
    p.flex_direction = ER_FLEX_ROW;
    p.padding_left = padding_left;
    p.background_color = bg;
    return p;
}

/** @brief root(white, 200x200) → bar(grey, full-width strip) → kid(orange 20x20). */
static void build(ERNode** out_bar)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bp = bar_props(0, 0xFF303030U);
    er_node_set_props(bar, &bp);

    ERNode* kid = er_node_create(ER_NODE_VIEW);
    ERProps kp = props_default();
    kp.width = KID;
    kp.height = KID;
    kp.background_color = 0xFFFF8800U;
    er_node_set_props(kid, &kp);

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
    ERProps bp = bar_props(0, 0xFF303030U);
    bp.max_width = SCREEN + 100;
    er_node_set_props(bar, &bp);

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

    ERProps bp = bar_props(40, 0xFF303030U);
    er_node_set_props(bar, &bp);
    er_commit();

    ERRect r;
    if (!er_get_dirty_rect(&r))
        return fail("moving a child reported no damage at all");
    /* Both footprints, or the contract is broken in the other direction: the reported rect has to
     * COVER what was repainted, and a move repaints the trail as well as the destination. Old spot
     * is x[0,20), new spot x[40,60). */
    if (r.x > 0 || r.x + r.w < 40 + KID)
        return fail("reported damage does not cover both the old and new position of a moved child");
    /* ...and no wider than that, or nothing was saved over damaging the whole 200px bar. */
    if (r.w > 100)
        return fail("a moved child damaged the whole re-measured container");
    if (r.h > BAR_H + 8)
        return fail("damage spilled outside the bar's own strip");
    return EXIT_SUCCESS;
}

/* The split must not swallow real appearance changes: a recolour still damages the full box. */
static int check_visual_change_still_damages(void)
{
    ERNode* bar = NULL;
    build(&bar);
    settle();

    ERProps bp = bar_props(0, 0xFF00AA00U);
    er_node_set_props(bar, &bp);
    er_commit();

    ERRect r;
    if (!er_get_dirty_rect(&r))
        return fail("a recolour reported no damage");
    if (r.w < SCREEN || r.h < BAR_H)
        return fail("a recolour damaged less than the node's box");
    if ((s_fb[10 * SCREEN + 150] & 0x00FFFFFFU) != 0x0000AA00U)
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

    ERProps bp = bar_props(40, 0xFF303030U);
    er_node_set_props(bar, &bp);
    er_commit();
    bp.max_width = SCREEN + 100; /* a second, movement-free reflow on top */
    er_node_set_props(bar, &bp);
    er_commit();
    memcpy(reference, s_fb, sizeof(reference));

    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();

    if (memcmp(reference, s_fb, sizeof(reference)) != 0)
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

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        memset(s_fb, 0, sizeof(s_fb));
        s_fill_count = 0;
        const int rc = cases[i]();
        if (rc != EXIT_SUCCESS)
            return rc;
        er_reset();
    }
    return EXIT_SUCCESS;
}
