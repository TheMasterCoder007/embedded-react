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

#include "er_scene.h"
#include "native_renderer.h"
#include "text_renderer.h"
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

/* Width the wrapping text nodes are constrained to; narrow enough to force wrapping. */
#define TEXT_BOX_WIDTH 120

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Captured computed height for each text node, written by its ER_EVENT_LAYOUT handler. */
static int s_one_line_height = -1;
static int s_two_line_height = -1;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief No-op fill_rect backend callback.
 *
 * @param[in] argb    Fill color (unused).
 * @param[in] x       Left edge (unused).
 * @param[in] y       Top edge (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Opaque context (unused).
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)argb;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief No-op copy_rect backend callback.
 *
 * @param[in] source  Source buffer (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Opaque context (unused).
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief No-op blend_rect backend callback.
 *
 * @param[in] source  Source buffer (unused).
 * @param[in] stride  Stride (unused).
 * @param[in] first   Global alpha (unused).
 * @param[in] x       X (unused).
 * @param[in] y       Y (unused).
 * @param[in] width   Width (unused).
 * @param[in] height  Height (unused).
 * @param[in] ctx     Opaque context (unused).
 */
static void blend_cb(const void* source, int stride, uint8_t first, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)first;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
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
 * @brief ER_EVENT_LAYOUT handler that records a node's full computed rect.
 *
 * @param[in] node       Node that fired the event (unused).
 * @param[in] data       Event payload carrying data->layout_rect.
 * @param[in] user_data  Pointer to an ERRect that receives the computed rectangle.
 */
static void on_layout_rect(ERNode* node, const EREventData* data, void* user_data)
{
    (void)node;
    *(ERRect*)user_data = data->layout_rect;
}

/**
 * @brief Builds an ERProps with all layout fields set to ER_LAYOUT_AUTO.
 *
 * @return Zero-initialised ERProps with AUTO layout sentinels and opacity 255.
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
 * @brief Measures how er_text_render() breaks a 14 px string at a given width.
 *
 * @param[in]  text       String to break.
 * @param[in]  width      Width to break at in pixels.
 * @param[out] out_width  Receives the widest line's width; may be NULL.
 *
 * @return The number of lines.
 */
static int lines_at(const char* text, int width, int* out_width)
{
    ERTextRenderParams params;
    memset(&params, 0, sizeof(params));
    params.text = text;
    params.font_size = 14;
    int widest = 0, line_count = 0;
    er_text_measure_lines(&params, width, &widest, &line_count);
    if (out_width)
        *out_width = widest;
    return line_count;
}

/**
 * @brief Creates a 14 px Text node that records its computed rect.
 *
 * @param[in]  props     Props to apply; the text and font size are set here.
 * @param[in]  text      String to show.
 * @param[out] out_rect  ERRect that receives the node's computed rect via ER_EVENT_LAYOUT.
 *
 * @return The created node.
 */
static ERNode* mk_text(ERProps props, const char* text, ERRect* out_rect)
{
    ERNode* node = er_node_create(ER_NODE_TEXT);
    props.font_size = 14;
    strncpy(props.text, text, ER_TEXT_MAX);
    er_node_set_props(node, &props);
    *out_rect = (ERRect){-1, -1, -1, -1};
    er_event_set(node, ER_EVENT_LAYOUT, on_layout_rect, out_rect);
    return node;
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

    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    static const char* k_long = "This is a long string that must wrap across multiple lines when constrained";

    er_reset();

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
    root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
    root_props.flex_direction = ER_FLEX_COL;
    root_props.align_items = ER_ALIGN_FLEX_START;
    er_node_set_props(root, &root_props);

    /* One-line text node, width-constrained. */
    ERNode* one = er_node_create(ER_NODE_TEXT);
    ERProps one_props = props_default();
    one_props.width = TEXT_BOX_WIDTH;
    one_props.font_size = 14;
    one_props.number_of_lines = 1;
    one_props.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(one_props.text, k_long, ER_TEXT_MAX);
    er_node_set_props(one, &one_props);
    er_event_set(one, ER_EVENT_LAYOUT, on_layout, &s_one_line_height);

    /* Two-line text node, identical text and width. */
    ERNode* two = er_node_create(ER_NODE_TEXT);
    ERProps two_props = props_default();
    two_props.width = TEXT_BOX_WIDTH;
    two_props.font_size = 14;
    two_props.number_of_lines = 2;
    two_props.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    strncpy(two_props.text, k_long, ER_TEXT_MAX);
    er_node_set_props(two, &two_props);
    er_event_set(two, ER_EVENT_LAYOUT, on_layout, &s_two_line_height);

    er_tree_append_child(root, one);
    er_tree_append_child(root, two);
    er_tree_set_root(root);

    er_commit();

    if (s_one_line_height <= 0)
        return fail("one-line text node reported non-positive computed height");
    if (s_two_line_height <= 0)
        return fail("two-line text node reported non-positive computed height");
    if (s_two_line_height <= s_one_line_height)
        return fail("number_of_lines=2 did not reserve more height than number_of_lines=1");
    /* Two lines should be roughly twice one line; allow generous tolerance. */
    if (s_two_line_height < s_one_line_height + s_one_line_height / 2)
        return fail("two-line height is not close to twice the one-line height");

    /* -----------------------------------------------------------------------
     * Test: aspectRatio — child derives height from explicit width.
     * Container: row 200×200.  Child: width=100, aspect_ratio=2.0 → height=50.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* aspect_root = er_node_create(ER_NODE_VIEW);
        ERProps aspect_root_props = props_default();
        aspect_root_props.width = 200;
        aspect_root_props.height = 200;
        aspect_root_props.flex_direction = ER_FLEX_ROW;
        aspect_root_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(aspect_root, &aspect_root_props);

        ERNode* aspect_child = er_node_create(ER_NODE_VIEW);
        ERRect aspect_rect = {-1, -1, -1, -1};
        aspect_root_props = props_default();
        aspect_root_props.width = 100;
        aspect_root_props.aspect_ratio = 2.0f; /* width/height = 2 → height = 50 */
        er_node_set_props(aspect_child, &aspect_root_props);
        er_event_set(aspect_child, ER_EVENT_LAYOUT, on_layout_rect, &aspect_rect);

        er_tree_append_child(aspect_root, aspect_child);
        er_tree_set_root(aspect_root);
        er_commit();

        if (aspect_rect.w != 100)
            return fail("aspectRatio: child width should be 100");
        if (aspect_rect.h != 50)
            return fail("aspectRatio: child height should be 50 (width/ratio=100/2)");

        er_tree_remove_child(aspect_root, aspect_child);
        er_node_destroy(aspect_child);
        er_node_destroy(aspect_root);
    }

    /* -----------------------------------------------------------------------
     * Test: flex_basis_pct — child claims 50% of parent's main axis.
     * Container: row 200×80.  Child: flex_basis_pct=50 → width=100.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* percent_root = er_node_create(ER_NODE_VIEW);
        ERProps percent_root_props = props_default();
        percent_root_props.width = 200;
        percent_root_props.height = 80;
        percent_root_props.flex_direction = ER_FLEX_ROW;
        percent_root_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(percent_root, &percent_root_props);

        ERNode* percent_child = er_node_create(ER_NODE_VIEW);
        ERRect percent_rect = {-1, -1, -1, -1};
        percent_root_props = props_default();
        percent_root_props.flex_basis_pct = 50.0f;
        er_node_set_props(percent_child, &percent_root_props);
        er_event_set(percent_child, ER_EVENT_LAYOUT, on_layout_rect, &percent_rect);

        er_tree_append_child(percent_root, percent_child);
        er_tree_set_root(percent_root);
        er_commit();

        if (percent_rect.w != 100)
            return fail("flex_basis_pct: 50% of 200px parent should yield width=100");

        er_tree_remove_child(percent_root, percent_child);
        er_node_destroy(percent_child);
        er_node_destroy(percent_root);
    }

    /* -----------------------------------------------------------------------
     * Test: marginHorizontal — expands to margin_left + margin_right.
     * Container: col 200×200.  Child: marginHorizontal=20 → x=20, width=160.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* margin_root = er_node_create(ER_NODE_VIEW);
        ERProps margin_root_props = props_default();
        margin_root_props.width = 200;
        margin_root_props.height = 200;
        margin_root_props.flex_direction = ER_FLEX_COL;
        margin_root_props.align_items = ER_ALIGN_STRETCH;
        er_node_set_props(margin_root, &margin_root_props);

        ERNode* margin_child = er_node_create(ER_NODE_VIEW);
        ERRect margin_rect = {-1, -1, -1, -1};
        margin_root_props = props_default();
        margin_root_props.height = 40;
        margin_root_props.margin_horizontal = 20;
        er_node_set_props(margin_child, &margin_root_props);
        er_event_set(margin_child, ER_EVENT_LAYOUT, on_layout_rect, &margin_rect);

        er_tree_append_child(margin_root, margin_child);
        er_tree_set_root(margin_root);
        er_commit();

        if (margin_rect.x != 20)
            return fail("marginHorizontal: child should be offset 20px from parent left");
        if (margin_rect.w != 160)
            return fail("marginHorizontal: child width should be 160 (200 - 20*2)");

        er_tree_remove_child(margin_root, margin_child);
        er_node_destroy(margin_child);
        er_node_destroy(margin_root);
    }

    /* -----------------------------------------------------------------------
     * Test: paddingHorizontal — adds padding to both left and right.
     * Container: col 200×200, paddingHorizontal=15. Child: stretch → x=15, w=170.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* padding_root = er_node_create(ER_NODE_VIEW);
        ERProps padding_root_props = props_default();
        padding_root_props.width = 200;
        padding_root_props.height = 200;
        padding_root_props.flex_direction = ER_FLEX_COL;
        padding_root_props.align_items = ER_ALIGN_STRETCH;
        padding_root_props.padding_horizontal = 15;
        er_node_set_props(padding_root, &padding_root_props);

        ERNode* padding_child = er_node_create(ER_NODE_VIEW);
        ERRect padding_rect = {-1, -1, -1, -1};
        padding_root_props = props_default();
        padding_root_props.height = 40;
        er_node_set_props(padding_child, &padding_root_props);
        er_event_set(padding_child, ER_EVENT_LAYOUT, on_layout_rect, &padding_rect);

        er_tree_append_child(padding_root, padding_child);
        er_tree_set_root(padding_root);
        er_commit();

        if (padding_rect.x != 15)
            return fail("paddingHorizontal: child should start at x=15");
        if (padding_rect.w != 170)
            return fail("paddingHorizontal: child width should be 170 (200 - 15*2)");

        er_tree_remove_child(padding_root, padding_child);
        er_node_destroy(padding_child);
        er_node_destroy(padding_root);
    }

    /* -----------------------------------------------------------------------
     * Test: padding on a Text node grows its AUTO size, exactly as on a View.
     *
     * A text node's glyph run is its content, so an auto axis has to grow around
     * it. This used to be dropped on the floor: measure_content()'s Text branch
     * returned the raw glyph measurement and never added the node's own padding,
     * so `padding` typechecked, marshalled, and then laid out as nothing.
     *
     * Every case measures the SAME string at the same size, so the padded boxes
     * are asserted against the bare one rather than against baked-in glyph
     * metrics — the numbers hold whatever font the build ships.
     * -----------------------------------------------------------------------*/
    {
        static const char* k_word = "Save";

        er_reset();

        ERNode* text_padding_root = er_node_create(ER_NODE_VIEW);
        ERProps text_padding_root_props = props_default();
        text_padding_root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
        text_padding_root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
        text_padding_root_props.flex_direction = ER_FLEX_COL;
        text_padding_root_props.align_items = ER_ALIGN_FLEX_START; /* shrink-wrap each child onto its own content */
        er_node_set_props(text_padding_root, &text_padding_root_props);

        /* Baseline: no padding. */
        ERNode* bare_text = er_node_create(ER_NODE_TEXT);
        ERRect bare_rect = {-1, -1, -1, -1};
        ERProps bare_text_props = props_default();
        bare_text_props.font_size = 14;
        strncpy(bare_text_props.text, k_word, ER_TEXT_MAX);
        er_node_set_props(bare_text, &bare_text_props);
        er_event_set(bare_text, ER_EVENT_LAYOUT, on_layout_rect, &bare_rect);

        /* Uniform shorthand: +8 on all four edges. */
        ERNode* padded_text = er_node_create(ER_NODE_TEXT);
        ERRect padded_rect = {-1, -1, -1, -1};
        ERProps padded_text_props = props_default();
        padded_text_props.font_size = 14;
        padded_text_props.padding = 8;
        strncpy(padded_text_props.text, k_word, ER_TEXT_MAX);
        er_node_set_props(padded_text, &padded_text_props);
        er_event_set(padded_text, ER_EVENT_LAYOUT, on_layout_rect, &padded_rect);

        /* Per-edge + paddingHorizontal, asymmetric so a swapped axis cannot pass. */
        ERNode* edges_text = er_node_create(ER_NODE_TEXT);
        ERRect edges_rect = {-1, -1, -1, -1};
        ERProps edges_text_props = props_default();
        edges_text_props.font_size = 14;
        edges_text_props.padding_horizontal = 10;
        edges_text_props.padding_top = 3;
        edges_text_props.padding_bottom = 7;
        strncpy(edges_text_props.text, k_word, ER_TEXT_MAX);
        er_node_set_props(edges_text, &edges_text_props);
        er_event_set(edges_text, ER_EVENT_LAYOUT, on_layout_rect, &edges_rect);

        /* Explicit size: padding lives INSIDE it (border-box), as everywhere else here. */
        ERNode* fixed_text = er_node_create(ER_NODE_TEXT);
        ERRect fixed_rect = {-1, -1, -1, -1};
        ERProps fixed_text_props = props_default();
        fixed_text_props.font_size = 14;
        fixed_text_props.width = 100;
        fixed_text_props.height = 40;
        fixed_text_props.padding = 8;
        strncpy(fixed_text_props.text, k_word, ER_TEXT_MAX);
        er_node_set_props(fixed_text, &fixed_text_props);
        er_event_set(fixed_text, ER_EVENT_LAYOUT, on_layout_rect, &fixed_rect);

        er_tree_append_child(text_padding_root, bare_text);
        er_tree_append_child(text_padding_root, padded_text);
        er_tree_append_child(text_padding_root, edges_text);
        er_tree_append_child(text_padding_root, fixed_text);
        er_tree_set_root(text_padding_root);
        er_commit();

        if (bare_rect.w <= 0 || bare_rect.h <= 0)
            return fail("text padding: unpadded baseline measured non-positive");
        if (padded_rect.w != bare_rect.w + 16)
            return fail("text padding: padding=8 did not widen the auto text node by 16");
        if (padded_rect.h != bare_rect.h + 16)
            return fail("text padding: padding=8 did not heighten the auto text node by 16");
        if (edges_rect.w != bare_rect.w + 20)
            return fail("text padding: paddingHorizontal=10 did not widen the text node by 20");
        if (edges_rect.h != bare_rect.h + 10)
            return fail("text padding: paddingTop=3 + paddingBottom=7 did not add 10 to the height");
        if (fixed_rect.w != 100 || fixed_rect.h != 40)
            return fail("text padding: an explicit width/height must already include its padding");

        er_tree_remove_child(text_padding_root, bare_text);
        er_tree_remove_child(text_padding_root, padded_text);
        er_tree_remove_child(text_padding_root, edges_text);
        er_tree_remove_child(text_padding_root, fixed_text);
        er_node_destroy(bare_text);
        er_node_destroy(padded_text);
        er_node_destroy(edges_text);
        er_node_destroy(fixed_text);
        er_node_destroy(text_padding_root);
    }

    /* -----------------------------------------------------------------------
     * Test: overflow:scroll — flex_shrink does not squish children to viewport.
     *
     * ScrollView 200×100 (flex_direction=col) contains 3 children each h=60.
     * Total content = 180 px, viewport = 100 px.  With flex_shrink=1 on each
     * child, the old code would shrink each child to ~33 px.  With the fix
     * children must keep their explicit 60 px height and the content size must
     * be reported as 180 px.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* scroll_view = er_node_create(ER_NODE_SCROLL_VIEW);
        ERProps scroll_view_props = props_default();
        scroll_view_props.width = 200;
        scroll_view_props.height = 100;
        scroll_view_props.flex_direction = ER_FLEX_COL;
        scroll_view_props.overflow = ER_OVERFLOW_SCROLL;
        er_node_set_props(scroll_view, &scroll_view_props);

        ERNode* kids[3];
        ERRect krect[3];
        for (int kid_index = 0; kid_index < 3; kid_index++)
        {
            kids[kid_index] = er_node_create(ER_NODE_VIEW);
            ERProps kids_props = props_default();
            kids_props.width = 200;
            kids_props.height = 60;
            kids_props.flex_shrink = 1; /* would shrink without the overflow:scroll fix */
            er_node_set_props(kids[kid_index], &kids_props);
            krect[kid_index] = (ERRect){-1, -1, -1, -1};
            er_event_set(kids[kid_index], ER_EVENT_LAYOUT, on_layout_rect, &krect[kid_index]);
            er_tree_append_child(scroll_view, kids[kid_index]);
        }

        er_tree_set_root(scroll_view);
        er_commit();

        /* Each child must keep its explicit 60 px height (no shrinking). */
        for (int kid_index = 0; kid_index < 3; kid_index++)
        {
            if (krect[kid_index].h != 60)
                return fail("overflow:scroll: child was shrunk to fit the viewport (flex_shrink not suppressed)");
        }
        /* Children stack starting at y=0 (ScrollView is root, placed at origin). */
        if (krect[0].y != 0)
            return fail("overflow:scroll: first child y should be 0");
        if (krect[1].y != 60)
            return fail("overflow:scroll: second child y should be 60");
        if (krect[2].y != 120)
            return fail("overflow:scroll: third child y should be 120");

        for (int kid_index = 0; kid_index < 3; kid_index++)
        {
            er_tree_remove_child(scroll_view, kids[kid_index]);
            er_node_destroy(kids[kid_index]);
        }
        er_node_destroy(scroll_view);
    }

    /* -----------------------------------------------------------------------
     * Test: layout-dirty fast path — er_commit() re-runs layout only when a
     * prop set, tree mutation, or LayoutAnimation requires it. Static frames
     * and animation-only frames must skip the flex + text-measure pass.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* dirty_root = er_node_create(ER_NODE_VIEW);
        ERProps dirty_root_props = props_default();
        dirty_root_props.width = 200;
        dirty_root_props.height = 200;
        dirty_root_props.flex_direction = ER_FLEX_COL;
        dirty_root_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(dirty_root, &dirty_root_props);

        ERNode* dirty_child = er_node_create(ER_NODE_VIEW);
        ERRect dirty_rect = {-1, -1, -1, -1};
        dirty_root_props = props_default();
        dirty_root_props.width = 80;
        dirty_root_props.height = 40;
        er_node_set_props(dirty_child, &dirty_root_props);
        er_event_set(dirty_child, ER_EVENT_LAYOUT, on_layout_rect, &dirty_rect);

        er_tree_append_child(dirty_root, dirty_child);
        er_tree_set_root(dirty_root);

        /* First commit must run a layout pass (tree was just built). */
        const uint32_t first_passes = er_layout_pass_count();
        er_commit();
        const uint32_t second_passes = er_layout_pass_count();
        if (second_passes != first_passes + 1)
            return fail("layout-dirty: first commit did not run exactly one layout pass");
        if (dirty_rect.w != 80 || dirty_rect.h != 40)
            return fail("layout-dirty: child rect wrong after first commit");

        /* A commit with no mutations must take the fast path (no layout pass). */
        er_commit();
        if (er_layout_pass_count() != second_passes)
            return fail("layout-dirty: static commit re-ran layout instead of skipping");

        /* Several idle commits in a row stay on the fast path. */
        er_commit();
        er_commit();
        if (er_layout_pass_count() != second_passes)
            return fail("layout-dirty: repeated idle commits re-ran layout");

        /* Computed rect must survive the skipped commits unchanged. */
        dirty_rect = (ERRect){-1, -1, -1, -1};
        er_event_set(dirty_child, ER_EVENT_LAYOUT, on_layout_rect, &dirty_rect);
        /* (No layout fired during skips, so dirty_rect stays at the sentinel — that itself
         * confirms ER_EVENT_LAYOUT did not fire when layout was skipped.) */
        if (dirty_rect.w != -1)
            return fail("layout-dirty: ER_EVENT_LAYOUT fired on a skipped commit");

        /* An animation mutates render-only props; it must NOT trigger a layout pass. */
        ERAnimConfig animation_config;
        memset(&animation_config, 0, sizeof(animation_config));
        animation_config.type = ER_ANIM_TIMING;
        animation_config.duration_ms = 100U;
        er_anim_start(dirty_child, ER_PROP_OPACITY, 0.0f, &animation_config);
        const uint32_t passes_before_animation = er_layout_pass_count();
        for (int frame_index = 0; frame_index < 5; frame_index++)
        {
            embedded_renderer_tick(16U); /* advances the animation */
            er_commit();
        }
        if (er_layout_pass_count() != passes_before_animation)
            return fail("layout-dirty: an opacity animation forced a layout pass");

        /* Changing a layout-affecting prop must request a fresh layout pass. */
        ERProps grow = props_default();
        grow.width = 120;
        grow.height = 50;
        er_node_set_props(dirty_child, &grow);
        const uint32_t passes_before_mutation = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != passes_before_mutation + 1)
            return fail("layout-dirty: prop change did not trigger a layout pass");
        if (dirty_rect.w != 120 || dirty_rect.h != 50)
            return fail("layout-dirty: child rect did not update after prop change");

        /* Back to idle — no further layout passes. */
        er_commit();
        if (er_layout_pass_count() != passes_before_mutation + 1)
            return fail("layout-dirty: commit after prop-change relayout did not return to fast path");

        /* A pending LayoutAnimation must force a layout pass even with no prop change. */
        const uint32_t passes_before_layout_animation = er_layout_pass_count();
        er_layout_anim_configure_next(&ER_LAYOUT_ANIM_EASE_IN_EASE_OUT);
        er_commit();
        if (er_layout_pass_count() != passes_before_layout_animation + 1)
            return fail("layout-dirty: pending LayoutAnimation did not force a layout pass");

        er_tree_remove_child(dirty_root, dirty_child);
        er_node_destroy(dirty_child);
        er_node_destroy(dirty_root);
    }

    /* -----------------------------------------------------------------------
     * Test: er_node_set_text_spans vs the global layout pass.
     *
     * Span text feeds a Text node's intrinsic size, so changing it must re-solve
     * layout — unless the node's box cannot change with the glyphs.  A pinned
     * width + single line produces the same rect for every string, so it must
     * take the fast path; content-sized and pinned-but-multi-line must not.
     * Guards the imperative-readout path (updateText on a dial's centre number
     * every drag move): a ~5.7 ms pass on ~90% of drag frames on an ESP32-S3.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* spans_root = er_node_create(ER_NODE_VIEW);
        ERProps spans_root_props = props_default();
        spans_root_props.width = 200;
        spans_root_props.height = 100;
        er_node_set_props(spans_root, &spans_root_props);

        ERNode* spans_auto = er_node_create(ER_NODE_TEXT); /* content-sized */
        spans_root_props = props_default();
        spans_root_props.font_size = 16;
        er_node_set_props(spans_auto, &spans_root_props);

        ERNode* spans_pinned = er_node_create(ER_NODE_TEXT); /* pinned width, single line */
        spans_root_props = props_default();
        spans_root_props.font_size = 16;
        spans_root_props.width = 120;
        spans_root_props.number_of_lines = 1;
        er_node_set_props(spans_pinned, &spans_root_props);

        ERNode* spans_multi = er_node_create(ER_NODE_TEXT); /* pinned width, wraps → height varies */
        spans_root_props = props_default();
        spans_root_props.font_size = 16;
        spans_root_props.width = 120;
        spans_root_props.number_of_lines = 0;
        er_node_set_props(spans_multi, &spans_root_props);

        er_tree_append_child(spans_root, spans_auto);
        er_tree_append_child(spans_root, spans_pinned);
        er_tree_append_child(spans_root, spans_multi);
        er_tree_set_root(spans_root);
        er_commit(); /* settle */

        ERTextSpan span;
        memset(&span, 0, sizeof(span));

        strncpy(span.text, "72", ER_SPAN_TEXT_MAX);
        er_node_set_text_spans(spans_auto, &span, 1);
        uint32_t passes = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != passes + 1)
            return fail("text-spans: content-sized Text did not re-run layout");

        strncpy(span.text, "68", ER_SPAN_TEXT_MAX);
        er_node_set_text_spans(spans_pinned, &span, 1);
        passes = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != passes)
            return fail("text-spans: pinned single-line Text forced a global layout pass");

        strncpy(span.text, "a much longer string that has to wrap", ER_SPAN_TEXT_MAX);
        er_node_set_text_spans(spans_multi, &span, 1);
        passes = er_layout_pass_count();
        er_commit();
        if (er_layout_pass_count() != passes + 1)
            return fail("text-spans: pinned multi-line Text skipped a needed layout pass");

        er_tree_remove_child(spans_root, spans_auto);
        er_tree_remove_child(spans_root, spans_pinned);
        er_tree_remove_child(spans_root, spans_multi);
        er_node_destroy(spans_auto);
        er_node_destroy(spans_pinned);
        er_node_destroy(spans_multi);
        er_node_destroy(spans_root);
    }

    /* -----------------------------------------------------------------------
     * Test: container auto cross-size — a row with no explicit height grows to
     * fit its children instead of collapsing to 0.  Regression for the Flow A
     * demo where a row of cards rendered but was not hit-testable because the
     * row had height 0 (hit-testing descends through the row's empty bounds).
     *
     * Column root 200×200 (align stretch).  Child "row" (flex_direction=row, no
     * height) holds two children with flex:1 (flex_basis 0) and height=60.  The
     * row must report height 60 and stretch to width 200, and each child must
     * land within the row's vertical bounds.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* cross_root = er_node_create(ER_NODE_VIEW);
        ERProps cross_root_props = props_default();
        cross_root_props.width = 200;
        cross_root_props.height = 200;
        cross_root_props.flex_direction = ER_FLEX_COL;
        cross_root_props.align_items = ER_ALIGN_STRETCH;
        er_node_set_props(cross_root, &cross_root_props);

        ERNode* cross_row = er_node_create(ER_NODE_VIEW);
        ERRect cross_row_rect = {-1, -1, -1, -1};
        cross_root_props = props_default();
        cross_root_props.flex_direction = ER_FLEX_ROW; /* no height — must be derived from children */
        er_node_set_props(cross_row, &cross_root_props);
        er_event_set(cross_row, ER_EVENT_LAYOUT, on_layout_rect, &cross_row_rect);
        er_tree_append_child(cross_root, cross_row);

        ERNode* cross_kids[2];
        ERRect cross_kid_rects[2];
        for (int kid_index = 0; kid_index < 2; kid_index++)
        {
            cross_kids[kid_index] = er_node_create(ER_NODE_VIEW);
            ERProps kid_props = props_default();
            kid_props.flex_grow = 1;
            kid_props.flex_shrink = 1;
            kid_props.flex_basis = 0;
            kid_props.height = 60;
            er_node_set_props(cross_kids[kid_index], &kid_props);
            cross_kid_rects[kid_index] = (ERRect){-1, -1, -1, -1};
            er_event_set(cross_kids[kid_index], ER_EVENT_LAYOUT, on_layout_rect, &cross_kid_rects[kid_index]);
            er_tree_append_child(cross_row, cross_kids[kid_index]);
        }

        er_tree_set_root(cross_root);
        er_commit();

        if (cross_row_rect.h != 60)
            return fail("auto cross-size: row should grow to its children's height (60), not collapse to 0");
        if (cross_row_rect.w != 200)
            return fail("auto cross-size: row should stretch to the column's width (200)");
        for (int kid_index = 0; kid_index < 2; kid_index++)
        {
            if (cross_kid_rects[kid_index].h != 60)
                return fail("auto cross-size: child height should be 60");
            if (cross_kid_rects[kid_index].y < cross_row_rect.y
                || cross_kid_rects[kid_index].y + cross_kid_rects[kid_index].h > cross_row_rect.y + cross_row_rect.h)
                return fail("auto cross-size: child falls outside the row's bounds (not hit-testable)");
        }

        for (int kid_index = 0; kid_index < 2; kid_index++)
        {
            er_tree_remove_child(cross_row, cross_kids[kid_index]);
            er_node_destroy(cross_kids[kid_index]);
        }
        er_tree_remove_child(cross_root, cross_row);
        er_node_destroy(cross_row);
        er_node_destroy(cross_root);
    }

    /* -----------------------------------------------------------------------
     * Test: er_tree_insert_before — ordered insertion, reordering (move), and
     * the NULL/non-child append fallback. Order is observed through the stacked
     * y positions of a column's children.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* insert_root = er_node_create(ER_NODE_VIEW);
        ERProps insert_root_props = props_default();
        insert_root_props.width = 100;
        insert_root_props.height = 100;
        insert_root_props.flex_direction = ER_FLEX_COL;
        insert_root_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(insert_root, &insert_root_props);

        ERNode* first = er_node_create(ER_NODE_VIEW);
        ERNode* second = er_node_create(ER_NODE_VIEW);
        ERNode* third = er_node_create(ER_NODE_VIEW);
        ERNode* fourth = er_node_create(ER_NODE_VIEW);
        ERRect first_rect = {0}, second_rect = {0}, third_rect = {0}, fourth_rect = {0};
        insert_root_props = props_default();
        insert_root_props.width = 40;
        insert_root_props.height = 20;
        er_node_set_props(first, &insert_root_props);
        er_node_set_props(second, &insert_root_props);
        er_node_set_props(third, &insert_root_props);
        er_node_set_props(fourth, &insert_root_props);
        er_event_set(first, ER_EVENT_LAYOUT, on_layout_rect, &first_rect);
        er_event_set(second, ER_EVENT_LAYOUT, on_layout_rect, &second_rect);
        er_event_set(third, ER_EVENT_LAYOUT, on_layout_rect, &third_rect);
        er_event_set(fourth, ER_EVENT_LAYOUT, on_layout_rect, &fourth_rect);

        /* Insert c before b in [a, b] → order a, c, b. */
        er_tree_append_child(insert_root, first);
        er_tree_append_child(insert_root, second);
        er_tree_insert_before(insert_root, third, second);
        er_tree_set_root(insert_root);
        er_commit();
        if (first_rect.y != 0 || third_rect.y != 20 || second_rect.y != 40)
            return fail("insert_before: ordered insert produced wrong order (expected a,c,b)");

        /* Move existing b before a → order b, a, c. */
        er_tree_insert_before(insert_root, second, first);
        er_commit();
        if (second_rect.y != 0 || first_rect.y != 20 || third_rect.y != 40)
            return fail("insert_before: move of existing child produced wrong order (expected b,a,c)");

        /* before == NULL appends → order b, a, c, d. */
        er_tree_insert_before(insert_root, fourth, NULL);
        er_commit();
        if (fourth_rect.y != 60)
            return fail("insert_before: NULL anchor should append at the end");

        er_tree_remove_child(insert_root, first);
        er_tree_remove_child(insert_root, second);
        er_tree_remove_child(insert_root, third);
        er_tree_remove_child(insert_root, fourth);
        er_node_destroy(first);
        er_node_destroy(second);
        er_node_destroy(third);
        er_node_destroy(fourth);
        er_node_destroy(insert_root);
    }

    /* -----------------------------------------------------------------------
     * Test: nested explicit sizes — a fixed-size child of the root keeps its
     * height, and its grandchildren stack at their own heights (regression for
     * the reconciler reorder demo where a 100x200 child filled the 480x320 root
     * and its 20px items spread to ~106px each).
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* nested_root = er_node_create(ER_NODE_VIEW);
        ERProps nested_root_props = props_default();
        nested_root_props.width = 480;
        nested_root_props.height = 320;
        nested_root_props.flex_direction = ER_FLEX_COL;
        er_node_set_props(nested_root, &nested_root_props);

        ERNode* nested_child = er_node_create(ER_NODE_VIEW);
        ERRect nested_child_rect = {0};
        nested_root_props = props_default();
        nested_root_props.width = 100;
        nested_root_props.height = 200;
        nested_root_props.flex_direction = ER_FLEX_COL;
        er_node_set_props(nested_child, &nested_root_props);
        er_event_set(nested_child, ER_EVENT_LAYOUT, on_layout_rect, &nested_child_rect);
        er_tree_append_child(nested_root, nested_child);

        ERNode* nested_items[3];
        ERRect nested_item_rects[3];
        for (int item_index = 0; item_index < 3; item_index++)
        {
            nested_items[item_index] = er_node_create(ER_NODE_VIEW);
            ERProps nested_items_props = props_default();
            nested_items_props.width = 40;
            nested_items_props.height = 20;
            er_node_set_props(nested_items[item_index], &nested_items_props);
            nested_item_rects[item_index] = (ERRect){-1, -1, -1, -1};
            er_event_set(nested_items[item_index], ER_EVENT_LAYOUT, on_layout_rect, &nested_item_rects[item_index]);
            er_tree_append_child(nested_child, nested_items[item_index]);
        }

        er_tree_set_root(nested_root);
        er_commit();

        if (nested_child_rect.h != 200)
            return fail("nested explicit size: fixed-height child should stay 200, not fill the root");
        if (nested_item_rects[0].y != 0 || nested_item_rects[1].y != 20 || nested_item_rects[2].y != 40)
            return fail("nested explicit size: grandchildren should stack at 0,20,40");
        if (nested_item_rects[0].h != 20)
            return fail("nested explicit size: grandchild height should stay 20");

        for (int item_index = 0; item_index < 3; item_index++)
        {
            er_tree_remove_child(nested_child, nested_items[item_index]);
            er_node_destroy(nested_items[item_index]);
        }
        er_tree_remove_child(nested_root, nested_child);
        er_node_destroy(nested_child);
        er_node_destroy(nested_root);
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() memoisation — a Text leaf must be measured once
     * per layout pass, not once per ancestor depth. Regression for issue #51:
     * Pass 1 called measure_content(child) for every in-flow child and that
     * recursed the child's entire subtree, so before memoisation a leaf D
     * levels deep was remeasured D times as compute_layout descended one
     * level at a time (compute_layout re-enters Pass 1 at every level, and
     * nothing cached the result of the level above).
     *
     * A chain of CACHE_DEPTH auto-sized Views under a fixed root, ending in one
     * auto-sized Text leaf, must produce exactly one er_text_measure*() call
     * per commit — and, since the memoisation cache is keyed by a per-pass
     * generation, a second commit after a real layout-affecting change must
     * remeasure it again exactly once (not zero, and not accumulate stale
     * counts from the first pass).
     * -----------------------------------------------------------------------*/
    {
#define CACHE_DEPTH 8
        er_reset();

        ERNode* cache_root = er_node_create(ER_NODE_VIEW);
        ERProps cache_root_props = props_default();
        cache_root_props.width = 200;
        cache_root_props.height = 200;
        er_node_set_props(cache_root, &cache_root_props);
        er_tree_set_root(cache_root);

        ERNode* cache_chain[CACHE_DEPTH];
        ERNode* cache_parent = cache_root;
        for (int depth_index = 0; depth_index < CACHE_DEPTH; depth_index++)
        {
            cache_chain[depth_index] = er_node_create(ER_NODE_VIEW);
            ERProps cache_chain_props = props_default(); /* width/height stay AUTO */
            er_node_set_props(cache_chain[depth_index], &cache_chain_props);
            er_tree_append_child(cache_parent, cache_chain[depth_index]);
            cache_parent = cache_chain[depth_index];
        }

        ERNode* cache_text = er_node_create(ER_NODE_TEXT);
        ERProps cache_text_props = props_default(); /* width/height stay AUTO */
        cache_text_props.font_size = 16;
        strncpy(cache_text_props.text, "measure me", ER_TEXT_MAX);
        er_node_set_props(cache_text, &cache_text_props);
        er_tree_append_child(cache_parent, cache_text);

        const uint32_t measures_initial = er_text_measure_count();
        er_commit();
        const uint32_t measures_unchanged = er_text_measure_count();
        if (measures_unchanged != measures_initial + 1U)
            return fail("measure-cache: deep chain remeasured the Text leaf more than once per pass");

        /* A real layout-affecting change must invalidate the per-pass cache and remeasure —
         * exactly once, proving the cache doesn't leak a stale hit into the next pass. */
        cache_root_props.width = 210;
        er_node_set_props(cache_root, &cache_root_props);
        er_commit();
        const uint32_t measures_changed = er_text_measure_count();
        if (measures_changed != measures_unchanged + 1U)
            return fail("measure-cache: second layout pass did not remeasure the Text leaf exactly once");

        er_tree_remove_child(cache_parent, cache_text);
        er_node_destroy(cache_text);
        for (int depth_index = CACHE_DEPTH - 1; depth_index >= 0; depth_index--)
        {
            ERNode* parent = (depth_index == 0) ? cache_root : cache_chain[depth_index - 1];
            er_tree_remove_child(parent, cache_chain[depth_index]);
            er_node_destroy(cache_chain[depth_index]);
        }
        er_node_destroy(cache_root);
#undef CACHE_DEPTH
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() memoisation — a wide (not deep) tree. Even a
     * single container with many auto-sized Text siblings used to measure
     * each one twice per commit: once while the container computed its own
     * intrinsic content size (summing each child's measurement), and once
     * more when compute_layout's Pass 1 ran for real on that same container.
     * With the cache, the second lookup is a hit — exactly one measurement
     * per Text sibling per commit.
     * -----------------------------------------------------------------------*/
    {
#define CACHE_WIDE_COUNT 6
        er_reset();

        ERNode* wide_root = er_node_create(ER_NODE_VIEW);
        ERProps wide_root_props = props_default();
        wide_root_props.width = 300;
        wide_root_props.height = 300;
        er_node_set_props(wide_root, &wide_root_props);

        ERNode* wide_container = er_node_create(ER_NODE_VIEW);
        ERProps wide_container_props = props_default(); /* width/height stay AUTO */
        er_node_set_props(wide_container, &wide_container_props);
        er_tree_append_child(wide_root, wide_container);

        ERNode* wide_texts[CACHE_WIDE_COUNT];
        for (int text_index = 0; text_index < CACHE_WIDE_COUNT; text_index++)
        {
            wide_texts[text_index] = er_node_create(ER_NODE_TEXT);
            ERProps wide_texts_props = props_default(); /* width/height stay AUTO */
            wide_texts_props.font_size = 16;
            strncpy(wide_texts_props.text, "sib", ER_TEXT_MAX);
            er_node_set_props(wide_texts[text_index], &wide_texts_props);
            er_tree_append_child(wide_container, wide_texts[text_index]);
        }
        er_tree_set_root(wide_root);

        const uint32_t wide_measures_before = er_text_measure_count();
        er_commit();
        const uint32_t wide_measures_after = er_text_measure_count();
        if (wide_measures_after != wide_measures_before + (uint32_t)CACHE_WIDE_COUNT)
            return fail("measure-cache: wide tree measured a Text sibling more than once per commit");

        for (int text_index = 0; text_index < CACHE_WIDE_COUNT; text_index++)
        {
            er_tree_remove_child(wide_container, wide_texts[text_index]);
            er_node_destroy(wide_texts[text_index]);
        }
        er_tree_remove_child(wide_root, wide_container);
        er_node_destroy(wide_container);
        er_node_destroy(wide_root);
#undef CACHE_WIDE_COUNT
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() short-circuit — a Text node with both width and
     * height explicit never needs its glyph run measured (the measured value
     * is not consulted for either axis), even nested inside auto-sized
     * ancestors that themselves must be measured. Zero er_text_measure*()
     * calls for the whole commit.
     * -----------------------------------------------------------------------*/
    {
        er_reset();

        ERNode* short_circuit_root = er_node_create(ER_NODE_VIEW);
        ERProps short_circuit_root_props = props_default();
        short_circuit_root_props.width = 200;
        short_circuit_root_props.height = 200;
        er_node_set_props(short_circuit_root, &short_circuit_root_props);

        ERNode* short_circuit_mid = er_node_create(ER_NODE_VIEW);
        ERProps short_circuit_mid_props =
            props_default(); /* auto — forces short_circuit_root's Pass 1 to measure short_circuit_mid */
        er_node_set_props(short_circuit_mid, &short_circuit_mid_props);
        er_tree_append_child(short_circuit_root, short_circuit_mid);

        ERNode* short_circuit_text = er_node_create(ER_NODE_TEXT);
        ERProps short_circuit_text_props = props_default();
        short_circuit_text_props.width = 60;
        short_circuit_text_props.height = 20;
        strncpy(short_circuit_text_props.text, "fixed size, never measured", ER_TEXT_MAX);
        er_node_set_props(short_circuit_text, &short_circuit_text_props);
        er_tree_append_child(short_circuit_mid, short_circuit_text);
        er_tree_set_root(short_circuit_root);

        const uint32_t short_circuit_measures_before = er_text_measure_count();
        er_commit();
        const uint32_t short_circuit_measures_after = er_text_measure_count();
        if (short_circuit_measures_after != short_circuit_measures_before)
            return fail("measure-cache: fully-explicit Text size still triggered a glyph-run measurement");

        er_tree_remove_child(short_circuit_mid, short_circuit_text);
        er_node_destroy(short_circuit_text);
        er_tree_remove_child(short_circuit_root, short_circuit_mid);
        er_node_destroy(short_circuit_mid);
        er_node_destroy(short_circuit_root);
    }

    /* -----------------------------------------------------------------------
     * Test: an auto-height Text is as tall as the lines it wraps into at the
     * width it gets, capped by numberOfLines — and a one-line text stays one
     * line tall even with numberOfLines set. Expected sizes come from
     * er_text_measure_lines(), the breaker er_text_render() draws with, so
     * they hold whatever font the build ships.
     * -----------------------------------------------------------------------*/
    {
        static const char* k_description = "A description long enough to wrap onto four or five lines at this width";
        int dummy = 0, line_height = 0;
        er_text_measure("Save", 14, NULL, 0, 0, &dummy, &line_height);
        int description_width_120 = 0;
        const int description_lines_120 = lines_at(k_description, 120, &description_width_120);
        if (line_height <= 0 || description_lines_120 < 3)
            return fail("text wrap: the long description should break onto at least three lines at 120 px");

        er_reset();
        ERNode* wrap_root = er_node_create(ER_NODE_VIEW);
        ERProps wrap_root_props = props_default();
        wrap_root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
        wrap_root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
        wrap_root_props.flex_direction = ER_FLEX_COL;
        wrap_root_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(wrap_root, &wrap_root_props);

        /* <View style={{width: 120}}> with three Texts, stretched to the column's width. */
        ERNode* column = er_node_create(ER_NODE_VIEW);
        ERProps column_props = props_default();
        column_props.width = 120;
        er_node_set_props(column, &column_props);
        ERRect save_rect, description_rect, short_rect, clip_rect;
        ERNode* save_text = mk_text(props_default(), "Save", &save_rect);
        ERNode* description_text = mk_text(props_default(), k_description, &description_rect);
        ERProps short_props = props_default();
        short_props.number_of_lines = 3;
        ERNode* short_text = mk_text(short_props, "Short", &short_rect);
        ERProps clip_props = props_default();
        clip_props.number_of_lines = 2;
        ERNode* clip_text = mk_text(clip_props, k_description, &clip_rect);
        er_tree_append_child(column, save_text);
        er_tree_append_child(column, description_text);
        er_tree_append_child(column, short_text);
        er_tree_append_child(column, clip_text);
        er_tree_append_child(wrap_root, column);

        /* A flex: 1 Text beside a 200 px box in a 300 px row gets 100 px and wraps there. */
        int description_width_100 = 0;
        const int description_lines_100 = lines_at(k_description, 100, &description_width_100);
        ERNode* row = er_node_create(ER_NODE_VIEW);
        ERRect row_rect = {-1, -1, -1, -1};
        ERProps row_props = props_default();
        row_props.width = 300;
        row_props.flex_direction = ER_FLEX_ROW;
        row_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(row, &row_props);
        er_event_set(row, ER_EVENT_LAYOUT, on_layout_rect, &row_rect);
        ERProps box_props = props_default();
        box_props.width = 200;
        box_props.height = 10;
        ERNode* box = er_node_create(ER_NODE_VIEW);
        er_node_set_props(box, &box_props);
        ERProps flex_props = props_default();
        flex_props.flex_grow = 1;
        flex_props.flex_shrink = 1;
        flex_props.flex_basis = 0;
        ERRect flex_rect;
        ERNode* flex_text = mk_text(flex_props, k_description, &flex_rect);
        er_tree_append_child(row, box);
        er_tree_append_child(row, flex_text);
        er_tree_append_child(wrap_root, row);

        /* A shrink-wrapped Text in the flex-start column: as wide as its widest line, never wider than the column. */
        ERNode* narrow = er_node_create(ER_NODE_VIEW);
        ERProps narrow_props = props_default();
        narrow_props.width = 120;
        narrow_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(narrow, &narrow_props);
        ERRect wrap_rect, one_line_rect;
        ERNode* wrap_text = mk_text(props_default(), k_description, &wrap_rect);
        ERProps one_line_props = props_default();
        one_line_props.number_of_lines = 1;
        one_line_props.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
        ERNode* one_line_text = mk_text(one_line_props, k_description, &one_line_rect);
        er_tree_append_child(narrow, wrap_text);
        er_tree_append_child(narrow, one_line_text);
        er_tree_append_child(wrap_root, narrow);

        er_tree_set_root(wrap_root);
        er_commit();

        if (save_rect.w != 120 || save_rect.h != line_height)
            return fail("text wrap: a label that fits keeps its one-line height");
        if (description_rect.w != 120 || description_rect.h != description_lines_120 * line_height)
            return fail("text wrap: the long text in the 120 px column is not as tall as its wrapped lines");
        if (short_rect.y != description_rect.y + description_rect.h || short_rect.h != line_height)
            return fail("text wrap: numberOfLines={3} on a one-line label must stay one line tall");
        if (clip_rect.h != 2 * line_height)
            return fail("text wrap: numberOfLines={2} must cap a long text at two lines");
        if (flex_rect.x != 200 || flex_rect.w != 100 || flex_rect.h != description_lines_100 * line_height)
            return fail("text wrap: the flex: 1 Text must wrap at the 100 px it flexes to");
        if (row_rect.h != flex_rect.h)
            return fail("text wrap: the row must grow to hold its wrapped flex: 1 Text");
        if (wrap_rect.w != description_width_120 || wrap_rect.h != description_lines_120 * line_height)
            return fail("text wrap: a shrink-wrapped Text should take its widest line at the width it is offered");
        if (one_line_rect.w > 120 || one_line_rect.h != line_height)
            return fail("text wrap: numberOfLines={1} must stay inside the column it would overflow");

        /* The same row without its own width takes the column's, and still grows to hold the Text. */
        row_props.width = ER_LAYOUT_AUTO;
        row_props.flex_direction = ER_FLEX_ROW;
        er_node_set_props(row, &row_props);
        wrap_root_props.align_items = ER_ALIGN_STRETCH;
        wrap_root_props.width = 300;
        er_node_set_props(wrap_root, &wrap_root_props);
        er_commit();
        if (row_rect.w != 300 || flex_rect.w != 100 || flex_rect.h != description_lines_100 * line_height
            || row_rect.h != flex_rect.h)
            return fail("text wrap: an auto-width row must flex and wrap its Text at the width its parent offers");

        er_reset();
    }

    /* -----------------------------------------------------------------------
     * Test: a row measures its children at most its own width, as Yoga does,
     * so a long Text that does not flex still wraps at the row's edge. Beside
     * a fixed box it keeps that width and overflows, since flexShrink is 0.
     * A row that scrolls offers no bound, and its Text stays on one line.
     * -----------------------------------------------------------------------*/
    {
        static const char* k_description = "A description long enough to wrap onto four or five lines at this width";
        int dummy = 0, line_height = 0, width_120 = 0, width_300 = 0;
        er_text_measure("Save", 14, NULL, 0, 0, &dummy, &line_height);
        const int lines120 = lines_at(k_description, 120, &width_120);
        const int lines300 = lines_at(k_description, 300, &width_300);
        if (lines120 < 3 || lines300 < 2)
            return fail("row text: the long description should wrap at both 120 px and 300 px");

        er_reset();
        ERNode* bound_root = er_node_create(ER_NODE_VIEW);
        ERProps bound_root_props = props_default();
        bound_root_props.width = (int16_t)FRAMEBUFFER_WIDTH;
        bound_root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
        bound_root_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(bound_root, &bound_root_props);

        ERProps narrow_row_props = props_default();
        narrow_row_props.flex_direction = ER_FLEX_ROW;
        narrow_row_props.align_items = ER_ALIGN_FLEX_START;

        narrow_row_props.width = 120;
        ERNode* narrow_row = er_node_create(ER_NODE_VIEW);
        er_node_set_props(narrow_row, &narrow_row_props);
        ERRect narrow_rect;
        er_tree_append_child(narrow_row, mk_text(props_default(), k_description, &narrow_rect));
        er_tree_append_child(bound_root, narrow_row);

        narrow_row_props.width = 300;
        ERNode* boxed_row = er_node_create(ER_NODE_VIEW);
        er_node_set_props(boxed_row, &narrow_row_props);
        ERNode* bound_box = er_node_create(ER_NODE_VIEW);
        ERProps bound_box_props = props_default();
        bound_box_props.width = 200;
        bound_box_props.height = 10;
        er_node_set_props(bound_box, &bound_box_props);
        ERRect boxed_rect;
        er_tree_append_child(boxed_row, bound_box);
        er_tree_append_child(boxed_row, mk_text(props_default(), k_description, &boxed_rect));
        er_tree_append_child(bound_root, boxed_row);

        narrow_row_props.width = 120;
        narrow_row_props.overflow = ER_OVERFLOW_SCROLL;
        ERNode* scroll_row = er_node_create(ER_NODE_VIEW);
        er_node_set_props(scroll_row, &narrow_row_props);
        ERRect scroll_rect;
        er_tree_append_child(scroll_row, mk_text(props_default(), k_description, &scroll_rect));
        er_tree_append_child(bound_root, scroll_row);

        er_tree_set_root(bound_root);
        er_commit();

        if (narrow_rect.w != width_120 || narrow_rect.h != lines120 * line_height)
            return fail("row text: a long Text in a 120 px row must wrap at the row's edge");
        if (boxed_rect.x != 200 || boxed_rect.w != width_300 || boxed_rect.h != lines300 * line_height)
            return fail("row text: beside a box, a non-shrinking Text keeps the width it wraps at in the row");
        if (scroll_rect.h != line_height || scroll_rect.w <= 120)
            return fail("row text: a scrolling row must leave its Text on one line");

        er_reset();
    }

    /* -----------------------------------------------------------------------
     * Test: measure_content() memoisation across a flexed row. Measuring the
     * row flexes it once and laying it out flexes it again; the Text is
     * measured at the row's width for its height, then at its flexed width,
     * and the cache answers every other ask — 3 calls: the single-line
     * measure at each width, plus breaking it into lines at 100 px.
     * -----------------------------------------------------------------------*/
    {
        er_reset();
        ERNode* flex_row_root = er_node_create(ER_NODE_VIEW);
        ERProps flex_row_root_props = props_default();
        flex_row_root_props.width = 300;
        flex_row_root_props.height = (int16_t)FRAMEBUFFER_HEIGHT;
        er_node_set_props(flex_row_root, &flex_row_root_props);

        ERNode* flex_row = er_node_create(ER_NODE_VIEW);
        ERProps flex_row_props = props_default();
        flex_row_props.flex_direction = ER_FLEX_ROW;
        flex_row_props.align_items = ER_ALIGN_FLEX_START;
        er_node_set_props(flex_row, &flex_row_props);
        ERNode* flex_row_box = er_node_create(ER_NODE_VIEW);
        ERProps flex_row_box_props = props_default();
        flex_row_box_props.width = 200;
        flex_row_box_props.height = 10;
        er_node_set_props(flex_row_box, &flex_row_box_props);
        ERProps flex_row_text_props = props_default();
        flex_row_text_props.flex_grow = 1;
        flex_row_text_props.flex_shrink = 1;
        flex_row_text_props.flex_basis = 0;
        ERRect flex_row_rect;
        ERNode* flex_row_text = mk_text(flex_row_text_props, "The same long description", &flex_row_rect);
        er_tree_append_child(flex_row, flex_row_box);
        er_tree_append_child(flex_row, flex_row_text);
        er_tree_append_child(flex_row_root, flex_row);
        er_tree_set_root(flex_row_root);

        int dummy = 0, line_height = 0;
        er_text_measure("Save", 14, NULL, 0, 0, &dummy, &line_height);
        const int flex_row_lines = lines_at("The same long description", 100, NULL);
        const uint32_t flex_measures_before = er_text_measure_count();
        er_commit();
        const uint32_t flex_measures_after = er_text_measure_count();
        if (flex_row_lines < 2 || flex_row_rect.w != 100 || flex_row_rect.h != flex_row_lines * line_height)
            return fail("text wrap: the flex: 1 Text must wrap at the 100 px it flexes to");
        if (flex_measures_after != flex_measures_before + 3U)
            return fail("measure-cache: a flexed row re-measured its Text more than once per width");

        er_reset();
    }

    return EXIT_SUCCESS;
}
