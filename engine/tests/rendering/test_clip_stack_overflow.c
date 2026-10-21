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
 * Clip pushes past a full clip stack stay balanced with their pops (issue #371).
 *
 * The clip stack holds a fixed number of entries. A push at a full stack used to be dropped while
 * its matching pop still popped, so that pop removed the clip of an ancestor still being painted,
 * and everything that ancestor painted afterwards escaped its clip.
 *
 * Two checks:
 *
 *   - the blit-layer API directly: push well past the stack's capacity, pop back down, and the
 *     outer clips must come back exactly as they were pushed,
 *   - the scene the issue reports: a clipping view holding a chain of nested clipping views, then a
 *     sibling that overflows it. At every chain depth the sibling must be cut off at the outer box.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_push_clip_rect / er_pop_clip_rect / er_get_clip_rect */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 200
#define SH 200

/*----------------------------------------------------------------------------------------------------------------------
 - Backend: composites for real
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_fb[SW * SH];

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
        {
            const uint32_t v = row[q];
            const uint32_t sa = (v >> 24) & 0xFFu;
            if (sa == 0u)
                continue;
            put(x + q, y + r, sa, (v >> 16) & 0xFFu, (v >> 8) & 0xFFu, v & 0xFFu);
        }
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
            if (sa == 0u)
                continue;
            put(x + q, y + r, sa, sr, sg, sb);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/* Far more pushes than the stack holds, so the overflow path is exercised many times over. */
#define DEEP_PUSHES 20

/* The outer clipping view's box, and a sibling inside it that hangs past its bottom-right corner. */
#define OUTER_W 100
#define OUTER_H 100
#define SIB_X 50
#define SIB_Y 50
#define SIB_W 100
#define SIB_H 100

/* A point on the sibling's overflow: outside the outer box, inside the sibling. */
#define OVER_X 120
#define OVER_Y 120
/* And one on the part of the sibling inside the outer box. */
#define INSIDE_X 70
#define INSIDE_Y 70

#define BG_COLOR 0xFF202020u
#define SIB_COLOR 0xFF4DA3FFu

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

static uint32_t at(int x, int y)
{
    return s_fb[y * SW + x] & 0xFFFFFFu;
}

/** @brief True when the top clip is exactly (x, y, w, h). */
static int clip_is(int x, int y, int w, int h)
{
    int cx, cy, cw, ch;
    return er_get_clip_rect(&cx, &cy, &cw, &ch) && cx == x && cy == y && cw == w && ch == h;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Direct API
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Pushes far past the stack's capacity and checks every pop restores the clip pushed before it.
 *
 * Push i is the rect (i, i, 200 - 2i, 200 - 2i), each strictly inside the last. Pushes past the
 * capacity are not applied (the top stays the deepest entry that fit), so the test does not assume
 * what the top is after each push — it records it, and on the way down every pop must bring back
 * exactly the clip that was on top before the matching push.
 */
static int check_api(void)
{
    if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        return fail("api: the clip stack is not empty between frames");

    int top[DEEP_PUSHES][4];
    for (int i = 0; i < DEEP_PUSHES; i++)
    {
        er_push_clip_rect(i, i, SW - 2 * i, SH - 2 * i);
        if (!er_get_clip_rect(&top[i][0], &top[i][1], &top[i][2], &top[i][3]))
            return fail("api: no clip is active after a push");
    }

    /* The deepest pushes cannot all have been applied, or the stack is not overflowing at all. */
    if (top[DEEP_PUSHES - 1][0] == DEEP_PUSHES - 1)
        return fail("api: every push was applied (the stack never filled; test is not testing anything)");

    /* A reset pushed onto the full stack must be balanced the same way. */
    er_push_clip_reset();
    er_pop_clip_rect();

    for (int i = DEEP_PUSHES - 1; i >= 0; i--)
    {
        if (!clip_is(top[i][0], top[i][1], top[i][2], top[i][3]))
        {
            int cx = 0, cy = 0, cw = 0, ch = 0;
            const bool any = er_get_clip_rect(&cx, &cy, &cw, &ch);
            fprintf(stderr,
                    "  api: back at depth %d the top clip is %s(%d,%d %dx%d), was (%d,%d %dx%d)\n",
                    i + 1,
                    any ? "" : "none ",
                    cx,
                    cy,
                    cw,
                    ch,
                    top[i][0],
                    top[i][1],
                    top[i][2],
                    top[i][3]);
            return fail("api: a pop removed a clip that belongs to a push still in force");
        }
        er_pop_clip_rect();
    }

    if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        return fail("api: balanced pushes and pops did not leave the stack empty");

    /* The pops that fit must still restore exactly: push 3, pop 1, the second push is on top. */
    er_push_clip_rect(0, 0, 100, 100);
    er_push_clip_rect(10, 10, 50, 50);
    er_push_clip_rect(20, 20, 10, 10);
    er_pop_clip_rect();
    if (!clip_is(10, 10, 50, 50))
        return fail("api: a pop within capacity did not restore the previous clip");
    er_pop_clip_rect();
    er_pop_clip_rect();

    /* An unmatched pop on an empty stack stays harmless, and leaves no debt for the next push. */
    er_pop_clip_rect();
    er_push_clip_rect(5, 6, 7, 8);
    if (!clip_is(5, 6, 7, 8))
        return fail("api: a push after an unmatched pop was not applied");
    er_pop_clip_rect();
    if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        return fail("api: the stack is not empty after the last pop");

    return 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Scene
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Builds the issue's scene: an outer clipping view holding @p depth nested clipping views of
 *        the same size, then a sibling that overflows the outer view.
 */
static void build_scene(int depth)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SW;
    rp.height = SH;
    rp.background_color = BG_COLOR;
    er_node_set_props(root, &rp);
    er_tree_set_root(root);

    ERProps cp = props_default();
    cp.position = ER_POS_ABSOLUTE;
    cp.left = 0;
    cp.top = 0;
    cp.width = OUTER_W;
    cp.height = OUTER_H;
    cp.overflow = ER_OVERFLOW_HIDDEN;

    ERNode* outer = er_node_create(ER_NODE_VIEW);
    er_node_set_props(outer, &cp);
    er_tree_append_child(root, outer);

    ERNode* parent = outer;
    for (int i = 0; i < depth; i++)
    {
        ERNode* n = er_node_create(ER_NODE_VIEW);
        er_node_set_props(n, &cp);
        er_tree_append_child(parent, n);
        parent = n;
    }

    ERNode* sib = er_node_create(ER_NODE_VIEW);
    ERProps sp = props_default();
    sp.position = ER_POS_ABSOLUTE;
    sp.left = SIB_X;
    sp.top = SIB_Y;
    sp.width = SIB_W;
    sp.height = SIB_H;
    sp.background_color = SIB_COLOR;
    er_node_set_props(sib, &sp);
    er_tree_append_child(outer, sib);
}

static int check_scene(void)
{
    for (int depth = 0; depth <= DEEP_PUSHES; depth++)
    {
        build_scene(depth);
        memset(s_fb, 0, sizeof(s_fb));
        er_force_full_repaint();
        er_commit();
        er_display_present();

        if (at(INSIDE_X, INSIDE_Y) != (SIB_COLOR & 0xFFFFFFu))
        {
            fprintf(stderr, "  scene: nested chain depth %d\n", depth);
            return fail("scene: the sibling was not drawn inside the outer view");
        }
        if (at(OVER_X, OVER_Y) != (BG_COLOR & 0xFFFFFFu))
        {
            fprintf(stderr, "  scene: nested chain depth %d, pixel=%06x\n", depth, at(OVER_X, OVER_Y));
            return fail("scene: the sibling was drawn past the outer view's clip");
        }
        if (er_get_clip_rect(NULL, NULL, NULL, NULL))
        {
            fprintf(stderr, "  scene: nested chain depth %d\n", depth);
            return fail("scene: the commit left the clip stack unbalanced");
        }
    }
    return 0;
}

int main(void)
{
    static EmbeddedRenderBackend be;
    be.fill_rect = fill_cb;
    be.copy_rect = copy_cb;
    be.blend_rect = blend_cb;
    embedded_renderer_set_backend(&be);

    if (check_api() != 0)
        return EXIT_FAILURE;
    if (check_scene() != 0)
        return EXIT_FAILURE;

    printf("PASS: clip stack overflow (balanced pushes and pops past capacity, outer clips kept)\n");
    return EXIT_SUCCESS;
}
