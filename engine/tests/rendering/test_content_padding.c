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
 * Every content-painting leaf has to SPEND the padding the layout pass reserved for it, not paint over
 * it (#180). <Text> has its own suite next door (test_text_padding.c); this is the rest of the set —
 * Image, Svg, ActivityIndicator, Switch, Arc — plus the one that keeps a built-in inset of its own,
 * TextInput.
 *
 * Same method throughout: give the node an EXPLICIT width/height so the box is byte-identical across
 * cases, render it once bare and once padded, and compare the bounding box of the draw calls the
 * backend was handed. The node box never moves, so any shift in that bounding box is the padding and
 * nothing else. No case carries a background fill, so every recorded draw is content.
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

/* The leaf node's box, identical in every case. */
#define BOX_X 40
#define BOX_Y 30
#define BOX_WIDTH 80
#define BOX_HEIGHT 64

#define PADDING_LEFT 12
#define PADDING_TOP 9
#define PADDING_RIGHT 6
#define PADDING_BOTTOM 5

/** @brief Content-box width/height the padded cases must paint inside. */
#define CONTENT_WIDTH (BOX_WIDTH - PADDING_LEFT - PADDING_RIGHT)
#define CONTENT_HEIGHT (BOX_HEIGHT - PADDING_TOP - PADDING_BOTTOM)

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

/** @brief A 16x16 opaque checkerboard, registered once as the Image/knob asset. */
static uint32_t s_bitmap[16 * 16];

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
 * @brief Pins a leaf's props to the shared box, optionally with the shared padding applied.
 *
 * @param[in,out] props   Props to stamp.
 * @param[in]     padded  Whether to set the four padding edges.
 */
static void pin_box(ERProps* props, bool padded)
{
    props->position = ER_POSITION_ABSOLUTE; /* pin the box so padding is the ONLY difference between cases */
    props->left = BOX_X;
    props->top = BOX_Y;
    props->width = BOX_WIDTH;
    props->height = BOX_HEIGHT;
    if (padded)
    {
        props->padding_left = PADDING_LEFT;
        props->padding_top = PADDING_TOP;
        props->padding_right = PADDING_RIGHT;
        props->padding_bottom = PADDING_BOTTOM;
    }
}

/**
 * @brief Renders one leaf node alone in the tree and records where it drew.
 *
 * @param[in]  leaf_props  Props for the leaf, already pinned to the shared box.
 * @param[in]  type        Node type to create.
 * @param[in]  configure   Optional hook run on the created node before it is committed.
 * @param[out] out         Receives the bounding box of every draw call the commit made.
 */
static void render_leaf(const ERProps* leaf_props, ERNodeType type, void (*configure)(ERNode*), DrawBox* out)
{
    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
    root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
    er_node_set_props(root, &root_props);

    ERNode* leaf = er_node_create(type);
    ERProps leaf_copy = *leaf_props;
    er_node_set_props(leaf, &leaf_copy);
    if (configure)
        configure(leaf);

    er_tree_append_child(root, leaf);
    er_tree_set_root(root);

    box_reset();
    er_commit();
    *out = s_box;

    er_tree_remove_child(root, leaf);
    er_node_destroy(leaf);
    er_node_destroy(root);
}

/**
 * @brief Uploads a filled 12x12 square tape to a vector node.
 *
 * Node-local coordinates, so the drawn square lands at the node's geometry origin — which is exactly
 * what the padded case has to move.
 *
 * @param[in] node  Vector node to configure.
 */
static void configure_vector(ERNode* node)
{
    static const float tape[] = {ER_VECTOR_OP_SHAPE,
                                 0,
                                 ER_VECTOR_OP_MOVE,
                                 0,
                                 0,
                                 ER_VECTOR_OP_LINE,
                                 12,
                                 0,
                                 ER_VECTOR_OP_LINE,
                                 12,
                                 12,
                                 ER_VECTOR_OP_LINE,
                                 0,
                                 12,
                                 ER_VECTOR_OP_CLOSE};
    ERVectorPaint paint = {0};
    paint.fill = 0xFFFF0000U;
    er_node_set_vector_ops(node, tape, (int)(sizeof(tape) / sizeof(tape[0])), &paint, 1, NULL, 0);
}

/**
 * @brief Asserts a padded draw sits inside the content box and moved by the leading padding.
 *
 * The trailing edges are checked as a containment bound rather than an exact shift: a shape that does
 * not fill its box (a spinner's dot ring, an arc's sweep) legitimately stops short of the right/bottom
 * content edge, but it must never cross it.
 *
 * @param[in] what    Node name, for the failure message.
 * @param[in] bare    Draw box with no padding.
 * @param[in] padded  Draw box with the shared padding.
 * @param[in] exact   Whether the content fills its box, so the shift is exact on all four edges.
 *
 * @return NULL on pass, or a static failure message.
 */
static const char* check_inset(const char* what, const DrawBox* bare, const DrawBox* padded, bool exact)
{
    static char message[192];
    if (bare->operation_count == 0 || padded->operation_count == 0)
    {
        snprintf(message, sizeof(message), "%s: no draw calls recorded — the node painted nothing", what);
        return message;
    }
    if (bare->min_x < BOX_X || bare->min_y < BOX_Y || bare->max_x > BOX_X + BOX_WIDTH
        || bare->max_y > BOX_Y + BOX_HEIGHT)
    {
        snprintf(message, sizeof(message), "%s: the unpadded draw left its own node box", what);
        return message;
    }
    /* The whole point: the padded draw lives strictly inside the content box. */
    if (padded->min_x < BOX_X + PADDING_LEFT || padded->min_y < BOX_Y + PADDING_TOP
        || padded->max_x > BOX_X + BOX_WIDTH - PADDING_RIGHT || padded->max_y > BOX_Y + BOX_HEIGHT - PADDING_BOTTOM)
    {
        snprintf(message,
                 sizeof(message),
                 "%s: padded draw [%d,%d..%d,%d] escaped the content box [%d,%d..%d,%d]",
                 what,
                 padded->min_x,
                 padded->min_y,
                 padded->max_x,
                 padded->max_y,
                 BOX_X + PADDING_LEFT,
                 BOX_Y + PADDING_TOP,
                 BOX_X + BOX_WIDTH - PADDING_RIGHT,
                 BOX_Y + BOX_HEIGHT - PADDING_BOTTOM);
        return message;
    }
    /* And it MOVED — a draw that merely got clipped would also pass the bound above. */
    if (padded->min_x <= bare->min_x || padded->min_y <= bare->min_y)
    {
        snprintf(message, sizeof(message), "%s: padding did not move the drawn origin inward", what);
        return message;
    }
    if (exact && (padded->min_x != bare->min_x + PADDING_LEFT || padded->min_y != bare->min_y + PADDING_TOP))
    {
        snprintf(message,
                 sizeof(message),
                 "%s: origin moved by (%d,%d), expected (%d,%d)",
                 what,
                 padded->min_x - bare->min_x,
                 padded->min_y - bare->min_y,
                 PADDING_LEFT,
                 PADDING_TOP);
        return message;
    }
    if (exact
        && (padded->max_x != BOX_X + BOX_WIDTH - PADDING_RIGHT || padded->max_y != BOX_Y + BOX_HEIGHT - PADDING_BOTTOM))
    {
        snprintf(message, sizeof(message), "%s: padded draw does not reach the trailing content edge", what);
        return message;
    }
    return NULL;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — style padding insets what every content-painting leaf draws.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    for (int pixel_index = 0; pixel_index < 16 * 16; pixel_index++)
        s_bitmap[pixel_index] = ((pixel_index + (pixel_index / 16)) & 1) ? 0xFF202020U : 0xFFE0E0E0U;
    er_image_load("checker", s_bitmap, 16, 16);

    const char* error;
    DrawBox bare, padded;

    /*------------------------------------------------------------------------------------------------
     * Image — the clear-cut replaced element. STRETCH fills its rect exactly, so the inset is exact on
     * all four edges: a 80x64 bitmap becomes 62x50 at (+12, +9).
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        bare_props.resize_mode = ER_RESIZE_STRETCH;
        strncpy(bare_props.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&bare_props, ER_NODE_IMAGE, NULL, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        padded_props.resize_mode = ER_RESIZE_STRETCH;
        strncpy(padded_props.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&padded_props, ER_NODE_IMAGE, NULL, &padded);

        if ((error = check_inset("Image(stretch)", &bare, &padded, true)) != NULL)
            return fail(error);
        if (padded.max_x - padded.min_x != CONTENT_WIDTH || padded.max_y - padded.min_y != CONTENT_HEIGHT)
            return fail("Image(stretch): scaled to the border box, not the content box");
    }

    /*------------------------------------------------------------------------------------------------
     * Image / repeat — tiles from the CONTENT origin (CSS background-origin: padding-box), so the
     * tiling is clipped by the content box on both axes rather than the border box.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        bare_props.resize_mode = ER_RESIZE_REPEAT;
        strncpy(bare_props.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&bare_props, ER_NODE_IMAGE, NULL, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        padded_props.resize_mode = ER_RESIZE_REPEAT;
        strncpy(padded_props.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&padded_props, ER_NODE_IMAGE, NULL, &padded);

        if ((error = check_inset("Image(repeat)", &bare, &padded, true)) != NULL)
            return fail(error);
    }

    /*------------------------------------------------------------------------------------------------
     * Vector / Svg — the tape's ORIGIN moves with the padding. The square is 12x12 in node-local
     * coordinates and the content box is bigger than that, so it translates without being clipped:
     * same drawn size, moved by exactly the leading padding.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        render_leaf(&bare_props, ER_NODE_VECTOR, configure_vector, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        render_leaf(&padded_props, ER_NODE_VECTOR, configure_vector, &padded);

        if ((error = check_inset("Svg", &bare, &padded, false)) != NULL)
            return fail(error);
        if (padded.min_x != bare.min_x + PADDING_LEFT || padded.min_y != bare.min_y + PADDING_TOP)
            return fail("Svg: tape origin did not move by the leading padding");
        if (padded.max_x - padded.min_x != bare.max_x - bare.min_x
            || padded.max_y - padded.min_y != bare.max_y - bare.min_y)
            return fail("Svg: padding resized the drawn geometry instead of translating it");
    }

    /*------------------------------------------------------------------------------------------------
     * Vector / Svg — the TIGHT sub-region damage rect. An app that updates part of a vector node hands
     * the engine a node-local rect in TAPE coordinates, and the tape's origin is now the content box —
     * so the padding has to be added back when that rect is placed on screen. Get this wrong and a
     * padded animated <Svg> repaints the wrong strip and smears; it is invisible to the paint tests
     * above, which never take the sub-region path.
     *----------------------------------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps root_props = props_default();
        root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
        root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
        er_node_set_props(root, &root_props);

        ERNode* vector = er_node_create(ER_NODE_VECTOR);
        ERProps vector_props = props_default();
        pin_box(&vector_props, true);
        er_node_set_props(vector, &vector_props);
        configure_vector(vector);

        er_tree_append_child(root, vector);
        er_tree_set_root(root);
        er_commit();

        /* A 10x10 change at tape-local (5, 5). */
        er_node_set_vector_dirty_rect(vector, 5, 5, 10, 10);
        er_commit();

        ERRect dirty;
        er_get_dirty_rect(&dirty);
        if (dirty.x != BOX_X + PADDING_LEFT + 5 || dirty.y != BOX_Y + PADDING_TOP + 5)
        {
            fprintf(stderr,
                    "FAIL: Svg sub-region damage at (%d,%d), expected (%d,%d)\n",
                    dirty.x,
                    dirty.y,
                    BOX_X + PADDING_LEFT + 5,
                    BOX_Y + PADDING_TOP + 5);
            return EXIT_FAILURE;
        }
        if (dirty.width != 10 || dirty.height != 10)
            return fail("Svg: padding changed the size of the sub-region damage rect");

        er_tree_remove_child(root, vector);
        er_node_destroy(vector);
        er_node_destroy(root);
    }

    /*------------------------------------------------------------------------------------------------
     * ActivityIndicator — the dot ring is sized off the smaller side and centred, so it shrinks and
     * re-centres inside the content box. It does not fill its box (dots on a ring), hence exact=false.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        bare_props.color = 0xFFFFFFFFU;
        render_leaf(&bare_props, ER_NODE_ACTIVITY_INDICATOR, NULL, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        padded_props.color = 0xFFFFFFFFU;
        render_leaf(&padded_props, ER_NODE_ACTIVITY_INDICATOR, NULL, &padded);

        if ((error = check_inset("ActivityIndicator", &bare, &padded, false)) != NULL)
            return fail(error);
    }

    /*------------------------------------------------------------------------------------------------
     * Switch — the track is a pill filling the whole box, so this one is exact too.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        bare_props.switch_value = 1U;
        render_leaf(&bare_props, ER_NODE_SWITCH, NULL, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        padded_props.switch_value = 1U;
        render_leaf(&padded_props, ER_NODE_SWITCH, NULL, &padded);

        if ((error = check_inset("Switch", &bare, &padded, true)) != NULL)
            return fail(error);
    }

    /*------------------------------------------------------------------------------------------------
     * Arc / Dial — a full 360 sweep so the ring bounds the whole content box. The inset lives inside
     * er_arc_geom(), so this also stands for the hit test and the knob-child placement, which read the
     * same geometry.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        bare_props.arc_sweep_angle = 360.0f;
        bare_props.arc_start_angle = 0.0f;
        bare_props.arc_max = 100.0f;
        bare_props.arc_value = 100.0f;
        bare_props.arc_width = 6;
        bare_props.arc_track_color = 0xFF3A3A3CU;
        render_leaf(&bare_props, ER_NODE_ARC, NULL, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        padded_props.arc_sweep_angle = 360.0f;
        padded_props.arc_start_angle = 0.0f;
        padded_props.arc_max = 100.0f;
        padded_props.arc_value = 100.0f;
        padded_props.arc_width = 6;
        padded_props.arc_track_color = 0xFF3A3A3CU;
        render_leaf(&padded_props, ER_NODE_ARC, NULL, &padded);

        if ((error = check_inset("Arc", &bare, &padded, false)) != NULL)
            return fail(error);
        /* The ring is inscribed in the SHORTER side, so its diameter must have shrunk by that side's
         * padding — proof the dial resized rather than just sliding over. */
        const int bare_diameter = bare.max_y - bare.min_y;
        const int padded_diameter = padded.max_y - padded.min_y;
        if (padded_diameter >= bare_diameter)
            return fail("Arc: padding did not shrink the dial");
    }

    /*------------------------------------------------------------------------------------------------
     * TextInput — the odd one. Its 4 px / 3 px inset is a DEFAULT padding, so an unset field must look
     * exactly as it always has, and a set one replaces the default per edge. The border box (background
     * + border) is unchanged either way, so the two cases are compared on the TEXT alone: the field is
     * given no background and no border, leaving the glyph run as the only thing drawn.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps bare_props = props_default();
        pin_box(&bare_props, false);
        bare_props.color = 0xFFFFFFFFU;
        bare_props.font_size = 14;
        strncpy(bare_props.text, "Hi", ER_TEXT_MAX);
        render_leaf(&bare_props, ER_NODE_TEXT_INPUT, NULL, &bare);

        ERProps padded_props = props_default();
        pin_box(&padded_props, true);
        padded_props.color = 0xFFFFFFFFU;
        padded_props.font_size = 14;
        strncpy(padded_props.text, "Hi", ER_TEXT_MAX);
        render_leaf(&padded_props, ER_NODE_TEXT_INPUT, NULL, &padded);

        if (bare.operation_count == 0 || padded.operation_count == 0)
            return fail("TextInput: no draw calls recorded");
        /* Default inset is 4 / 3; the padded case replaces it with PADDING_LEFT / PADDING_TOP. */
        if (padded.min_x - bare.min_x != PADDING_LEFT - 4)
            return fail("TextInput: paddingLeft did not replace the built-in 4px inset");
        if (padded.min_y - bare.min_y != PADDING_TOP - 3)
            return fail("TextInput: paddingTop did not replace the built-in 3px inset");
    }

    return EXIT_SUCCESS;
}
