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
 * A transformed <Svg> paints through its matrix, with no transform buffer.
 *
 * The first half drives the rasterizer: translate, rotate and scale matrices against an untransformed
 * render of the same picture, the clip to the turned box, the analytic arc route and the edge cache.
 * The second half (ERUI_TRANSFORMS=FULL) drives a node larger than the transform buffer through the
 * compositor: it turns, an idle commit repaints nothing, and a partial repaint matches a full one.
 */

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
#define FILL_COLOR 0xFF3355AAu
#define STROKE_COLOR 0xFFFFFFFFu
#define PI_FLOAT 3.14159265f

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

/*----------------------------------------------------------------------------------------------------------------------
 - Matrices
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Fills in the inverse of a transform's matrix. */
static void invert(ERVectorTransform* transform)
{
    const float determinant = transform->a * transform->d - transform->b * transform->c;
    transform->inverse_a = transform->d / determinant;
    transform->inverse_b = -transform->b / determinant;
    transform->inverse_c = -transform->c / determinant;
    transform->inverse_d = transform->a / determinant;
    transform->inverse_translate_x =
        -(transform->inverse_a * transform->translate_x + transform->inverse_c * transform->translate_y);
    transform->inverse_translate_y =
        -(transform->inverse_b * transform->translate_x + transform->inverse_d * transform->translate_y);
}

/** @brief A matrix from its six terms, clipped to a layout-space box. */
static ERVectorTransform make_transform(float a, float b, float c, float d, float translate_x, float translate_y)
{
    ERVectorTransform transform = {0};
    transform.a = a;
    transform.b = b;
    transform.c = c;
    transform.d = d;
    transform.translate_x = translate_x;
    transform.translate_y = translate_y;
    transform.box_x0 = 0.0f;
    transform.box_y0 = 0.0f;
    transform.box_x1 = (float)RASTER_SIDE;
    transform.box_y1 = (float)RASTER_SIDE;
    invert(&transform);
    return transform;
}

/** @brief A rotation with a scale on each axis, about a pivot. */
static ERVectorTransform make_rotation(float degrees, float scale_x, float scale_y, float pivot_x, float pivot_y)
{
    const float radians = degrees * PI_FLOAT / 180.0f;
    const float a = scale_x * cosf(radians), b = scale_x * sinf(radians);
    const float c = -scale_y * sinf(radians), d = scale_y * cosf(radians);
    return make_transform(a, b, c, d, pivot_x - (a * pivot_x + c * pivot_y), pivot_y - (b * pivot_x + d * pivot_y));
}

/*----------------------------------------------------------------------------------------------------------------------
 - Rasterizer
 ---------------------------------------------------------------------------------------------------------------------*/

static const ERVectorPaint s_fill_paint = {
    FILL_COLOR, 0x00000000u, 0.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0};

/** @brief Appends a filled rectangle shape, using paint 0, to a tape. */
static int tape_rect(float* tape, float x0, float y0, float x1, float y1)
{
    const float rect[] = {ER_VECTOR_OP_SHAPE,
                          0,
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
    memcpy(tape, rect, sizeof(rect));
    return (int)(sizeof(rect) / sizeof(rect[0]));
}

/** @brief Renders a slot over a cleared frame. */
static void render(int slot, int origin_x, int origin_y, const ERVectorTransform* transform)
{
    memset(s_framebuffer, 0, sizeof(uint32_t) * RASTER_SIDE * RASTER_SIDE);
    er_vector_render_slot(slot, origin_x, origin_y, transform, 0, 0, RASTER_SIDE, RASTER_SIDE);
}

/* A filled and stroked quad, a round-joined zigzag and a full circle: every route the rasterizer has. */
static const float s_mixed_tape[] = {ER_VECTOR_OP_SHAPE,
                                     0,
                                     ER_VECTOR_OP_MOVE,
                                     12,
                                     12,
                                     ER_VECTOR_OP_LINE,
                                     52,
                                     16,
                                     ER_VECTOR_OP_LINE,
                                     48,
                                     52,
                                     ER_VECTOR_OP_LINE,
                                     14,
                                     44,
                                     ER_VECTOR_OP_CLOSE,
                                     ER_VECTOR_OP_SHAPE,
                                     1,
                                     ER_VECTOR_OP_MOVE,
                                     70,
                                     20,
                                     ER_VECTOR_OP_LINE,
                                     90,
                                     40,
                                     ER_VECTOR_OP_LINE,
                                     74,
                                     58,
                                     ER_VECTOR_OP_LINE,
                                     96,
                                     74,
                                     ER_VECTOR_OP_SHAPE,
                                     2,
                                     ER_VECTOR_OP_ARC,
                                     40,
                                     90,
                                     20,
                                     0.0f,
                                     6.2832f,
                                     0};
#define MIXED_TAPE_COUNT ((int)(sizeof(s_mixed_tape) / sizeof(s_mixed_tape[0])))
static const ERVectorPaint s_mixed_paints[3] = {
    {FILL_COLOR, STROKE_COLOR, 3.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
    {0x00000000u, 0xFF22CC44u, 5.0f, 0.0f, ER_VECTOR_CAP_ROUND, ER_VECTOR_JOIN_ROUND, ER_VECTOR_FILL_NONZERO, 0, 0},
    {0xFF884422u, 0xFFDDDDDDu, 4.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0},
};

/** @brief A translate matrix paints what a moved origin paints. */
static int check_translate_matches_origin(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    er_vector_reset();
    const int slot = er_vector_store(-1, s_mixed_tape, MIXED_TAPE_COUNT, s_mixed_paints, 3, NULL, 0);
    if (slot < 0)
        return fail("translate: no storage slot");
    render(slot, 10, 6, NULL);
    memcpy(reference, s_framebuffer, sizeof(reference));

    ERVectorTransform transform = make_transform(1.0f, 0.0f, 0.0f, 1.0f, 10.0f, 6.0f);
    transform.box_x0 = -10.0f;
    transform.box_y0 = -6.0f;
    render(slot, 0, 0, &transform);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a translate matrix differs from the same shift of the origin");
    return EXIT_SUCCESS;
}

/** @brief A quarter turn of a bar is the upright bar, and a doubled line has a doubled stroke. */
static int check_rotate_and_scale(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    float tape[32];
    er_vector_reset();

    int tape_count = tape_rect(tape, 56, 20, 72, 108);
    const int upright = er_vector_store(-1, tape, tape_count, &s_fill_paint, 1, NULL, 0);
    tape_count = tape_rect(tape, 20, 56, 108, 72);
    const int flat = er_vector_store(-1, tape, tape_count, &s_fill_paint, 1, NULL, 0);
    render(upright, 0, 0, NULL);
    memcpy(reference, s_framebuffer, sizeof(reference));
    /* 90 degrees about the centre, written out so the terms are exact. */
    const ERVectorTransform quarter_turn = make_transform(0.0f, 1.0f, -1.0f, 0.0f, 128.0f, 0.0f);
    render(flat, 0, 0, &quarter_turn);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a bar turned 90 degrees is not the upright bar");
    if (get_pixel(64, 30) != FILL_COLOR || get_pixel(30, 64) != 0U)
        return fail("the turned bar is not where it should be");

    const ERVectorPaint thin = {
        0x00000000u, STROKE_COLOR, 4.0f, 0.0f, ER_VECTOR_CAP_BUTT, ER_VECTOR_JOIN_MITER, ER_VECTOR_FILL_NONZERO, 0, 0};
    ERVectorPaint thick = thin;
    thick.stroke_width = 8.0f;
    const float short_line[] = {ER_VECTOR_OP_SHAPE, 0, ER_VECTOR_OP_MOVE, 10, 20, ER_VECTOR_OP_LINE, 50, 20};
    const float long_line[] = {ER_VECTOR_OP_SHAPE, 0, ER_VECTOR_OP_MOVE, 20, 40, ER_VECTOR_OP_LINE, 100, 40};
    const int short_slot = er_vector_store(-1, short_line, 8, &thin, 1, NULL, 0);
    const int long_slot = er_vector_store(-1, long_line, 8, &thick, 1, NULL, 0);
    render(long_slot, 0, 0, NULL);
    memcpy(reference, s_framebuffer, sizeof(reference));
    ERVectorTransform doubled = make_transform(2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f);
    doubled.box_x1 = doubled.box_y1 = (float)RASTER_SIDE / 2.0f;
    render(short_slot, 0, 0, &doubled);
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("a line scaled by two does not have a stroke twice as wide");
    if (get_pixel(60, 37) != STROKE_COLOR || get_pixel(60, 34) != 0U)
        return fail("the scaled stroke is not where it should be");
    return EXIT_SUCCESS;
}

/** @brief Ink is clipped to the turned box, not to its screen bounds. */
static int check_box_clip(void)
{
    float tape[32];
    er_vector_reset();
    const int tape_count = tape_rect(tape, -60, -60, 190, 190);
    const int slot = er_vector_store(-1, tape, tape_count, &s_fill_paint, 1, NULL, 0);
    ERVectorTransform transform = make_rotation(45.0f, 1.0f, 1.0f, 64.0f, 64.0f);
    transform.box_x0 = transform.box_y0 = 32.0f;
    transform.box_x1 = transform.box_y1 = 96.0f;
    render(slot, 0, 0, &transform);
    /* The box is now a diamond reaching 45 px from the centre along each axis. */
    if (get_pixel(64, 64) != FILL_COLOR || get_pixel(64, 24) != FILL_COLOR || get_pixel(100, 64) != FILL_COLOR)
        return fail("the turned box is missing ink");
    if (get_pixel(36, 36) != 0U || get_pixel(92, 92) != 0U)
        return fail("ink shows in a corner the turned box left");
    if (get_pixel(64, 14) != 0U || get_pixel(114, 64) != 0U)
        return fail("ink shows past the turned box");
    return EXIT_SUCCESS;
}

/** @brief A circle stays on the analytic route under a turn, and leaves it when the scale is uneven. */
static int check_arc_route(void)
{
    er_vector_reset();
    const int slot = er_vector_store(-1, s_mixed_tape, MIXED_TAPE_COUNT, s_mixed_paints, 3, NULL, 0);
    const ERVectorTransform turned = make_rotation(30.0f, 1.0f, 1.0f, 64.0f, 64.0f);
    er_vector_analytic_arc_count_reset();
    render(slot, 0, 0, &turned);
    if (er_vector_analytic_arc_count() != 1U)
        return fail("a turned circle left the analytic arc route");
    float centre_x = 40.0f, centre_y = 90.0f;
    int screen_x = (int)(turned.a * centre_x + turned.c * centre_y + turned.translate_x);
    int screen_y = (int)(turned.b * centre_x + turned.d * centre_y + turned.translate_y);
    if (get_pixel(screen_x, screen_y) != s_mixed_paints[2].fill)
        return fail("the turned circle is not at its mapped centre");

    const ERVectorTransform squashed = make_rotation(30.0f, 1.2f, 0.7f, 64.0f, 64.0f);
    er_vector_analytic_arc_count_reset();
    render(slot, 0, 0, &squashed);
    if (er_vector_analytic_arc_count() != 0U)
        return fail("an unevenly scaled circle took the analytic arc route");
    screen_x = (int)(squashed.a * centre_x + squashed.c * centre_y + squashed.translate_x);
    screen_y = (int)(squashed.b * centre_x + squashed.d * centre_y + squashed.translate_y);
    if (get_pixel(screen_x, screen_y) != s_mixed_paints[2].fill)
        return fail("the squashed circle is not at its mapped centre");
    return EXIT_SUCCESS;
}

/** @brief One recording replays through other matrices and origins, matching an uncached render. */
static int check_cache_replay(void)
{
    static uint32_t reference[RASTER_SIDE * RASTER_SIDE];
    er_vector_reset();
    er_vector_cache_stats_reset();
    const int cached = er_vector_store(-1, s_mixed_tape, MIXED_TAPE_COUNT, s_mixed_paints, 3, NULL, 0);
    if (cached < 0)
        return fail("cache: no storage slot");

    const ERVectorTransform first = make_rotation(20.0f, 1.0f, 1.0f, 64.0f, 64.0f);
    render(cached, 0, 0, &first); /* arms */
    memcpy(reference, s_framebuffer, sizeof(reference));
    render(cached, 0, 0, &first); /* records */
    if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
        return fail("the recording render differs from the one before it");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 1U || er_vector_cache_hits() != 0U)
        return fail("a transformed node did not record on its second render");
#endif

    /* Each replay is compared with a first render of a fresh slot, which the cache never serves. */
    const ERVectorTransform turns[] = {make_rotation(75.0f, 1.0f, 1.0f, 64.0f, 64.0f),
                                       make_rotation(200.0f, 1.0f, 1.0f, 64.0f, 64.0f),
                                       make_rotation(40.0f, 1.3f, 0.8f, 64.0f, 64.0f)};
    for (int turn = 0; turn < 3; turn++)
    {
        const int fresh = er_vector_store(-1, s_mixed_tape, MIXED_TAPE_COUNT, s_mixed_paints, 3, NULL, 0);
        render(fresh, 0, 0, &turns[turn]);
        memcpy(reference, s_framebuffer, sizeof(reference));
        er_vector_free(fresh);
        render(cached, 0, 0, &turns[turn]);
        if (memcmp(s_framebuffer, reference, sizeof(reference)) != 0)
            return fail("a replay through a new matrix differs from an uncached render");
    }
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 1U || er_vector_cache_hits() != 3U)
        return fail("a new matrix did not replay the recording");
#endif

    /* A moved node replays too: its edges are shifted, so allow the rounding that brings. */
    ERVectorTransform moved = make_rotation(75.0f, 1.0f, 1.0f, 69.0f, 67.0f);
    moved.box_x0 = 5.0f;
    moved.box_y0 = 3.0f;
    moved.box_x1 = (float)RASTER_SIDE + 5.0f;
    moved.box_y1 = (float)RASTER_SIDE + 3.0f;
    const int fresh = er_vector_store(-1, s_mixed_tape, MIXED_TAPE_COUNT, s_mixed_paints, 3, NULL, 0);
    render(fresh, 5, 3, &moved);
    memcpy(reference, s_framebuffer, sizeof(reference));
    render(cached, 5, 3, &moved);
    if (frame_difference(s_framebuffer, reference, RASTER_SIDE * RASTER_SIDE) > 3)
        return fail("a replay at a new origin differs from an uncached render");
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_hits() != 4U)
        return fail("a moved transformed node did not replay the recording");
#endif

    /* Without a transform the node is a different key: screen-space edges, recorded afresh. */
    render(cached, 0, 0, NULL);
    render(cached, 0, 0, NULL);
#if ERUI_VECTOR_EDGE_CACHE
    if (er_vector_cache_builds() != 2U)
        return fail("dropping the transform did not record the node again");
#endif
    return EXIT_SUCCESS;
}

#if ERUI_GRADIENT
/** @brief A gradient turns with its shape. */
static int check_gradient_follows(void)
{
    float tape[32];
    er_vector_reset();
    const int tape_count = tape_rect(tape, 24, 44, 104, 84);
    ERVectorPaint paint = s_fill_paint;
    paint.fill_gradient = 1;
    ERVectorGradient gradient = {0};
    gradient.type = ER_GRADIENT_LINEAR;
    gradient.stop_count = 2;
    gradient.stops[0].color = 0xFFFF0000u;
    gradient.stops[0].position = 0.0f;
    gradient.stops[1].color = 0xFF0000FFu;
    gradient.stops[1].position = 1.0f;
    gradient.ax = 24.0f;
    gradient.ay = 64.0f;
    gradient.bx = 104.0f;
    gradient.by = 64.0f;
    const int slot = er_vector_store(-1, tape, tape_count, &paint, 1, &gradient, 1);
    const ERVectorTransform quarter_turn = make_transform(0.0f, 1.0f, -1.0f, 0.0f, 128.0f, 0.0f);
    render(slot, 0, 0, &quarter_turn);
    /* Left to right in the tape is top to bottom on screen. */
    const uint32_t top = get_pixel(64, 28), bottom = get_pixel(64, 100);
    if (((top >> 16) & 0xFFU) < 200U || (top & 0xFFU) > 55U)
        return fail("the turned gradient does not start red at the top");
    if ((bottom & 0xFFU) < 200U || ((bottom >> 16) & 0xFFU) > 55U)
        return fail("the turned gradient does not end blue at the bottom");
    if (get_pixel(50, 64) != get_pixel(78, 64))
        return fail("the turned gradient changes across the bar");
    return EXIT_SUCCESS;
}
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Compositor
 ---------------------------------------------------------------------------------------------------------------------*/

#if ERUI_TRANSFORMS_FULL

#define NODE_LEFT 40
#define NODE_TOP 40
#define BACKGROUND_COLOR 0xFFFFFFFFu

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
    if (!s_framebuffer || !partial)
        return fail("node: out of memory");
    s_framebuffer_side = screen;

    er_reset();
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = (int16_t)screen;
    root_props.height = (int16_t)screen;
    root_props.background_color = BACKGROUND_COLOR;
    er_node_set_props(root, &root_props);

    ERNode* svg = er_node_create(ER_NODE_VECTOR);
    ERProps svg_props = props_default();
    svg_props.position = ER_POSITION_ABSOLUTE;
    svg_props.left = NODE_LEFT;
    svg_props.top = NODE_TOP;
    svg_props.width = (int16_t)side;
    svg_props.height = (int16_t)side;
    svg_props.transform_rotate_z = 90.0f;
    svg_props.transform_origin_x = 0.5f;
    svg_props.transform_origin_y = 0.5f;
    er_node_set_props(svg, &svg_props);
    /* A bar along the top quarter of the box. */
    float tape[32];
    const int tape_count = tape_rect(tape, 0.0f, 0.0f, (float)side, (float)side / 4.0f);
    er_node_set_vector_ops(svg, tape, tape_count, &s_fill_paint, 1, NULL, 0);

    er_tree_append_child(root, svg);
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
    svg_props.transform_rotate_z = 180.0f;
    er_node_set_props(svg, &svg_props);
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
    svg_props.transform_rotate_z = 33.0f;
    er_node_set_props(svg, &svg_props);
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

    er_node_destroy(root);
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
    static uint32_t raster_framebuffer[RASTER_SIDE * RASTER_SIDE];
    s_framebuffer = raster_framebuffer;
    s_framebuffer_side = RASTER_SIDE;
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, NULL};
    embedded_renderer_set_backend(&backend);

    if (check_translate_matches_origin() != EXIT_SUCCESS || check_rotate_and_scale() != EXIT_SUCCESS
        || check_box_clip() != EXIT_SUCCESS || check_arc_route() != EXIT_SUCCESS
        || check_cache_replay() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#if ERUI_GRADIENT
    if (check_gradient_follows() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#endif
#if ERUI_TRANSFORMS_FULL
    if (check_node_larger_than_buffer() != EXIT_SUCCESS)
        return EXIT_FAILURE;
#endif

    printf("PASS: vector transform\n");
    return EXIT_SUCCESS;
}
