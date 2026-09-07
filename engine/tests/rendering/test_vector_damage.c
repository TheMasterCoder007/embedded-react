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

static uint32_t s_fb[SCREEN * SCREEN];

static uint32_t div255(uint32_t v)
{
    return (v + 127U) / 255U;
}

/** @brief Straight-alpha source-over fill (mirrors backends/software). */
static void fb_fill(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t a = (argb >> 24) & 0xFFU;
    if (a == 0U || w <= 0 || h <= 0)
        return;
    for (int row = y; row < y + h; row++)
    {
        if (row < 0 || row >= SCREEN)
            continue;
        for (int col = x; col < x + w; col++)
        {
            if (col < 0 || col >= SCREEN)
                continue;
            uint32_t* d = &s_fb[row * SCREEN + col];
            if (a == 0xFFU)
            {
                *d = 0xFF000000U | (argb & 0x00FFFFFFU);
            }
            else
            {
                const uint32_t inv = 255U - a;
                const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
                const uint32_t sr = (argb >> 16) & 0xFFU, sg = (argb >> 8) & 0xFFU, sb = argb & 0xFFU;
                const uint32_t r = div255(sr * a) + div255(dr * inv);
                const uint32_t g = div255(sg * a) + div255(dg * inv);
                const uint32_t b = div255(sb * a) + div255(db * inv);
                *d = 0xFF000000U | (r << 16) | (g << 8) | b;
            }
        }
    }
}

/** @brief Copies a premultiplied ARGB8888 buffer in (opaque overwrite). */
static void fb_copy(const void* src, int stride, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t* px_src = src;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : w;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            const int fy = y + row, fx = x + col;
            if (fy >= 0 && fx >= 0 && fy < SCREEN && fx < SCREEN)
                s_fb[fy * SCREEN + fx] = px_src[row * pitch + col];
        }
}

/**
 * @brief Source-over blend of a PREMULTIPLIED ARGB8888 buffer at a global alpha.
 *
 * This is where anti-aliased vector output lands — a backend without it silently draws nothing at
 * all, which is exactly how a damage test can pass while painting no ink.
 */
static void fb_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const uint32_t* px_src = src;
    const int pitch = (stride > 0) ? stride / (int)sizeof(uint32_t) : w;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            const int fy = y + row, fx = x + col;
            if (fy < 0 || fx < 0 || fy >= SCREEN || fx >= SCREEN)
                continue;
            const uint32_t sp = px_src[row * pitch + col];
            const uint32_t sa = div255(((sp >> 24) & 0xFFU) * alpha);
            const uint32_t sr = div255(((sp >> 16) & 0xFFU) * alpha);
            const uint32_t sg = div255(((sp >> 8) & 0xFFU) * alpha);
            const uint32_t sb = div255((sp & 0xFFU) * alpha);
            uint32_t* d = &s_fb[fy * SCREEN + fx];
            const uint32_t inv = 255U - sa;
            const uint32_t dr = (*d >> 16) & 0xFFU, dg = (*d >> 8) & 0xFFU, db = *d & 0xFFU;
            *d = 0xFF000000U | ((sr + div255(dr * inv)) << 16) | ((sg + div255(dg * inv)) << 8)
                 | (sb + div255(db * inv));
        }
}

static ERProps props_default(void)
{
    ERProps p = {0};
    p.left = p.top = p.right = p.bottom = ER_LAYOUT_AUTO;
    p.width = p.height = ER_LAYOUT_AUTO;
    p.min_width = p.max_width = ER_LAYOUT_AUTO;
    p.min_height = p.max_height = ER_LAYOUT_AUTO;
    p.padding = p.padding_left = p.padding_top = ER_LAYOUT_AUTO;
    p.padding_right = p.padding_bottom = ER_LAYOUT_AUTO;
    p.margin = p.margin_left = p.margin_top = ER_LAYOUT_AUTO;
    p.margin_right = p.margin_bottom = ER_LAYOUT_AUTO;
    p.gap = p.row_gap = p.column_gap = ER_LAYOUT_AUTO;
    p.flex_basis = ER_LAYOUT_AUTO;
    p.opacity = 255U;
    return p;
}

static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
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
    g_paint.stroke_w = 3.0f;
    g_paint.miter = 4.0f;
}

/** @brief Builds the scene: an opaque root with one 160x160 <Svg> node at (20,20). */
static void build_scene(ERNode** out_root, ERNode** out_svg)
{
    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps rp = props_default();
    rp.width = SCREEN;
    rp.height = SCREEN;
    rp.background_color = 0xFF101820U;
    er_node_set_props(root, &rp);

    ERNode* svg = er_node_create(ER_NODE_VECTOR);
    ERProps sp = props_default();
    sp.width = 160;
    sp.height = 160;
    sp.position = ER_POS_ABSOLUTE;
    sp.left = 20;
    sp.top = 20;
    er_node_set_props(svg, &sp);

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
typedef void (*TapeFn)(int step, float* ops, int* n_ops);

/* Local coordinates inside the 160x160 <Svg> box. */
#define HUB_X 80.0f
#define HUB_Y 80.0f
#define NEEDLE_R 60.0f

static void tape_needle_open(int step, float* ops, int* n_ops)
{
    const float a = 0.12f * (float)step;
    int i = 0;
    ops[i++] = (float)ER_VOP_SHAPE;
    ops[i++] = 0.0f;
    ops[i++] = (float)ER_VOP_MOVE;
    ops[i++] = HUB_X;
    ops[i++] = HUB_Y;
    ops[i++] = (float)ER_VOP_LINE;
    ops[i++] = HUB_X + NEEDLE_R * cosf(a);
    ops[i++] = HUB_Y + NEEDLE_R * sinf(a);
    *n_ops = i;
}

static void tape_needle_closed(int step, float* ops, int* n_ops)
{
    int i = 0;
    tape_needle_open(step, ops, &i);
    ops[i++] = (float)ER_VOP_CLOSE;
    *n_ops = i;
}

static void tape_curve(int step, float* ops, int* n_ops)
{
    const float a = 0.12f * (float)step;
    int i = 0;
    ops[i++] = (float)ER_VOP_SHAPE;
    ops[i++] = 0.0f;
    ops[i++] = (float)ER_VOP_MOVE;
    ops[i++] = HUB_X;
    ops[i++] = HUB_Y;
    ops[i++] = (float)ER_VOP_CUBIC;
    ops[i++] = HUB_X + 30.0f * cosf(a);
    ops[i++] = HUB_Y + 30.0f * sinf(a);
    ops[i++] = HUB_X + 50.0f * cosf(a);
    ops[i++] = HUB_Y + 50.0f * sinf(a);
    ops[i++] = HUB_X + NEEDLE_R * cosf(a);
    ops[i++] = HUB_Y + NEEDLE_R * sinf(a);
    *n_ops = i;
}

/** @brief The anchor slides down the left edge; the far endpoint never moves, so the line pivots. */
static void tape_pivot_line(int step, float* ops, int* n_ops)
{
    int i = 0;
    ops[i++] = (float)ER_VOP_SHAPE;
    ops[i++] = 0.0f;
    ops[i++] = (float)ER_VOP_MOVE;
    ops[i++] = 30.0f;
    ops[i++] = 40.0f + 8.0f * (float)step;
    ops[i++] = (float)ER_VOP_LINE;
    ops[i++] = 140.0f;
    ops[i++] = 80.0f;
    *n_ops = i;
}

/** @brief Same pivot, with every cubic control point held fixed. */
static void tape_pivot_cubic(int step, float* ops, int* n_ops)
{
    int i = 0;
    ops[i++] = (float)ER_VOP_SHAPE;
    ops[i++] = 0.0f;
    ops[i++] = (float)ER_VOP_MOVE;
    ops[i++] = 30.0f;
    ops[i++] = 40.0f + 8.0f * (float)step;
    ops[i++] = (float)ER_VOP_CUBIC;
    ops[i++] = 70.0f;
    ops[i++] = 30.0f;
    ops[i++] = 110.0f;
    ops[i++] = 30.0f;
    ops[i++] = 140.0f;
    ops[i++] = 80.0f;
    *n_ops = i;
}

/** @brief Same pivot into an arc: the segment from the pen to the arc's start point sweeps, while the
 *         arc's own parameters never change. */
static void tape_pivot_arc(int step, float* ops, int* n_ops)
{
    int i = 0;
    ops[i++] = (float)ER_VOP_SHAPE;
    ops[i++] = 0.0f;
    ops[i++] = (float)ER_VOP_MOVE;
    ops[i++] = 30.0f;
    ops[i++] = 40.0f + 8.0f * (float)step;
    ops[i++] = (float)ER_VOP_ARC;
    ops[i++] = HUB_X;
    ops[i++] = HUB_Y;
    ops[i++] = 40.0f;
    ops[i++] = 0.0f;
    ops[i++] = 1.2f;
    ops[i++] = 0.0f;
    *n_ops = i;
}

/** @brief Compares the incremental result against a forced full repaint of the same scene. */
static int check_matches_full_repaint(const char* what)
{
    uint32_t incremental[SCREEN * SCREEN];
    memcpy(incremental, s_fb, sizeof(incremental));
    er_force_full_repaint();
    frame();

    /* A backend with no blend_rect draws no anti-aliased vector output at all, which would make every
     * assertion below pass against a blank screen. Prove the shape actually inked something first. */
    long ink = 0;
    for (int i = 0; i < SCREEN * SCREEN; i++)
        if (s_fb[i] != 0xFF101820U)
            ink++;
    if (ink < 100)
        return fail("the reference frame drew (almost) nothing — the scene never painted");

    if (memcmp(incremental, s_fb, sizeof(incremental)) != 0)
    {
        long bad = 0;
        for (int i = 0; i < SCREEN * SCREEN; i++)
            if (incremental[i] != s_fb[i])
                bad++;
        fprintf(stderr, "  %ld stale pixels\n", bad);
        return fail(what);
    }
    return EXIT_SUCCESS;
}

/** @brief Animates @p fn for a few frames, then asserts the incremental result is pixel-exact. */
static int check_sweep(const char* label, TapeFn fn)
{
    ERNode *root, *svg;
    build_scene(&root, &svg);
    float ops[24];
    int n = 0;

    fn(0, ops, &n);
    er_node_set_vector_ops(svg, ops, n, &g_paint, 1, NULL, 0);
    frame();
    frame();

    for (int step = 1; step <= 6; step++)
    {
        fn(step, ops, &n);
        er_node_set_vector_ops(svg, ops, n, &g_paint, 1, NULL, 0);
        frame();
    }

    const int rc = check_matches_full_repaint(label);
    er_node_destroy(root);
    if (rc == EXIT_SUCCESS)
        printf("PASS: %s matches a full repaint\n", label);
    return rc;
}

int main(void)
{
    static const EmbeddedRenderBackend k_backend = {
        .fill_rect = fb_fill,
        .copy_rect = fb_copy,
        .blend_rect = fb_blend,
    };
    memset(s_fb, 0, sizeof(s_fb));
    embedded_renderer_set_backend(&k_backend);
    paint_init();

    static const struct
    {
        const char* label;
        TapeFn fn;
    } k_cases[] = {
        {"rotating needle", tape_needle_open},
        {"closed rotating needle", tape_needle_closed},
        {"curved pointer", tape_curve},
        {"pivoting line (anchor moves)", tape_pivot_line},
        {"pivoting cubic (anchor moves)", tape_pivot_cubic},
        {"pivoting arc (anchor moves)", tape_pivot_arc},
    };

    /* Every case runs even after one fails: the set of failures says which shape kinds lost damage. */
    int failures = 0;
    for (size_t k = 0; k < sizeof(k_cases) / sizeof(k_cases[0]); k++)
    {
        if (check_sweep(k_cases[k].label, k_cases[k].fn) != EXIT_SUCCESS)
            failures++;
        er_reset();
    }
    if (failures > 0)
    {
        fprintf(stderr, "%d vector damage case(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    printf("All vector damage tests passed\n");
    return EXIT_SUCCESS;
}
