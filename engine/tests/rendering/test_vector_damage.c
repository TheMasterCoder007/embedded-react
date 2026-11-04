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
 * Incremental <Svg> damage must land the same pixels a full repaint would. er_node_set_vector_ops
 * diffs the incoming op-tape against the stored one and damages only the changed sub-region; these
 * scenarios move geometry the way an app does and compare the incremental framebuffer against a
 * forced full repaint of the identical scene.
 *
 *   - rotating needle: a line whose far endpoint moves — the whole segment sweeps, not just the tip,
 *   - closed needle: the same with a trailing Z, whose closing segment sweeps too,
 *   - curved pointer: a cubic whose end control points move,
 *   - pivoting line / cubic / arc: the mirror image, where a segment's own coordinates are untouched
 *     and its ANCHOR moves instead — the shape still sweeps, about its far end.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h" /* er_force_full_repaint — the reference frame */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN 200
#define OPACITY_OPAQUE 255U

static uint32_t s_framebuffer[SCREEN * SCREEN];

static uint32_t div255(uint32_t value)
{
    return (value + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
}

/** @brief Straight-alpha source-over fill (mirrors backends/software). */
static void fb_fill(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t angle = (argb >> 24) & 0xFFU;
    if (angle == 0U || width <= 0 || height <= 0)
        return;
    for (int row = y; row < y + height; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int column = x; column < x + width; column++)
        {
            if (column < 0 || column >= SCREEN)
                continue;
            uint32_t* destination = &s_framebuffer[row * SCREEN + column];
            if (angle == 0xFFU)
            {
                *destination = 0xFF000000U | (argb & 0x00FFFFFFU);
            }
            else
            {
                const uint32_t inverse_alpha = ER_CHANNEL_MAX - angle;
                const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                               destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
                const uint32_t source_red = (argb >> 16) & 0xFFU, source_green = (argb >> 8) & 0xFFU,
                               source_blue = argb & 0xFFU;
                const uint32_t red = div255(source_red * angle) + div255(destination_red * inverse_alpha);
                const uint32_t green = div255(source_green * angle) + div255(destination_green * inverse_alpha);
                const uint32_t blue = div255(source_blue * angle) + div255(destination_blue * inverse_alpha);
                *destination = 0xFF000000U | (red << 16) | (green << 8) | blue;
            }
        }
    }
}

/** @brief Copies a premultiplied ARGB8888 buffer in (opaque overwrite). */
static void fb_copy(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t* source_pixels = source;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : width;
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++)
        {
            const int framebuffer_y = y + row, framebuffer_x = x + column;
            if (framebuffer_y >= 0 && framebuffer_x >= 0 && framebuffer_y < SCREEN && framebuffer_x < SCREEN)
                s_framebuffer[framebuffer_y * SCREEN + framebuffer_x] = source_pixels[row * pitch + column];
        }
}

/**
 * @brief Source-over blend of a PREMULTIPLIED ARGB8888 buffer at a global alpha.
 *
 * This is where anti-aliased vector output lands — a backend without it silently draws nothing at
 * all, which is exactly how a damage test can pass while painting no ink.
 */
static void fb_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const uint32_t* source_pixels = source;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : width;
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++)
        {
            const int framebuffer_y = y + row, framebuffer_x = x + column;
            if (framebuffer_y < 0 || framebuffer_x < 0 || framebuffer_y >= SCREEN || framebuffer_x >= SCREEN)
                continue;
            const uint32_t svg_props = source_pixels[row * pitch + column];
            const uint32_t source_alpha = div255(((svg_props >> 24) & 0xFFU) * alpha);
            const uint32_t source_red = div255(((svg_props >> 16) & 0xFFU) * alpha);
            const uint32_t source_green = div255(((svg_props >> 8) & 0xFFU) * alpha);
            const uint32_t source_blue = div255((svg_props & 0xFFU) * alpha);
            uint32_t* destination = &s_framebuffer[framebuffer_y * SCREEN + framebuffer_x];
            const uint32_t inverse_alpha = ER_CHANNEL_MAX - source_alpha;
            const uint32_t destination_red = (*destination >> 16) & 0xFFU,
                           destination_green = (*destination >> 8) & 0xFFU, destination_blue = *destination & 0xFFU;
            *destination = 0xFF000000U | ((source_red + div255(destination_red * inverse_alpha)) << 16)
                           | ((source_green + div255(destination_green * inverse_alpha)) << 8)
                           | (source_blue + div255(destination_blue * inverse_alpha));
        }
}

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

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

static void frame(void)
{
    er_commit();
    er_display_present();
}

/* One stroked shape, so the paint table never changes between uploads. */
static ERVectorPaint g_paint;

static void paint_init(void)
{
    memset(&g_paint, 0, sizeof(g_paint));
    g_paint.fill = 0x00000000U;
    g_paint.stroke = 0xFFF4A261U;
    g_paint.stroke_width = 3.0f;
    g_paint.miter = 4.0f;
}

/** @brief Builds the scene: an opaque root with one 160x160 <Svg> node at (20,20). */
static void build_scene(ERNode** out_root, ERNode** out_svg)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = SCREEN;
    root_props.height = SCREEN;
    root_props.background_color = 0xFF101820U;
    er_node_set_props(root, &root_props);

    ERNode* svg = er_node_create(ER_NODE_VECTOR);
    ERProps svg_props = props_default();
    svg_props.width = 160;
    svg_props.height = 160;
    svg_props.position = ER_POSITION_ABSOLUTE;
    svg_props.left = 20;
    svg_props.top = 20;
    er_node_set_props(svg, &svg_props);

    er_tree_append_child(root, svg);
    er_tree_set_root(root);
    *out_root = root;
    *out_svg = svg;
}

/**
 * @brief Builds one frame of a tape. @p step advances the animation; 0 is the resting pose.
 *
 * The `needle`/`curve` builders move a segment's own coordinates and hold its anchor still; the
 * `pivot` builders do the opposite, moving the anchor and holding the segment's coordinates. Both
 * sweep the shape, and both must be damaged in full.
 */
typedef void (*TapeFn)(int step, float* tape, int* tape_count);

/* Local coordinates inside the 160x160 <Svg> box. */
#define HUB_X 80.0f
#define HUB_Y 80.0f
#define NEEDLE_RADIUS 60.0f

static void tape_needle_open(int step, float* tape, int* tape_count)
{
    const float angle = 0.12f * (float)step;
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = HUB_X;
    tape[cursor++] = HUB_Y;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = HUB_X + NEEDLE_RADIUS * cosf(angle);
    tape[cursor++] = HUB_Y + NEEDLE_RADIUS * sinf(angle);
    *tape_count = cursor;
}

static void tape_needle_closed(int step, float* tape, int* tape_count)
{
    int cursor = 0;
    tape_needle_open(step, tape, &cursor);
    tape[cursor++] = (float)ER_VECTOR_OP_CLOSE;
    *tape_count = cursor;
}

static void tape_curve(int step, float* tape, int* tape_count)
{
    const float angle = 0.12f * (float)step;
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = HUB_X;
    tape[cursor++] = HUB_Y;
    tape[cursor++] = (float)ER_VECTOR_OP_CUBIC;
    tape[cursor++] = HUB_X + 30.0f * cosf(angle);
    tape[cursor++] = HUB_Y + 30.0f * sinf(angle);
    tape[cursor++] = HUB_X + 50.0f * cosf(angle);
    tape[cursor++] = HUB_Y + 50.0f * sinf(angle);
    tape[cursor++] = HUB_X + NEEDLE_RADIUS * cosf(angle);
    tape[cursor++] = HUB_Y + NEEDLE_RADIUS * sinf(angle);
    *tape_count = cursor;
}

/** @brief A line from inside the node to far past it, moved down: its changed bounds run past the int16 rect. */
static void tape_far_line(int step, float* tape, int* tape_count)
{
    const float y = 20.0f + 20.0f * (float)step;
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = 20.0f;
    tape[cursor++] = y;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = 40000.0f;
    tape[cursor++] = y;
    *tape_count = cursor;
}

/**
 * @brief An arc's end swept through huge and infinite angles and back. A turn or more draws the whole circle and
 *        an infinite sweep nothing; every change between them has to be damaged in full.
 */
static void tape_arc_blowup(int step, float* tape, int* tape_count)
{
    static const float k_end[7] = {1.2f, 1e10f, -1e10f, INFINITY, 1.2f, -INFINITY, 2.0f};
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = HUB_X + 40.0f;
    tape[cursor++] = HUB_Y;
    tape[cursor++] = (float)ER_VECTOR_OP_ARC;
    tape[cursor++] = HUB_X;
    tape[cursor++] = HUB_Y;
    tape[cursor++] = 40.0f;
    tape[cursor++] = 0.0f;
    tape[cursor++] = k_end[step];
    tape[cursor++] = 0.0f;
    *tape_count = cursor;
}

/** @brief The anchor slides down the left edge; the far endpoint never moves, so the line pivots. */
static void tape_pivot_line(int step, float* tape, int* tape_count)
{
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = 30.0f;
    tape[cursor++] = 40.0f + 8.0f * (float)step;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = 140.0f;
    tape[cursor++] = 80.0f;
    *tape_count = cursor;
}

/** @brief Same pivot, with every cubic control point held fixed. */
static void tape_pivot_cubic(int step, float* tape, int* tape_count)
{
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = 30.0f;
    tape[cursor++] = 40.0f + 8.0f * (float)step;
    tape[cursor++] = (float)ER_VECTOR_OP_CUBIC;
    tape[cursor++] = 70.0f;
    tape[cursor++] = 30.0f;
    tape[cursor++] = 110.0f;
    tape[cursor++] = 30.0f;
    tape[cursor++] = 140.0f;
    tape[cursor++] = 80.0f;
    *tape_count = cursor;
}

/** @brief Same pivot into an arc: the segment from the pen to the arc's start point sweeps, while the
 *         arc's own parameters never change. */
static void tape_pivot_arc(int step, float* tape, int* tape_count)
{
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = 30.0f;
    tape[cursor++] = 40.0f + 8.0f * (float)step;
    tape[cursor++] = (float)ER_VECTOR_OP_ARC;
    tape[cursor++] = HUB_X;
    tape[cursor++] = HUB_Y;
    tape[cursor++] = 40.0f;
    tape[cursor++] = 0.0f;
    tape[cursor++] = 1.2f;
    tape[cursor++] = 0.0f;
    *tape_count = cursor;
}

/** @brief Compares the incremental result against a forced full repaint of the same scene. */
static int check_matches_full_repaint(const char* what)
{
    uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    er_force_full_repaint();
    frame();

    /* A backend with no blend_rect draws no anti-aliased vector output at all, which would make every
     * assertion below pass against a blank screen. Prove the shape actually inked something first. */
    long ink = 0;
    for (int pixel_index = 0; pixel_index < SCREEN * SCREEN; pixel_index++)
        if (s_framebuffer[pixel_index] != 0xFF101820U)
            ink++;
    if (ink < 100)
        return fail("the reference frame drew (almost) nothing — the scene never painted");

    if (memcmp(incremental, s_framebuffer, sizeof(incremental)) != 0)
    {
        long bad = 0;
        for (int pixel_index = 0; pixel_index < SCREEN * SCREEN; pixel_index++)
            if (incremental[pixel_index] != s_framebuffer[pixel_index])
                bad++;
        fprintf(stderr, "  %ld stale pixels\n", bad);
        return fail(what);
    }
    return EXIT_SUCCESS;
}

/** @brief Animates @p build_tape for a few frames, then asserts the incremental result is pixel-exact. */
static int check_sweep(const char* label, TapeFn build_tape)
{
    ERNode *root, *svg;
    build_scene(&root, &svg);
    float tape[24];
    int count = 0;

    build_tape(0, tape, &count);
    er_node_set_vector_ops(svg, tape, count, &g_paint, 1, NULL, 0);
    frame();
    frame();

    for (int step = 1; step <= 6; step++)
    {
        build_tape(step, tape, &count);
        er_node_set_vector_ops(svg, tape, count, &g_paint, 1, NULL, 0);
        frame();
    }

    const int result = check_matches_full_repaint(label);
    er_node_destroy(root);
    if (result == EXIT_SUCCESS)
        printf("PASS: %s matches a full repaint\n", label);
    return result;
}

/**
 * @brief A dirty rect hinted far past the node on both sides still damages all of it.
 *
 * The engine keeps the rect in int16 fields, where a cast alone wraps -40000 and 80000 into a rect off to the
 * side that damages none of the node, so the needle's new pose would never reach the screen.
 */
static int check_huge_dirty_rect(void)
{
    ERNode *root, *svg;
    build_scene(&root, &svg);
    float tape[24];
    int count = 0;

    tape_needle_open(0, tape, &count);
    er_node_set_vector_ops(svg, tape, count, &g_paint, 1, NULL, 0);
    frame();
    frame();

    tape_needle_open(3, tape, &count);
    er_node_set_vector_ops(svg, tape, count, &g_paint, 1, NULL, 0);
    er_node_set_vector_dirty_rect(svg, -40000, -40000, 80000, 80000);
    frame();

    const int result = check_matches_full_repaint("a dirty rect past the int16 range should still damage the node");
    er_node_destroy(root);
    if (result == EXIT_SUCCESS)
        printf("PASS: a dirty rect past the int16 range still damages the node\n");
    return result;
}

#if ERUI_GRADIENT
/**
 * @brief A tape of two shapes: a rect filled with grads[0] that never moves, and a stroked line at @p line_y.
 *
 * The rect sits left of the line, so the line's own damage never reaches it.
 */
static int tape_grad_rect_and_line(float line_y, float* tape)
{
    int cursor = 0;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 0.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = 10.0f;
    tape[cursor++] = 10.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = 60.0f;
    tape[cursor++] = 10.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = 60.0f;
    tape[cursor++] = 150.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = 10.0f;
    tape[cursor++] = 150.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_CLOSE;
    tape[cursor++] = (float)ER_VECTOR_OP_SHAPE;
    tape[cursor++] = 1.0f;
    tape[cursor++] = (float)ER_VECTOR_OP_MOVE;
    tape[cursor++] = 110.0f;
    tape[cursor++] = line_y;
    tape[cursor++] = (float)ER_VECTOR_OP_LINE;
    tape[cursor++] = 140.0f;
    tape[cursor++] = line_y;
    return cursor;
}

/**
 * @brief A gradient changed in the same upload that moves another shape repaints the whole node.
 *
 * The tape diff sees only ops and paints, so the moving line alone would narrow the damage to its own bounds
 * and leave the gradient-filled rect, whose geometry never changes, in its old colours.
 */
static int check_gradient_change(void)
{
    ERNode *root, *svg;
    build_scene(&root, &svg);

    ERVectorPaint paints[2];
    memset(paints, 0, sizeof(paints));
    paints[0].fill_rule = ER_VECTOR_FILL_NONZERO;
    paints[0].fill_gradient = 1; /* grads[0] */
    paints[1] = g_paint;

    ERVectorGradient grad;
    memset(&grad, 0, sizeof(grad));
    grad.type = ER_GRADIENT_LINEAR;
    grad.stop_count = 2;
    grad.stops[0].color = 0xFFE63946U;
    grad.stops[0].position = 0.0f;
    grad.stops[1].color = 0xFF457B9DU;
    grad.stops[1].position = 1.0f;
    grad.ax = 10.0f;
    grad.ay = 10.0f;
    grad.bx = 10.0f;
    grad.by = 150.0f; /* down the rect */

    float tape[32];
    int count = tape_grad_rect_and_line(20.0f, tape);
    er_node_set_vector_ops(svg, tape, count, paints, 2, &grad, 1);
    frame();
    frame();

    /* One upload moves the line and recolours the rect's gradient. */
    count = tape_grad_rect_and_line(60.0f, tape);
    grad.stops[0].color = 0xFF2A9D8FU;
    grad.stops[1].color = 0xFFE9C46AU;
    er_node_set_vector_ops(svg, tape, count, paints, 2, &grad, 1);
    frame();

    const int result = check_matches_full_repaint("a gradient changed beside a moving shape should repaint the node");
    er_node_destroy(root);
    if (result == EXIT_SUCCESS)
        printf("PASS: a gradient changed beside a moving shape repaints the node\n");
    return result;
}
#endif

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {
        .fill_rect = fb_fill,
        .copy_rect = fb_copy,
        .blend_rect = fb_blend,
    };
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    embedded_renderer_set_backend(&k_backend);
    paint_init();

    static const struct
    {
        const char* label;
        TapeFn build_tape;
    } k_cases[] = {
        {"rotating needle", tape_needle_open},
        {"closed rotating needle", tape_needle_closed},
        {"curved pointer", tape_curve},
        {"pivoting line (anchor moves)", tape_pivot_line},
        {"pivoting cubic (anchor moves)", tape_pivot_cubic},
        {"pivoting arc (anchor moves)", tape_pivot_arc},
        {"line running far past the node", tape_far_line},
        {"arc swept through huge and infinite angles", tape_arc_blowup},
    };

    /* Every case runs even after one fails: the set of failures says which shape kinds lost damage. */
    int failures = 0;
    for (size_t case_index = 0; case_index < sizeof(k_cases) / sizeof(k_cases[0]); case_index++)
    {
        if (check_sweep(k_cases[case_index].label, k_cases[case_index].build_tape) != EXIT_SUCCESS)
            failures++;
        er_reset();
    }
    if (check_huge_dirty_rect() != EXIT_SUCCESS)
        failures++;
    er_reset();
#if ERUI_GRADIENT
    if (check_gradient_change() != EXIT_SUCCESS)
        failures++;
    er_reset();
#endif
    if (failures > 0)
    {
        fprintf(stderr, "%d vector damage case(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    printf("All vector damage tests passed\n");
    return EXIT_SUCCESS;
}
