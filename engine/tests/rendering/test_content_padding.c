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

#define FB_W 320
#define FB_H 240

/* The leaf node's box, identical in every case. */
#define BOX_X 40
#define BOX_Y 30
#define BOX_W 80
#define BOX_H 64

#define PAD_L 12
#define PAD_T 9
#define PAD_R 6
#define PAD_B 5

/** @brief Content-box width/height the padded cases must paint inside. */
#define CONTENT_W (BOX_W - PAD_L - PAD_R)
#define CONTENT_H (BOX_H - PAD_T - PAD_B)

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
 * @brief Pins a leaf's props to the shared box, optionally with the shared padding applied.
 *
 * @param[in,out] p    Props to stamp.
 * @param[in]     padded  Whether to set the four padding edges.
 */
static void pin_box(ERProps* p, bool padded)
{
    p->position = ER_POS_ABSOLUTE; /* pin the box so padding is the ONLY difference between cases */
    p->left = BOX_X;
    p->top = BOX_Y;
    p->width = BOX_W;
    p->height = BOX_H;
    if (padded)
    {
        p->padding_left = PAD_L;
        p->padding_top = PAD_T;
        p->padding_right = PAD_R;
        p->padding_bottom = PAD_B;
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
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = (int16_t)FB_W;
    rp.height = (int16_t)FB_H;
    er_node_set_props(root, &rp);

    ERNode* leaf = er_node_create(type);
    ERProps lp = *leaf_props;
    er_node_set_props(leaf, &lp);
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
 * @param[in] n  Vector node to configure.
 */
static void configure_vector(ERNode* n)
{
    static const float ops[] = {
        ER_VOP_SHAPE, 0, ER_VOP_MOVE, 0, 0, ER_VOP_LINE, 12, 0, ER_VOP_LINE, 12, 12, ER_VOP_LINE, 0, 12, ER_VOP_CLOSE};
    ERVectorPaint paint = {0};
    paint.fill = 0xFFFF0000U;
    er_node_set_vector_ops(n, ops, (int)(sizeof(ops) / sizeof(ops[0])), &paint, 1, NULL, 0);
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
    static char msg[192];
    if (bare->ops == 0 || padded->ops == 0)
    {
        snprintf(msg, sizeof(msg), "%s: no draw calls recorded — the node painted nothing", what);
        return msg;
    }
    if (bare->min_x < BOX_X || bare->min_y < BOX_Y || bare->max_x > BOX_X + BOX_W || bare->max_y > BOX_Y + BOX_H)
    {
        snprintf(msg, sizeof(msg), "%s: the unpadded draw left its own node box", what);
        return msg;
    }
    /* The whole point: the padded draw lives strictly inside the content box. */
    if (padded->min_x < BOX_X + PAD_L || padded->min_y < BOX_Y + PAD_T || padded->max_x > BOX_X + BOX_W - PAD_R
        || padded->max_y > BOX_Y + BOX_H - PAD_B)
    {
        snprintf(msg,
                 sizeof(msg),
                 "%s: padded draw [%d,%d..%d,%d] escaped the content box [%d,%d..%d,%d]",
                 what,
                 padded->min_x,
                 padded->min_y,
                 padded->max_x,
                 padded->max_y,
                 BOX_X + PAD_L,
                 BOX_Y + PAD_T,
                 BOX_X + BOX_W - PAD_R,
                 BOX_Y + BOX_H - PAD_B);
        return msg;
    }
    /* And it MOVED — a draw that merely got clipped would also pass the bound above. */
    if (padded->min_x <= bare->min_x || padded->min_y <= bare->min_y)
    {
        snprintf(msg, sizeof(msg), "%s: padding did not move the drawn origin inward", what);
        return msg;
    }
    if (exact && (padded->min_x != bare->min_x + PAD_L || padded->min_y != bare->min_y + PAD_T))
    {
        snprintf(msg,
                 sizeof(msg),
                 "%s: origin moved by (%d,%d), expected (%d,%d)",
                 what,
                 padded->min_x - bare->min_x,
                 padded->min_y - bare->min_y,
                 PAD_L,
                 PAD_T);
        return msg;
    }
    if (exact && (padded->max_x != BOX_X + BOX_W - PAD_R || padded->max_y != BOX_Y + BOX_H - PAD_B))
    {
        snprintf(msg, sizeof(msg), "%s: padded draw does not reach the trailing content edge", what);
        return msg;
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
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&be);

    for (int i = 0; i < 16 * 16; i++)
        s_bitmap[i] = ((i + (i / 16)) & 1) ? 0xFF202020U : 0xFFE0E0E0U;
    er_image_load("checker", s_bitmap, 16, 16);

    const char* err;
    DrawBox bare, padded;

    /*------------------------------------------------------------------------------------------------
     * Image — the clear-cut replaced element. STRETCH fills its rect exactly, so the inset is exact on
     * all four edges: a 80x64 bitmap becomes 62x50 at (+12, +9).
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        p.resize_mode = ER_RESIZE_STRETCH;
        strncpy(p.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&p, ER_NODE_IMAGE, NULL, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        q.resize_mode = ER_RESIZE_STRETCH;
        strncpy(q.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&q, ER_NODE_IMAGE, NULL, &padded);

        if ((err = check_inset("Image(stretch)", &bare, &padded, true)) != NULL)
            return fail(err);
        if (padded.max_x - padded.min_x != CONTENT_W || padded.max_y - padded.min_y != CONTENT_H)
            return fail("Image(stretch): scaled to the border box, not the content box");
    }

    /*------------------------------------------------------------------------------------------------
     * Image / repeat — tiles from the CONTENT origin (CSS background-origin: padding-box), so the
     * tiling is clipped by the content box on both axes rather than the border box.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        p.resize_mode = ER_RESIZE_REPEAT;
        strncpy(p.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&p, ER_NODE_IMAGE, NULL, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        q.resize_mode = ER_RESIZE_REPEAT;
        strncpy(q.image_name, "checker", ER_IMAGE_NAME_MAX);
        render_leaf(&q, ER_NODE_IMAGE, NULL, &padded);

        if ((err = check_inset("Image(repeat)", &bare, &padded, true)) != NULL)
            return fail(err);
    }

    /*------------------------------------------------------------------------------------------------
     * Vector / Svg — the tape's ORIGIN moves with the padding. The square is 12x12 in node-local
     * coordinates and the content box is bigger than that, so it translates without being clipped:
     * same drawn size, moved by exactly the leading padding.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        render_leaf(&p, ER_NODE_VECTOR, configure_vector, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        render_leaf(&q, ER_NODE_VECTOR, configure_vector, &padded);

        if ((err = check_inset("Svg", &bare, &padded, false)) != NULL)
            return fail(err);
        if (padded.min_x != bare.min_x + PAD_L || padded.min_y != bare.min_y + PAD_T)
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
        ERNode* root = er_node_create(ER_NODE_VIEW);
        ERProps rp = props_default();
        rp.width = (int16_t)FB_W;
        rp.height = (int16_t)FB_H;
        er_node_set_props(root, &rp);

        ERNode* v = er_node_create(ER_NODE_VECTOR);
        ERProps vp = props_default();
        pin_box(&vp, true);
        er_node_set_props(v, &vp);
        configure_vector(v);

        er_tree_append_child(root, v);
        er_tree_set_root(root);
        er_commit();

        /* A 10x10 change at tape-local (5, 5). */
        er_node_set_vector_dirty_rect(v, 5, 5, 10, 10);
        er_commit();

        ERRect dr;
        er_get_dirty_rect(&dr);
        if (dr.x != BOX_X + PAD_L + 5 || dr.y != BOX_Y + PAD_T + 5)
        {
            fprintf(stderr,
                    "FAIL: Svg sub-region damage at (%d,%d), expected (%d,%d)\n",
                    dr.x,
                    dr.y,
                    BOX_X + PAD_L + 5,
                    BOX_Y + PAD_T + 5);
            return EXIT_FAILURE;
        }
        if (dr.w != 10 || dr.h != 10)
            return fail("Svg: padding changed the size of the sub-region damage rect");

        er_tree_remove_child(root, v);
        er_node_destroy(v);
        er_node_destroy(root);
    }

    /*------------------------------------------------------------------------------------------------
     * ActivityIndicator — the dot ring is sized off the smaller side and centred, so it shrinks and
     * re-centres inside the content box. It does not fill its box (dots on a ring), hence exact=false.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        p.color = 0xFFFFFFFFU;
        render_leaf(&p, ER_NODE_ACTIVITY_INDICATOR, NULL, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        q.color = 0xFFFFFFFFU;
        render_leaf(&q, ER_NODE_ACTIVITY_INDICATOR, NULL, &padded);

        if ((err = check_inset("ActivityIndicator", &bare, &padded, false)) != NULL)
            return fail(err);
    }

    /*------------------------------------------------------------------------------------------------
     * Switch — the track is a pill filling the whole box, so this one is exact too.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        p.switch_value = 1U;
        render_leaf(&p, ER_NODE_SWITCH, NULL, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        q.switch_value = 1U;
        render_leaf(&q, ER_NODE_SWITCH, NULL, &padded);

        if ((err = check_inset("Switch", &bare, &padded, true)) != NULL)
            return fail(err);
    }

    /*------------------------------------------------------------------------------------------------
     * Arc / Dial — a full 360 sweep so the ring bounds the whole content box. The inset lives inside
     * er_arc_geom(), so this also stands for the hit test and the knob-child placement, which read the
     * same geometry.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        p.arc_sweep_angle = 360.0f;
        p.arc_start_angle = 0.0f;
        p.arc_max = 100.0f;
        p.arc_value = 100.0f;
        p.arc_width = 6;
        p.arc_track_color = 0xFF3A3A3CU;
        render_leaf(&p, ER_NODE_ARC, NULL, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        q.arc_sweep_angle = 360.0f;
        q.arc_start_angle = 0.0f;
        q.arc_max = 100.0f;
        q.arc_value = 100.0f;
        q.arc_width = 6;
        q.arc_track_color = 0xFF3A3A3CU;
        render_leaf(&q, ER_NODE_ARC, NULL, &padded);

        if ((err = check_inset("Arc", &bare, &padded, false)) != NULL)
            return fail(err);
        /* The ring is inscribed in the SHORTER side, so its diameter must have shrunk by that side's
         * padding — proof the dial resized rather than just sliding over. */
        const int bare_d = bare.max_y - bare.min_y;
        const int pad_d = padded.max_y - padded.min_y;
        if (pad_d >= bare_d)
            return fail("Arc: padding did not shrink the dial");
    }

    /*------------------------------------------------------------------------------------------------
     * TextInput — the odd one. Its 4 px / 3 px inset is a DEFAULT padding, so an unset field must look
     * exactly as it always has, and a set one replaces the default per edge. The border box (background
     * + border) is unchanged either way, so the two cases are compared on the TEXT alone: the field is
     * given no background and no border, leaving the glyph run as the only thing drawn.
     *----------------------------------------------------------------------------------------------*/
    {
        ERProps p = props_default();
        pin_box(&p, false);
        p.color = 0xFFFFFFFFU;
        p.font_size = 14;
        strncpy(p.text, "Hi", ER_TEXT_MAX);
        render_leaf(&p, ER_NODE_TEXT_INPUT, NULL, &bare);

        ERProps q = props_default();
        pin_box(&q, true);
        q.color = 0xFFFFFFFFU;
        q.font_size = 14;
        strncpy(q.text, "Hi", ER_TEXT_MAX);
        render_leaf(&q, ER_NODE_TEXT_INPUT, NULL, &padded);

        if (bare.ops == 0 || padded.ops == 0)
            return fail("TextInput: no draw calls recorded");
        /* Default inset is 4 / 3; the padded case replaces it with PAD_L / PAD_T. */
        if (padded.min_x - bare.min_x != PAD_L - 4)
            return fail("TextInput: paddingLeft did not replace the built-in 4px inset");
        if (padded.min_y - bare.min_y != PAD_T - 3)
            return fail("TextInput: paddingTop did not replace the built-in 3px inset");
    }

    return EXIT_SUCCESS;
}
