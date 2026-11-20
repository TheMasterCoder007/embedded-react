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

/* Images in the vector rasterizer (ER_VECTOR_OP_IMAGE). */

#include "er_limits.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint: the reference frame */
#include "vector.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Harness: blit calls land in a flat ARGB framebuffer
 ---------------------------------------------------------------------------------------------------------------------*/

#define RASTER_SIDE 128
#define WHITE 0xFFFFFFFFu

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
    (void)source;
    (void)stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
    s_operation_count++;
}

/* Premultiplied pixels overwrite: enough to compare two renders of the same picture. Alpha 0 is skipped. */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    (void)ctx;
    s_operation_count++;
    const uint32_t* source_pixels = source;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : width;
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++)
            if ((source_pixels[row * pitch + column] >> 24) != 0U)
                put_pixel(x + column, y + row, source_pixels[row * pitch + column]);
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/** @brief The largest channel difference between two frames. */
static int frame_difference(const uint32_t* first, const uint32_t* second, int pixel_count)
{
    int largest = 0;
    for (int pixel = 0; pixel < pixel_count; pixel++)
        for (int shift = 0; shift < 32; shift += 8)
        {
            const int difference = (int)((first[pixel] >> shift) & 0xFFU) - (int)((second[pixel] >> shift) & 0xFFU);
            if (abs(difference) > largest)
                largest = abs(difference);
        }
    return largest;
}

/** @brief A pixel's alpha: with a white paint, how much of the pixel was drawn. */
static int alpha_at(int x, int y)
{
    return (int)(get_pixel(x, y) >> 24);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tapes
 ---------------------------------------------------------------------------------------------------------------------*/

#define TAPE_MAX 256

static float s_tape[TAPE_MAX];
static int s_tape_count;

static void tape_begin(void)
{
    s_tape_count = 0;
}

static void tape_push(const float* values, int count)
{
    memcpy(s_tape + s_tape_count, values, sizeof(float) * (size_t)count);
    s_tape_count += count;
}

/** @brief Appends an image, its pixels mapped to the tape by (a b c d e f). */
static void tape_image(int id, float alpha, float a, float b, float c, float d, float e, float f)
{
    const float image[] = {ER_VECTOR_OP_IMAGE, (float)id, alpha, a, b, c, d, e, f};
    tape_push(image, (int)(sizeof(image) / sizeof(image[0])));
}

static void tape_rect(int paint, float x0, float y0, float x1, float y1)
{
    const float rect[] = {ER_VECTOR_OP_SHAPE,
                          (float)paint,
                          ER_VECTOR_OP_MOVE,
                          x0,
                          y0,
                          ER_VECTOR_OP_LINE,
                          x1,
                          y0,
                          ER_VECTOR_OP_LINE,
                          x1,
                          y1,
                          ER_VECTOR_OP_LINE,
                          x0,
                          y1,
                          ER_VECTOR_OP_CLOSE};
    tape_push(rect, (int)(sizeof(rect) / sizeof(rect[0])));
}

static void tape_clip(float x0, float y0, float x1, float y1)
{
    const float mask[] = {ER_VECTOR_OP_MASK, (float)ER_VECTOR_MASK_CLIP, 1.0f};
    tape_push(mask, 3);
    tape_rect(1, x0, y0, x1, y1);
}

static const ERVectorPaint s_paints[] = {
    {0xFF0000FFu, 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
    {WHITE, 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
};
#define PAINT_COUNT ((int)(sizeof(s_paints) / sizeof(s_paints[0])))

/* A 4x4 image of four 2x2 quarters: red, green / blue, half-transparent white. Registered as id 0x00ab12. */
#define IMAGE_ID 0x00ab12
#define IMAGE_NAME ER_VECTOR_IMAGE_PREFIX "00ab12"
#define RED 0xFFFF0000u
#define GREEN 0xFF00FF00u
#define BLUE 0xFF0000FFu
#define HALF 0x80808080u
static const uint32_t s_image[16] = {
    RED,
    RED,
    GREEN,
    GREEN,
    RED,
    RED,
    GREEN,
    GREEN,
    BLUE,
    BLUE,
    HALF,
    HALF,
    BLUE,
    BLUE,
    HALF,
    HALF,
};

static void render_bands(int origin_x, int origin_y, const ERVectorTransform* transform, int band)
{
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_vector_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    for (int top = 0; top < RASTER_SIDE; top += band)
        er_vector_render_slot(slot, origin_x, origin_y, transform, 0, top, RASTER_SIDE, top + band);
}

static void render(void)
{
    render_bands(0, 0, NULL, RASTER_SIDE);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Checks
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief An image scaled up 16 times shows each of its quarters where its matrix puts it. */
static int check_placement(void)
{
    tape_begin();
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    render();
    if (get_pixel(30, 20) != RED || get_pixel(70, 20) != GREEN || get_pixel(30, 60) != BLUE
        || get_pixel(70, 60) != HALF)
        return fail("an image's quarters are not where its matrix puts them");
    if (get_pixel(19, 40) != 0U || get_pixel(85, 40) != 0U || get_pixel(50, 9) != 0U || get_pixel(50, 75) != 0U)
        return fail("an image draws outside its rectangle");

    /* Turned a quarter: its top-left quarter lands top right. */
    tape_begin();
    tape_image(IMAGE_ID, 1.0f, 0.0f, 16.0f, -16.0f, 0.0f, 100.0f, 10.0f);
    render();
    if (get_pixel(90, 20) != RED || get_pixel(50, 20) != BLUE || get_pixel(90, 60) != GREEN)
        return fail("a turned image is not turned");

    /* An image under a matrix draws as the same image with that matrix folded into its own. */
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    tape_begin();
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 27.0f, 13.0f);
    render();
    memcpy(reference, s_framebuffer, sizeof(reference));
    tape_begin();
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    ERVectorTransform shift = {0};
    shift.a = shift.d = shift.inverse_a = shift.inverse_d = 1.0f;
    shift.translate_x = 7.0f;
    shift.translate_y = 3.0f;
    shift.inverse_translate_x = -7.0f;
    shift.inverse_translate_y = -3.0f;
    shift.box_x0 = -7.0f;
    shift.box_y0 = -3.0f;
    shift.box_x1 = shift.box_y1 = (float)RASTER_SIDE;
    render_bands(0, 0, &shift, RASTER_SIDE);
    if (frame_difference(s_framebuffer, reference, RASTER_SIDE * RASTER_SIDE) > 1)
        return fail("an image under a translate matrix differs from the same shift of its own");
    render_bands(7, 3, NULL, 5);
    if (frame_difference(s_framebuffer, reference, RASTER_SIDE * RASTER_SIDE) > 0)
        return fail("an image drawn in bands from a moved origin differs");
    return EXIT_SUCCESS;
}

/** @brief An image draws in order with shapes, fades by its alpha and takes the masks in force. */
static int check_order_alpha_masks(void)
{
    tape_begin();
    tape_rect(0, 0, 0, 128, 128);
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    tape_rect(0, 60, 0, 128, 128);
    render();
    if (get_pixel(30, 20) != RED || get_pixel(70, 20) != 0xFF0000FFu)
        return fail("an image is not drawn in order with the shapes around it");

    tape_begin();
    tape_image(IMAGE_ID, 0.5f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    render();
    if (abs(alpha_at(30, 20) - 128) > 1 || abs(alpha_at(70, 60) - 64) > 1)
        return fail("an image's alpha does not fade it");

    tape_begin();
    tape_clip(0, 0, 50, 128);
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    render();
    if (get_pixel(30, 20) != RED || get_pixel(70, 20) != 0U)
        return fail("a clip does not narrow an image");
    return EXIT_SUCCESS;
}

/** @brief An RGB565 image is expanded to full channels. */
static int check_rgb565(void)
{
    static const uint16_t pixels[4] = {0xF800u, 0xF800u, 0x001Fu, 0x001Fu}; /* red over blue */
    er_image_load_rgb565(ER_VECTOR_IMAGE_PREFIX "000565", pixels, 2, 2);
    tape_begin();
    tape_image(0x565, 1.0f, 20.0f, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f);
    render();
    if (get_pixel(20, 5) != RED || get_pixel(20, 35) != BLUE)
        return fail("an RGB565 image does not draw its colours");
    return EXIT_SUCCESS;
}

/** @brief A missing or malformed image draws nothing and leaves the rest of the tape, and none is cached. */
static int check_missing_and_cache(void)
{
    tape_begin();
    tape_image(0x123456, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    tape_image(IMAGE_ID, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 20.0f, 10.0f); /* no area */
    tape_image(IMAGE_ID, 1.0f, NAN, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    tape_image(ER_VECTOR_IMAGE_ID_MAX + 1, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    tape_rect(0, 100, 100, 110, 110);
    render();
    for (int y = 0; y < RASTER_SIDE; y++)
        for (int x = 0; x < RASTER_SIDE; x++)
            if (get_pixel(x, y) != ((x >= 100 && x < 110 && y >= 100 && y < 110) ? 0xFF0000FFu : 0U))
                return fail("a missing or malformed image drew something, or stopped the tape");

    /* Cut short. */
    tape_begin();
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    s_tape_count -= 3;
    render();

    tape_begin();
    tape_image(IMAGE_ID, 1.0f, 16.0f, 0.0f, 0.0f, 16.0f, 20.0f, 10.0f);
    er_vector_reset();
    er_vector_cache_stats_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    for (int frame = 0; frame < 3; frame++)
    {
        memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
        er_vector_render_slot(slot, 0, 0, NULL, 0, 0, RASTER_SIDE, RASTER_SIDE);
        if (get_pixel(30, 20) != RED)
            return fail("an image is missing from a later frame");
    }
    if (er_vector_cache_builds() != 0U || er_vector_cache_hits() != 0U)
        return fail("an image went through the edge cache");
    return EXIT_SUCCESS;
}

int main(void)
{
    static uint32_t raster_framebuffer[RASTER_SIDE * RASTER_SIDE];
    s_framebuffer = raster_framebuffer;
    s_framebuffer_side = RASTER_SIDE;
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);
    er_image_load(IMAGE_NAME, s_image, 4, 4);

    if (check_placement() != EXIT_SUCCESS || check_order_alpha_masks() != EXIT_SUCCESS || check_rgb565() != EXIT_SUCCESS
        || check_missing_and_cache() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    printf("PASS: vector image\n");
    return EXIT_SUCCESS;
}
