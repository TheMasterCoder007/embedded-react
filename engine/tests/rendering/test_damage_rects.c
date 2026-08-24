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

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: source-over framebuffer + per-pixel touch tracking
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[SCREEN * SCREEN];
static uint8_t s_touched[SCREEN * SCREEN]; /* pixels written by any op since touched_reset() */

static void touched_reset(void)
{
    memset(s_touched, 0, sizeof(s_touched));
}

static void mark_touched(int x, int y, int w, int h)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            if (row >= 0 && row < SCREEN && col >= 0 && col < SCREEN)
                s_touched[row * SCREEN + col] = 1U;
}

static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

/** @brief Straight-alpha source-over fill (mirrors backends/software), plus touch tracking. */
static void fb_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    mark_touched(x, y, w, h);
    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || col >= SCREEN)
                continue;
            uint32_t* d = &s_fb[row * SCREEN + col];
            if (a == 0xFFU)
            {
                *d = 0xFF000000U | (argb & 0x00FFFFFFU);
            }
            else
            {
                const uint32_t inv = 255U - a;
                const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
                const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;
                const uint32_t r = div255(sr * a) + div255(dr * inv);
                const uint32_t g = div255(sg * a) + div255(dg * inv);
                const uint32_t b = div255(sb * a) + div255(db * inv);
                *d = 0xFF000000U | (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void fb_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)ctx;
    mark_touched(x, y, w, h); /* content irrelevant to these scenes; the footprint is what matters */
}

static void fb_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)stride;
    (void)alpha;
    (void)ctx;
    mark_touched(x, y, w, h);
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
static void build_corner_scene(uint8_t alpha, ERNode** out_root, ERNode** out_a, ERNode** out_b)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    ERNode* a = er_node_create(ER_NODE_VIEW);
    ERProps ap = props_default();
    ap.width = 30;
    ap.height = 30;
    ap.margin_left = 10;
    ap.margin_top = 10;
    ap.background_color = ((uint32_t)alpha << 24) | 0x003366FFU;
    er_node_set_props(a, &ap);

    ERNode* b = er_node_create(ER_NODE_VIEW);
    ERProps bp = props_default();
    bp.width = 30;
    bp.height = 30;
    bp.margin_left = 160;
    bp.margin_top = 120; /* below A's bottom edge (y=40): 40+120 = 160 */
    bp.background_color = ((uint32_t)alpha << 24) | 0x00CC3344U;
    er_node_set_props(b, &bp);

    er_tree_append_child(root, a);
    er_tree_append_child(root, b);
    er_tree_set_root(root);
    *out_root = root;
    *out_a = a;
    *out_b = b;
}

/** @brief Recolours a 30x30 corner box in place (same geometry, new colour). */
static void recolour(ERNode* box, int margin_left, int margin_top, uint32_t argb)
{
    ERProps p = props_default();
    p.width = 30;
    p.height = 30;
    p.margin_left = (int16_t)margin_left;
    p.margin_top = (int16_t)margin_top;
    p.background_color = argb;
    er_node_set_props(box, &p);
}

static bool point_in(const ERRect* r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests
 ---------------------------------------------------------------------------------------------------------------------*/

/* The headline behaviour: two opposite-corner changes repaint two small areas, centre untouched. */
static int check_corner_isolation(void)
{
    ERNode *root, *a, *b;
    build_corner_scene(0xFFU, &root, &a, &b);
    frame(); /* mount: full repaint */
    frame(); /* settle */

    /* Recolour BOTH corners in one commit. */
    recolour(a, 10, 10, 0xFF22CC88U);
    recolour(b, 160, 120, 0xFF8822CCU);
    touched_reset();
    frame();

    /* The reported repaint: exactly two disjoint rects, one per corner. */
    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int n = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    printf("corner recolour: %d dirty rects\n", n);
    for (int i = 0; i < n; i++)
        printf("  rect[%d] = (%d,%d %dx%d)\n", i, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
    if (n != 2)
        return fail("two far-apart changes did not report two dirty rects");
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (!(rects[i].x + rects[i].w < rects[j].x || rects[j].x + rects[j].w < rects[i].x
                  || rects[i].y + rects[i].h < rects[j].y || rects[j].y + rects[j].h < rects[i].y))
                return fail("reported rects overlap or abut");

    /* The rects must cover the two boxes... */
    bool a_cov = false, b_cov = false;
    for (int i = 0; i < n; i++)
    {
        if (point_in(&rects[i], 10, 10) && point_in(&rects[i], 39, 39))
            a_cov = true;
        if (point_in(&rects[i], 160, 160) && point_in(&rects[i], 189, 189))
            b_cov = true;
    }
    if (!a_cov || !b_cov)
        return fail("the dirty rects do not cover the changed boxes");

    /* ...every touched pixel must lie inside a reported rect (the scissors really held)... */
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
        {
            if (!s_touched[y * SCREEN + x])
                continue;
            bool inside = false;
            for (int i = 0; i < n; i++)
                if (point_in(&rects[i], x, y))
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
    ERNode *root, *a, *b;
    build_corner_scene(0x80U, &root, &a, &b); /* translucent corners over the white root */
    frame();
    frame();

    /* Also park a translucent box NEAR box A (4 px gap: the 2 px pads make the damage rects touch,
     * so the set must coalesce them — stored-overlap would double-composite the seam). */
    ERNode* c = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.width = 30;
    cp.height = 30;
    cp.position = ER_POS_ABSOLUTE;
    cp.left = 44;
    cp.top = 10;
    cp.background_color = 0x8000AA55U;
    er_node_set_props(c, &cp);
    er_tree_append_child(root, c);
    frame(); /* paint C */
    frame(); /* settle */

    /* Recolour all three translucent boxes in one commit (A and C damage-merge; B stays separate). */
    recolour(a, 10, 10, 0x80CC8833U);
    cp.background_color = 0x803355AAU;
    er_node_set_props(c, &cp);
    recolour(b, 160, 120, 0x8044BB66U);
    frame();

    uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_fb, sizeof(incremental));

    /* Reference: force a full repaint of the identical scene and compare every pixel. */
    er_force_full_repaint();
    frame();
    if (memcmp(incremental, s_fb, sizeof(incremental)) != 0)
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

    ERNode *root, *a, *b;
    build_corner_scene(0xFFU, &root, &a, &b);
    frame(); /* fill buffer 0 */
    frame(); /* fill buffer 1 */
    frame(); /* settle */
    frame();

    recolour(a, 10, 10, 0xFFDD8800U);
    recolour(b, 160, 120, 0xFF0088DDU);
    frame(); /* change frame: paints into one buffer */

    /* Replay frame: the OTHER buffer owes both corners. */
    touched_reset();
    frame();
    bool a_touched = false, b_touched = false;
    for (int y = 0; y < SCREEN; y++)
        for (int x = 0; x < SCREEN; x++)
            if (s_touched[y * SCREEN + x])
            {
                if (x < 60 && y < 60)
                    a_touched = true;
                else if (x >= 140 && y >= 140)
                    b_touched = true;
            }
    if (!a_touched || !b_touched)
        return fail("the replay frame did not repaint both corners into the stale buffer");
    for (int y = 60; y < 140; y++)
        for (int x = 60; x < 140; x++)
            if (s_touched[y * SCREEN + x])
                return fail("the replay frame repainted the centre band (debt ballooned to a bbox)");

    /* Debt drained: the next frames paint nothing. */
    touched_reset();
    frame();
    frame();
    for (int i = 0; i < SCREEN * SCREEN; i++)
        if (s_touched[i])
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
#define GRID_COLS 4
#define GRID_ROWS 3
#define GRID_CELL 20
static const int k_grid_x[GRID_COLS] = {5, 55, 105, 155};
static const int k_grid_y[GRID_ROWS] = {5, 90, 175};

static int check_scattered_grid(void)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &rp);

    ERNode* cell[GRID_ROWS * GRID_COLS];
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
        {
            ERProps cp = props_default();
            cp.width = GRID_CELL;
            cp.height = GRID_CELL;
            cp.position = ER_POS_ABSOLUTE;
            cp.left = (float)k_grid_x[c];
            cp.top = (float)k_grid_y[r];
            cp.background_color = 0x80336699U;
            ERNode* n = er_node_create(ER_NODE_VIEW);
            er_node_set_props(n, &cp);
            er_tree_append_child(root, n);
            cell[r * GRID_COLS + c] = n;
        }
    er_tree_set_root(root);
    frame(); /* mount: full repaint */
    frame(); /* settle */

    /* Recolour every cell in ONE commit — the scattered-update frame. */
    for (int r = 0; r < GRID_ROWS; r++)
        for (int c = 0; c < GRID_COLS; c++)
        {
            ERProps cp = props_default();
            cp.width = GRID_CELL;
            cp.height = GRID_CELL;
            cp.position = ER_POS_ABSOLUTE;
            cp.left = (float)k_grid_x[c];
            cp.top = (float)k_grid_y[r];
            /* Every cell gets a colour distinct from the 0x80336699 it mounted with (a cell whose
             * props do not actually change is not dirty, and would contribute no rect). */
            const uint32_t rgb = ((uint32_t)(r * GRID_COLS + c) * 0x1F3A5CU + 0x040506U) & 0x00FFFFFFU;
            cp.background_color = 0x80000000U | rgb;
            er_node_set_props(cell[r * GRID_COLS + c], &cp);
        }
    touched_reset();
    frame();

    /* ER_DAMAGE_RECTS_MAX is a build-time budget that constrained targets legitimately lower (the CYD
     * and RP2040 examples pin it to 4). Below one rect per cell the engine is SUPPOSED to saturate and
     * merge, so only the tightness claims are gated on a full budget — every correctness check below
     * (coverage, containment, full-repaint equivalence) runs at any budget. */
    const int cells = GRID_ROWS * GRID_COLS;
    const bool full_budget = (ER_DAMAGE_RECTS_MAX >= cells);

    ERRect rects[ER_DAMAGE_RECTS_MAX];
    const int n = er_get_dirty_rects(rects, ER_DAMAGE_RECTS_MAX);
    printf("scattered grid: %d dirty rects for %d cells (budget %d)\n", n, cells, ER_DAMAGE_RECTS_MAX);
    for (int i = 0; i < n; i++)
        printf("  rect[%d] = (%d,%d %dx%d)\n", i, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
    if (full_budget)
    {
        if (n != cells)
            return fail("the scattered grid did not report one rect per cell");
    }
    else if (n < 1 || n > ER_DAMAGE_RECTS_MAX)
    {
        /* Saturated: merging is expected, but never past the budget and never down to nothing. */
        return fail("the saturated grid reported a rect count outside the configured budget");
    }

    uint32_t area = 0U;
    for (int i = 0; i < n; i++)
        area += (uint32_t)rects[i].w * (uint32_t)rects[i].h;
    ERRect bbox;
    if (!er_get_dirty_rect(&bbox))
        return fail("no covering dirty rect after the grid update");
    const uint32_t bbox_area = (uint32_t)bbox.w * (uint32_t)bbox.h;
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
            for (int i = 0; i < n; i++)
                if (point_in(&rects[i], x, y))
                    inside = true;
            if (!inside)
                return fail("a grid op touched pixels outside every reported dirty rect");
        }
    if (full_budget && (s_touched[45 * SCREEN + 40] || s_touched[140 * SCREEN + 90]))
        return fail("the lanes between the grid cells were repainted (cascade-merge is back)");

    /* Twelve clipped passes must still composite exactly like one unclipped repaint. */
    uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_fb, sizeof(incremental));
    er_force_full_repaint();
    frame();
    if (memcmp(incremental, s_fb, sizeof(incremental)) != 0)
        return fail("the grid's incremental result differs from a full repaint (compositing bug)");

    er_node_destroy(root);
    printf("PASS: scattered grid — %s, pixels match full repaint\n",
           full_budget ? "one rect per cell, lanes untouched" : "saturated within budget, coverage held");
    return EXIT_SUCCESS;
}

/* er_get_dirty_rects() contract edges: count query, bbox collapse on a too-small buffer. */
static int check_api_contract(void)
{
    ERNode *root, *a, *b;
    build_corner_scene(0xFFU, &root, &a, &b);
    frame();

    /* Mount was a full repaint: one root-sized rect. */
    ERRect r;
    if (er_get_dirty_rects(&r, 1) != 1 || r.w != SCREEN || r.h != SCREEN)
        return fail("a full repaint did not report one root-sized rect");

    /* A commit that paints nothing is non-destructive: the last commit that DID paint stays readable.
     * (Flow A commits inside er_runtime_pump(), so the host's own er_commit() is the no-op one — it
     * must not be the one that answers.) */
    frame();
    if (er_get_dirty_rects(&r, 1) != 1 || r.w != SCREEN || r.h != SCREEN)
        return fail("a clean frame erased the last painting commit's rect");

    recolour(a, 10, 10, 0xFF112233U);
    recolour(b, 160, 120, 0xFF445566U);
    frame();
    if (er_get_dirty_rects(NULL, 0) != 2)
        return fail("NULL/0 did not query the rect count");

    frame(); /* still two: the no-op commit leaves the multi-rect set alone as well */
    if (er_get_dirty_rects(NULL, 0) != 2)
        return fail("a clean frame erased the last painting commit's rect set");

    /* Capacity 1 for 2 rects: collapse to the covering bbox (coverage must hold at any capacity). */
    if (er_get_dirty_rects(&r, 1) != 1)
        return fail("a too-small buffer did not collapse to one rect");
    if (!point_in(&r, 10, 10) || !point_in(&r, 189, 189))
        return fail("the collapsed bbox does not cover both corners");

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
    memset(s_fb, 0, sizeof(s_fb));
    touched_reset();
    embedded_renderer_set_backend(&k_backend);

    int rc = check_corner_isolation();
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();

    rc = check_pixel_equivalence();
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();

    rc = check_multibuffer_replay();
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();

    rc = check_api_contract();
    if (rc != EXIT_SUCCESS)
        return rc;
    er_reset();

    rc = check_scattered_grid();
    if (rc != EXIT_SUCCESS)
        return rc;

    return EXIT_SUCCESS;
}
