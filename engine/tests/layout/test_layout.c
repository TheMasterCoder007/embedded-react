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

/* Width the wrapping text nodes are constrained to; narrow enough to force wrapping. */
#define TEXT_BOX_W 120

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Captured computed height for each text node, written by its ER_EVENT_LAYOUT handler. */
static int s_one_line_h = -1;
static int s_two_line_h = -1;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief No-op fill_rect backend callback.
 *
 * @param[in] argb  Fill color (unused).
 * @param[in] x     Left edge (unused).
 * @param[in] y     Top edge (unused).
 * @param[in] w     Width (unused).
 * @param[in] h     Height (unused).
 * @param[in] ctx   Opaque context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief No-op copy_rect backend callback.
 *
 * @param[in] src  Source buffer (unused).
 * @param[in] s    Stride (unused).
 * @param[in] x    X (unused).
 * @param[in] y    Y (unused).
 * @param[in] w    Width (unused).
 * @param[in] h    Height (unused).
 * @param[in] ctx  Opaque context (unused).
 */
static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief No-op blend_rect backend callback.
 *
 * @param[in] src  Source buffer (unused).
 * @param[in] s    Stride (unused).
 * @param[in] a    Global alpha (unused).
 * @param[in] x    X (unused).
 * @param[in] y    Y (unused).
 * @param[in] w    Width (unused).
 * @param[in] h    Height (unused).
 * @param[in] ctx  Opaque context (unused).
 */
static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief ER_EVENT_LAYOUT handler that records a node's computed height into an int.
 *
 * @param[in] node       Node that fired the event (unused).
 * @param[in] data       Event payload carrying data->layout_rect.
 * @param[in] user_data  Pointer to the int that receives layout_rect.h.
 */
static void on_layout(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    *(int*)user_data = data->layout_rect.h;
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
 * @brief Builds an ERProps with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return Zero-initialised ERProps with AUTO layout sentinels and opacity 255.
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
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — verifies layout behaviour, including text intrinsic height.
 *
 * Builds a root with two width-constrained Text nodes containing the same long string:
 * one with number_of_lines = 1 and one with number_of_lines = 2. Asserts that the
 * two-line node is allocated more vertical space than the one-line node, confirming
 * that number_of_lines feeds the intrinsic-height measurement during layout.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    /* Smoke: er_commit with no root must not crash. */
    er_commit();

    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&be);

    static const char* k_long = "This is a long string that must wrap across multiple lines when constrained";

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = (int16_t)FB_W;
    rp.height = (int16_t)FB_H;
    rp.flex_direction = ER_FLEX_COL;
    rp.align_items = ER_ALIGN_FLEX_START;
    er_node_set_props(root, &rp);

    /* One-line text node, width-constrained. */
    ERNode* one = er_node_create(ER_NODE_TEXT);
    ERProps op = props_default();
    op.width = TEXT_BOX_W;
    op.font_size = 14;
    op.number_of_lines = 1;
    op.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(op.text, k_long, ER_TEXT_MAX);
    er_node_set_props(one, &op);
    er_event_set(one, ER_EVENT_LAYOUT, on_layout, &s_one_line_h);

    /* Two-line text node, identical text and width. */
    ERNode* two = er_node_create(ER_NODE_TEXT);
    ERProps tp = props_default();
    tp.width = TEXT_BOX_W;
    tp.font_size = 14;
    tp.number_of_lines = 2;
    tp.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(tp.text, k_long, ER_TEXT_MAX);
    er_node_set_props(two, &tp);
    er_event_set(two, ER_EVENT_LAYOUT, on_layout, &s_two_line_h);

    er_tree_append_child(root, one);
    er_tree_append_child(root, two);
    er_tree_set_root(root);

    er_commit();

    if (s_one_line_h <= 0)
        return fail("one-line text node reported non-positive computed height");
    if (s_two_line_h <= 0)
        return fail("two-line text node reported non-positive computed height");
    if (s_two_line_h <= s_one_line_h)
        return fail("number_of_lines=2 did not reserve more height than number_of_lines=1");
    /* Two lines should be roughly twice one line; allow generous tolerance. */
    if (s_two_line_h < s_one_line_h + s_one_line_h / 2)
        return fail("two-line height is not close to twice the one-line height");

    return EXIT_SUCCESS;
}
