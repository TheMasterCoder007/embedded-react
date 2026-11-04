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

#define SCREEN_WIDTH 200
#define SCREEN_HEIGHT 200
#define SCREEN_PIXELS ((long)SCREEN_WIDTH * (long)SCREEN_HEIGHT)
#define ALPHA_OPAQUE 255
#define CHANNEL_MAX 255u
#define CHANNEL_ROUND 127u

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real, and records which pixels were written
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint8_t s_touched[SCREEN_WIDTH * SCREEN_HEIGHT];

static uint32_t div255(uint32_t value)
{
    return (value + CHANNEL_ROUND) / CHANNEL_MAX;
}

static void put(int x, int y, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
        return;
    uint32_t* destination = &s_framebuffer[y * SCREEN_WIDTH + x];
    const uint32_t inverse_alpha = CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (*destination >> 16) & 0xFFu, destination_green = (*destination >> 8) & 0xFFu,
                   destination_blue = *destination & 0xFFu;
    *destination = 0xFF000000u | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                   | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                   | (source_blue + div255(destination_blue * inverse_alpha));
    s_touched[y * SCREEN_WIDTH + x] = 1u;
}

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFu;
    if (!alpha)
        return;
    const uint32_t source_red = div255(((argb >> 16) & 0xFFu) * alpha);
    const uint32_t source_green = div255(((argb >> 8) & 0xFFu) * alpha);
    const uint32_t source_blue = div255((argb & 0xFFu) * alpha);
    for (int pixel_y = y; pixel_y < y + height; pixel_y++)
        for (int pixel_x = x; pixel_x < x + width; pixel_x++)
            put(pixel_x, pixel_y, alpha, source_red, source_green, source_blue);
}

/*
 * copy_rect hands over a PREMULTIPLIED ARGB8888 buffer, not an opaque one — the engine has a separate
 * copy_rect_fmt for the known-opaque case. So the per-pixel alpha is honoured here, and a fully
 * transparent pixel is skipped outright rather than "blended" at alpha 0: put() would leave the colour
 * alone but still count the pixel as touched, and this test is built entirely on touched-pixel counts
 * ("a hidden page is free per frame"). The page is solid opaque Views today, so neither this nor
 * blend_cb is reached at all — the handling is here so that stays true if anyone gives the page an
 * image, text or an opacity group.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
        {
            const uint32_t value = row[column];
            const uint32_t source_alpha = (value >> 24) & 0xFFu;
            if (source_alpha == 0u)
                continue;
            put(x + column, y + row_index, source_alpha, (value >> 16) & 0xFFu, (value >> 8) & 0xFFu, value & 0xFFu);
        }
    }
}

static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint8_t* source_bytes = source;
    for (int row_index = 0; row_index < height; row_index++, source_bytes += stride)
    {
        const uint32_t* row = (const uint32_t*)source_bytes;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = row[column];
            uint32_t source_alpha = (source_pixel >> 24) & 0xFFu, source_red = (source_pixel >> 16) & 0xFFu,
                     source_green = (source_pixel >> 8) & 0xFFu, source_blue = source_pixel & 0xFFu;
            if (alpha < ALPHA_OPAQUE)
            {
                source_alpha = div255(source_alpha * alpha);
                source_red = div255(source_red * alpha);
                source_green = div255(source_green * alpha);
                source_blue = div255(source_blue * alpha);
            }
            if (source_alpha == 0u)
                continue; /* scaled away to nothing: not a write, so not a touched pixel either */
            put(x + column, y + row_index, source_alpha, source_red, source_green, source_blue);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static ERProps props_default(void)
{
    ERProps props;
    er_props_default(&props);
    return props;
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/** @brief Commits one frame and returns the number of DISTINCT pixels written. */
static long frame_distinct(void)
{
    memset(s_touched, 0, sizeof(s_touched));
    er_commit();
    er_display_present();
    long distinct_count = 0;
    for (long pixel_index = 0; pixel_index < SCREEN_PIXELS; pixel_index++)
        if (s_touched[pixel_index])
            distinct_count++;
    return distinct_count;
}

static uint32_t at(int x, int y)
{
    return s_framebuffer[y * SCREEN_WIDTH + x] & 0xFFFFFFu;
}

/** @brief Area of the dirty rect er_commit() reported, or 0 when it reported nothing. */
static long reported_dirty_area(void)
{
    ERRect dirty;
    if (!er_get_dirty_rect(&dirty))
        return 0;
    return (long)dirty.w * (long)dirty.h;
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
#define CONTENT_WIDTH 120
#define CONTENT_HEIGHT 120
#define CONTENT_COLOR 0xFF00FF00u
#define BACKGROUND_COLOR 0xFF000000u

/* A point on the page's content background, clear of the leaf widgets (which tile its top-left
 * corner), and one on the sibling page that stays visible throughout. */
#define IN_PAGE_X 140
#define IN_PAGE_Y 140
/* Inside the first leaf widget: content origin (PAGE + CONTENT) + the leaf's own (4,4) offset. */
#define IN_LEAF_X (PAGE_X + CONTENT_X + 6)
#define IN_LEAF_Y (PAGE_Y + CONTENT_Y + 6)
#define SIBLING_X 175
#define SIBLING_Y 175
#define SIBLING_WIDTH 20
#define SIBLING_HEIGHT 20

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
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN_WIDTH;
    root_props.height = SCREEN_HEIGHT;
    root_props.background_color = BACKGROUND_COLOR;
    er_node_set_props(root, &root_props);
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
    ERProps content_props = props_default();
    content_props.position = ER_POS_ABSOLUTE;
    content_props.left = CONTENT_X;
    content_props.top = CONTENT_Y;
    content_props.width = CONTENT_WIDTH;
    content_props.height = CONTENT_HEIGHT;
    content_props.background_color = CONTENT_COLOR;
    er_node_set_props(content, &content_props);
    er_tree_append_child(s_page, content);

    /* The page's own widgets — the "few hundred nodes" a cached page is made of, scaled down. */
    s_leaf_props = props_default();
    s_leaf_props.position = ER_POS_ABSOLUTE;
    s_leaf_props.width = 6;
    s_leaf_props.height = 6;
    s_leaf_props.background_color = 0xFF2255AAu;
    for (int leaf_index = 0; leaf_index < LEAF_COUNT; leaf_index++)
    {
        s_leaves[leaf_index] = er_node_create(ER_NODE_VIEW);
        ERProps leaf_props = s_leaf_props;
        leaf_props.left = (int16_t)(4 + (leaf_index % 8) * 14);
        leaf_props.top = (int16_t)(4 + (leaf_index / 8) * 14);
        er_node_set_props(s_leaves[leaf_index], &leaf_props);
        er_tree_append_child(content, s_leaves[leaf_index]);
    }

    /* A second page that is never hidden: hiding must be a SUBTREE operation, not a global one. */
    s_sibling = er_node_create(ER_NODE_VIEW);
    s_sibling_props = props_default();
    s_sibling_props.position = ER_POS_ABSOLUTE;
    s_sibling_props.left = SIBLING_X;
    s_sibling_props.top = SIBLING_Y;
    s_sibling_props.width = SIBLING_WIDTH;
    s_sibling_props.height = SIBLING_HEIGHT;
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
    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    embedded_renderer_set_backend(&backend);

    build_scene();

    /* Mount, then settle. The first frame legitimately paints everything. */
    if (frame_distinct() < SCREEN_PIXELS / 2)
        return fail("mount frame did not paint the screen");
    frame_distinct();
    const int mounted_nodes = er_node_in_use_count();

    if (at(IN_PAGE_X, IN_PAGE_Y) != (CONTENT_COLOR & 0xFFFFFFu))
        return fail("page content did not paint on mount");

    /* --- hide: the subtree's pixels go, including the part outside the page's own box --- */
    set_page_display(ER_DISPLAY_NONE);
    if (frame_distinct() == 0)
        return fail("hiding a page painted nothing (its pixels were left on screen)");
    if (at(IN_PAGE_X, IN_PAGE_Y) != (BACKGROUND_COLOR & 0xFFFFFFu))
        return fail("hiding a page left its content on screen (descendant outside the page's own box)");
    if (at(SIBLING_X + 1, SIBLING_Y + 1) != 0xFF8800u)
        return fail("hiding one page erased a sibling page");

    /* --- while hidden: free, and reported as such ---
     * A commit that paints nothing leaves the last painting commit's rect in place (the hide frame's,
     * here), so the invariant is that idle frames add no NEW damage: the reported rect never changes. */
    const long hidden_area = reported_dirty_area();
    for (int frame_index = 0; frame_index < 8; frame_index++)
    {
        const long painted = frame_distinct();
        if (painted != 0)
        {
            fprintf(stderr, "  idle frame with a hidden page painted %ld px\n", painted);
            return fail("a hidden page is not free per frame");
        }
        if (reported_dirty_area() != hidden_area)
            return fail("a hidden page reported new damage on an idle frame");
    }

    /* --- ...and still free while React keeps re-rendering it ---
     * A cached page stays mounted, so its components keep committing props into nodes the paint walk
     * prunes. Those dirty flags can never be cleared by painting; if they are not retired the page
     * damages its own rect on every commit for the rest of the run. */
    for (int frame_index = 0; frame_index < 8; frame_index++)
    {
        for (int leaf_index = 0; leaf_index < LEAF_COUNT; leaf_index++)
        {
            ERProps leaf_props = s_leaf_props;
            leaf_props.left = (int16_t)(4 + (leaf_index % 8) * 14);
            leaf_props.top = (int16_t)(4 + (leaf_index / 8) * 14);
            leaf_props.background_color =
                (frame_index & 1) ? 0xFF2255AAu : 0xFFAA5522u; /* a real prop change each pass */
            er_node_set_props(s_leaves[leaf_index], &leaf_props);
        }
        const long painted = frame_distinct();
        if (painted != 0)
        {
            fprintf(stderr, "  frame painted %ld px while updating a hidden page\n", painted);
            return fail("updating props inside a hidden page repainted the screen");
        }
        if (reported_dirty_area() != hidden_area)
            return fail("updating props inside a hidden page reported new damage");
    }

    /* One last prop set while hidden, with a value the page has never rendered: showing the page
     * must bring the CURRENT props, not the ones it went off screen with. */
    for (int leaf_index = 0; leaf_index < LEAF_COUNT; leaf_index++)
    {
        ERProps leaf_props = s_leaf_props;
        leaf_props.left = (int16_t)(4 + (leaf_index % 8) * 14);
        leaf_props.top = (int16_t)(4 + (leaf_index / 8) * 14);
        leaf_props.background_color = HIDDEN_LEAF_COLOR;
        er_node_set_props(s_leaves[leaf_index], &leaf_props);
    }
    if (frame_distinct() != 0)
        return fail("a prop set inside a hidden page repainted the screen");

    /* --- the sibling page still updates normally, and stays local --- */
    s_sibling_props.background_color = 0xFF00AAFFu;
    er_node_set_props(s_sibling, &s_sibling_props);
    const long sibling_painted = frame_distinct();
    if (sibling_painted == 0)
        return fail("a visible sibling stopped repainting while another page was hidden");
    if (sibling_painted > (long)(SIBLING_WIDTH + 8) * (long)(SIBLING_HEIGHT + 8))
    {
        fprintf(stderr, "  sibling update painted %ld px\n", sibling_painted);
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
    for (int frame_index = 0; frame_index < 4; frame_index++)
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
    for (int frame_index = 0; frame_index < 3; frame_index++)
    {
        set_page_display(ER_DISPLAY_NONE);
        frame_distinct();
        if (at(IN_PAGE_X, IN_PAGE_Y) != (BACKGROUND_COLOR & 0xFFFFFFu))
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
