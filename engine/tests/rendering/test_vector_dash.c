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

/* Dashed strokes in the vector rasterizer (ER_VECTOR_OP_DASH, ER_VECTOR_OP_DASH_OFFSET). */

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

static void tape_begin(int paint)
{
    s_tape_count = 0;
    s_tape[s_tape_count++] = ER_VECTOR_OP_SHAPE;
    s_tape[s_tape_count++] = (float)paint;
}

static void tape_dash(float dash, float gap)
{
    s_tape[s_tape_count++] = ER_VECTOR_OP_DASH;
    s_tape[s_tape_count++] = dash;
    s_tape[s_tape_count++] = gap;
}

static void tape_dash_offset(float offset, float scale)
{
    s_tape[s_tape_count++] = ER_VECTOR_OP_DASH_OFFSET;
    s_tape[s_tape_count++] = offset;
    s_tape[s_tape_count++] = scale;
}

/** @brief A line along y = 64, from x = 10 to x = 110. */
static void tape_line(void)
{
    const float line[] = {ER_VECTOR_OP_MOVE, 10.0f, 64.0f, ER_VECTOR_OP_LINE, 110.0f, 64.0f};
    memcpy(s_tape + s_tape_count, line, sizeof(line));
    s_tape_count += (int)(sizeof(line) / sizeof(line[0]));
}

/** @brief A closed square from (40, 40) to (90, 90), 200 around, starting at its top left corner. */
static void tape_square(void)
{
    const float square[] = {ER_VECTOR_OP_MOVE,
                            40.0f,
                            40.0f,
                            ER_VECTOR_OP_LINE,
                            90.0f,
                            40.0f,
                            ER_VECTOR_OP_LINE,
                            90.0f,
                            90.0f,
                            ER_VECTOR_OP_LINE,
                            40.0f,
                            90.0f,
                            ER_VECTOR_OP_CLOSE};
    memcpy(s_tape + s_tape_count, square, sizeof(square));
    s_tape_count += (int)(sizeof(square) / sizeof(square[0]));
}

static void tape_circle(float center_x, float center_y, float radius)
{
    const float circle[] = {ER_VECTOR_OP_MOVE,
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

#define STROKE(cap) {0x00000000u, WHITE, 8.0f, 0.0f, (cap), ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0}

static const ERVectorPaint s_paints[] = {
    STROKE(ER_VECTOR_CAP_BUTT),
    STROKE(ER_VECTOR_CAP_ROUND),
    STROKE(ER_VECTOR_CAP_SQUARE),
};
#define PAINT_COUNT ((int)(sizeof(s_paints) / sizeof(s_paints[0])))
#define BUTT 0
#define ROUND 1
#define SQUARE 2

/** @brief Renders the tape over a cleared frame, in bands of @p band rows, under a node dash offset. */
static void render_bands(float node_offset, int band)
{
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_vector_reset();
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    for (int top = 0; top < RASTER_SIDE; top += band)
    {
        er_vector_dash_offset(node_offset);
        er_vector_render_slot(slot, 0, 0, NULL, 0, top, RASTER_SIDE, top + band);
    }
}

static void render(void)
{
    render_bands(0.0f, RASTER_SIDE);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Checks
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Dashes and gaps fall where the pattern puts them, and the offset moves them. */
static int check_pattern(void)
{
    tape_begin(BUTT);
    tape_dash(10.0f, 10.0f);
    tape_line();
    render();
    if (alpha_at(15, 64) != 255 || alpha_at(25, 64) != 0 || alpha_at(35, 64) != 255 || alpha_at(105, 64) != 0)
        return fail("a 10 on, 10 off line is not dashed there");
    if (alpha_at(15, 61) != 255 || alpha_at(15, 58) != 0)
        return fail("a dash is not as wide as the stroke");

    /* The stroke starts 5 into the pattern: 5 on, 10 off, 10 on. */
    tape_begin(BUTT);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(5.0f, 0.0f);
    tape_line();
    render();
    if (alpha_at(12, 64) != 255 || alpha_at(20, 64) != 0 || alpha_at(30, 64) != 255)
        return fail("a dash offset did not move the dashes back");

    /* A negative one starts in the gap before the first dash. */
    tape_begin(BUTT);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(-5.0f, 0.0f);
    tape_line();
    render();
    if (alpha_at(12, 64) != 0 || alpha_at(20, 64) != 255 || alpha_at(30, 64) != 0)
        return fail("a negative dash offset did not move the dashes on");

    /* Two pairs: 4 on, 6 off, 20 on, 10 off. */
    tape_begin(BUTT);
    tape_dash(4.0f, 6.0f);
    tape_dash(20.0f, 10.0f);
    tape_line();
    render();
    if (alpha_at(12, 64) != 255 || alpha_at(17, 64) != 0 || alpha_at(30, 64) != 255 || alpha_at(45, 64) != 0
        || alpha_at(52, 64) != 255)
        return fail("a pattern of two dashes is not drawn in order");
    return EXIT_SUCCESS;
}

/** @brief Each dash takes the paint's cap, and a dash of no length is a dot under a round or square one. */
static int check_caps(void)
{
    tape_begin(ROUND);
    tape_dash(10.0f, 20.0f);
    tape_line();
    render();
    if (alpha_at(22, 64) != 255 || alpha_at(8, 64) != 255 || alpha_at(27, 64) != 0)
        return fail("a round cap does not reach past each end of a dash");
    if (alpha_at(23, 60) != 0)
        return fail("a round cap is square");

    tape_begin(SQUARE);
    tape_dash(10.0f, 20.0f);
    tape_line();
    render();
    if (alpha_at(23, 60) != 255 || alpha_at(27, 64) != 0)
        return fail("a square cap does not square off a dash");

    tape_begin(ROUND);
    tape_dash(0.0f, 20.0f);
    tape_line();
    render();
    if (alpha_at(10, 64) != 255 || alpha_at(30, 64) != 255 || alpha_at(31, 65) != 255 || alpha_at(40, 64) != 0)
        return fail("a dash of no length is not a dot under a round cap");

    tape_begin(SQUARE);
    tape_dash(0.0f, 20.0f);
    tape_line();
    render();
    if (alpha_at(32, 62) != 255 || alpha_at(40, 64) != 0)
        return fail("a dash of no length is not a square under a square cap");

    tape_begin(BUTT);
    tape_dash(0.0f, 20.0f);
    tape_line();
    render();
    for (int pixel = 0; pixel < RASTER_SIDE * RASTER_SIDE; pixel++)
        if (s_framebuffer[pixel] != 0U)
            return fail("a dash of no length drew under a butt cap");
    return EXIT_SUCCESS;
}

/** @brief A closed path: one dash all the way round is the solid stroke, and a dash across the start is joined. */
static int check_closed(void)
{
    static uint32_t solid[RASTER_SIDE * RASTER_SIDE];
    tape_begin(BUTT);
    tape_square();
    render();
    memcpy(solid, s_framebuffer, sizeof(solid));

    tape_begin(BUTT);
    tape_dash(200.0f, 200.0f);
    tape_square();
    render();
    if (memcmp(s_framebuffer, solid, sizeof(solid)) != 0)
        return fail("a dash as long as a closed path is not its solid stroke");

    /* On for the first and last 50: the two meet at the top left corner as one dash. */
    tape_begin(BUTT);
    tape_dash(100.0f, 100.0f);
    tape_dash_offset(50.0f, 0.0f);
    tape_square();
    render();
    if (alpha_at(37, 37) != 255)
        return fail("a dash across a closed path's start is not joined there");
    if (alpha_at(65, 40) != 255 || alpha_at(40, 65) != 255 || alpha_at(90, 65) != 0 || alpha_at(65, 90) != 0)
        return fail("a closed path is not dashed along its length");

    /* A whole gap draws nothing. */
    tape_begin(BUTT);
    tape_dash(200.0f, 200.0f);
    tape_dash_offset(200.0f, 0.0f);
    tape_square();
    render();
    for (int pixel = 0; pixel < RASTER_SIDE * RASTER_SIDE; pixel++)
        if (s_framebuffer[pixel] != 0U)
            return fail("a gap as long as the path drew");
    return EXIT_SUCCESS;
}

/** @brief The node's dash offset moves the shapes that take it, by their scale. */
static int check_node_offset(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    tape_begin(ROUND);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(7.0f, 0.0f);
    tape_line();
    render();
    memcpy(reference, s_framebuffer, sizeof(reference));

    tape_begin(ROUND);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(1.0f, 2.0f);
    tape_line();
    render_bands(3.0f, RASTER_SIDE);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a node dash offset of 3 at a scale of 2 is not an offset of 6");

    tape_begin(ROUND);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(7.0f, 0.0f);
    tape_line();
    render_bands(3.0f, RASTER_SIDE);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a shape with a scale of 0 followed the node's dash offset");

    /* The offset is the next render's only. */
    const int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    s_tape[5] = 0.0f; /* unused: the stored tape is what renders */
    er_vector_dash_offset(40.0f);
    er_vector_render_slot(slot, 0, 0, NULL, 0, 0, 0, 0);
    tape_begin(ROUND);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(7.0f, 1.0f);
    tape_line();
    render();
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a node dash offset outlived its render");
    return EXIT_SUCCESS;
}

/** @brief Bands, the edge cache and the arc fast path. */
static int check_bands_cache_arcs(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    tape_begin(ROUND);
    tape_dash(12.0f, 9.0f);
    tape_dash_offset(4.0f, 0.0f);
    tape_circle(64.0f, 64.0f, 40.0f);
    er_vector_analytic_arc_count_reset();
    render();
    memcpy(reference, s_framebuffer, sizeof(reference));
    if (er_vector_analytic_arc_count() != 0U)
        return fail("a dashed circle took the solid arc path");
    if (alpha_at(104, 67) != 255)
        return fail("a dashed circle has no dash where it starts");
    int gaps = 0;
    for (int degrees = 0; degrees < 360; degrees++)
    {
        const float angle = (float)degrees * 3.14159265f / 180.0f;
        if (alpha_at((int)(64.0f + 40.0f * cosf(angle)), (int)(64.0f + 40.0f * sinf(angle))) == 0)
            gaps++;
    }
    if (gaps == 0)
        return fail("a dashed circle has no gaps");

    render_bands(0.0f, 16);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a dashed drawing differs when drawn in bands");
    render_bands(0.0f, 1);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a dashed drawing differs when drawn a row at a time");

#if ERUI_VECTOR_EDGE_CACHE
    /* Dashes that stay put are cached like any stroke. */
    er_vector_reset();
    er_vector_cache_stats_reset();
    int slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    for (int frame = 0; frame < 4; frame++)
    {
        memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
        er_vector_render_slot(slot, 0, 0, NULL, 0, 0, RASTER_SIDE, RASTER_SIDE);
        if (frame_difference(s_framebuffer, reference, RASTER_SIDE * RASTER_SIDE) > 1)
            return fail("a dashed drawing changed between frames");
    }
    if (er_vector_cache_builds() != 1U || er_vector_cache_hits() == 0U)
        return fail("a dashed drawing that stays put was not cached");

    /* Dashes that follow the node's offset are rebuilt. */
    tape_begin(ROUND);
    tape_dash(12.0f, 9.0f);
    tape_dash_offset(0.0f, 1.0f);
    tape_circle(64.0f, 64.0f, 40.0f);
    er_vector_reset();
    er_vector_cache_stats_reset();
    slot = er_vector_store(-1, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    for (int frame = 0; frame < 4; frame++)
    {
        memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
        er_vector_dash_offset(4.0f);
        er_vector_render_slot(slot, 0, 0, NULL, 0, 0, RASTER_SIDE, RASTER_SIDE);
        if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
            return fail("a drawing following the node's dash offset changed between frames");
    }
    if (er_vector_cache_builds() != 0U || er_vector_cache_hits() != 0U)
        return fail("a drawing following the node's dash offset went through the edge cache");
#endif
    return EXIT_SUCCESS;
}

/** @brief Dashes are measured along a curve, not its chords: dots that fit a circle exactly never lose one. */
static int check_curve_length(void)
{
    const float around = 2.0f * 3.14159265f * 40.0f;
    const float spacing = around / 16.0f;
    long least = 0, most = 0;
    for (int step = 0; step < 200; step++)
    {
        tape_begin(ROUND);
        tape_dash(0.0f, spacing);
        tape_dash_offset(-spacing * (float)step / 200.0f, 0.0f);
        tape_circle(64.0f, 64.0f, 40.0f);
        render();
        long drawn = 0;
        for (int pixel = 0; pixel < RASTER_SIDE * RASTER_SIDE; pixel++)
            drawn += (long)(s_framebuffer[pixel] >> 24);
        if (step == 0 || drawn < least)
            least = drawn;
        if (drawn > most)
            most = drawn;
    }
    /* One dot of the 16 missing is a sixteenth less. */
    if (least * 100 < most * 97)
        return fail("a dot went missing as 16 dots marched round a circle they fit");
    return EXIT_SUCCESS;
}

/** @brief Patterns that draw solid, and tapes cut short. */
static int check_limits(void)
{
    static uint32_t solid[RASTER_SIDE * RASTER_SIDE];
    tape_begin(ROUND);
    tape_line();
    render();
    memcpy(solid, s_framebuffer, sizeof(solid));

    const float patterns[][2] = {{0.0f, 0.0f}, {-4.0f, 4.0f}, {4.0f, NAN}, {0.1f, 0.1f}, {INFINITY, 2.0f}};
    for (size_t pattern = 0; pattern < sizeof(patterns) / sizeof(patterns[0]); pattern++)
    {
        tape_begin(ROUND);
        tape_dash(patterns[pattern][0], patterns[pattern][1]);
        tape_dash_offset(3.0f, 1.0f);
        tape_line();
        render_bands(NAN, RASTER_SIDE);
        if (memcmp(s_framebuffer, solid, sizeof(solid)) != 0)
            return fail("a pattern with no length to it is not drawn solid");
    }

    /* Pairs past the limit are left off. */
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    tape_begin(BUTT);
    for (int pair = 0; pair < ER_VECTOR_DASH_MAX; pair++)
        tape_dash(3.0f, 2.0f);
    tape_line();
    render();
    memcpy(reference, s_framebuffer, sizeof(reference));
    tape_begin(BUTT);
    for (int pair = 0; pair < ER_VECTOR_DASH_MAX; pair++)
        tape_dash(3.0f, 2.0f);
    tape_dash(40.0f, 40.0f);
    tape_line();
    render();
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a dash past the limit was drawn");

    /* An offset no float can place, and tapes that end inside a dash op. */
    tape_begin(BUTT);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(INFINITY, 0.0f);
    tape_line();
    render();
    if (alpha_at(15, 64) != 255 || alpha_at(25, 64) != 0)
        return fail("an infinite dash offset is not taken as none");
    tape_begin(BUTT);
    tape_dash(10.0f, 10.0f);
    s_tape_count--;
    render();
    tape_begin(BUTT);
    tape_line();
    tape_dash_offset(1.0f, 1.0f);
    s_tape_count -= 2;
    render();
    return EXIT_SUCCESS;
}

/** @brief ER_PROP_VECTOR_DASH_OFFSET reaches a vector node, from its props and from an animation. */
static int check_node_prop(void)
{
    er_reset();
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERNode* vector = er_node_create(ER_NODE_VECTOR);
    ERProps props;
    er_props_default(&props);
    props.width = RASTER_SIDE;
    props.height = RASTER_SIDE;
    er_node_set_props(root, &props);
    er_node_set_props(vector, &props);
    er_tree_set_root(root);
    er_tree_append_child(root, vector);

    tape_begin(BUTT);
    tape_dash(10.0f, 10.0f);
    tape_dash_offset(0.0f, 1.0f);
    tape_line();
    er_node_set_vector_ops(vector, s_tape, s_tape_count, s_paints, PAINT_COUNT, NULL, 0);
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_commit();
    if (alpha_at(15, 64) != 255 || alpha_at(25, 64) != 0)
        return fail("a vector node did not draw its dashes");

    props.vector_dash_offset = 10.0f;
    er_node_set_props(vector, &props);
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_force_full_repaint();
    er_commit();
    if (alpha_at(15, 64) != 0 || alpha_at(25, 64) != 255)
        return fail("vector_dash_offset did not move a vector node's dashes");

    er_animation_start(vector, ER_PROP_VECTOR_DASH_OFFSET, 20.0f, NULL);
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_commit();
    if (alpha_at(15, 64) != 255 || alpha_at(25, 64) != 0)
        return fail("ER_PROP_VECTOR_DASH_OFFSET did not move a vector node's dashes, or did not repaint it");

    if (er_animation_start(root, ER_PROP_VECTOR_DASH_OFFSET, 5.0f, NULL) != ER_ANIMATION_HANDLE_INVALID)
        return fail("ER_PROP_VECTOR_DASH_OFFSET was taken by a node that is not a vector");
    er_reset();
    return EXIT_SUCCESS;
}

int main(void)
{
    static uint32_t raster_framebuffer[RASTER_SIDE * RASTER_SIDE];
    s_framebuffer = raster_framebuffer;
    s_framebuffer_side = RASTER_SIDE;
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    if (check_pattern() != EXIT_SUCCESS || check_caps() != EXIT_SUCCESS || check_closed() != EXIT_SUCCESS
        || check_node_offset() != EXIT_SUCCESS || check_bands_cache_arcs() != EXIT_SUCCESS
        || check_limits() != EXIT_SUCCESS || check_node_prop() != EXIT_SUCCESS || check_curve_length() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    printf("PASS: vector dash\n");
    return EXIT_SUCCESS;
}
