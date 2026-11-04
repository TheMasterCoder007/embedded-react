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

#define FRAMEBUFFER_WIDTH 320
#define FRAMEBUFFER_HEIGHT 240
#define OPACITY_OPAQUE 255U

/* The text node's box. Wide enough that "Save" plus the largest padding still fits on one line. */
#define BOX_X 40
#define BOX_Y 30
#define BOX_WIDTH 160
#define BOX_HEIGHT 60

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Bounding box of everything the backend was asked to draw in one commit. */
typedef struct
{
    int operation_count;
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
    s_box.operation_count = 0;
    s_box.min_x = FRAMEBUFFER_WIDTH;
    s_box.min_y = FRAMEBUFFER_HEIGHT;
    s_box.max_x = 0;
    s_box.max_y = 0;
}

/**
 * @brief Folds one drawn rectangle into the recorded bounding box.
 *
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width.
 * @param[in] height  Height.
 */
static void box_add(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    s_box.operation_count++;
    if (x < s_box.min_x)
        s_box.min_x = x;
    if (y < s_box.min_y)
        s_box.min_y = y;
    if (x + width > s_box.max_x)
        s_box.max_x = x + width;
    if (y + height > s_box.max_y)
        s_box.max_y = y + height;
}

/**
 * @brief Backend fill_rect that records the drawn rectangle.
 *
 * @param[in] argb    Fill color (unused).
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width.
 * @param[in] height  Height.
 * @param[in] ctx     Unused.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)ctx;
    box_add(x, y, width, height);
}

/**
 * @brief Backend copy_rect that records the drawn rectangle.
 *
 * @param[in] source  Source buffer (unused).
 * @param[in] stride  Row stride (unused).
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width.
 * @param[in] height  Height.
 * @param[in] ctx     Unused.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)ctx;
    box_add(x, y, width, height);
}

/**
 * @brief Backend blend_rect that records the drawn rectangle.
 *
 * @param[in] source  Source buffer (unused).
 * @param[in] stride  Row stride (unused).
 * @param[in] alpha   Global alpha (unused).
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width.
 * @param[in] height  Height.
 * @param[in] ctx     Unused.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)ctx;
    box_add(x, y, width, height);
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] message  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
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
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.padding_horizontal = props.padding_vertical = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.margin_horizontal = props.margin_vertical = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
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
    ERProps root_props = props_default();
    root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
    root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
    er_node_set_props(root, &root_props);

    ERNode* text = er_node_create(ER_NODE_TEXT);
    ERProps text_props = props_default();
    text_props.position = ER_POSITION_ABSOLUTE; /* pin the box so padding is the ONLY difference between cases */
    text_props.left = BOX_X;
    text_props.top = BOX_Y;
    text_props.width = BOX_WIDTH;
    text_props.height = BOX_HEIGHT;
    text_props.font_size = 16;
    text_props.color = 0xFFFFFFFFU;
    text_props.padding_left = pad_left;
    text_props.padding_top = pad_top;
    strncpy(text_props.text, "Save", ER_TEXT_MAX);
    er_node_set_props(text, &text_props);

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
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    DrawBox bare, padded;
    render_padded_text(ER_LAYOUT_AUTO, ER_LAYOUT_AUTO, &bare);
    render_padded_text(12, 9, &padded);

    if (bare.operation_count == 0 || padded.operation_count == 0)
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
    if (padded.max_x > BOX_X + BOX_WIDTH || padded.max_y > BOX_Y + BOX_HEIGHT)
        return fail("padded glyphs drew outside the node box");

    return EXIT_SUCCESS;
}
