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
 * display:none as a page cache — the LVGL screen model, where a page is built once and shown or
 * hidden forever after instead of being torn down and rebuilt in interpreted JS.
 *
 * That only works if hiding is honest in both directions, so this pins the whole contract:
 *
 *   - hiding erases the subtree's pixels, INCLUDING descendants that paint outside the hidden
 *     node's own box (an absolutely-positioned child of a small container is the ordinary shape of
 *     a page mounted under an anchor). The damage pre-pass measures each node on its own, and a
 *     hidden node's descendants stop being maintained by layout, so they read as unchanged and in
 *     place — without the vacate-on-hide bookkeeping their pixels stay on screen for good,
 *   - showing paints them back,
 *   - a hidden page costs NOTHING per frame: no pixels, and — the part that matters on a panel
 *     with a partial-update transfer — an empty reported dirty rect,
 *   - that holds even while React keeps re-rendering the hidden page and pushing props into it.
 *     A dirty flag on a node the paint walk prunes can never be cleared by painting, so without
 *     the sweep that retires it the page re-damages its own rect on every commit, forever,
 *   - the nodes stay allocated across the whole cycle — the point of hiding over unmounting,
 *   - and hiding is a subtree operation: a sibling page keeps rendering normally.
 */

#include "er_scene.h"
#include "native_renderer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 200
#define SH 200
#define SCREEN_PX ((long)SW * (long)SH)

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real, and records which pixels were written
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[SW * SH];
static uint8_t s_touched[SW * SH];

static uint32_t div255(uint32_t v)
{
    return (v + 127u) / 255u;
}

static void put(int x, int y, uint32_t sa, uint32_t sr, uint32_t sg, uint32_t sb)
{
    if (x < 0 || x >= SW || y < 0 || y >= SH)
        return;
    uint32_t* d = &s_fb[y * SW + x];
    const uint32_t inv = 255u - sa;
    const uint32_t dr = (*d >> 16) & 0xFFu, dg = (*d >> 8) & 0xFFu, db = *d & 0xFFu;
    *d = 0xFF000000u | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8) | (sb + div255(db * inv));
    s_touched[y * SW + x] = 1u;
}

static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFu;
    if (!a)
        return;
    const uint32_t sr = div255(((argb >> 16) & 0xFFu) * a);
    const uint32_t sg = div255(((argb >> 8) & 0xFFu) * a);
    const uint32_t sb = div255((argb & 0xFFu) * a);
    for (int r = y; r < y + h; r++)
        for (int q = x; q < x + w; q++)
            put(q, r, a, sr, sg, sb);
}

static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint8_t* p = src;
    for (int r = 0; r < h; r++, p += stride)
    {
        const uint32_t* row = (const uint32_t*)p;
        for (int q = 0; q < w; q++)
            put(x + q, y + r, 255u, (row[q] >> 16) & 0xFFu, (row[q] >> 8) & 0xFFu, row[q] & 0xFFu);
    }
}

static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint8_t* p = src;
    for (int r = 0; r < h; r++, p += stride)
    {
        const uint32_t* row = (const uint32_t*)p;
        for (int q = 0; q < w; q++)
        {
            const uint32_t v = row[q];
            uint32_t sa = (v >> 24) & 0xFFu, sr = (v >> 16) & 0xFFu, sg = (v >> 8) & 0xFFu, sb = v & 0xFFu;
            if (alpha < 255)
            {
                sa = div255(sa * alpha);
                sr = div255(sr * alpha);
                sg = div255(sg * alpha);
                sb = div255(sb * alpha);
            }
            put(x + q, y + r, sa, sr, sg, sb);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps p;
    er_props_default(&p);
    return p;
}

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/** @brief Commits one frame and returns the number of DISTINCT pixels written. */
static long frame_distinct(void)
{
    memset(s_touched, 0, sizeof(s_touched));
    er_commit();
    er_display_present();
    long n = 0;
    for (long i = 0; i < SCREEN_PX; i++)
        if (s_touched[i])
            n++;
    return n;
}

static uint32_t at(int x, int y)
{
    return s_fb[y * SW + x] & 0xFFFFFFu;
}

/** @brief Area of the dirty rect er_commit() reported, or 0 when it reported nothing. */
static long reported_dirty_area(void)
{
    ERRect d;
    if (!er_get_dirty_rect(&d))
        return 0;
    return (long)d.w * (long)d.h;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Scene
 ---------------------------------------------------------------------------------------------------------------------*/

/* The page's own box is a 1x1 anchor: its content is an absolutely-positioned child far outside it.
 * Measuring the page alone therefore misses every pixel it actually owns — which is the case the
 * hide/show bookkeeping has to get right. */
#define PAGE_X 10
#define PAGE_Y 10
#define CONTENT_X 40
#define CONTENT_Y 40
#define CONTENT_W 120
#define CONTENT_H 120
#define CONTENT_COLOR 0xFF00FF00u
#define BG_COLOR 0xFF000000u

/* A point on the page's content background, clear of the leaf widgets (which tile its top-left
 * corner), and one on the sibling page that stays visible throughout. */
#define IN_PAGE_X 140
#define IN_PAGE_Y 140
/* Inside the first leaf widget: content origin (PAGE + CONTENT) + the leaf's own (4,4) offset. */
#define IN_LEAF_X (PAGE_X + CONTENT_X + 6)
#define IN_LEAF_Y (PAGE_Y + CONTENT_Y + 6)
#define SIBLING_X 175
#define SIBLING_Y 175
#define SIBLING_W 20
#define SIBLING_H 20

/* Enough leaves that a per-frame cost would be unmistakable if the pruning regressed. */
#define LEAF_COUNT 32
/* Applied to the leaves while the page is hidden — a colour the page has never rendered. */
#define HIDDEN_LEAF_COLOR 0xFFEE00EEu

static ERNode* s_page;
static ERNode* s_leaves[LEAF_COUNT];
static ERProps s_page_props;
static ERProps s_leaf_props;
static ERNode* s_sibling;
static ERProps s_sibling_props;

static void build_scene(void)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SW;
    rp.height = SH;
    rp.background_color = BG_COLOR;
    er_node_set_props(root, &rp);
    er_tree_set_root(root);

    s_page = er_node_create(ER_NODE_VIEW);
    s_page_props = props_default();
    s_page_props.position = ER_POS_ABSOLUTE;
    s_page_props.left = PAGE_X;
    s_page_props.top = PAGE_Y;
    s_page_props.width = 1;
    s_page_props.height = 1;
    er_node_set_props(s_page, &s_page_props);
    er_tree_append_child(root, s_page);

    ERNode* content = er_node_create(ER_NODE_VIEW);
    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = CONTENT_X;
    cp.top = CONTENT_Y;
    cp.width = CONTENT_W;
    cp.height = CONTENT_H;
    cp.background_color = CONTENT_COLOR;
    er_node_set_props(content, &cp);
    er_tree_append_child(s_page, content);

    /* The page's own widgets — the "few hundred nodes" a cached page is made of, scaled down. */
    s_leaf_props = props_default();
    s_leaf_props.position = ER_POS_ABSOLUTE;
    s_leaf_props.width = 6;
    s_leaf_props.height = 6;
    s_leaf_props.background_color = 0xFF2255AAu;
    for (int i = 0; i < LEAF_COUNT; i++)
    {
        s_leaves[i] = er_node_create(ER_NODE_VIEW);
        ERProps lp = s_leaf_props;
        lp.left = (int16_t)(4 + (i % 8) * 14);
        lp.top = (int16_t)(4 + (i / 8) * 14);
        er_node_set_props(s_leaves[i], &lp);
        er_tree_append_child(content, s_leaves[i]);
    }

    /* A second page that is never hidden: hiding must be a SUBTREE operation, not a global one. */
    s_sibling = er_node_create(ER_NODE_VIEW);
    s_sibling_props = props_default();
    s_sibling_props.position = ER_POS_ABSOLUTE;
    s_sibling_props.left = SIBLING_X;
    s_sibling_props.top = SIBLING_Y;
    s_sibling_props.width = SIBLING_W;
    s_sibling_props.height = SIBLING_H;
    s_sibling_props.background_color = 0xFFFF8800u;
    er_node_set_props(s_sibling, &s_sibling_props);
    er_tree_append_child(root, s_sibling);
}

/** @brief Applies display:none / display:flex to the cached page. */
static void set_page_display(uint8_t display)
{
    s_page_props.display = display;
    er_node_set_props(s_page, &s_page_props);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    static EmbeddedRenderBackend be;
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    build_scene();

    /* Mount, then settle. The first frame legitimately paints everything. */
    if (frame_distinct() < SCREEN_PX / 2)
        return fail("mount frame did not paint the screen");
    frame_distinct();
    const int mounted_nodes = er_node_in_use_count();

    if (at(IN_PAGE_X, IN_PAGE_Y) != (CONTENT_COLOR & 0xFFFFFFu))
        return fail("page content did not paint on mount");

    /* --- hide: the subtree's pixels go, including the part outside the page's own box --- */
    set_page_display(ER_DISPLAY_NONE);
    if (frame_distinct() == 0)
        return fail("hiding a page painted nothing (its pixels were left on screen)");
    if (at(IN_PAGE_X, IN_PAGE_Y) != (BG_COLOR & 0xFFFFFFu))
        return fail("hiding a page left its content on screen (descendant outside the page's own box)");
    if (at(SIBLING_X + 1, SIBLING_Y + 1) != 0xFF8800u)
        return fail("hiding one page erased a sibling page");

    /* --- while hidden: free, and reported as such ---
     * A commit that paints nothing leaves the last painting commit's rect in place (the hide frame's,
     * here), so the invariant is that idle frames add no NEW damage: the reported rect never changes. */
    const long hidden_area = reported_dirty_area();
    for (int i = 0; i < 8; i++)
    {
        const long px = frame_distinct();
        if (px != 0)
        {
            fprintf(stderr, "  idle frame with a hidden page painted %ld px\n", px);
            return fail("a hidden page is not free per frame");
        }
        if (reported_dirty_area() != hidden_area)
            return fail("a hidden page reported new damage on an idle frame");
    }

    /* --- ...and still free while React keeps re-rendering it ---
     * A cached page stays mounted, so its components keep committing props into nodes the paint walk
     * prunes. Those dirty flags can never be cleared by painting; if they are not retired the page
     * damages its own rect on every commit for the rest of the run. */
    for (int i = 0; i < 8; i++)
    {
        for (int k = 0; k < LEAF_COUNT; k++)
        {
            ERProps lp = s_leaf_props;
            lp.left = (int16_t)(4 + (k % 8) * 14);
            lp.top = (int16_t)(4 + (k / 8) * 14);
            lp.background_color = (i & 1) ? 0xFF2255AAu : 0xFFAA5522u; /* a real prop change each pass */
            er_node_set_props(s_leaves[k], &lp);
        }
        const long px = frame_distinct();
        if (px != 0)
        {
            fprintf(stderr, "  frame painted %ld px while updating a hidden page\n", px);
            return fail("updating props inside a hidden page repainted the screen");
        }
        if (reported_dirty_area() != hidden_area)
            return fail("updating props inside a hidden page reported new damage");
    }

    /* One last prop set while hidden, with a value the page has never rendered: showing the page
     * must bring the CURRENT props, not the ones it went off screen with. */
    for (int k = 0; k < LEAF_COUNT; k++)
    {
        ERProps lp = s_leaf_props;
        lp.left = (int16_t)(4 + (k % 8) * 14);
        lp.top = (int16_t)(4 + (k / 8) * 14);
        lp.background_color = HIDDEN_LEAF_COLOR;
        er_node_set_props(s_leaves[k], &lp);
    }
    if (frame_distinct() != 0)
        return fail("a prop set inside a hidden page repainted the screen");

    /* --- the sibling page still updates normally, and stays local --- */
    s_sibling_props.background_color = 0xFF00AAFFu;
    er_node_set_props(s_sibling, &s_sibling_props);
    const long sib_px = frame_distinct();
    if (sib_px == 0)
        return fail("a visible sibling stopped repainting while another page was hidden");
    if (sib_px > (long)(SIBLING_W + 8) * (long)(SIBLING_H + 8))
    {
        fprintf(stderr, "  sibling update painted %ld px\n", sib_px);
        return fail("a sibling update was not local while another page was hidden");
    }
    frame_distinct();

    /* --- show: the page comes back, with the props it accumulated while hidden --- */
    set_page_display(ER_DISPLAY_FLEX);
    if (frame_distinct() == 0)
        return fail("showing a hidden page painted nothing");
    if (at(IN_PAGE_X, IN_PAGE_Y) != (CONTENT_COLOR & 0xFFFFFFu))
        return fail("showing a hidden page did not repaint its content");
    if (at(IN_LEAF_X, IN_LEAF_Y) != (HIDDEN_LEAF_COLOR & 0xFFFFFFu))
        return fail("a prop set while hidden was not applied when the page was shown");

    /* --- and settles back to idle --- */
    for (int i = 0; i < 4; i++)
    {
        if (frame_distinct() != 0)
            return fail("a re-shown page kept repainting on idle frames");
    }

    /* --- the nodes were never torn down: that is the whole point of hiding over unmounting --- */
    if (er_node_in_use_count() != mounted_nodes)
    {
        fprintf(stderr, "  %d nodes in use, expected %d\n", er_node_in_use_count(), mounted_nodes);
        return fail("hide/show did not preserve the page's native nodes");
    }

    /* --- repeat the cycle: no drift, no accumulating damage --- */
    for (int i = 0; i < 3; i++)
    {
        set_page_display(ER_DISPLAY_NONE);
        frame_distinct();
        if (at(IN_PAGE_X, IN_PAGE_Y) != (BG_COLOR & 0xFFFFFFu))
            return fail("a repeated hide left the page on screen");
        if (frame_distinct() != 0)
            return fail("a repeated hide did not settle to idle");

        set_page_display(ER_DISPLAY_FLEX);
        frame_distinct();
        if (at(IN_PAGE_X, IN_PAGE_Y) != (CONTENT_COLOR & 0xFFFFFFu))
            return fail("a repeated show did not repaint the page");
        if (frame_distinct() != 0)
            return fail("a repeated show did not settle to idle");
    }

    printf("PASS: display:none page cache (hide/show damage, idle cost, node retention)\n");
    return EXIT_SUCCESS;
}
