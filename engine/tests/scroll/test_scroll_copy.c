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
 * Scroll by copy: a ScrollView offset change moves the painted viewport through the backend's move_rect
 * and repaints only the exposed strip. Every check compares the framebuffer against a full repaint of the
 * same scene, pixel for pixel.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W 160
#define FB_H 160
#define ROWS 30

typedef struct
{
    uint32_t fb[FB_W * FB_H];
    int moves;
    long painted_px; /* pixels written by fill/copy/blend since the last reset */
} TestCtx;

static TestCtx g_ctx;
static EmbeddedRenderBackend g_be;
static int g_failures = 0;

/** @brief Backend fill_rect: writes or source-over blends a straight-alpha colour; counts painted pixels. */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    const uint32_t a = argb >> 24;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || row < 0 || col >= FB_W || row >= FB_H)
                continue;
            uint32_t* d = &t->fb[row * FB_W + col];
            if (a == 255U)
                *d = argb;
            else if (a != 0U)
            {
                uint32_t out = 0xFF000000U;
                for (int sh = 0; sh < 24; sh += 8)
                {
                    const uint32_t s = (argb >> sh) & 0xFFU, dd = (*d >> sh) & 0xFFU;
                    out |= ((s * a + dd * (255U - a)) / 255U) << sh;
                }
                *d = out;
            }
            t->painted_px++;
        }
}

/** @brief Backend copy_rect: writes every non-transparent source pixel; counts painted pixels. */
static void copy_cb(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            const uint32_t p = ((const uint32_t*)((const uint8_t*)src + (size_t)row * stride))[col];
            if (x + col >= 0 && y + row >= 0 && x + col < FB_W && y + row < FB_H && (p >> 24) != 0U)
                t->fb[(y + row) * FB_W + x + col] = p;
            t->painted_px++;
        }
}

/** @brief Backend blend_rect: the same as copy_cb (the scene's translucent sources are fills). */
static void blend_cb(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)alpha;
    copy_cb(src, stride, x, y, w, h, ctx);
}

/** @brief Backend move_rect: copies the rect through a temporary buffer, so overlap is safe; counts moves. */
static void move_cb(int sx, int sy, int w, int h, int dx, int dy, void* ctx)
{
    TestCtx* t = ctx;
    t->moves++;
    static uint32_t tmp[FB_W * FB_H];
    for (int row = 0; row < h; row++)
        memcpy(&tmp[row * w], &t->fb[(sy + row) * FB_W + sx], (size_t)w * 4U);
    for (int row = 0; row < h; row++)
        memcpy(&t->fb[(dy + row) * FB_W + dx], &tmp[row * w], (size_t)w * 4U);
}

/**
 * @brief Installs the test backend, with or without move_rect.
 *
 * @param[in] with_move  Whether to provide move_rect.
 */
static void install(bool with_move)
{
    memset(&g_be, 0, sizeof(g_be));
    g_be.fill_rect = fill_cb;
    g_be.copy_rect = copy_cb;
    g_be.blend_rect = blend_cb;
    g_be.move_rect = with_move ? move_cb : NULL;
    g_be.ctx = &g_ctx;
    embedded_renderer_set_backend(&g_be); /* also forces the next commit to repaint in full */
}

/**
 * @brief Creates a sized View with a background colour and appends it to @p parent (when given).
 *
 * @return The new node.
 */
static ERNode* view(ERNode* parent, int w, int h, uint32_t bg)
{
    ERNode* n = er_node_create(ER_NODE_VIEW);
    ERProps p;
    er_props_default(&p);
    p.width = (int16_t)w;
    p.height = (int16_t)h;
    p.background_color = bg;
    er_node_set_props(n, &p);
    if (parent)
        er_tree_append_child(parent, n);
    return n;
}

/**
 * @brief Compares the framebuffer after a scroll with a full repaint of the same scene, pixel for pixel.
 *
 * @param[in] what       Name of the check, for the failure message.
 * @param[in] with_move  Whether the reference backend provides move_rect.
 */
static void check(const char* what, bool with_move)
{
    static uint32_t after_scroll[FB_W * FB_H];
    memcpy(after_scroll, g_ctx.fb, sizeof(after_scroll));
    memset(g_ctx.fb, 0, sizeof(g_ctx.fb));
    install(with_move);
    er_commit(); /* full repaint: the reference */
    int diffs = 0, first = -1;
    for (int i = 0; i < FB_W * FB_H; i++)
        if (after_scroll[i] != g_ctx.fb[i] && diffs++ == 0)
            first = i;
    if (diffs)
    {
        fprintf(stderr,
                "FAIL %s: %d pixels differ from a full repaint, first at (%d,%d) %08x vs %08x\n",
                what,
                diffs,
                first % FB_W,
                first / FB_W,
                after_scroll[first],
                g_ctx.fb[first]);
        g_failures++;
    }
}

/**
 * @brief Runs the scroll-by-copy checks.
 *
 * @return EXIT_SUCCESS when every check passes.
 */
int main(void)
{
    install(true);
    er_reset();

    /* An opaque root, a translucent overlay above the viewport's top edge and a ScrollView of striped rows,
     * some with inner boxes. */
    ERNode* root = view(NULL, FB_W, FB_H, 0xFF102030U);
    ERProps sp;
    er_props_default(&sp);
    sp.position = ER_POS_ABSOLUTE;
    sp.left = 20;
    sp.top = 30;
    sp.width = 110;
    sp.height = 100;
    ERNode* sv = er_node_create(ER_NODE_SCROLL_VIEW);
    er_node_set_props(sv, &sp);
    er_tree_append_child(root, sv);
    ERNode* content = view(sv, 110, ROWS * 12, 0);
    ERNode* rows[ROWS];
    for (int i = 0; i < ROWS; i++)
    {
        rows[i] = view(content, 110, 12, 0xFF000000U | (uint32_t)(i * 2654435761U >> 8));
        if (i % 3 == 0)
            view(rows[i], 30 + i, 6, 0x80FFFFFFU);
    }
    ERNode* overlay = er_node_create(ER_NODE_VIEW);
    ERProps op;
    er_props_default(&op);
    op.position = ER_POS_ABSOLUTE;
    op.left = 60;
    op.top = 22;
    op.width = 40;
    op.height = 16;
    op.background_color = 0xC0FF8000U;
    er_node_set_props(overlay, &op);
    er_tree_append_child(root, overlay);
    er_tree_set_root(root);
    er_commit();

    /* Single steps down and up, each against a full repaint. A step of a viewport height or more has
     * nothing to keep and repaints in full. */
    const float steps[] = {5.0f, 17.0f, 40.5f, 39.0f, 0.0f, 99.0f, 250.0f};
    int painted_at = 0;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++)
    {
        const int delta = abs((int)steps[i] - painted_at);
        painted_at = (int)steps[i];
        g_ctx.moves = 0;
        g_ctx.painted_px = 0;
        er_scroll_view_set_offset(sv, 0.0f, steps[i]);
        er_commit();
        char what[64];
        snprintf(what, sizeof what, "step to %.1f", (double)steps[i]);
        if (g_ctx.moves != (delta < 100 ? 1 : 0))
        {
            fprintf(stderr, "FAIL %s: expected %d move(s), got %d\n", what, delta < 100 ? 1 : 0, g_ctx.moves);
            g_failures++;
        }
        check(what, true);
    }

    /* A chain of copies with no full repaint in between, then one comparison. */
    for (int i = 1; i <= 12; i++)
    {
        er_scroll_view_set_offset(sv, 0.0f, (float)(i * 7));
        er_commit();
    }
    check("chain of 12 copies", true);

    /* A copy must not cost a viewport repaint: 7 px of scroll repaints a strip, not 110 x 100. */
    g_ctx.painted_px = 0;
    er_scroll_view_set_offset(sv, 0.0f, 91.0f);
    er_commit();
    if (g_ctx.painted_px > 110 * 40)
    {
        fprintf(stderr, "FAIL strip only: %ld pixels painted for a 7 px scroll\n", g_ctx.painted_px);
        g_failures++;
    }
    check("strip only", true);

    /* The overlay and a row change in the same commit as the scroll. */
    op.background_color = 0xC00080FFU;
    er_node_set_props(overlay, &op);
    ERProps rp;
    er_props_default(&rp);
    rp.width = 110;
    rp.height = 12;
    rp.background_color = 0xFFFFFFFFU;
    er_node_set_props(rows[10], &rp);
    er_scroll_view_set_offset(sv, 0.0f, 80.0f);
    er_commit();
    check("scroll with overlay and row changes", true);

    /* A bordered ScrollView paints its border inside the viewport: no copy, still exact. */
    sp.border_width = 2;
    sp.border_color = 0xFFFF0000U;
    er_node_set_props(sv, &sp);
    er_commit();
    g_ctx.moves = 0;
    er_scroll_view_set_offset(sv, 0.0f, 60.0f);
    er_commit();
    if (g_ctx.moves != 0)
    {
        fprintf(stderr, "FAIL bordered: expected no move, got %d\n", g_ctx.moves);
        g_failures++;
    }
    check("bordered fallback", true);

    /* Without move_rect the viewport repaints as before. */
    install(false);
    er_commit();
    er_scroll_view_set_offset(sv, 0.0f, 20.0f);
    er_commit();
    check("no move_rect", false);

    /* More ScrollViews scrolled in one commit than the copy tracks: the extra ones repaint in full, and a
     * later copy of any of them starts from the offset that repaint showed. */
    install(true);
    er_reset();
    ERNode* grid = view(NULL, FB_W, FB_H, 0xFF102030U);
    ERProps gp;
    er_props_default(&gp);
    gp.flex_direction = ER_FLEX_ROW;
    gp.width = FB_W;
    gp.height = FB_H;
    gp.background_color = 0xFF102030U;
    er_node_set_props(grid, &gp);
    ERNode* many[10];
    for (int k = 0; k < 10; k++)
    {
        ERProps mp;
        er_props_default(&mp);
        mp.width = 14;
        mp.height = 80;
        mp.margin_left = 2;
        many[k] = er_node_create(ER_NODE_SCROLL_VIEW);
        er_node_set_props(many[k], &mp);
        er_tree_append_child(grid, many[k]);
        ERNode* col = view(many[k], 14, 20 * 8, 0);
        for (int i = 0; i < 8; i++)
            view(col, 14, 20, 0xFF000000U | (uint32_t)((k * 8 + i) * 2654435761U >> 8));
    }
    er_tree_set_root(grid);
    er_commit();
    for (int k = 0; k < 10; k++)
        er_scroll_view_set_offset(many[k], 0.0f, 30.0f);
    er_commit();
    check("ten ScrollViews in one commit", true);
    g_ctx.moves = 0;
    g_ctx.painted_px = 0;
    er_scroll_view_set_offset(many[9], 0.0f, 36.0f);
    er_commit();
    /* 6 px of a 14 px wide viewport; moving from a stale offset would misplace every row's footprint and
     * repaint the whole 14 x 80 viewport. */
    if (g_ctx.moves != 1 || g_ctx.painted_px > 14 * 20)
    {
        fprintf(stderr,
                "FAIL untracked ScrollView: expected 1 move and a strip, got %d move(s), %ld pixels\n",
                g_ctx.moves,
                g_ctx.painted_px);
        g_failures++;
    }
    check("copy of a ScrollView that was repainted untracked", true);

    /* A container that paints nothing, whose box overlaps the viewport's top rows (a section header with a
     * margin, say): it has no pixels there, so a copy repaints the exposed strip and not its box. */
    install(true);
    er_reset();
    ERNode* page = view(NULL, FB_W, FB_H, 0xFF102030U);
    ERProps hp;
    er_props_default(&hp);
    hp.position = ER_POS_ABSOLUTE;
    hp.width = FB_W;
    hp.height = 40;
    ERNode* header = er_node_create(ER_NODE_VIEW);
    er_node_set_props(header, &hp);
    er_tree_append_child(page, header);
    view(header, 20, 8, 0xFFFFFFFFU);
    sp.border_width = 0;
    ERNode* list = er_node_create(ER_NODE_SCROLL_VIEW);
    er_node_set_props(list, &sp);
    er_tree_append_child(page, list);
    ERNode* items = view(list, 110, ROWS * 12, 0);
    for (int i = 0; i < ROWS; i++)
        view(items, 110, 12, 0xFF000000U | (uint32_t)(i * 2246822519U >> 8));
    er_tree_set_root(page);
    er_commit();
    g_ctx.moves = 0;
    g_ctx.painted_px = 0;
    er_scroll_view_set_offset(list, 0.0f, 6.0f);
    er_commit();
    /* The exposed 6 px strip with its damage margins stays under 110 x 22 pixels; the header's 10
     * overlapping rows, repainted where they are and where the move carried them, would more than double
     * that. */
    if (g_ctx.moves != 1 || g_ctx.painted_px > 110 * 22)
    {
        fprintf(stderr,
                "FAIL paint-free container: expected 1 move and a strip, got %d move(s), %ld pixels\n",
                g_ctx.moves,
                g_ctx.painted_px);
        g_failures++;
    }
    check("copy beside a paint-free container", true);

    if (g_failures)
        return EXIT_FAILURE;
    printf("scroll copy: all checks passed\n");
    return EXIT_SUCCESS;
}
