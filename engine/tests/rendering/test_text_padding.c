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
 * Padding on a <Text> has to move the GLYPHS, not just the box. The layout suite proves the box grows
 * (tests/layout/test_layout.c); this proves the compositor spends that space, by watching where the
 * glyph blits actually land.
 *
 * Every case gives the node an EXPLICIT width/height, so the node box is identical across cases and
 * only the padding differs — the shift in the drawn bounding box is then the padding and nothing else.
 * The scene carries no background fill, so every draw call the backend sees is a glyph.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 320
#define FB_H 240

/* The text node's box. Wide enough that "Save" plus the largest padding still fits on one line. */
#define BOX_X 40
#define BOX_Y 30
#define BOX_W 160
#define BOX_H 60

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Bounding box of everything the backend was asked to draw in one commit. */
typedef struct
{
    int ops;
    int min_x, min_y, max_x, max_y;
} DrawBox;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static DrawBox s_box;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Resets the recorded draw box to its empty sentinel state.
 */
static void box_reset(void)
{
    s_box.ops = 0;
    s_box.min_x = FB_W;
    s_box.min_y = FB_H;
    s_box.max_x = 0;
    s_box.max_y = 0;
}

/**
 * @brief Folds one drawn rectangle into the recorded bounding box.
 *
 * @param[in] x  Left edge.
 * @param[in] y  Top edge.
 * @param[in] w  Width.
 * @param[in] h  Height.
 */
static void box_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    s_box.ops++;
    if (x < s_box.min_x)
        s_box.min_x = x;
    if (y < s_box.min_y)
        s_box.min_y = y;
    if (x + w > s_box.max_x)
        s_box.max_x = x + w;
    if (y + h > s_box.max_y)
        s_box.max_y = y + h;
}

/**
 * @brief Backend fill_rect that records the drawn rectangle.
 *
 * @param[in] argb  Fill color (unused).
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] w     Width.
 * @param[in] h     Height.
 * @param[in] ctx   Unused.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)ctx;
    box_add(x, y, w, h);
}

/**
 * @brief Backend copy_rect that records the drawn rectangle.
 *
 * @param[in] src  Source buffer (unused).
 * @param[in] s    Row stride (unused).
 * @param[in] x    Left edge.
 * @param[in] y    Top edge.
 * @param[in] w    Width.
 * @param[in] h    Height.
 * @param[in] ctx  Unused.
 */
static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)ctx;
    box_add(x, y, w, h);
}

/**
 * @brief Backend blend_rect that records the drawn rectangle.
 *
 * @param[in] src  Source buffer (unused).
 * @param[in] s    Row stride (unused).
 * @param[in] a    Global alpha (unused).
 * @param[in] x    Left edge.
 * @param[in] y    Top edge.
 * @param[in] w    Width.
 * @param[in] h    Height.
 * @param[in] ctx  Unused.
 */
static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)a;
    (void)ctx;
    box_add(x, y, w, h);
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/**
 * @brief Builds an ERProps with every layout field set to ER_LAYOUT_AUTO.
 *
 * Zero-init alone is not enough: min/max width/height default to 0, which is a hard cap.
 *
 * @return Default-initialised ERProps, opaque.
 */
static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.padding_horizontal = p.padding_vertical = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.margin_horizontal = p.margin_vertical = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/**
 * @brief Renders "Save" in a fixed-size absolutely-placed Text node and records where it drew.
 *
 * @param[in]  pad_left  paddingLeft in pixels, or ER_LAYOUT_AUTO for none.
 * @param[in]  pad_top   paddingTop in pixels, or ER_LAYOUT_AUTO for none.
 * @param[out] out       Receives the bounding box of every draw call the commit made.
 */
static void render_padded_text(int16_t pad_left, int16_t pad_top, DrawBox* out)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = (int16_t)FB_W;
    rp.height = (int16_t)FB_H;
    er_node_set_props(root, &rp);

    ERNode* text = er_node_create(ER_NODE_TEXT);
    ERProps tp = props_default();
    tp.position = ER_POS_ABSOLUTE; /* pin the box so padding is the ONLY difference between cases */
    tp.left = BOX_X;
    tp.top = BOX_Y;
    tp.width = BOX_W;
    tp.height = BOX_H;
    tp.font_size = 16;
    tp.color = 0xFFFFFFFFU;
    tp.padding_left = pad_left;
    tp.padding_top = pad_top;
    strncpy(tp.text, "Save", ER_TEXT_MAX);
    er_node_set_props(text, &tp);

    er_tree_append_child(root, text);
    er_tree_set_root(root);

    box_reset();
    er_commit();
    *out = s_box;

    er_tree_remove_child(root, text);
    er_node_destroy(text);
    er_node_destroy(root);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — padding on a Text node insets the glyphs it paints.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&be);

    DrawBox bare, padded;
    render_padded_text(ER_LAYOUT_AUTO, ER_LAYOUT_AUTO, &bare);
    render_padded_text(12, 9, &padded);

    if (bare.ops == 0 || padded.ops == 0)
        return fail("no draw calls recorded — the text node painted nothing");
    if (bare.min_x < BOX_X || bare.min_y < BOX_Y)
        return fail("unpadded glyphs drew outside their own node box");

    if (padded.min_x != bare.min_x + 12)
        return fail("paddingLeft=12 did not move the glyph origin 12px right");
    if (padded.min_y != bare.min_y + 9)
        return fail("paddingTop=9 did not move the glyph origin 9px down");

    /* The run is the same string at the same size, so it only moved — it must not have re-wrapped or
     * been clipped by the narrower content box. */
    if (padded.max_x - padded.min_x != bare.max_x - bare.min_x)
        return fail("padding changed the width of the drawn glyph run");
    if (padded.max_y - padded.min_y != bare.max_y - bare.min_y)
        return fail("padding changed the height of the drawn glyph run");

    /* And it still lives inside the node: the padded run must not spill past the padding box. */
    if (padded.max_x > BOX_X + BOX_W || padded.max_y > BOX_Y + BOX_H)
        return fail("padded glyphs drew outside the node box");

    return EXIT_SUCCESS;
}
