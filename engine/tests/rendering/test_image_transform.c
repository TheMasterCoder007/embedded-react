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
 * A transformed <Image> samples its asset through the inverse matrix, with no transform buffer.
 *
 * The first half drives the sampler: a quarter turn lands every pixel, a scale fades the edges, the resize
 * modes place the image where the untransformed renderer does, the tint applies per sample, an RGB565 asset
 * samples as its ARGB8888 twin does, extreme matrices stay in bounds and nothing is written outside the clip. The
 * second half drives a node larger than the transform buffer through the compositor: it turns, an idle commit repaints
 * nothing, a partial repaint matches a full one, and the cases left on the buffer stay there.
 */

#include "er_limits.h"
#include "er_scene.h"
#include "image_scaler.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint: the reference frame */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if ERUI_TRANSFORMS_FULL

/*----------------------------------------------------------------------------------------------------------------------
 - Harness: blit calls land in a flat ARGB framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

#define RASTER_SIDE 96
#define FILL_COLOR 0xFF3355AAu
#define BACKGROUND_COLOR 0xFFFFFFFFu

static uint32_t* s_framebuffer;
static int s_framebuffer_side;
static int s_operation_count;

static void put_pixel(int x, int y, uint32_t argb)
{
    if (x >= 0 && y >= 0 && x < s_framebuffer_side && y < s_framebuffer_side)
        s_framebuffer[y * s_framebuffer_side + x] = argb;
}

static uint32_t get_pixel(int x, int y)
{
    return s_framebuffer[y * s_framebuffer_side + x];
}

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_operation_count++;
    for (int row = y; row < y + height; row++)
        for (int column = x; column < x + width; column++)
            put_pixel(column, row, argb);
}

static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_operation_count++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
            put_pixel(x + column, y + row, source_row[column]);
    }
}

/* Premultiplied pixels overwrite: enough to compare two renders of the same picture. Alpha 0 is skipped. */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    (void)ctx;
    s_operation_count++;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + row * stride);
        for (int column = 0; column < width; column++)
            if ((source_row[column] >> 24) != 0U)
                put_pixel(x + column, y + row, source_row[column]);
    }
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

static void clear_frame(void)
{
    memset(s_framebuffer, 0, sizeof(uint32_t) * (size_t)s_framebuffer_side * (size_t)s_framebuffer_side);
}

/** @brief Counts the pixels of the frame that were written. */
static int ink_count(void)
{
    int count = 0;
    for (int pixel = 0; pixel < s_framebuffer_side * s_framebuffer_side; pixel++)
        if (s_framebuffer[pixel] != 0U)
            count++;
    return count;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Sampler
 ---------------------------------------------------------------------------------------------------------------------*/

#define PICTURE_WIDTH 16
#define PICTURE_HEIGHT 12

static uint32_t s_picture[PICTURE_WIDTH * PICTURE_HEIGHT];

/** @brief An opaque picture with a different colour in every pixel. */
static void load_picture(void)
{
    for (int row = 0; row < PICTURE_HEIGHT; row++)
        for (int column = 0; column < PICTURE_WIDTH; column++)
            s_picture[row * PICTURE_WIDTH + column] =
                0xFF000000u | ((uint32_t)(column * 16) << 16) | ((uint32_t)(row * 20) << 8) | 0x40u;
    er_image_load("picture", s_picture, PICTURE_WIDTH, PICTURE_HEIGHT);
}

static ERImageProps image_props(const char* name, ERResizeMode mode)
{
    ERImageProps props;
    memset(&props, 0, sizeof(props));
    strncpy(props.image_name, name, ER_IMAGE_NAME_MAX);
    props.resize_mode = (uint8_t)mode;
    props.opacity = 255U;
    return props;
}

/** @brief Renders through the inverse matrix, with the whole frame as the clip. */
static void render_through(const ERImageProps* props,
                           int width,
                           int height,
                           float inverse_a,
                           float inverse_b,
                           float inverse_c,
                           float inverse_d,
                           float inverse_translate_x,
                           float inverse_translate_y)
{
    er_image_render_transformed(props,
                                0,
                                0,
                                width,
                                height,
                                inverse_a,
                                inverse_b,
                                inverse_c,
                                inverse_d,
                                inverse_translate_x,
                                inverse_translate_y,
                                0,
                                0,
                                s_framebuffer_side,
                                s_framebuffer_side);
}

/** @brief A quarter turn puts every source pixel on its own screen pixel, unblended. */
static int check_quarter_turn(void)
{
    const int shift_x = 40, shift_y = 20;
    const ERImageProps props = image_props("picture", ER_RESIZE_STRETCH);
    clear_frame();
    /* Forward: screen = (shift_x - y, shift_y + x). Inverse: x = screen_y - shift_y, y = shift_x - screen_x. */
    render_through(&props, PICTURE_WIDTH, PICTURE_HEIGHT, 0.0f, -1.0f, 1.0f, 0.0f, (float)-shift_y, (float)shift_x);
    for (int row = 0; row < PICTURE_HEIGHT; row++)
        for (int column = 0; column < PICTURE_WIDTH; column++)
            if (get_pixel(shift_x - row, shift_y + column) != s_picture[row * PICTURE_WIDTH + column])
                return fail("a quarter turn moved a pixel to the wrong place");
    if (ink_count() != PICTURE_WIDTH * PICTURE_HEIGHT)
        return fail("a quarter turn painted outside the image");
    return EXIT_SUCCESS;
}

/** @brief Scaled up, the inside stays opaque and the edge fades out. */
static int check_scale_fades_edges(void)
{
    static uint32_t solid[8 * 8];
    for (int pixel = 0; pixel < 8 * 8; pixel++)
        solid[pixel] = FILL_COLOR;
    er_image_load("solid", solid, 8, 8);
    const ERImageProps props = image_props("solid", ER_RESIZE_STRETCH);
    clear_frame();
    /* Twice the size, moved to (20, 20). */
    render_through(&props, 8, 8, 0.5f, 0.0f, 0.0f, 0.5f, -10.0f, -10.0f);
    for (int row = 20; row <= 34; row++)
        for (int column = 20; column <= 34; column++)
            if (get_pixel(column, row) != FILL_COLOR)
                return fail("a scaled image is not solid inside");
    const uint32_t edge = get_pixel(19, 27);
    if ((edge >> 24) == 0U || (edge >> 24) == 0xFFU)
        return fail("a scaled image has no faded edge");
    if (get_pixel(17, 27) != 0U || get_pixel(37, 27) != 0U)
        return fail("a scaled image painted past its edge");
    return EXIT_SUCCESS;
}

/** @brief Each resize mode puts the image where the untransformed renderer puts it. */
static int check_resize_modes(void)
{
    /* Three solid bands, so the two scalers agree away from the band edges. */
    enum
    {
        BANDS_WIDTH = 30,
        BANDS_HEIGHT = 12,
        BOX_WIDTH = 48,
        BOX_HEIGHT = 40
    };
    static uint32_t bands[BANDS_WIDTH * BANDS_HEIGHT];
    static const uint32_t k_band_colors[3] = {0xFFAA0000u, 0xFF00AA00u, 0xFF0000AAu};
    for (int row = 0; row < BANDS_HEIGHT; row++)
        for (int column = 0; column < BANDS_WIDTH; column++)
            bands[row * BANDS_WIDTH + column] = k_band_colors[column / 10];
    er_image_load("bands", bands, BANDS_WIDTH, BANDS_HEIGHT);

    static const ERResizeMode k_modes[4] = {ER_RESIZE_STRETCH, ER_RESIZE_COVER, ER_RESIZE_CONTAIN, ER_RESIZE_CENTER};
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    for (int mode_index = 0; mode_index < 4; mode_index++)
    {
        const ERImageProps props = image_props("bands", k_modes[mode_index]);
        clear_frame();
        er_image_render(&props, 0, 0, BOX_WIDTH, BOX_HEIGHT, NULL);
        memcpy(reference, s_framebuffer, sizeof(reference));
        clear_frame();
        render_through(&props, BOX_WIDTH, BOX_HEIGHT, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

        int band_edge_pixels = 0;
        for (int pixel = 0; pixel < RASTER_SIDE * RASTER_SIDE; pixel++)
        {
            if ((reference[pixel] == 0U) != (s_framebuffer[pixel] == 0U))
                return fail("a resize mode covers different pixels through the matrix");
            if (reference[pixel] != s_framebuffer[pixel])
                band_edge_pixels++;
        }
        /* Two band edges, a couple of blended columns each. */
        if (band_edge_pixels > 2 * 4 * BOX_HEIGHT)
            return fail("a resize mode samples a different part of the image through the matrix");
    }
    return EXIT_SUCCESS;
}

/** @brief The tint replaces the colour of each sample and keeps its alpha. */
static int check_tint(void)
{
    static uint32_t glyph[4 * 4];
    for (int pixel = 0; pixel < 4 * 4; pixel++)
        glyph[pixel] = (pixel < 8) ? 0xFFFFFFFFu : 0x80808080u;
    er_image_load("glyph", glyph, 4, 4);
    ERImageProps props = image_props("glyph", ER_RESIZE_STRETCH);
    props.tint_color = 0xFFFF0000u;
    clear_frame();
    /* A quarter turn, as above: source (x, y) lands on screen (30 - y, 10 + x). */
    render_through(&props, 4, 4, 0.0f, -1.0f, 1.0f, 0.0f, -10.0f, 30.0f);
    if (get_pixel(30, 10) != 0xFFFF0000u)
        return fail("an opaque sample did not take the tint");
    if (get_pixel(27, 10) != 0x80800000u)
        return fail("a translucent sample did not keep its alpha under the tint");
    return EXIT_SUCCESS;
}

/** @brief An RGB565 asset samples to the same frame as the same picture stored as ARGB8888. */
static int check_rgb565(void)
{
    static uint16_t packed[PICTURE_WIDTH * PICTURE_HEIGHT];
    static uint32_t expanded[PICTURE_WIDTH * PICTURE_HEIGHT];
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    for (int pixel = 0; pixel < PICTURE_WIDTH * PICTURE_HEIGHT; pixel++)
    {
        const uint32_t red5 = (uint32_t)(pixel * 7) & 0x1Fu;
        const uint32_t green6 = (uint32_t)(pixel * 11) & 0x3Fu;
        const uint32_t blue5 = (uint32_t)(pixel * 13) & 0x1Fu;
        packed[pixel] = (uint16_t)((red5 << 11) | (green6 << 5) | blue5);
        expanded[pixel] = 0xFF000000u | (((red5 << 3) | (red5 >> 2)) << 16) | (((green6 << 2) | (green6 >> 4)) << 8)
                          | ((blue5 << 3) | (blue5 >> 2));
    }
    er_image_load_rgb565("packed", packed, PICTURE_WIDTH, PICTURE_HEIGHT);
    er_image_load("expanded", expanded, PICTURE_WIDTH, PICTURE_HEIGHT);

    /* About 30 degrees and twice the size, as in the clip check. */
    const ERImageProps expanded_props = image_props("expanded", ER_RESIZE_STRETCH);
    clear_frame();
    render_through(
        &expanded_props, PICTURE_WIDTH, PICTURE_HEIGHT, 0.4330127f, -0.25f, 0.25f, 0.4330127f, -20.0f, -5.0f);
    memcpy(reference, s_framebuffer, sizeof(reference));
    if (ink_count() < PICTURE_WIDTH * PICTURE_HEIGHT * 3)
        return fail("the ARGB8888 picture painted too little");

    const ERImageProps packed_props = image_props("packed", ER_RESIZE_STRETCH);
    clear_frame();
    render_through(&packed_props, PICTURE_WIDTH, PICTURE_HEIGHT, 0.4330127f, -0.25f, 0.25f, 0.4330127f, -20.0f, -5.0f);
    if (memcmp(reference, s_framebuffer, sizeof(reference)) != 0)
        return fail("an RGB565 asset samples differently from the same picture as ARGB8888");
    return EXIT_SUCCESS;
}

/** @brief Extreme and broken matrices draw inside the clip or not at all, and read nothing out of range. */
static int check_extremes(void)
{
    static const float k_matrices[][6] = {
        /* Lopsided: one axis squashed to nothing, turned and not. */
        {1.0f, 0.0f, 0.0f, 500000.0f, -20.0f, -9000000.0f},
        {0.7f, 400000.0f, -0.7f, 400000.0f, -20.0f, -9000000.0f},
        {30000.0f, 0.0f, 0.0f, 1.0f, -1200000.0f, -20.0f},
        {30000.0f, -30000.0f, 30000.0f, 30000.0f, 0.0f, 0.0f},
        /* Past the sampler's range, and far off screen. */
        {1.0e9f, 0.0f, 0.0f, 1.0e9f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f, 3.0e9f, -3.0e9f},
        {1.0e-9f, 0.0f, 0.0f, 1.0e-9f, 5.0f, 5.0f},
        {1.0f, 1.0e30f, 1.0e30f, 1.0f, 1.0e30f, 1.0e30f},
    };
    const ERImageProps props = image_props("picture", ER_RESIZE_STRETCH);
    const int clip_x0 = 8, clip_y0 = 8, clip_x1 = RASTER_SIDE - 8, clip_y1 = RASTER_SIDE - 8;
    for (size_t matrix = 0; matrix < sizeof(k_matrices) / sizeof(k_matrices[0]); matrix++)
    {
        clear_frame();
        er_image_render_transformed(&props,
                                    0,
                                    0,
                                    PICTURE_WIDTH,
                                    PICTURE_HEIGHT,
                                    k_matrices[matrix][0],
                                    k_matrices[matrix][1],
                                    k_matrices[matrix][2],
                                    k_matrices[matrix][3],
                                    k_matrices[matrix][4],
                                    k_matrices[matrix][5],
                                    clip_x0,
                                    clip_y0,
                                    clip_x1,
                                    clip_y1);
        for (int row = 0; row < RASTER_SIDE; row++)
            for (int column = 0; column < RASTER_SIDE; column++)
                if (get_pixel(column, row) != 0U
                    && (column < clip_x0 || column >= clip_x1 || row < clip_y0 || row >= clip_y1))
                    return fail("an extreme matrix wrote outside its clip");
    }
    /* A huge clip is cut to the screen's range. */
    clear_frame();
    er_image_render_transformed(&props,
                                0,
                                0,
                                PICTURE_WIDTH,
                                PICTURE_HEIGHT,
                                1.0f,
                                0.0f,
                                0.0f,
                                1.0f,
                                0.0f,
                                0.0f,
                                -2000000000,
                                0,
                                2000000000,
                                RASTER_SIDE);
    if (ink_count() != PICTURE_WIDTH * PICTURE_HEIGHT)
        return fail("a huge clip changed what an identity matrix draws");
    return EXIT_SUCCESS;
}

/** @brief Nothing is written outside the clip, and the part inside matches an unclipped render. */
static int check_clip(void)
{
    static uint32_t whole[RASTER_SIDE * RASTER_SIDE];
    const ERImageProps props = image_props("picture", ER_RESIZE_STRETCH);
    /* About 30 degrees and twice the size. */
    const float inverse_a = 0.4330127f, inverse_b = -0.25f, inverse_c = 0.25f, inverse_d = 0.4330127f;
    const float inverse_translate_x = -20.0f, inverse_translate_y = -5.0f;
    clear_frame();
    render_through(&props,
                   PICTURE_WIDTH,
                   PICTURE_HEIGHT,
                   inverse_a,
                   inverse_b,
                   inverse_c,
                   inverse_d,
                   inverse_translate_x,
                   inverse_translate_y);
    memcpy(whole, s_framebuffer, sizeof(whole));
    if (ink_count() < PICTURE_WIDTH * PICTURE_HEIGHT * 3)
        return fail("a turned and scaled image painted too little");

    const int clip_x0 = 44, clip_y0 = 30, clip_x1 = 57, clip_y1 = 41;
    clear_frame();
    er_image_render_transformed(&props,
                                0,
                                0,
                                PICTURE_WIDTH,
                                PICTURE_HEIGHT,
                                inverse_a,
                                inverse_b,
                                inverse_c,
                                inverse_d,
                                inverse_translate_x,
                                inverse_translate_y,
                                clip_x0,
                                clip_y0,
                                clip_x1,
                                clip_y1);
    for (int row = 0; row < RASTER_SIDE; row++)
        for (int column = 0; column < RASTER_SIDE; column++)
        {
            const bool inside = column >= clip_x0 && column < clip_x1 && row >= clip_y0 && row < clip_y1;
            const uint32_t expected = inside ? whole[row * RASTER_SIDE + column] : 0U;
            if (get_pixel(column, row) != expected)
                return fail(inside ? "a clipped render differs from the whole one" : "a render wrote outside its clip");
        }
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Compositor: a node larger than the transform buffer
 ---------------------------------------------------------------------------------------------------------------------*/

#define NODE_LEFT 40
#define NODE_TOP 40

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
    props.opacity = 255U;
    return props;
}

/** @brief A node larger than the transform buffer turns, stays idle when idle, and repaints cleanly. */
static int check_node_larger_than_buffer(void)
{
    const int limit = (ERUI_TRANSFORM_WIDTH > ERUI_TRANSFORM_HEIGHT) ? ERUI_TRANSFORM_WIDTH : ERUI_TRANSFORM_HEIGHT;
    const int side = limit + 20;
    const int screen = side + 2 * NODE_LEFT;
    const size_t frame_bytes = sizeof(uint32_t) * (size_t)screen * (size_t)screen;
    s_framebuffer = calloc(1, frame_bytes);
    uint32_t* partial = malloc(frame_bytes);
    /* A bar along the top quarter, clear below. */
    uint32_t* bar = calloc((size_t)side * (size_t)side, sizeof(uint32_t));
    if (!s_framebuffer || !partial || !bar)
        return fail("node: out of memory");
    for (int pixel = 0; pixel < side * (side / 4); pixel++)
        bar[pixel] = FILL_COLOR;
    s_framebuffer_side = screen;

    er_reset();
    er_image_load("bar", bar, side, side);
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)screen;
    root_props.height = (int16_t)screen;
    root_props.background_color = BACKGROUND_COLOR;
    er_node_set_props(root, &root_props);

    ERNode* image = er_node_create(ER_NODE_IMAGE);
    ERProps image_node_props = props_default();
    image_node_props.position = ER_POSITION_ABSOLUTE;
    image_node_props.left = NODE_LEFT;
    image_node_props.top = NODE_TOP;
    image_node_props.width = (int16_t)side;
    image_node_props.height = (int16_t)side;
    image_node_props.resize_mode = ER_RESIZE_STRETCH;
    strncpy(image_node_props.image_name, "bar", ER_IMAGE_NAME_MAX);
    image_node_props.transform_rotate_z = 90.0f;
    image_node_props.transform_origin_x = 0.5f;
    image_node_props.transform_origin_y = 0.5f;
    er_node_set_props(image, &image_node_props);

    er_tree_append_child(root, image);
    er_tree_set_root(root);
    er_commit();

    const int near = 10, far = side - 10;
    /* Turned a quarter, the bar runs down the right side. */
    if (get_pixel(NODE_LEFT + far, NODE_TOP + far) != FILL_COLOR
        || get_pixel(NODE_LEFT + far, NODE_TOP + near) != FILL_COLOR)
        return fail("a node larger than the transform buffer did not turn");
    if (get_pixel(NODE_LEFT + near, NODE_TOP + near) != BACKGROUND_COLOR)
        return fail("a node larger than the transform buffer painted untransformed");

    er_commit();
    s_operation_count = 0;
    er_commit();
    if (s_operation_count != 0)
        return fail("an idle commit repainted the transformed node");

    /* Half a turn: the bar runs along the bottom, and the old one is erased. */
    image_node_props.transform_rotate_z = 180.0f;
    er_node_set_props(image, &image_node_props);
    er_commit();
    ERRect reported = {0, 0, 0, 0};
    er_get_dirty_rect(&reported);
    if (get_pixel(NODE_LEFT + near, NODE_TOP + far) != FILL_COLOR
        || get_pixel(NODE_LEFT + far, NODE_TOP + far) != FILL_COLOR)
        return fail("the node did not turn to its new angle");
    if (get_pixel(NODE_LEFT + far, NODE_TOP + near) != BACKGROUND_COLOR)
        return fail("the node left its old angle on screen");
    if (reported.x < NODE_LEFT - 4 || reported.y < NODE_TOP - 4 || reported.width > side + 8
        || reported.height > side + 8)
        return fail("turning the node reported more than its own bounds");
    memcpy(partial, s_framebuffer, frame_bytes);
    er_force_full_repaint();
    er_commit();
    if (memcmp(partial, s_framebuffer, frame_bytes) != 0)
        return fail("a partial repaint of the turned node differs from a full repaint");

    /* An angle between: still no idle damage once it has settled. */
    image_node_props.transform_rotate_z = 33.0f;
    er_node_set_props(image, &image_node_props);
    er_commit();
    memcpy(partial, s_framebuffer, frame_bytes);
    s_operation_count = 0;
    er_commit();
    if (s_operation_count != 0)
        return fail("an idle commit repainted the node at an odd angle");
    er_force_full_repaint();
    er_commit();
    if (memcmp(partial, s_framebuffer, frame_bytes) != 0)
        return fail("a partial repaint at an odd angle differs from a full repaint");

    /* `repeat` and rounded corners stay on the transform buffer, which this node does not fit. */
    image_node_props.transform_rotate_z = 90.0f;
    image_node_props.resize_mode = ER_RESIZE_REPEAT;
    er_node_set_props(image, &image_node_props);
    er_commit();
    if (get_pixel(NODE_LEFT + near, NODE_TOP + near) != FILL_COLOR
        || get_pixel(NODE_LEFT + far, NODE_TOP + far) != BACKGROUND_COLOR)
        return fail("a tiled image did not stay on the transform buffer");
    image_node_props.resize_mode = ER_RESIZE_STRETCH;
    image_node_props.border_radius = 12;
    er_node_set_props(image, &image_node_props);
    er_commit();
    if (get_pixel(NODE_LEFT + side / 2, NODE_TOP + near) != FILL_COLOR
        || get_pixel(NODE_LEFT + far, NODE_TOP + far) != BACKGROUND_COLOR)
        return fail("a rounded image did not stay on the transform buffer");

    er_node_destroy(root);
    free(bar);
    free(partial);
    free(s_framebuffer);
    return EXIT_SUCCESS;
}

#endif /* ERUI_TRANSFORMS_FULL */

/*----------------------------------------------------------------------------------------------------------------------
 - Test entry point
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
#if ERUI_TRANSFORMS_FULL
    static uint32_t raster_framebuffer[RASTER_SIDE * RASTER_SIDE];
    s_framebuffer = raster_framebuffer;
    s_framebuffer_side = RASTER_SIDE;
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);
    load_picture();

    if (check_quarter_turn() != EXIT_SUCCESS || check_scale_fades_edges() != EXIT_SUCCESS
        || check_resize_modes() != EXIT_SUCCESS || check_tint() != EXIT_SUCCESS || check_rgb565() != EXIT_SUCCESS
        || check_extremes() != EXIT_SUCCESS || check_clip() != EXIT_SUCCESS
        || check_node_larger_than_buffer() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#endif

    printf("PASS: image transform\n");
    return EXIT_SUCCESS;
}
