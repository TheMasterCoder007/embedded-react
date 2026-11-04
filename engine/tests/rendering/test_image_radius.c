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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 64
#define FRAMEBUFFER_HEIGHT 64
#define CHANNEL_MAX 255u
#define CHANNEL_ROUND 127u
#define OPACITY_OPAQUE 255U

/* The node every scene draws: placed off the origin so the box, not the framebuffer, sets the corners. */
#define BOX_X 6
#define BOX_Y 5
#define BOX_WIDTH 40
#define BOX_HEIGHT 30

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test framebuffer composited source-over from every backend call.
 */
typedef struct
{
    uint32_t framebuffer[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT]; /**< Premultiplied ARGB8888 pixel storage. */
    int format_calls;                                             /**< copy_rect_fmt calls received. */
} TestCtx;

/**
 * @brief One way of drawing the image, each reaching a different path of the image scaler.
 */
typedef struct
{
    const char* what;    /**< Label for failure messages. */
    const char* image;   /**< Registered asset name. */
    bool format_backend; /**< Whether the backend offers copy_rect_fmt. */
    uint8_t resize;      /**< ERResizeMode. */
    int16_t padding;     /**< Node padding; the bitmap goes in the content box, the clip stays on the border box. */
} DrawCase;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static TestCtx s_test_context;

/* Opaque white sources: 1:1 for the box in both formats, and a small one the box scales up. */
static uint32_t s_argb_box[BOX_WIDTH * BOX_HEIGHT];
static uint16_t s_rgb565_box[BOX_WIDTH * BOX_HEIGHT];
static uint32_t s_argb_small[8 * 8];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — backend stubs
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Composites one premultiplied pixel source-over into the framebuffer.
 *
 * @param[in] x       Column.
 * @param[in] y       Row.
 * @param[in] source  Premultiplied ARGB8888 source pixel.
 */
static void over(int x, int y, uint32_t source)
{
    if (x < 0 || x >= FRAMEBUFFER_WIDTH || y < 0 || y >= FRAMEBUFFER_HEIGHT)
        return;
    uint32_t* destination = &s_test_context.framebuffer[y * FRAMEBUFFER_WIDTH + x];
    const uint32_t inverse_alpha = CHANNEL_MAX - (source >> 24);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const uint32_t channel = ((source >> shift) & 0xFFu)
                                 + (((*destination >> shift) & 0xFFu) * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
        out |= (channel > CHANNEL_MAX ? CHANNEL_MAX : channel) << shift;
    }
    *destination = out;
}

/**
 * @brief fill_rect stub — premultiplies the straight-alpha colour and composites it.
 *
 * @param[in] argb    Fill color (straight-alpha ARGB8888).
 * @param[in] x       Left edge.
 * @param[in] y       Top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Unused.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFu;
    const uint32_t red = ((argb >> 16) & 0xFFu) * alpha / CHANNEL_MAX;
    const uint32_t green = ((argb >> 8) & 0xFFu) * alpha / CHANNEL_MAX;
    const uint32_t blue = (argb & 0xFFu) * alpha / CHANNEL_MAX;
    const uint32_t premultiplied = (alpha << 24) | (red << 16) | (green << 8) | blue;
    for (int pixel_y = y; pixel_y < y + height; pixel_y++)
        for (int pixel_x = x; pixel_x < x + width; pixel_x++)
            over(pixel_x, pixel_y, premultiplied);
}

/**
 * @brief copy_rect stub — replaces the destination pixels.
 *
 * @param[in] source  Premultiplied ARGB8888 source.
 * @param[in] stride  Source row stride in bytes.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Unused.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
            if (x + column >= 0 && x + column < FRAMEBUFFER_WIDTH && y + row >= 0 && y + row < FRAMEBUFFER_HEIGHT)
                s_test_context.framebuffer[(y + row) * FRAMEBUFFER_WIDTH + x + column] = source_row[column];
    }
}

/**
 * @brief blend_rect stub — composites premultiplied rows source-over.
 *
 * @param[in] source  Premultiplied ARGB8888 source.
 * @param[in] stride  Source row stride in bytes.
 * @param[in] alpha   Global opacity (always 255 here).
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Unused.
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    (void)ctx;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)row * (size_t)stride);
        for (int column = 0; column < width; column++)
            over(x + column, y + row, source_row[column]);
    }
}

/**
 * @brief copy_rect_fmt stub — writes ARGB rows verbatim and expands RGB565 ones, as a DMA2D blitter would.
 *
 * @param[in] source  Source rows in @p format.
 * @param[in] stride  Source row stride in bytes.
 * @param[in] format  Source pixel format.
 * @param[in] x       Destination left edge.
 * @param[in] y       Destination top edge.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Unused.
 */
static void
copy_fmt_cb(const void* source, int stride, ERImageFormat format, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_test_context.format_calls++;
    for (int row = 0; row < height; row++)
    {
        const uint8_t* source_row = (const uint8_t*)source + (size_t)row * (size_t)stride;
        for (int column = 0; column < width; column++)
        {
            uint32_t pixel;
            if (format == ER_IMG_RGB565)
            {
                const uint16_t rgb565 = ((const uint16_t*)source_row)[column];
                const uint32_t red_bits = (rgb565 >> 11) & 0x1Fu, green_bits = (rgb565 >> 5) & 0x3Fu,
                               blue_bits = rgb565 & 0x1Fu;
                pixel = 0xFF000000u | (((red_bits << 3) | (red_bits >> 2)) << 16)
                        | (((green_bits << 2) | (green_bits >> 4)) << 8) | ((blue_bits << 3) | (blue_bits >> 2));
            }
            else
                pixel = ((const uint32_t*)source_row)[column];
            if (x + column >= 0 && x + column < FRAMEBUFFER_WIDTH && y + row >= 0 && y + row < FRAMEBUFFER_HEIGHT)
                s_test_context.framebuffer[(y + row) * FRAMEBUFFER_WIDTH + x + column] = pixel;
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private — helpers
 ---------------------------------------------------------------------------------------------------------------------*/

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
 * @brief Installs the test backend, with or without copy_rect_fmt, and registers the test images.
 *
 * Installing a backend resets the image registry, so the images are registered after it.
 *
 * @param[in] with_format  Whether to offer copy_rect_fmt.
 */
static void set_backend(bool with_format)
{
    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.copy_rect_fmt = with_format ? copy_fmt_cb : NULL;
    backend.ctx = &s_test_context;
    embedded_renderer_set_backend(&backend);
    er_image_load("argb_box", s_argb_box, BOX_WIDTH, BOX_HEIGHT);
    er_image_load_rgb565("rgb565_box", s_rgb565_box, BOX_WIDTH, BOX_HEIGHT);
    er_image_load("argb_small", s_argb_small, 8, 8);
}

/**
 * @brief Returns an ERProps with all int16_t layout fields set to ER_LAYOUT_AUTO.
 *
 * @return ERProps with all layout sentinels initialised and opacity set to 255.
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
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/**
 * @brief Renders one node at the test box over a transparent root and returns the framebuffer's coverage.
 *
 * @param[in]  type      ER_NODE_VIEW (the reference) or ER_NODE_IMAGE.
 * @param[in]  shape     Props carrying the radii (and, for an image, its source); position and size are set here.
 * @param[out] coverage  Receives each pixel's premultiplied alpha.
 */
static void
render_coverage(ERNodeType type, const ERProps* shape, uint8_t coverage[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT])
{
    er_reset();
    memset(s_test_context.framebuffer, 0, sizeof(s_test_context.framebuffer));
    s_test_context.format_calls = 0;

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = FRAMEBUFFER_WIDTH;
    root_props.height = FRAMEBUFFER_HEIGHT;
    er_node_set_props(root, &root_props);

    ERNode* node = er_node_create(type);
    ERProps props = *shape;
    props.position = ER_POS_ABSOLUTE;
    props.left = BOX_X;
    props.top = BOX_Y;
    props.width = BOX_WIDTH;
    props.height = BOX_HEIGHT;
    er_node_set_props(node, &props);

    er_tree_append_child(root, node);
    er_tree_set_root(root);
    er_commit();

    for (int pixel_index = 0; pixel_index < FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT; pixel_index++)
        coverage[pixel_index] = (uint8_t)(s_test_context.framebuffer[pixel_index] >> 24);

    er_tree_remove_child(root, node);
    er_node_destroy(node);
    er_node_destroy(root);
}

/**
 * @brief Compares two coverage masks over a rect, reporting the first difference.
 *
 * @param[in] want   Reference coverage.
 * @param[in] got    Coverage under test.
 * @param[in] min_x  Left edge of the compared rect.
 * @param[in] min_y  Top edge.
 * @param[in] max_x  One past the right edge.
 * @param[in] max_y  One past the bottom edge.
 * @param[in] what   Label for the failure message.
 *
 * @return true when every pixel matches.
 */
static bool
same_coverage(const uint8_t* want, const uint8_t* got, int min_x, int min_y, int max_x, int max_y, const char* what)
{
    for (int y = min_y; y < max_y; y++)
    {
        for (int x = min_x; x < max_x; x++)
        {
            if (want[y * FRAMEBUFFER_WIDTH + x] != got[y * FRAMEBUFFER_WIDTH + x])
            {
                fprintf(stderr,
                        "  %s at (%d,%d): background alpha %u, image alpha %u\n",
                        what,
                        x,
                        y,
                        want[y * FRAMEBUFFER_WIDTH + x],
                        got[y * FRAMEBUFFER_WIDTH + x]);
                return false;
            }
        }
    }
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tests that an Image is clipped to its corner radii.
 *
 * A rounded image has to stop at exactly the edge a background with the same radii fills, anti-aliased
 * fringe included. Each case renders an opaque white image and a white rounded View at the same box and
 * requires their coverage to match pixel for pixel, through every path of the image scaler: the ARGB and
 * RGB565 1:1 transfers (with and without copy_rect_fmt), the scaled row path, per-corner radii, and an
 * image inset by padding, which is clipped to the border box rather than to its content box.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    for (int pixel_index = 0; pixel_index < BOX_WIDTH * BOX_HEIGHT; pixel_index++)
    {
        s_argb_box[pixel_index] = 0xFFFFFFFFu;
        s_rgb565_box[pixel_index] = 0xFFFFu;
    }
    for (int pixel_index = 0; pixel_index < 8 * 8; pixel_index++)
        s_argb_small[pixel_index] = 0xFFFFFFFFu;

    static uint8_t want[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];
    static uint8_t got[FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT];

    /* -----------------------------------------------------------------------
     * Uniform radius: every path of the scaler matches the background fill.
     * ---------------------------------------------------------------------- */
    static const DrawCase k_cases[] = {
        {"ARGB 1:1", "argb_box", true, ER_RESIZE_STRETCH, 0},
        {"ARGB 1:1 without copy_rect_fmt", "argb_box", false, ER_RESIZE_STRETCH, 0},
        {"RGB565 1:1", "rgb565_box", true, ER_RESIZE_STRETCH, 0},
        {"RGB565 1:1 without copy_rect_fmt", "rgb565_box", false, ER_RESIZE_STRETCH, 0},
        {"scaled", "argb_small", true, ER_RESIZE_STRETCH, 0},
        {"cover", "argb_small", true, ER_RESIZE_COVER, 0},
    };
    for (size_t case_index = 0; case_index < sizeof(k_cases) / sizeof(k_cases[0]); case_index++)
    {
        set_backend(k_cases[case_index].format_backend);

        ERProps shape = props_default();
        shape.border_radius = 12;
        shape.background_color = 0xFFFFFFFFu;
        render_coverage(ER_NODE_VIEW, &shape, want);

        if (want[BOX_Y * FRAMEBUFFER_WIDTH + BOX_X] != 0)
            return fail("the background reference did not round its own corner");

        shape = props_default();
        shape.border_radius = 12;
        strncpy(shape.image_name, k_cases[case_index].image, ER_IMAGE_NAME_MAX);
        shape.resize_mode = k_cases[case_index].resize;
        render_coverage(ER_NODE_IMAGE, &shape, got);

        if (!same_coverage(want, got, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, k_cases[case_index].what))
            return fail("a rounded image's coverage differs from a rounded background's");

        /* At 1:1 the rows between the arcs still reach the backend as one whole-rect transfer. */
        if (k_cases[case_index].format_backend && strcmp(k_cases[case_index].image, "argb_small") != 0
            && s_test_context.format_calls != 1)
            return fail("a rounded 1:1 image no longer sends its straight rows as one copy_rect_fmt call");
    }

    /* -----------------------------------------------------------------------
     * Per-corner radii: each corner is cut by its own radius, and a 0 corner stays square.
     * ---------------------------------------------------------------------- */
    set_backend(true);
    {
        ERProps shape = props_default();
        shape.border_top_left_radius = 14;
        shape.border_bottom_right_radius = 6;
        shape.background_color = 0xFFFFFFFFu;
        render_coverage(ER_NODE_VIEW, &shape, want);

        shape = props_default();
        shape.border_top_left_radius = 14;
        shape.border_bottom_right_radius = 6;
        strncpy(shape.image_name, "argb_box", ER_IMAGE_NAME_MAX);
        render_coverage(ER_NODE_IMAGE, &shape, got);

        if (got[BOX_Y * FRAMEBUFFER_WIDTH + BOX_X] != 0)
            return fail("per-corner radius: the rounded top-left corner still painted");
        if (got[BOX_Y * FRAMEBUFFER_WIDTH + BOX_X + BOX_WIDTH - 1] != 0xFF)
            return fail("per-corner radius: the square top-right corner was clipped");
        if (!same_coverage(want, got, 0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, "per-corner radii"))
            return fail("per-corner radius: the image's coverage differs from the background's");
    }

    /* -----------------------------------------------------------------------
     * Padding: the bitmap sits in the content box, but the corners are the border box's.
     * ---------------------------------------------------------------------- */
    {
        const int pad = 2;
        ERProps shape = props_default();
        shape.border_radius = 12;
        shape.background_color = 0xFFFFFFFFu;
        render_coverage(ER_NODE_VIEW, &shape, want);

        shape = props_default();
        shape.border_radius = 12;
        shape.padding = (int16_t)pad;
        strncpy(shape.image_name, "argb_small", ER_IMAGE_NAME_MAX);
        render_coverage(ER_NODE_IMAGE, &shape, got);

        if (got[(BOX_Y + pad) * FRAMEBUFFER_WIDTH + BOX_X + pad] != 0)
            return fail("padding: the content box's corner pixel is outside the border box's arc yet painted");
        if (!same_coverage(
                want, got, BOX_X + pad, BOX_Y + pad, BOX_X + BOX_WIDTH - pad, BOX_Y + BOX_HEIGHT - pad, "padded image"))
            return fail("padding: the image is not clipped to the border box's rounded silhouette");
    }

    /* -----------------------------------------------------------------------
     * No radius: the bitmap still covers its whole box, square corners included.
     * ---------------------------------------------------------------------- */
    {
        ERProps shape = props_default();
        strncpy(shape.image_name, "argb_box", ER_IMAGE_NAME_MAX);
        render_coverage(ER_NODE_IMAGE, &shape, got);
        for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++)
            for (int x = 0; x < FRAMEBUFFER_WIDTH; x++)
            {
                const bool inside = x >= BOX_X && x < BOX_X + BOX_WIDTH && y >= BOX_Y && y < BOX_Y + BOX_HEIGHT;
                if (got[y * FRAMEBUFFER_WIDTH + x] != (inside ? 0xFF : 0x00))
                    return fail("no radius: a square image does not cover exactly its box");
            }
    }

    return EXIT_SUCCESS;
}
