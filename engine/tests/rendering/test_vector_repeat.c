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

/* Repeated tiles in the vector rasterizer (ER_VECTOR_OP_REPEAT). */

#include "er_limits.h"
#include "er_scene.h"
#include "native_renderer.h"
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
#define PIXEL_COUNT (RASTER_SIDE * RASTER_SIDE)
#define WHITE 0xFFFFFFFFu

static uint32_t s_framebuffer[PIXEL_COUNT];

static void put_pixel(int x, int y, uint32_t argb)
{
    if (x >= 0 && y >= 0 && x < RASTER_SIDE && y < RASTER_SIDE)
        s_framebuffer[y * RASTER_SIDE + x] = argb;
}

static uint32_t get_pixel(int x, int y)
{
    return s_framebuffer[y * RASTER_SIDE + x];
}

static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
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
}

/* Premultiplied pixels overwrite, so a seam drawn twice shows its second coverage. Alpha 0 is skipped. */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)alpha;
    (void)ctx;
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
static int frame_difference(const uint32_t* first, const uint32_t* second)
{
    int largest = 0;
    for (int pixel = 0; pixel < PIXEL_COUNT; pixel++)
        for (int shift = 0; shift < 32; shift += 8)
        {
            const int difference = (int)((first[pixel] >> shift) & 0xFFU) - (int)((second[pixel] >> shift) & 0xFFU);
            if (abs(difference) > largest)
                largest = abs(difference);
        }
    return largest;
}

static bool frame_blank(void)
{
    for (int pixel = 0; pixel < PIXEL_COUNT; pixel++)
        if (s_framebuffer[pixel] != 0U)
            return false;
    return true;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tapes
 ---------------------------------------------------------------------------------------------------------------------*/

#define TAPE_MAX ERUI_VECTOR_TAPE_MAX

static float s_tape[TAPE_MAX];
static int s_tape_count;

static void tape_begin(void)
{
    s_tape_count = 0;
}

static void tape_push(const float* values, int count)
{
    if (s_tape_count + count > TAPE_MAX)
    {
        fprintf(stderr, "test tape full\n");
        exit(EXIT_FAILURE);
    }
    memcpy(s_tape + s_tape_count, values, sizeof(float) * (size_t)count);
    s_tape_count += count;
}

static void tape_value(float value)
{
    tape_push(&value, 1);
}

static void tape_shape(int paint)
{
    const float shape[] = {ER_VECTOR_OP_SHAPE, (float)paint};
    tape_push(shape, 2);
}

/** @brief Appends a closed polygon's path, moved by (move_x, move_y). */
static void tape_polygon(const float* points, int point_count, float move_x, float move_y)
{
    for (int point = 0; point < point_count; point++)
    {
        const float op[] = {point == 0 ? ER_VECTOR_OP_MOVE : ER_VECTOR_OP_LINE,
                            points[2 * point] + move_x,
                            points[2 * point + 1] + move_y};
        tape_push(op, 3);
    }
    tape_value(ER_VECTOR_OP_CLOSE);
}

static void tape_rect(int paint, float x0, float y0, float x1, float y1)
{
    const float corners[] = {x0, y0, x1, y0, x1, y1, x0, y1};
    tape_shape(paint);
    tape_polygon(corners, 4, 0.0f, 0.0f);
}

static void tape_mask(int kind, int count)
{
    const float mask[] = {ER_VECTOR_OP_MASK, (float)kind, (float)count};
    tape_push(mask, 3);
}

static void tape_repeat(int count, float columns, float rows, int cut, float x, float y, const float lattice[4])
{
    const float repeat[] = {ER_VECTOR_OP_REPEAT,
                            (float)count,
                            columns,
                            rows,
                            (float)cut,
                            x,
                            y,
                            lattice[0],
                            lattice[1],
                            lattice[2],
                            lattice[3]};
    tape_push(repeat, (int)(sizeof(repeat) / sizeof(repeat[0])));
}

/** @brief How far the copy at a column and row moves, worked out as the engine does. */
static void lattice_move(const float lattice[4], int column, int row, float* move_x, float* move_y)
{
    *move_x = (float)column * lattice[0] + (float)row * lattice[2];
    *move_y = (float)column * lattice[1] + (float)row * lattice[3];
}

enum
{
    PAINT_BLUE,
    PAINT_WHITE,
    PAINT_STROKE,
    PAINT_COUNT
};

static const ERVectorPaint s_paints[PAINT_COUNT] = {
    {0xFF0000FFu, 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
    {WHITE, 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
    {0x00000000u, WHITE, 3.0f, 0.0f, ER_VECTOR_CAP_ROUND, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO, 0, 0},
};

static void render_bands(int origin_x, int origin_y, const ERVectorTransform* transform, int band)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
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

static uint32_t s_reference[PIXEL_COUNT];

/* A diamond in a 10 x 9.5 tile, and a bar that meets the next row's at a fractional y. */
static const float s_diamond[] = {5.0f, 1.0f, 9.0f, 4.75f, 5.0f, 8.5f, 1.0f, 4.75f};
static const float s_bar[] = {0.25f, 0.25f, 3.25f, 0.25f, 3.25f, 9.75f, 0.25f, 9.75f};
static const float s_square_lattice[4] = {10.0f, 0.0f, 0.0f, 9.5f};
static const float s_sheared_lattice[4] = {10.0f, 2.0f, -3.0f, 9.5f};

/** @brief Uncut copies draw as the tile's shapes, each merged with its own copies into one shape. */
static int check_merged(const float lattice[4], const char* message)
{
    enum
    {
        COLUMNS = 6,
        ROWS = 5
    };
    const float start_x = 20.0f, start_y = 10.0f;
    tape_begin();
    for (int piece = 0; piece < 2; piece++)
    {
        tape_shape(piece == 0 ? PAINT_BLUE : PAINT_WHITE);
        for (int row = 0; row < ROWS; row++)
            for (int column = 0; column < COLUMNS; column++)
            {
                float move_x, move_y;
                lattice_move(lattice, column, row, &move_x, &move_y);
                tape_polygon(piece == 0 ? s_bar : s_diamond, 4, start_x + move_x, start_y + move_y);
            }
    }
    render();
    memcpy(s_reference, s_framebuffer, sizeof(s_reference));

    tape_begin();
    tape_repeat(2, COLUMNS, ROWS, 0, start_x, start_y, lattice);
    tape_shape(PAINT_BLUE);
    tape_polygon(s_bar, 4, start_x, start_y);
    tape_shape(PAINT_WHITE);
    tape_polygon(s_diamond, 4, start_x, start_y);
    render();
    if (frame_difference(s_framebuffer, s_reference) > 0)
        return fail(message);
    return EXIT_SUCCESS;
}

/** @brief Bars that meet leave no seam: their copies are painted in one pass. */
static int check_seamless(void)
{
    tape_begin();
    tape_repeat(1, 2, 3, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 0.25f, 0.25f, 3.25f, 9.75f);
    render();
    /* Rows meet at y 9.75 and 19.25. */
    if ((get_pixel(1, 9) >> 24) != 0xFFU || (get_pixel(11, 19) >> 24) != 0xFFU)
        return fail("copies that meet leave a seam");
    if (get_pixel(5, 5) != 0U || get_pixel(1, 30) != 0U || get_pixel(21, 5) != 0U)
        return fail("a repeat draws past its columns and rows");
    return EXIT_SUCCESS;
}

/** @brief A cut copy draws only inside its tile, as a clip of the tile over that copy would. */
static int check_cut(void)
{
    enum
    {
        COLUMNS = 4,
        ROWS = 3
    };
    static const float lattice[4] = {16.0f, 0.0f, 0.0f, 16.0f};
    static const float line[] = {-2.0f, -2.0f, 18.0f, 18.0f};
    const float start_x = 30.0f, start_y = 20.0f;

    /* Under a clip of the whole, each tile's clip then its copy. */
    tape_begin();
    for (int row = 0; row < ROWS; row++)
        for (int column = 0; column < COLUMNS; column++)
        {
            float move_x, move_y;
            lattice_move(lattice, column, row, &move_x, &move_y);
            tape_mask(ER_VECTOR_MASK_CLIP, 1);
            tape_rect(PAINT_WHITE, 0.0f, 0.0f, 128.0f, 50.0f);
            tape_mask(ER_VECTOR_MASK_CLIP, 1);
            tape_rect(
                PAINT_WHITE, start_x + move_x, start_y + move_y, start_x + move_x + 16.0f, start_y + move_y + 16.0f);
            tape_shape(PAINT_STROKE);
            const float path[] = {ER_VECTOR_OP_MOVE,
                                  start_x + move_x + line[0],
                                  start_y + move_y + line[1],
                                  ER_VECTOR_OP_LINE,
                                  start_x + move_x + line[2],
                                  start_y + move_y + line[3]};
            tape_push(path, 6);
            tape_mask(ER_VECTOR_MASK_NONE, 0);
        }
    render();
    memcpy(s_reference, s_framebuffer, sizeof(s_reference));

    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(PAINT_WHITE, 0.0f, 0.0f, 128.0f, 50.0f);
    tape_repeat(1, COLUMNS, ROWS, 1, start_x, start_y, lattice);
    tape_shape(PAINT_STROKE);
    const float path[] = {ER_VECTOR_OP_MOVE,
                          start_x + line[0],
                          start_y + line[1],
                          ER_VECTOR_OP_LINE,
                          start_x + line[2],
                          start_y + line[3]};
    tape_push(path, 6);
    render();
    if (frame_difference(s_framebuffer, s_reference) > 2)
        return fail("cut copies differ from copies each under a clip of its tile");
    /* The round caps past the first tile's corner, and the last row past the outer clip, are cut away. */
    if (get_pixel(28, 18) != 0U || get_pixel(60, 52) != 0U)
        return fail("a cut copy draws outside its tile");
    if ((get_pixel(38, 28) >> 24) == 0U)
        return fail("a cut copy is missing inside its tile");
    return EXIT_SUCCESS;
}

/** @brief A repeat drawn in bands, under a matrix or from a moved origin draws the same. */
static int check_bands_and_transform(void)
{
    tape_begin();
    tape_mask(ER_VECTOR_MASK_CLIP, 1);
    tape_rect(PAINT_WHITE, 5.0f, 5.0f, 100.0f, 90.0f);
    tape_repeat(2, 9, 8, 1, 3.0f, 2.0f, s_sheared_lattice);
    tape_shape(PAINT_STROKE);
    const float path[] = {ER_VECTOR_OP_MOVE, 3.0f, 2.0f, ER_VECTOR_OP_LINE, 14.0f, 13.0f};
    tape_push(path, 6);
    tape_shape(PAINT_BLUE);
    tape_polygon(s_diamond, 4, 3.0f, 2.0f);
    render_bands(7, 3, NULL, RASTER_SIDE);
    memcpy(s_reference, s_framebuffer, sizeof(s_reference));
    render_bands(7, 3, NULL, 5);
    if (frame_difference(s_framebuffer, s_reference) > 0)
        return fail("a repeat drawn in bands differs");

    ERVectorTransform shift = {0};
    shift.a = shift.d = shift.inverse_a = shift.inverse_d = 1.0f;
    shift.translate_x = 7.0f;
    shift.translate_y = 3.0f;
    shift.inverse_translate_x = -7.0f;
    shift.inverse_translate_y = -3.0f;
    shift.box_x0 = -7.0f;
    shift.box_y0 = -3.0f;
    shift.box_x1 = shift.box_y1 = (float)RASTER_SIDE;
    render_bands(0, 0, &shift, 7);
    if (frame_difference(s_framebuffer, s_reference) > 1)
        return fail("a repeat under a translate matrix differs from the same shift of its origin");
    return EXIT_SUCCESS;
}

/** @brief More copies than the edge pool holds at once are painted in turns, the same as one at a time. */
static int check_many_copies(void)
{
    enum
    {
        SIDES = 24,
        COLUMNS = 32,
        ROWS = 32
    };
    static const float lattice[4] = {4.0f, 0.0f, 0.0f, 4.0f};
    float dot[2 * SIDES];
    for (int side = 0; side < SIDES; side++)
    {
        const float angle = 6.2831853f * (float)side / (float)SIDES;
        dot[2 * side] = 2.0f + 1.3f * cosf(angle);
        dot[2 * side + 1] = 2.0f + 1.3f * sinf(angle);
    }

    /* One dot, drawn from each copy's origin in turn. */
    memset(s_reference, 0, sizeof(s_reference));
    tape_begin();
    tape_shape(PAINT_WHITE);
    tape_polygon(dot, SIDES, 0.0f, 0.0f);
    er_vector_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    for (int row = 0; row < ROWS; row++)
        for (int column = 0; column < COLUMNS; column++)
            er_vector_render_slot(slot, column * 4, row * 4, NULL, 0, 0, RASTER_SIDE, RASTER_SIDE);
    memcpy(s_reference, s_framebuffer, sizeof(s_reference));

    tape_begin();
    tape_repeat(1, COLUMNS, ROWS, 0, 0.0f, 0.0f, lattice);
    tape_shape(PAINT_WHITE);
    tape_polygon(dot, SIDES, 0.0f, 0.0f);
    render();
    if (frame_difference(s_framebuffer, s_reference) > 0)
        return fail("copies past the edge pool differ from the same dots drawn one at a time");
    return EXIT_SUCCESS;
}

/** @brief A lattice it cannot draw draws the tile once; a count past the tape stops at its end; none is cached. */
static int check_limits(void)
{
    static const float nan_lattice[4] = {NAN, 0.0f, 0.0f, 10.0f};
    const struct
    {
        float columns, rows;
        const float* lattice;
    } once[] = {
        {ER_VECTOR_REPEAT_MAX, 2.0f, s_square_lattice},
        {0.0f, 3.0f, s_square_lattice},
        {2.5f, 3.0f, s_square_lattice},
        {NAN, 3.0f, s_square_lattice},
        {3.0f, 3.0f, nan_lattice},
    };
    for (size_t index = 0; index < sizeof(once) / sizeof(once[0]); index++)
    {
        tape_begin();
        tape_repeat(1, once[index].columns, once[index].rows, 1, 0.0f, 0.0f, once[index].lattice);
        tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
        render();
        if (get_pixel(4, 4) != WHITE || get_pixel(14, 4) != 0U || get_pixel(4, 14) != 0U)
            return fail("a repeat it cannot draw does not draw its tile once");
    }

    /* A drawing's repeats copy at most ER_VECTOR_REPEAT_MAX shapes in all: one past that draws its tile once. */
    static const float tiny_lattice[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    tape_begin();
    tape_repeat(1, 128, ER_VECTOR_REPEAT_MAX / 128, 0, 0.0f, 0.0f, tiny_lattice);
    tape_rect(PAINT_BLUE, 2.0f, 50.0f, 6.0f, 54.0f);
    tape_repeat(1, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
    render();
    if (get_pixel(4, 52) != 0xFF0000FFu || get_pixel(4, 4) != WHITE || get_pixel(14, 4) != 0U)
        return fail("a repeat past the drawing's budget is not drawn once");
    tape_begin();
    tape_repeat(1, 128, ER_VECTOR_REPEAT_MAX / 128 - 1, 0, 0.0f, 0.0f, tiny_lattice);
    tape_rect(PAINT_BLUE, 2.0f, 50.0f, 6.0f, 54.0f);
    tape_repeat(1, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
    render();
    if (get_pixel(24, 4) != WHITE)
        return fail("a repeat within the drawing's budget is not copied");

    /* The tile is the shapes after it; the rest of the tape draws once. */
    tape_begin();
    tape_repeat(1, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
    tape_rect(PAINT_BLUE, 2.0f, 50.0f, 6.0f, 54.0f);
    render();
    if (get_pixel(24, 4) != WHITE || get_pixel(4, 52) != 0xFF0000FFu || get_pixel(14, 52) != 0U)
        return fail("a repeat takes more or fewer shapes than its count");

    /* A count past the tape, a tile that stops at a mask, and a tape cut short in the op. */
    tape_begin();
    tape_repeat(50, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
    render();
    if (get_pixel(24, 4) != WHITE)
        return fail("a count past the tape does not draw the shapes it has");
    tape_begin();
    tape_repeat(2, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
    tape_mask(ER_VECTOR_MASK_NONE, 0);
    tape_rect(PAINT_BLUE, 2.0f, 50.0f, 6.0f, 54.0f);
    render();
    if (get_pixel(24, 4) != WHITE || get_pixel(4, 52) != 0xFF0000FFu || get_pixel(14, 52) != 0U)
        return fail("a tile does not stop at a mask op");
    tape_begin();
    tape_repeat(1, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    s_tape_count -= 4;
    render();
    if (!frame_blank())
        return fail("a repeat cut short drew something");

    tape_begin();
    tape_repeat(1, 3, 1, 0, 0.0f, 0.0f, s_square_lattice);
    tape_rect(PAINT_WHITE, 2.0f, 2.0f, 6.0f, 6.0f);
    er_vector_reset();
    er_vector_cache_stats_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    for (int frame = 0; frame < 3; frame++)
    {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        er_vector_render_slot(slot, 0, 0, NULL, 0, 0, RASTER_SIDE, RASTER_SIDE);
        if (get_pixel(24, 4) != WHITE)
            return fail("a copy is missing from a later frame");
    }
    if (er_vector_cache_builds() != 0U || er_vector_cache_hits() != 0U)
        return fail("a repeat went through the edge cache");
    return EXIT_SUCCESS;
}

/** @brief An image in a tile is copied with it, and cut to its tile. */
static int check_image(void)
{
    static const uint32_t pixels[4] = {WHITE, WHITE, WHITE, WHITE};
    er_image_load(ER_VECTOR_IMAGE_PREFIX "00c0de", pixels, 2, 2);
    tape_begin();
    tape_repeat(1, 3, 2, 1, 0.0f, 0.0f, s_square_lattice);
    const float image[] = {ER_VECTOR_OP_IMAGE, (float)0xc0de, 1.0f, 6.0f, 0.0f, 0.0f, 6.0f, 6.0f, 2.0f};
    tape_push(image, (int)(sizeof(image) / sizeof(image[0])));
    tape_rect(PAINT_BLUE, 2.0f, 50.0f, 6.0f, 54.0f);
    render();
    if (get_pixel(8, 4) != WHITE || get_pixel(28, 13) != WHITE)
        return fail("an image is not copied with its tile");
    if (get_pixel(11, 4) != 0U || get_pixel(8, 10) != 0U)
        return fail("an image copy is not cut to its tile");
    if (get_pixel(4, 52) != 0xFF0000FFu)
        return fail("the shape after a tiled image is not drawn");
    return EXIT_SUCCESS;
}

int main(void)
{
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    if (check_merged(s_square_lattice, "copies differ from the same shapes merged") != EXIT_SUCCESS
        || check_merged(s_sheared_lattice, "sheared copies differ from the same shapes merged") != EXIT_SUCCESS
        || check_seamless() != EXIT_SUCCESS || check_cut() != EXIT_SUCCESS
        || check_bands_and_transform() != EXIT_SUCCESS || check_many_copies() != EXIT_SUCCESS
        || check_limits() != EXIT_SUCCESS || check_image() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    printf("PASS: vector repeat\n");
    return EXIT_SUCCESS;
}
