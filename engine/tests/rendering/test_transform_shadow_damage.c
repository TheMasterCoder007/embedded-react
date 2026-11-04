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
 * Regression: a transformed node that FALLS BACK to an untransformed paint must damage its shadow too.
 *
 * render_tree renders a scale/rotate node by capturing its subtree into the transform scratch and
 * inverse-mapping it out, and it deliberately skips the shadow on that path — the shadow would be
 * rasterised into the source and distorted by the blit. When the capture cannot be started, though, it
 * degrades to painting the node untransformed at its raw box, and on THAT path the shadow is rendered
 * like any other node's.
 *
 * The damage pre-pass has two branches. The plain node_screen_rect() one grows both the new footprint
 * and the vacated one by the shadow bleed (see expand_for_shadow, added for the move-leaves-its-shadow
 * fix in 97d536a). The transformed branch never did — correctly for a captured node, which casts no
 * shadow, but wrongly for a fallback one. A move therefore left the old shadow on screen and clipped
 * the new one at the damage edge.
 *
 * Asserted the way that cannot be argued with: the damage-clipped frame must be byte-identical to a
 * forced full repaint of the same scene state.
 *
 * CONFIGURATION. Both halves have to be reachable at once — the capture must fail while the shadow
 * still renders — and the two have separate size limits: er_transform_source_begin() refuses a node
 * larger than ERUI_XFORM_W/H, er_shadow_render() bails on one larger than ERUI_SCRATCH_W/H. With the
 * default ERUI_XFORM_W = ERUI_SCRATCH_W any node over the transform limit is over the shadow limit too
 * and simply casts no shadow, so the defect needs the decoupled configuration these knobs exist for
 * ("decouple from ERUI_SCRATCH_W when strips are screen-wide"). The scenarios below compile out unless
 * the build actually provides that gap; .github/workflows/ci.yml has a pass that does.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 400
#define FRAMEBUFFER_HEIGHT 400
#define OPACITY_OPAQUE 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Framebuffer the backend stubs rasterise into. */
typedef struct
{
    uint32_t* framebuffer;  /**< Flat premultiplied ARGB8888 framebuffer. */
    int framebuffer_width;  /**< Width in pixels. */
    int framebuffer_height; /**< Height in pixels. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds a 0–65025 product back to 0–255 the way the engine's blenders do. */
static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

/**
 * @brief Backend fill_rect: source-over composites a straight-alpha colour into the framebuffer.
 *
 * @param[in] argb    Straight-alpha ARGB8888 fill colour.
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U || width <= 0 || height <= 0)
        return;
    const uint32_t inverse_alpha = ER_CHANNEL_MAX - alpha;
    const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU, source_blue = argb & 0xFFU;

    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= test_context->framebuffer_height)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= test_context->framebuffer_width)
                continue;
            uint32_t* destination = &test_context->framebuffer[row * test_context->framebuffer_width + column];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((div255(source_red * alpha) + div255(destination_red * inverse_alpha)) << 16)
                           | ((div255(source_green * alpha) + div255(destination_green * inverse_alpha)) << 8)
                           | (div255(source_blue * alpha) + div255(destination_blue * inverse_alpha));
        }
    }
}

/**
 * @brief Backend copy_rect: source-over composites a premultiplied buffer into the framebuffer.
 *
 * Not a replacing blit — only the separate copy_opaque callback carries that guarantee. Overwriting
 * unconditionally would erase the background under the shadow's own antialiased edge and make the
 * comparison depend on repaint history rather than scene state, which is the whole thing under test.
 *
 * @param[in] source  Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= test_context->framebuffer_width || destination_y < 0
                || destination_y >= test_context->framebuffer_height)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            if (source_alpha == 0U)
                continue;
            uint32_t* destination =
                &test_context->framebuffer[destination_y * test_context->framebuffer_width + destination_x];
            if (source_alpha == 0xFFU)
            {
                *destination = source_pixel;
                continue;
            }
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U
                           | ((((source_pixel >> 16) & 0xFFU) + div255(destination_red * inverse_alpha)) << 16)
                           | ((((source_pixel >> 8) & 0xFFU) + div255(destination_green * inverse_alpha)) << 8)
                           | ((source_pixel & 0xFFU) + div255(destination_blue * inverse_alpha));
        }
    }
}

/**
 * @brief Backend blend_rect: source-over composites premultiplied pixels scaled by a global alpha.
 *
 * This is the callback the shadow itself arrives through, so its fidelity is what makes a missing
 * shadow band show up as a pixel difference.
 *
 * @param[in] source  Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global alpha scale 0–255.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Pointer to TestCtx.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
        {
            const int destination_x = x + column, destination_y = y + row;
            if (destination_x < 0 || destination_x >= test_context->framebuffer_width || destination_y < 0
                || destination_y >= test_context->framebuffer_height)
                continue;
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = div255(((source_pixel >> 24) & 0xFFU) * (uint32_t)alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t source_red = div255(((source_pixel >> 16) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_green = div255(((source_pixel >> 8) & 0xFFU) * (uint32_t)alpha);
            const uint32_t source_blue = div255((source_pixel & 0xFFU) * (uint32_t)alpha);
            uint32_t* destination =
                &test_context->framebuffer[destination_y * test_context->framebuffer_width + destination_x];
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                           | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                           | (source_blue + div255(destination_blue * inverse_alpha));
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Prints a failure message.
 *
 * @param[in] message  Message describing the failed assertion.
 *
 * @return EXIT_FAILURE, so callers can `return fail(...)`.
 */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/** @brief ERProps with every layout field at AUTO — the documented starting point. */
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
    props.shadow_color = 0xFF000000U;
    return props;
}

#if ERUI_SHADOWS && ERUI_TRANSFORMS_FULL && (ERUI_XFORM_W < ERUI_SCRATCH_W) && (ERUI_XFORM_H < ERUI_SCRATCH_H)
#define XFORM_SHADOW_GAP 1

/* A node in this range fails the transform capture but still gets a shadow — the only size band where
 * the two halves of the defect coexist. */
#define FALLBACK_SIDE (ERUI_XFORM_W + 1)
#define CAPTURED_SIDE (ERUI_XFORM_W / 2)

/** @brief Gives a node a shadow big enough that a clipped band is unmistakable. */
static void set_shadow(ERProps* props)
{
    props->shadow_opacity = 0.8f;
    props->shadow_radius = 10;
    props->shadow_offset_x = 6;
    props->shadow_offset_y = 6;
}

static uint32_t g_framebuffer[FRAMEBUFFER_HEIGHT * FRAMEBUFFER_WIDTH];
static uint32_t g_snapshot[FRAMEBUFFER_HEIGHT * FRAMEBUFFER_WIDTH];

/**
 * @brief Compares the saved damage-clipped frame against the framebuffer's current (full-repaint) state.
 *
 * @param[in] label  Scenario name, printed either way.
 *
 * @return Number of differing pixels (0 = the incremental frame was correct).
 */
static int compare_to_full_repaint(const char* label)
{
    int bad = 0, min_x = FRAMEBUFFER_WIDTH, min_y = FRAMEBUFFER_HEIGHT, max_x = -1, max_y = -1;
    for (int row = 0; row < FRAMEBUFFER_HEIGHT; row++)
    {
        for (int column = 0; column < FRAMEBUFFER_WIDTH; column++)
        {
            if (g_snapshot[row * FRAMEBUFFER_WIDTH + column] == g_framebuffer[row * FRAMEBUFFER_WIDTH + column])
                continue;
            bad++;
            if (column < min_x)
                min_x = column;
            if (row < min_y)
                min_y = row;
            if (column > max_x)
                max_x = column;
            if (row > max_y)
                max_y = row;
        }
    }
    if (bad)
        printf("  %-44s %d px differ, region %d,%d..%d,%d\n", label, bad, min_x, min_y, max_x, max_y);
    else
        printf("  %-44s identical\n", label);
    return bad;
}

/**
 * @brief A sibling reflow shifts a shadow-casting transformed node; the incremental frame must be exact.
 *
 * The card is scaled (so the pre-pass takes the transformed branch) and carries a shadow. Growing or
 * shrinking the bar above it moves the card WITHOUT making it source_dirty, which is precisely the case
 * that has to reason about the vacated footprint — the old shadow — on its own.
 *
 * @param[in] side               Card size; > ERUI_XFORM_W makes its scratch capture fail.
 * @param[in] bar_height_before  Bar height before the reflow.
 * @param[in] bar_height_after   Bar height after it.
 * @param[in] label              Scenario name for the report.
 *
 * @return Number of differing pixels.
 */
static int reflow_case(int side, int bar_height_before, int bar_height_after, const char* label)
{
    er_reset();
    memset(g_framebuffer, 0, sizeof g_framebuffer);

    ERNode* root = er_node_create(ER_NODE_VIEW); /* default flex column */
    ERProps root_props = props_default();
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.background_color = 0xFFFFFFFFU; /* opaque, so a missed erase really shows */
    er_node_set_props(root, &root_props);

    ERNode* bar = er_node_create(ER_NODE_VIEW);
    ERProps bar_props = props_default();
    bar_props.width = FRAMEBUFFER_WIDTH;
    bar_props.height = (int16_t)bar_height_before;
    bar_props.background_color = 0xFF333333U;
    er_node_set_props(bar, &bar_props);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps card_props = props_default();
    card_props.width = (int16_t)side;
    card_props.height = (int16_t)side;
    card_props.margin_left = 60;
    card_props.background_color = 0xFF3366FFU;
    card_props.transform_scale_x = 0.9f; /* non-translate → the transformed damage branch */
    card_props.transform_scale_y = 0.9f;
    set_shadow(&card_props);
    er_node_set_props(card, &card_props);

    er_tree_append_child(root, bar);
    er_tree_append_child(root, card);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit(); /* settle: the card has a last-painted footprint now */

    bar_props.height = (int16_t)bar_height_after;
    er_node_set_props(bar, &bar_props);
    er_commit(); /* the frame under test: damage-clipped */
    memcpy(g_snapshot, g_framebuffer, sizeof g_framebuffer);

    er_force_full_repaint();
    er_commit(); /* the reference */

    const int bad = compare_to_full_repaint(label);
    er_node_destroy(root);
    return bad;
}

/**
 * @brief The card resizes itself — source_dirty AND moved edges, both footprints shadowed.
 *
 * @param[in] from   Card size before.
 * @param[in] to     Card size after.
 * @param[in] label  Scenario name for the report.
 *
 * @return Number of differing pixels.
 */
static int resize_case(int from, int to, const char* label)
{
    er_reset();
    memset(g_framebuffer, 0, sizeof g_framebuffer);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.position = ER_POS_RELATIVE;
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    root_props.background_color = 0xFFFFFFFFU;
    er_node_set_props(root, &root_props);

    ERNode* card = er_node_create(ER_NODE_VIEW);
    ERProps card_props = props_default();
    card_props.position = ER_POS_ABSOLUTE;
    card_props.left = 60;
    card_props.top = 60;
    card_props.width = (int16_t)from;
    card_props.height = (int16_t)from;
    card_props.background_color = 0xFF3366FFU;
    card_props.transform_scale_x = 0.9f;
    card_props.transform_scale_y = 0.9f;
    set_shadow(&card_props);
    er_node_set_props(card, &card_props);

    er_tree_append_child(root, card);
    er_tree_set_root(root);

    er_force_full_repaint();
    er_commit();
    er_commit();

    card_props.width = (int16_t)to;
    card_props.height = (int16_t)to;
    er_node_set_props(card, &card_props);
    er_commit();
    memcpy(g_snapshot, g_framebuffer, sizeof g_framebuffer);

    er_force_full_repaint();
    er_commit();

    const int bad = compare_to_full_repaint(label);
    er_node_destroy(root);
    return bad;
}
#endif /* the transform/shadow size gap */

/*----------------------------------------------------------------------------------------------------------------------
 - Test
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
#ifdef XFORM_SHADOW_GAP
    TestCtx ctx = {g_framebuffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    EmbeddedRenderBackend backend = {0};
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.ctx = &ctx;
    embedded_renderer_set_backend(&backend);

    printf("transform limit %dx%d, shadow limit %dx%d → fallback side %d, captured side %d\n",
           ERUI_XFORM_W,
           ERUI_XFORM_H,
           ERUI_SCRATCH_W,
           ERUI_SCRATCH_H,
           FALLBACK_SIDE,
           CAPTURED_SIDE);

    int bad = 0;
    /* Control: small enough to capture, so it casts no shadow at all and must stay exact either way. */
    bad += reflow_case(CAPTURED_SIDE, 20, 40, "captured node, moved down (control)");
    /* The defect, in each direction the two footprints can be wrong. */
    bad += reflow_case(FALLBACK_SIDE, 20, 40, "fallback node, moved down");
    bad += reflow_case(FALLBACK_SIDE, 60, 20, "fallback node, moved up");
    bad += reflow_case(FALLBACK_SIDE, 20, 21, "fallback node, moved 1px (trail overlaps)");
    bad += resize_case(FALLBACK_SIDE, FALLBACK_SIDE - 20, "fallback node, shrank");
    bad += resize_case(FALLBACK_SIDE - 20, FALLBACK_SIDE, "fallback node, grew");

    if (bad)
        return fail("a damage-clipped frame disagreed with a full repaint — shadow bleed not damaged");

    printf("PASS: a fallback-painted transformed node damages its shadow\n");
#else
    printf("SKIP: needs ERUI_SHADOWS, ERUI_TRANSFORMS=FULL, and ERUI_XFORM_W/H < ERUI_SCRATCH_W/H\n");
#endif
    return EXIT_SUCCESS;
}
