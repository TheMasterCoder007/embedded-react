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
 * A translate moves the node's whole subtree: its children are drawn and damage-tracked at the offset,
 * and each incremental frame matches a full repaint.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define HEADER_H 128
#define ITEM_W 200
#define ITEM_H 80

#define WHITE 0x00FFFFFFU
#define BLUE 0x004DA3FFU
#define GREEN 0x003DDC84U

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

/** @brief Backend blend_rect: source-over of premultiplied pixels scaled by a group alpha (the opacity composite). */
static void fb_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* sp = (const uint32_t*)((const uint8_t*)src + (size_t)row * (size_t)stride);
        for (int col = 0; col < w; col++)
        {
            const int dx = x + col, dy = y + row;
            if (dx < 0 || dx >= SCREEN || dy < 0 || dy >= SCREEN)
                continue;
            const uint32_t p = sp[col];
            const uint32_t sa = div255(((p >> 24) & 0xFFU) * (uint32_t)alpha);
            if (sa == 0U)
                continue;
            const uint32_t inv = 255U - sa;
            const uint32_t sr = div255(((p >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sg = div255(((p >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t sb = div255((p & 0xFFU) * (uint32_t)alpha);
            uint32_t* d = &s_fb[dy * SCREEN + dx];
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8)
                 | (sb + div255(db * inv));
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

static int fail(const char* msg, int lift)
{
    fprintf(stderr, "FAIL (lift %d): %s\n", lift, msg);
    return EXIT_FAILURE;
}

static uint32_t px_at(int x, int y)
{
    return s_fb[y * SCREEN + x] & 0x00FFFFFFU;
}

/** @brief The header's props: absolute across the top of the screen, slid by a translate. */
static ERProps header_props(float tx, float ty, uint8_t opacity)
{
    ERProps p = props_default();
    p.position = ER_POS_ABSOLUTE;
    p.top = 0;
    p.left = 0;
    p.right = 0;
    p.height = HEADER_H;
    p.background_color = 0xFF000000U | BLUE;
    p.opacity = opacity;
    p.transform_translate_x = tx;
    p.transform_translate_y = ty;
    return p;
}

/**
 * @brief root(white) → header(blue, 128 px, translated) → item(green 200x80).
 *
 * With @p nested the item holds a second translated node, so offsets have to accumulate.
 */
static void build(ERNode** out_header, ERNode** out_inner, uint8_t opacity, bool nested)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFF000000U | WHITE;
    er_node_set_props(root, &rp);

    ERNode* header = er_node_create(ER_NODE_VIEW);
    ERProps hp = header_props(0.0f, 0.0f, opacity);
    er_node_set_props(header, &hp);

    ERNode* item = er_node_create(ER_NODE_VIEW);
    ERProps ip = props_default();
    ip.width = ITEM_W;
    ip.height = ITEM_H;
    ip.background_color = 0xFF000000U | GREEN;
    er_node_set_props(item, &ip);

    er_tree_append_child(header, item);
    if (nested)
    {
        /* A 20x20 red dot at the item's top-left, itself offset by (30, 10). */
        ERNode* dot = er_node_create(ER_NODE_VIEW);
        ERProps dp = props_default();
        dp.width = 20;
        dp.height = 20;
        dp.background_color = 0xFFFF0000U;
        dp.transform_translate_x = 30.0f;
        dp.transform_translate_y = 10.0f;
        er_node_set_props(dot, &dp);
        er_tree_append_child(item, dot);
        *out_inner = dot;
    }
    er_tree_append_child(root, header);
    er_tree_set_root(root);
    *out_header = header;
}

/** @brief Renders a settled starting frame (full repaint, then one idle commit). */
static void settle(void)
{
    er_force_full_repaint();
    er_commit();
    s_fill_count = 0;
    er_commit();
}

/** @brief Compares the current frame to a full repaint of the same scene, leaving the full repaint on screen. */
static bool matches_full_repaint(void)
{
    static uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_fb, sizeof(incremental));
    memset(s_fb, 0, sizeof(s_fb));
    er_force_full_repaint();
    er_commit();
    return memcmp(incremental, s_fb, sizeof(incremental)) == 0;
}

/* An opaque header lifted by a translate takes its item with it. */
static int check_lift(int lift)
{
    ERNode* header = NULL;
    ERNode* unused = NULL;
    build(&header, &unused, 255U, false);
    settle();
    if (px_at(10, 10) != GREEN || px_at(10, 100) != BLUE || px_at(10, 150) != WHITE)
        return fail("the settled scene is not header-with-item", lift);

    ERProps hp = header_props(0.0f, (float)-lift, 255U);
    er_node_set_props(header, &hp);
    er_commit();

    /* Every row on screen is the item, the header, or vacated background — where the lifted layout puts it. */
    for (int y = 0; y < SCREEN; y++)
    {
        const int ly = y + lift; /* row of the untranslated header this screen row now shows */
        const uint32_t want = (ly < ITEM_H) ? GREEN : (ly < HEADER_H) ? BLUE : WHITE;
        if (px_at(10, y) != want)
        {
            if (want == WHITE && px_at(10, y) == GREEN)
                return fail("the item was left behind at its untranslated position", lift);
            return fail("the lifted header and item are not where the translate puts them", lift);
        }
    }

    s_fill_count = 0;
    er_commit();
    if (s_fill_count != 0)
        return fail("the idle commit after the lift repainted something", lift);

    if (!matches_full_repaint())
        return fail("the incremental lift differs from a full repaint", lift);
    return EXIT_SUCCESS;
}

/* A translucent header composites its subtree through scratch; the item has to move inside it too. */
static int check_translucent_lift(void)
{
    const int lift = 64;
    ERNode* header = NULL;
    ERNode* unused = NULL;
    build(&header, &unused, 160U, false);
    settle();
    const uint32_t item_tint = px_at(10, 10);
    const uint32_t header_tint = px_at(10, 100);
    if (item_tint == header_tint || header_tint == WHITE)
        return fail("the settled translucent scene does not tell the item from the header", lift);

    ERProps hp = header_props(0.0f, (float)-lift, 160U);
    er_node_set_props(header, &hp);
    er_commit();

    if (px_at(10, 10) != item_tint)
        return fail("the translucent header's item is not under the lifted header", lift);
    if (px_at(10, ITEM_H - lift + 4) != header_tint)
        return fail("the translucent header's item was drawn below the lifted header's top", lift);
    if (px_at(10, ITEM_H + 4) != WHITE && px_at(10, ITEM_H + 4) != header_tint)
        return fail("the translucent header's item left a trail", lift);
    if (px_at(10, HEADER_H - lift + 4) != WHITE)
        return fail("the translucent header left a trail", lift);

    s_fill_count = 0;
    er_commit();
    if (s_fill_count != 0)
        return fail("the idle commit after a translucent lift repainted something", lift);

    if (!matches_full_repaint())
        return fail("the incremental translucent lift differs from a full repaint", lift);
    return EXIT_SUCCESS;
}

/* Offsets accumulate: a translated dot inside a translated header moves by both, on both axes. */
static int check_nested_translate(void)
{
    ERNode* header = NULL;
    ERNode* dot = NULL;
    build(&header, &dot, 255U, true);
    settle();
    if (px_at(30 + 10, 10 + 10) != 0x00FF0000U)
        return fail("the settled dot is not at its own translate", 0);

    ERProps hp = header_props(50.0f, 40.0f, 255U);
    er_node_set_props(header, &hp);
    er_commit();

    /* Dot box (0,0,20,20) + its own (30,10) + the header's (50,40) = (80,50,20,20). */
    if (px_at(80 + 10, 50 + 10) != 0x00FF0000U)
        return fail("the nested dot did not move by both translates", 40);
    if (px_at(30 + 10, 10 + 10) != WHITE)
        return fail("the nested dot left a trail at its old position", 40);
    if (px_at(50 + 10, 40 + 10) != GREEN)
        return fail("the item did not move with the header on both axes", 40);

    s_fill_count = 0;
    er_commit();
    if (s_fill_count != 0)
        return fail("the idle commit after a nested translate repainted something", 40);

    if (!matches_full_repaint())
        return fail("the incremental nested translate differs from a full repaint", 40);
    return EXIT_SUCCESS;
}

/* Moving only the inner node: the header's translate still has to be counted when measuring it. */
static int check_inner_move_under_translate(void)
{
    ERNode* header = NULL;
    ERNode* dot = NULL;
    build(&header, &dot, 255U, true);
    ERProps hp = header_props(50.0f, 40.0f, 255U);
    er_node_set_props(header, &hp);
    settle();

    ERProps dp = props_default();
    dp.width = 20;
    dp.height = 20;
    dp.background_color = 0xFFFF0000U;
    dp.transform_translate_x = 100.0f;
    dp.transform_translate_y = 10.0f;
    er_node_set_props(dot, &dp);
    er_commit();

    if (px_at(150 + 10, 50 + 10) != 0x00FF0000U)
        return fail("the inner dot is not at both translates after its own move", 40);
    if (px_at(80 + 10, 50 + 10) != GREEN)
        return fail("the inner dot left a trail inside the translated header", 40);

    s_fill_count = 0;
    er_commit();
    if (s_fill_count != 0)
        return fail("the idle commit after an inner move repainted something", 40);

    if (!matches_full_repaint())
        return fail("the incremental inner move differs from a full repaint", 40);
    return EXIT_SUCCESS;
}

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {.fill_rect = fb_fill, .blend_rect = fb_blend};
    embedded_renderer_set_backend(&k_backend);

    static const int k_lifts[] = {64, HEADER_H, 400};
    for (size_t i = 0; i < sizeof(k_lifts) / sizeof(k_lifts[0]); i++)
    {
        memset(s_fb, 0, sizeof(s_fb));
        const int rc = check_lift(k_lifts[i]);
        if (rc != EXIT_SUCCESS)
            return rc;
        er_reset();
    }

    int (*const cases[])(void) = {
        check_translucent_lift,
        check_nested_translate,
        check_inner_move_under_translate,
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
