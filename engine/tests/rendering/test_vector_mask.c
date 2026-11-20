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

/* Clips and masks in the vector rasterizer (ER_VECTOR_OP_MASK). */

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

static void tape_mask(int kind, int shape_count)
{
    s_tape[s_tape_count++] = ER_VECTOR_OP_MASK;
    s_tape[s_tape_count++] = (float)kind;
    s_tape[s_tape_count++] = (float)shape_count;
}

/** @brief Appends a filled rectangle; @p reversed winds it the other way. */
static void tape_rect(int paint, float x0, float y0, float x1, float y1, bool reversed)
{
    const float forward[] = {ER_VECTOR_OP_SHAPE,
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
    const float backward[] = {ER_VECTOR_OP_SHAPE,
                              (float)paint,
                              ER_VECTOR_OP_MOVE,
                              x0,
                              y0,
                              ER_VECTOR_OP_LINE,
                              x0,
                              y1,
                              ER_VECTOR_OP_LINE,
                              x1,
                              y1,
                              ER_VECTOR_OP_LINE,
                              x1,
                              y0,
                              ER_VECTOR_OP_CLOSE};
    memcpy(s_tape + s_tape_count, reversed ? backward : forward, sizeof(forward));
    s_tape_count += (int)(sizeof(forward) / sizeof(forward[0]));
}

static void tape_circle(int paint, float center_x, float center_y, float radius)
{
    const float circle[] = {ER_VECTOR_OP_SHAPE,
                            (float)paint,
                            ER_VECTOR_OP_MOVE,
                            center_x + radius,
                            center_y,
                            ER_VECTOR_OP_ARC,
                            center_x,
                            center_y,
                            radius,
                            0.0f,
                            6.2831853f,
                            0.0f,
                            ER_VECTOR_OP_CLOSE};
    memcpy(s_tape + s_tape_count, circle, sizeof(circle));
    s_tape_count += (int)(sizeof(circle) / sizeof(circle[0]));
}

#define SOLID(color)                                                                                                   \
    {(color), 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0}

/* Paint 0 draws; the rest are what a mask's shapes are painted with. */
static const ERVectorPaint s_paints[] = {
    SOLID(WHITE),       /* 0: the drawn shape */
    SOLID(0xFFFF0000u), /* 1: a clip shape's own colour, which must never show */
    SOLID(0xFF808080u), /* 2: mid grey */
    SOLID(0x80FFFFFFu), /* 3: white at half alpha */
    SOLID(0xFF000000u), /* 4: black */
    {0x00000000u, WHITE, 8.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0}, /* 5 */
    {WHITE, 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 1, 0}, /* 6 */
};
#define PAINT_COUNT ((int)(sizeof(s_paints) / sizeof(s_paints[0])))

/* White to black, left to right, from x = 20 to x = 100. */
static const ERVectorGradient s_fade = {
    .type = ER_GRADIENT_LINEAR,
    .stop_count = 2,
    .stops = {{0xFFFFFFFFu, 0.0f}, {0xFF000000u, 1.0f}},
    .ax = 20.0f,
    .ay = 0.0f,
    .bx = 100.0f,
    .by = 0.0f,
};

/** @brief Renders the tape over a cleared frame, in bands of @p band rows. */
static void render_bands(int origin_x, int origin_y, const ERVectorTransform* transform, int band)
{
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_vector_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, &s_fade, 1);
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

/** @brief A clip keeps what is inside it, and its own shapes are never drawn. */
static int check_clip(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    tape_begin();
    tape_rect(0, 40, 30, 80, 90, false);
    render();
    memcpy(reference, s_framebuffer, sizeof(reference));

    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(1, 40, 30, 80, 90, false);
    tape_rect(0, 10, 10, 120, 120, false);
    render();
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a square clipped to a rectangle is not that rectangle");

    /* A clip of no shapes shows nothing. */
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 0);
    tape_rect(0, 10, 10, 120, 120, false);
    render();
    for (int pixel = 0; pixel < RASTER_SIDE * RASTER_SIDE; pixel++)
        if (s_framebuffer[pixel] != 0U)
            return fail("a clip of no shapes let its shape through");

    /* A clip edge that falls inside a pixel leaves it partly drawn. */
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(1, 40.5f, 30, 80, 90, false);
    tape_rect(0, 10, 10, 120, 120, false);
    render();
    if (alpha_at(39, 60) != 0 || abs(alpha_at(40, 60) - 128) > 2 || alpha_at(41, 60) != 255)
        return fail("a clip edge is not anti-aliased");
    return EXIT_SUCCESS;
}

/** @brief A clip is every one of its shapes together, however each is wound. */
static int check_clip_union(void)
{
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 2);
    tape_rect(1, 20, 20, 70, 70, false);
    tape_rect(1, 50, 50, 100, 100, true);
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (alpha_at(30, 30) != 255 || alpha_at(90, 90) != 255 || alpha_at(60, 60) != 255)
        return fail("two clip shapes do not add up where they overlap");
    if (alpha_at(90, 30) != 0 || alpha_at(30, 90) != 0 || alpha_at(110, 110) != 0)
        return fail("ink shows outside both clip shapes");
    return EXIT_SUCCESS;
}

/** @brief A second mask narrows the first, and ER_VECTOR_MASK_NONE lifts both. */
static int check_nesting_and_lifting(void)
{
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(1, 20, 20, 80, 80, false);
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(1, 50, 50, 110, 110, false);
    tape_rect(0, 0, 0, 128, 60, false);
    tape_mask(ER_VECTOR_MASK_NONE, 0);
    tape_rect(0, 0, 100, 128, 128, false);
    render();
    if (alpha_at(60, 55) != 255)
        return fail("nothing shows where two clips overlap");
    if (alpha_at(30, 30) != 0 || alpha_at(100, 55) != 0 || alpha_at(60, 70) != 0)
        return fail("a second clip did not narrow the first");
    if (alpha_at(5, 110) != 255 || alpha_at(120, 110) != 255)
        return fail("a shape after the masks were lifted is still masked");
    return EXIT_SUCCESS;
}

/** @brief A mask scales what it covers by its luminance or its alpha, painted in order. */
static int check_mask_values(void)
{
    tape_begin();
    tape_mask(ER_VECTOR_MASK_LUMINANCE, 3);
    tape_rect(2, 0, 0, 128, 128, false); /* grey everywhere */
    tape_rect(4, 64, 0, 128, 64, false); /* black over the top right */
    tape_rect(3, 0, 64, 64, 128, false); /* half white over the bottom left */
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (abs(alpha_at(20, 20) - 128) > 2)
        return fail("a grey luminance mask does not halve the shape");
    if (alpha_at(100, 20) != 0)
        return fail("black painted over a mask does not hide the shape");
    if (abs(alpha_at(20, 100) - 191) > 3)
        return fail("half white over grey is not three quarters");

    tape_begin();
    tape_mask(ER_VECTOR_MASK_ALPHA, 2);
    tape_rect(4, 0, 0, 64, 128, false);   /* black, but opaque */
    tape_rect(3, 64, 0, 128, 128, false); /* half alpha */
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (alpha_at(20, 60) != 255 || abs(alpha_at(100, 60) - 128) > 2)
        return fail("an alpha mask does not follow its shapes' alpha");

    /* A stroke is part of what a mask paints; a clip takes the fill area alone. */
    tape_begin();
    tape_mask(ER_VECTOR_MASK_LUMINANCE, 1);
    tape_rect(5, 40, 40, 90, 90, false);
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (alpha_at(40, 60) != 255 || alpha_at(60, 60) != 0 || alpha_at(30, 60) != 0)
        return fail("a stroked mask shape does not mask to its stroke");
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(5, 40, 40, 90, 90, false);
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (alpha_at(60, 60) != 255 || alpha_at(37, 60) != 0)
        return fail("a clip does not take its shape's fill area");
    return EXIT_SUCCESS;
}

#if ERUI_GRADIENT
/** @brief A gradient in a mask fades the shape across it. */
static int check_mask_gradient(void)
{
    tape_begin();
    tape_mask(ER_VECTOR_MASK_LUMINANCE, 1);
    tape_rect(6, 0, 0, 128, 128, false);
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (alpha_at(10, 60) != 255 || alpha_at(110, 60) != 0)
        return fail("a gradient mask is not full at its start and empty at its end");
    if (abs(alpha_at(60, 60) - 126) > 4 || alpha_at(40, 60) <= alpha_at(80, 60))
        return fail("a gradient mask does not fade across the shape");

    /* The gradient moves with the node. */
    render_bands(10, 0, NULL, RASTER_SIDE);
    if (abs(alpha_at(70, 60) - 126) > 4)
        return fail("a gradient mask does not move with the node's origin");
    return EXIT_SUCCESS;
}
#endif

/** @brief Bands, a matrix and a second frame all draw the same masked picture. */
static int check_bands_transform_repaint(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 2);
    tape_circle(1, 50, 50, 30.5f);
    tape_rect(1, 60.25f, 70.5f, 110, 100, false);
    tape_mask(ER_VECTOR_MASK_LUMINANCE, 1);
    tape_rect(2, 0, 0, 128, 128, false);
    tape_rect(0, 5, 5, 120, 120, false);
    tape_circle(0, 60, 60, 20);
    render();
    memcpy(reference, s_framebuffer, sizeof(reference));
    if (abs(alpha_at(50, 50) - 128) > 2 || alpha_at(100, 30) != 0)
        return fail("a clip and a mask together are wrong");

    render_bands(0, 0, NULL, 16);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a masked drawing differs when drawn in bands");
    render_bands(0, 0, NULL, 1);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a masked drawing differs when drawn a row at a time");

    render_bands(7, 3, NULL, RASTER_SIDE);
    memcpy(reference, s_framebuffer, sizeof(reference));
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
        return fail("a masked drawing under a translate matrix differs from the same shift of the origin");

    /* A masked node is not cached: every frame reads its mask off the tape. */
    er_vector_reset();
    er_vector_cache_stats_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, &s_fade, 1);
    for (int frame = 0; frame < 3; frame++)
    {
        memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
        er_vector_render_slot(slot, 7, 3, NULL, 0, 0, RASTER_SIDE, RASTER_SIDE);
        if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
            return fail("a masked drawing changed between frames");
    }
    if (er_vector_cache_builds() != 0U || er_vector_cache_hits() != 0U)
        return fail("a masked drawing went through the edge cache");
    return EXIT_SUCCESS;
}

/** @brief Masks past the limit leave shapes unmasked, and a bad tape draws none of a mask's shapes. */
static int check_limits(void)
{
    tape_begin();
    for (int mask = 0; mask <= ER_VECTOR_MASK_GROUPS_MAX; mask++)
    {
        tape_mask(ER_VECTOR_MASK_CLIP, 1);
        tape_rect(1, 40, 40, 80, 80, false);
    }
    tape_rect(0, 0, 0, 128, 128, false);
    render();
    if (alpha_at(10, 10) != 255 || get_pixel(60, 60) != WHITE)
        return fail("one mask too many did not leave the shape unmasked");

    /* A count past the end of the tape, then one the tape cuts short. */
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 9);
    tape_rect(1, 40, 40, 80, 80, false);
    render();
    for (int pixel = 0; pixel < RASTER_SIDE * RASTER_SIDE; pixel++)
        if (s_framebuffer[pixel] != 0U)
            return fail("a mask's shape was drawn");
    s_tape_count = 2;
    render();
    tape_begin();
    tape_mask(7, -3);
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    s_tape[s_tape_count++] = ER_VECTOR_OP_SHAPE;
    render();
    return EXIT_SUCCESS;
}

int main(void)
{
    static uint32_t raster_framebuffer[RASTER_SIDE * RASTER_SIDE];
    s_framebuffer = raster_framebuffer;
    s_framebuffer_side = RASTER_SIDE;
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    if (check_clip() != EXIT_SUCCESS || check_clip_union() != EXIT_SUCCESS
        || check_nesting_and_lifting() != EXIT_SUCCESS || check_mask_values() != EXIT_SUCCESS
        || check_bands_transform_repaint() != EXIT_SUCCESS || check_limits() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#if ERUI_GRADIENT
    if (check_mask_gradient() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#endif

    printf("PASS: vector mask\n");
    return EXIT_SUCCESS;
}
