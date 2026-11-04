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
#define HEADER_HEIGHT 128
#define ITEM_WIDTH 200
#define ITEM_HEIGHT 80
#define OPACITY_OPAQUE 255U

#define WHITE 0x00FFFFFFU
#define BLUE 0x004DA3FFU
#define GREEN 0x003DDC84U

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
            const uint32_t inverse = ER_CHANNEL_MAX - alpha;
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU,
                           source_blue = argb & 0xFFU;
            *destination = 0xFF000000U | ((div255(source_red * alpha) + div255(destination_red * inverse)) << 16)
                           | ((div255(source_green * alpha) + div255(destination_green * inverse)) << 8)
                           | (div255(source_blue * alpha) + div255(destination_blue * inverse));
        }
    }
}

/** @brief Backend blend_rect: source-over of premultiplied pixels scaled by a group alpha (the opacity composite). */
static void fb_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
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
            const uint32_t pixel = source_row[column];
            const uint32_t source_alpha = div255(((pixel >> 24) & 0xFFU) * (uint32_t)alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t inverse = ER_CHANNEL_MAX - source_alpha;
            const uint32_t source_red = div255(((pixel >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_green = div255(((pixel >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_blue = div255((pixel & 0xFFU) * (uint32_t)alpha);
            uint32_t* destination = &s_framebuffer[destination_y * SCREEN + destination_x];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((source_red + div255(destination_red * inverse)) << 16)
                           | ((source_green + div255(destination_green * inverse)) << 8)
                           | (source_blue + div255(destination_blue * inverse));
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

static int fail(const char* message, int lift)
{
    fprintf(stderr, "FAIL (lift %d): %s\n", lift, message);
    return EXIT_FAILURE;
}

static uint32_t px_at(int x, int y)
{
    return s_framebuffer[y * SCREEN + x] & 0x00FFFFFFU;
}

/** @brief The header's props: absolute across the top of the screen, slid by a translate. */
static ERProps header_props(float translate_x, float translate_y, uint8_t opacity)
{
    ERProps props = props_default();
    props.position = ER_POS_ABSOLUTE;
    props.top = 0;
    props.left = 0;
    props.right = 0;
    props.height = HEADER_HEIGHT;
    props.background_color = 0xFF000000U | BLUE;
    props.opacity = opacity;
    props.transform_translate_x = translate_x;
    props.transform_translate_y = translate_y;
    return props;
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
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFF000000U | WHITE;
    er_node_set_props(root, &root_props);

    ERNode* header = er_node_create(ER_NODE_VIEW);
    ERProps header_node_props = header_props(0.0f, 0.0f, opacity);
    er_node_set_props(header, &header_node_props);

    ERNode* item = er_node_create(ER_NODE_VIEW);
    ERProps item_props = props_default();
    item_props.width = ITEM_WIDTH;
    item_props.height = ITEM_HEIGHT;
    item_props.background_color = 0xFF000000U | GREEN;
    er_node_set_props(item, &item_props);

    er_tree_append_child(header, item);
    if (nested)
    {
        /* A 20x20 red dot at the item's top-left, itself offset by (30, 10). */
        ERNode* dot = er_node_create(ER_NODE_VIEW);
        ERProps dot_props = props_default();
        dot_props.width = 20;
        dot_props.height = 20;
        dot_props.background_color = 0xFFFF0000U;
        dot_props.transform_translate_x = 30.0f;
        dot_props.transform_translate_y = 10.0f;
        er_node_set_props(dot, &dot_props);
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
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    er_force_full_repaint();
    er_commit();
    return memcmp(incremental, s_framebuffer, sizeof(incremental)) == 0;
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

    ERProps header_node_props = header_props(0.0f, (float)-lift, 255U);
    er_node_set_props(header, &header_node_props);
    er_commit();

    /* Every row on screen is the item, the header, or vacated background — where the lifted layout puts it. */
    for (int y = 0; y < SCREEN; y++)
    {
        const int header_row = y + lift; /* row of the untranslated header this screen row now shows */
        const uint32_t want = (header_row < ITEM_HEIGHT) ? GREEN : (header_row < HEADER_HEIGHT) ? BLUE : WHITE;
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

    ERProps header_node_props = header_props(0.0f, (float)-lift, 160U);
    er_node_set_props(header, &header_node_props);
    er_commit();

    if (px_at(10, 10) != item_tint)
        return fail("the translucent header's item is not under the lifted header", lift);
    if (px_at(10, ITEM_HEIGHT - lift + 4) != header_tint)
        return fail("the translucent header's item was drawn below the lifted header's top", lift);
    if (px_at(10, ITEM_HEIGHT + 4) != WHITE && px_at(10, ITEM_HEIGHT + 4) != header_tint)
        return fail("the translucent header's item left a trail", lift);
    if (px_at(10, HEADER_HEIGHT - lift + 4) != WHITE)
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

    ERProps header_node_props = header_props(50.0f, 40.0f, 255U);
    er_node_set_props(header, &header_node_props);
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
    ERProps header_node_props = header_props(50.0f, 40.0f, 255U);
    er_node_set_props(header, &header_node_props);
    settle();

    ERProps dot_props = props_default();
    dot_props.width = 20;
    dot_props.height = 20;
    dot_props.background_color = 0xFFFF0000U;
    dot_props.transform_translate_x = 100.0f;
    dot_props.transform_translate_y = 10.0f;
    er_node_set_props(dot, &dot_props);
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

    static const int k_lifts[] = {64, HEADER_HEIGHT, 400};
    for (size_t lift_index = 0; lift_index < sizeof(k_lifts) / sizeof(k_lifts[0]); lift_index++)
    {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        const int result = check_lift(k_lifts[lift_index]);
        if (result != EXIT_SUCCESS)
            return result;
        er_reset();
    }

    int (*const cases[])(void) = {
        check_translucent_lift,
        check_nested_translate,
        check_inner_move_under_translate,
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
