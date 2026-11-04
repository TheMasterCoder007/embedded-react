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

#include "transform.h"
#include "er_limits.h"
#include "renderer_internal.h"
#include "scratch_pool.h"
#include <math.h>
#include <string.h>

/* Below these the matrix is treated as singular and the node falls back to an untransformed paint.
 * Named because er_transform_is_invertible() has to answer with EXACTLY the same rule the inverting
 * functions apply — the damage pre-pass picks which ancestor captures by asking it, and a predicate
 * that disagreed with the paint by an epsilon would escalate damage to the wrong node. */
#define ER_TRANSFORM_DETERMINANT_EPSILON_2D 1e-6f
#define ER_TRANSFORM_DETERMINANT_EPSILON_3D 1e-7f

/** @brief A perspective distance at or below this is treated as none (orthographic). */
#define ER_TRANSFORM_PERSPECTIVE_MIN 1e-3f

/** @brief Starting value for a min/max scan over projected corners. */
#define ER_TRANSFORM_UNBOUNDED 1e30f

/** @brief The cache tag that names no node. */
#define ER_TRANSFORM_NO_TAG 0xFFFFu

/** @brief Bilinear weights are fixed point: ER_BILERP_WEIGHT_ONE is a weight of 1.0. */
#define ER_BILERP_WEIGHT_BITS 8u
#define ER_BILERP_WEIGHT_ONE 256u
#define ER_BILERP_WEIGHT_SCALE 256.0f

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

#if ERUI_TRANSFORMS_FULL
/* Source buffer: the untransformed subtree is rendered into this scratch slot. One per render
 * worker so concurrent transform captures on disjoint screen regions never collide (each extra
 * worker costs a full buffer — document when raising ERUI_RENDER_WORKERS). */
static uint32_t s_transform_source_pool[ERUI_RENDER_WORKERS][ERUI_TRANSFORM_HEIGHT * ERUI_TRANSFORM_WIDTH];
/* Row segment buffer: transformed output is streamed out one row segment at a time
 * (≤ ERUI_TRANSFORM_WIDTH pixels), so the destination AABB is not size-limited — only the
 * SOURCE subtree must fit the scratch. */
static uint32_t s_transform_row_pool[ERUI_RENDER_WORKERS][ERUI_TRANSFORM_WIDTH];

/**
 * @brief Per-worker transform-capture state (see er_render_worker_id).
 *
 * Zero-initialized: cache_tag 0 with cache_gen 0 can never false-match because generation tags
 * handed to er_transform_source_is_cached come from a counter that starts at 1.
 */
typedef struct
{
    bool active;         /**< True while a source capture is active. */
    uint8_t saved_alpha; /**< Inherited draw alpha at capture start: the source is captured at full
                            intensity and the alpha is re-applied once at the output blend. */
    /* Source-cache identity: which node's subtree currently sits in this worker's source buffer,
     * and the banded composite pass generation it was captured in. Lets the compositor replay the
     * (expensive) capture-free blit for every band tile after the first instead of re-rendering
     * the subtree once per tile. A new capture invalidates it until the compositor re-notes it. */
    uint16_t cache_tag;
    uint32_t cache_generation;
} XformCtx;

static XformCtx s_worker_contexts[ERUI_RENDER_WORKERS];

/** @brief The calling worker's transform context (constant &s_worker_contexts[0] in single-worker builds). */
static inline XformCtx* xc(void)
{
    return &s_worker_contexts[er_render_worker_id()];
}

/** @brief The calling worker's source capture buffer. */
static inline uint32_t* xsrc(void)
{
    return s_transform_source_pool[er_render_worker_id()];
}

/** @brief The calling worker's output row segment buffer. */
static inline uint32_t* xrowbuf(void)
{
    return s_transform_row_pool[er_render_worker_id()];
}

/* Bilinear channel lerp: a,b in [0,255]; t in [0,256]. Result in [0,255]. */
#define ER_LERP_CH(from, to, weight)                                                                                   \
    (((uint32_t)(from) * (ER_BILERP_WEIGHT_ONE - (weight)) + (uint32_t)(to) * (weight)) >> ER_BILERP_WEIGHT_BITS)

/**
 * @brief Bilinearly blends four premultiplied ARGB8888 corners. weight_x/weight_y in [0,256].
 *
 * Shared by er_bilerp (general path) and the scale-only fast path so both produce identical output.
 */
static inline uint32_t er_blend4(
    uint32_t corner00, uint32_t corner10, uint32_t corner01, uint32_t corner11, uint32_t weight_x, uint32_t weight_y)
{
    const uint32_t out_alpha = ER_LERP_CH(ER_LERP_CH(corner00 >> 24, corner10 >> 24, weight_x),
                                          ER_LERP_CH(corner01 >> 24, corner11 >> 24, weight_x),
                                          weight_y);
    const uint32_t out_red = ER_LERP_CH(ER_LERP_CH((corner00 >> 16) & 0xFFu, (corner10 >> 16) & 0xFFu, weight_x),
                                        ER_LERP_CH((corner01 >> 16) & 0xFFu, (corner11 >> 16) & 0xFFu, weight_x),
                                        weight_y);
    const uint32_t out_green = ER_LERP_CH(ER_LERP_CH((corner00 >> 8) & 0xFFu, (corner10 >> 8) & 0xFFu, weight_x),
                                          ER_LERP_CH((corner01 >> 8) & 0xFFu, (corner11 >> 8) & 0xFFu, weight_x),
                                          weight_y);
    const uint32_t out_blue = ER_LERP_CH(ER_LERP_CH(corner00 & 0xFFu, corner10 & 0xFFu, weight_x),
                                         ER_LERP_CH(corner01 & 0xFFu, corner11 & 0xFFu, weight_x),
                                         weight_y);
    return (out_alpha << 24u) | (out_red << 16u) | (out_green << 8u) | out_blue;
}

/**
 * @brief Samples the source scratch with bilinear filtering.
 *
 * Premultiplied ARGB8888 channels are interpolated independently, so no
 * unpremultiply/repremultiply pass is needed.  Out-of-bounds neighbours
 * are treated as fully transparent (0).
 *
 * @param[in] source         Source pixel buffer (row stride = ERUI_TRANSFORM_WIDTH).
 * @param[in] source_width   Logical width of the source region.
 * @param[in] source_height  Logical height of the source region.
 * @param[in] sample_x       Sub-pixel X in source-local coordinates.
 * @param[in] sample_y       Sub-pixel Y in source-local coordinates.
 *
 * @return Bilinearly filtered premultiplied ARGB8888 pixel.
 */
static uint32_t er_bilerp(const uint32_t* source, int source_width, int source_height, float sample_x, float sample_y)
{
    const int x0 = (int)floorf(sample_x);
    const int y0 = (int)floorf(sample_y);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const uint32_t weight_x = (uint32_t)((sample_x - (float)x0) * ER_BILERP_WEIGHT_SCALE);
    const uint32_t weight_y = (uint32_t)((sample_y - (float)y0) * ER_BILERP_WEIGHT_SCALE);

    const uint32_t corner00 =
        (x0 >= 0 && x0 < source_width && y0 >= 0 && y0 < source_height) ? source[y0 * ERUI_TRANSFORM_WIDTH + x0] : 0u;
    const uint32_t corner10 =
        (x1 >= 0 && x1 < source_width && y0 >= 0 && y0 < source_height) ? source[y0 * ERUI_TRANSFORM_WIDTH + x1] : 0u;
    const uint32_t corner01 =
        (x0 >= 0 && x0 < source_width && y1 >= 0 && y1 < source_height) ? source[y1 * ERUI_TRANSFORM_WIDTH + x0] : 0u;
    const uint32_t corner11 =
        (x1 >= 0 && x1 < source_width && y1 >= 0 && y1 < source_height) ? source[y1 * ERUI_TRANSFORM_WIDTH + x1] : 0u;

    return er_blend4(corner00, corner10, corner01, corner11, weight_x, weight_y);
}

/**
 * @brief Intersects the destination AABB with the active scissor before the emit loops.
 *
 * Rows/columns outside the scissor would be rejected at the blit anyway, so skipping their
 * back-projection entirely turns a banded composite pass from O(dst area) into O(strip area).
 *
 * @return false when nothing survives the intersection.
 */
static bool xform_emit_bounds(int destination_x,
                              int destination_y,
                              int destination_width,
                              int destination_height,
                              int* emit_x0,
                              int* emit_y0,
                              int* emit_x1,
                              int* emit_y1)
{
    *emit_x0 = destination_x;
    *emit_y0 = destination_y;
    *emit_x1 = destination_x + destination_width;
    *emit_y1 = destination_y + destination_height;
    int clip_x, clip_y, clip_width, clip_height;
    if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height))
    {
        if (clip_x > *emit_x0)
            *emit_x0 = clip_x;
        if (clip_y > *emit_y0)
            *emit_y0 = clip_y;
        if (clip_x + clip_width < *emit_x1)
            *emit_x1 = clip_x + clip_width;
        if (clip_y + clip_height < *emit_y1)
            *emit_y1 = clip_y + clip_height;
    }
    return *emit_x0 < *emit_x1 && *emit_y0 < *emit_y1;
}

/**
 * @brief Inverse-maps the source scratch through a 2D affine transform and blends it out.
 *
 * Shared by the end-of-capture blit and the banded replay blit. Streams one row segment at a
 * time through xrowbuf(); the emit region is bounded to the active scissor.
 */
static void xform_emit_2d(int source_x,
                          int source_y,
                          int source_width,
                          int source_height,
                          float inverse_a,
                          float inverse_b,
                          float inverse_c,
                          float inverse_d,
                          float inverse_translate_x,
                          float inverse_translate_y,
                          int destination_x,
                          int destination_y,
                          int destination_width,
                          int destination_height)
{
    int emit_x0, emit_y0, emit_x1, emit_y1;
    if (!xform_emit_bounds(destination_x,
                           destination_y,
                           destination_width,
                           destination_height,
                           &emit_x0,
                           &emit_y0,
                           &emit_x1,
                           &emit_y1))
        return;

    /* Scale-only fast path: with no rotation/shear (inverse_b == inverse_c == 0) the inverse map is separable —
     * sample_x depends only on the screen column, sample_y only on the screen row — so per column segment
     * we precompute sample_x, floorf(sample_x) and weight_x per column, and sample_y/floorf(sample_y)/weight_y plus row
     * pointers per row. The inner loop is then just four reads + the shared bilinear blend, so the output is
     * bit-identical to the general path. Rotation/shear falls through below. */
    if (inverse_b == 0.0f && inverse_c == 0.0f)
    {
        int column_x0[ERUI_TRANSFORM_WIDTH];
        uint16_t column_weight_x[ERUI_TRANSFORM_WIDTH];
        for (int segment_x = emit_x0; segment_x < emit_x1; segment_x += ERUI_TRANSFORM_WIDTH)
        {
            const int segment_width =
                (emit_x1 - segment_x) < ERUI_TRANSFORM_WIDTH ? (emit_x1 - segment_x) : ERUI_TRANSFORM_WIDTH;
            for (int output_column = 0; output_column < segment_width; output_column++)
            {
                const float sample_x =
                    inverse_a * (float)(segment_x + output_column) + inverse_translate_x - (float)source_x;
                /* Off the source there is no neighbour to read, and a column that far out (or NaN, which
                 * fails every comparison) would make the cast undefined. -2 reads as out, like x0 + 1. */
                if (!(sample_x >= -1.0f && sample_x < (float)source_width))
                {
                    column_x0[output_column] = -2;
                    column_weight_x[output_column] = 0;
                    continue;
                }
                const int x0 = (int)floorf(sample_x);
                column_x0[output_column] = x0;
                column_weight_x[output_column] = (uint16_t)((sample_x - (float)x0) * ER_BILERP_WEIGHT_SCALE);
            }
            for (int output_y = emit_y0; output_y < emit_y1; output_y++)
            {
                const float sample_y = inverse_d * (float)output_y + inverse_translate_y - (float)source_y;
                if (!(sample_y >= -1.0f && sample_y < (float)source_height))
                    continue; /* the row reads nothing, as for the columns above */
                const int y0 = (int)floorf(sample_y);
                const int y1 = y0 + 1;
                const uint32_t weight_y = (uint32_t)((sample_y - (float)y0) * ER_BILERP_WEIGHT_SCALE);
                const uint32_t* row0 =
                    (y0 >= 0 && y0 < source_height) ? xsrc() + (size_t)y0 * ERUI_TRANSFORM_WIDTH : NULL;
                const uint32_t* row1 =
                    (y1 >= 0 && y1 < source_height) ? xsrc() + (size_t)y1 * ERUI_TRANSFORM_WIDTH : NULL;
                uint32_t any_nonzero = 0u;
                for (int output_column = 0; output_column < segment_width; output_column++)
                {
                    const int x0 = column_x0[output_column];
                    const int x1 = x0 + 1;
                    const uint32_t weight_x = column_weight_x[output_column];
                    const bool x0_inside = (x0 >= 0 && x0 < source_width);
                    const bool x1_inside = (x1 >= 0 && x1 < source_width);
                    const uint32_t corner00 = (row0 && x0_inside) ? row0[x0] : 0u;
                    const uint32_t corner10 = (row0 && x1_inside) ? row0[x1] : 0u;
                    const uint32_t corner01 = (row1 && x0_inside) ? row1[x0] : 0u;
                    const uint32_t corner11 = (row1 && x1_inside) ? row1[x1] : 0u;
                    const uint32_t out = er_blend4(corner00, corner10, corner01, corner11, weight_x, weight_y);
                    xrowbuf()[output_column] = out;
                    any_nonzero |= out;
                }
                if (any_nonzero)
                    er_blit_blend(xrowbuf(),
                                  segment_width * (int)sizeof(uint32_t),
                                  ER_ALPHA_OPAQUE,
                                  segment_x,
                                  output_y,
                                  segment_width,
                                  1);
            }
        }
        return;
    }

    /* General path: for each screen-space pixel in the (scissor-bounded) destination box,
     * inverse-map to the source scratch and sample bilinearly. */
    for (int output_y = emit_y0; output_y < emit_y1; output_y++)
    {
        const float screen_y = (float)output_y;

        for (int segment_x = emit_x0; segment_x < emit_x1; segment_x += ERUI_TRANSFORM_WIDTH)
        {
            const int segment_width =
                (emit_x1 - segment_x) < ERUI_TRANSFORM_WIDTH ? (emit_x1 - segment_x) : ERUI_TRANSFORM_WIDTH;
            uint32_t any_nonzero = 0u;
            for (int output_column = 0; output_column < segment_width; output_column++)
            {
                const float screen_x = (float)(segment_x + output_column);

                /* Inverse-transform screen point → source-local sub-pixel coordinates. */
                const float sample_x =
                    (inverse_a * screen_x + inverse_c * screen_y + inverse_translate_x) - (float)source_x;
                const float sample_y =
                    (inverse_b * screen_x + inverse_d * screen_y + inverse_translate_y) - (float)source_y;

                /* Coarse bounds reject: skip if all four bilinear neighbours are outside. Written to pass
                 * only an in-range point: NaN fails every comparison and would reach er_bilerp's cast. */
                if (!(sample_x >= -1.0f && sample_x < (float)source_width && sample_y >= -1.0f
                      && sample_y < (float)source_height))
                {
                    xrowbuf()[output_column] = 0u;
                    continue;
                }

                const uint32_t out = er_bilerp(xsrc(), source_width, source_height, sample_x, sample_y);
                xrowbuf()[output_column] = out;
                any_nonzero |= out;
            }
            if (any_nonzero)
                er_blit_blend(xrowbuf(),
                              segment_width * (int)sizeof(uint32_t),
                              ER_ALPHA_OPAQUE,
                              segment_x,
                              output_y,
                              segment_width,
                              1);
        }
    }
}
#endif

/* Every screen coordinate a transform produces is clamped to this before an int cast. A huge or lopsided
 * scale, or a 3D corner near the camera plane, projects past the int range, where the cast is undefined;
 * ±32767 is off every screen and keeps a box's width (max - min) inside an int too. */
#define ER_TRANSFORM_COORDINATE_MAX 32767.0f

/**
 * @brief Clamps a projected coordinate to ±ER_TRANSFORM_COORDINATE_MAX, so the int cast after it is defined.
 *
 * @param[in] coordinate  Coordinate; the caller rejects NaN first, which no clamp can place.
 *
 * @return v clamped.
 */
static inline float xform_clamp(float coordinate)
{
    return coordinate < -ER_TRANSFORM_COORDINATE_MAX
               ? -ER_TRANSFORM_COORDINATE_MAX
               : (coordinate > ER_TRANSFORM_COORDINATE_MAX ? ER_TRANSFORM_COORDINATE_MAX : coordinate);
}

/**
 * @brief Turns one axis of a box's float bounds into an int start and length, clipped to ±ER_PAINT_RECT_MAX.
 *
 * Clamped to ±ER_TRANSFORM_COORDINATE_MAX alone, a box could be 65534 wide, which wraps in the int16 the compositor
 * records a node's last paint in, and the next commit would erase none of it.
 *
 * @param[in]  low       Least coordinate (not NaN).
 * @param[in]  high      Greatest coordinate (not NaN).
 * @param[in]  padding   Pixels added on each side.
 * @param[out] position  Clipped start.
 * @param[out] length    Clipped length, never negative.
 */
static inline void xform_box_axis(float low, float high, int padding, int* position, int* length)
{
    int start = (int)floorf(xform_clamp(low)) - padding;
    int end = (int)ceilf(xform_clamp(high)) + padding;
    start = start < -ER_PAINT_RECT_MAX ? -ER_PAINT_RECT_MAX : (start > ER_PAINT_RECT_MAX ? ER_PAINT_RECT_MAX : start);
    end = end < -ER_PAINT_RECT_MAX ? -ER_PAINT_RECT_MAX : (end > ER_PAINT_RECT_MAX ? ER_PAINT_RECT_MAX : end);
    *position = start;
    *length = end > start ? end - start : 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_transform_is_translate_only(const ERNode* node)
{
#if ERUI_3D_TRANSFORMS
    if (node->transform_rotate_x != 0.0f || node->transform_rotate_y != 0.0f || node->transform_perspective != 0.0f)
        return false;
#endif
    return node->transform_scale_x == 0.0f && node->transform_scale_y == 0.0f && node->transform_rotate_z == 0.0f;
}

#if ERUI_3D_TRANSFORMS
bool er_transform_is_3d(const ERNode* node)
{
    return node->transform_rotate_x != 0.0f || node->transform_rotate_y != 0.0f || node->transform_perspective != 0.0f;
}

void er_transform_compute_homography_3d(
    const ERNode* node, int ref_x, int ref_y, int width, int height, float homography[9])
{
    const float scale_x = (node->transform_scale_x == 0.0f) ? 1.0f : node->transform_scale_x;
    const float scale_y = (node->transform_scale_y == 0.0f) ? 1.0f : node->transform_scale_y;
    const float rotate_z = node->transform_rotate_z * ER_DEG2RAD;
    const float rotate_x = node->transform_rotate_x * ER_DEG2RAD;
    const float rotate_y = node->transform_rotate_y * ER_DEG2RAD;

    const float cos_x = cosf(rotate_x), sin_x = sinf(rotate_x);
    const float cos_y = cosf(rotate_y), sin_y = sinf(rotate_y);
    const float cos_z = cosf(rotate_z), sin_z = sinf(rotate_z);

    /* Pivot origin in layout space. */
    const float origin_fraction_x = (node->transform_origin_x < 0.0f) ? 0.5f : node->transform_origin_x;
    const float origin_fraction_y = (node->transform_origin_y < 0.0f) ? 0.5f : node->transform_origin_y;
    const float origin_x = (float)ref_x + origin_fraction_x * (float)width;
    const float origin_y = (float)ref_y + origin_fraction_y * (float)height;
    const float translate_x = node->transform_translate_x;
    const float translate_y = node->transform_translate_y;
    const float perspective = node->transform_perspective;

    /* Upper-left 3×3 of M = RotY * RotX * RotZ (columns scaled by scale_x, scale_y).
     * Only columns 0 and 1 matter since z = 0 for all source points. */
    /* Column 0 (u0 direction), scaled by scale_x: */
    const float rotation00 = scale_x * (cos_y * cos_z + sin_x * sin_y * sin_z);
    const float rotation10 = scale_x * (cos_x * sin_z);
    const float rotation20 = scale_x * (-sin_y * cos_z + sin_x * cos_y * sin_z);
    /* Column 1 (v0 direction), scaled by scale_y: */
    const float rotation01 = scale_y * (sin_y * sin_x * cos_z - cos_y * sin_z);
    const float rotation11 = scale_y * (cos_x * cos_z);
    const float rotation21 = scale_y * (cos_y * sin_x * cos_z + sin_y * sin_z);

    /* Constant term (translation contribution):
     *   X = rotation00*u0 + rotation01*v0 + origin_x + translate_x   where u0 = u - origin_x, v0 = v - origin_y
     *     = rotation00*u  + rotation01*v  + (-rotation00*origin_x - rotation01*origin_y + origin_x + translate_x) */
    const float constant_x = -rotation00 * origin_x - rotation01 * origin_y + origin_x + translate_x;
    const float constant_y = -rotation10 * origin_x - rotation11 * origin_y + origin_y + translate_y;
    /* W row: W = 1 + Z/perspective = 1 + (rotation20*(u-origin_x) + rotation21*(v-origin_y)) / perspective */
    const float constant_w =
        1.0f - rotation20 * origin_x / (perspective > ER_TRANSFORM_PERSPECTIVE_MIN ? perspective : 1.0f)
        - rotation21 * origin_y / (perspective > ER_TRANSFORM_PERSPECTIVE_MIN ? perspective : 1.0f);

    if (perspective > ER_TRANSFORM_PERSPECTIVE_MIN)
    {
        homography[0] = rotation00;
        homography[1] = rotation01;
        homography[2] = constant_x;
        homography[3] = rotation10;
        homography[4] = rotation11;
        homography[5] = constant_y;
        homography[6] = rotation20 / perspective;
        homography[7] = rotation21 / perspective;
        homography[8] = constant_w;
    }
    else
    {
        /* Orthographic (no perspective division). */
        homography[0] = rotation00;
        homography[1] = rotation01;
        homography[2] = constant_x;
        homography[3] = rotation10;
        homography[4] = rotation11;
        homography[5] = constant_y;
        homography[6] = 0.0f;
        homography[7] = 0.0f;
        homography[8] = 1.0f;
    }
}

bool er_transform_homography_invert(const float homography[9], float inverse[9])
{
    /* Cofactors. */
    const float cofactor00 = homography[4] * homography[8] - homography[5] * homography[7];
    const float cofactor01 = homography[5] * homography[6] - homography[3] * homography[8];
    const float cofactor02 = homography[3] * homography[7] - homography[4] * homography[6];
    const float cofactor10 = homography[2] * homography[7] - homography[1] * homography[8];
    const float cofactor11 = homography[0] * homography[8] - homography[2] * homography[6];
    const float cofactor12 = homography[1] * homography[6] - homography[0] * homography[7];
    const float cofactor20 = homography[1] * homography[5] - homography[2] * homography[4];
    const float cofactor21 = homography[2] * homography[3] - homography[0] * homography[5];
    const float cofactor22 = homography[0] * homography[4] - homography[1] * homography[3];

    const float determinant = homography[0] * cofactor00 + homography[1] * cofactor01 + homography[2] * cofactor02;
    /* Written to pass only a usable determinant: NaN fails every comparison, and one that overflowed to
     * infinity has no usable inverse either. */
    if (!(fabsf(determinant) >= ER_TRANSFORM_DETERMINANT_EPSILON_3D && fabsf(determinant) < INFINITY))
        return false;

    const float inverse_determinant = 1.0f / determinant;
    const float result[9] = {cofactor00 * inverse_determinant,
                             cofactor10 * inverse_determinant,
                             cofactor20 * inverse_determinant,
                             cofactor01 * inverse_determinant,
                             cofactor11 * inverse_determinant,
                             cofactor21 * inverse_determinant,
                             cofactor02 * inverse_determinant,
                             cofactor12 * inverse_determinant,
                             cofactor22 * inverse_determinant};
    /* A usable determinant does not make every cofactor usable: one huge entry times another overflows. An
     * inverse with a NaN or infinite coefficient has no usable map either. */
    for (int coefficient_index = 0; coefficient_index < 9; coefficient_index++)
        if (!(fabsf(result[coefficient_index]) < INFINITY))
            return false;
    for (int coefficient_index = 0; coefficient_index < 9; coefficient_index++)
        inverse[coefficient_index] = result[coefficient_index];
    return true;
}

void er_transform_aabb_3d(int ref_x,
                          int ref_y,
                          int width,
                          int height,
                          const float homography[9],
                          int* out_x,
                          int* out_y,
                          int* out_width,
                          int* out_height)
{
    const float corner_u[4] = {(float)ref_x, (float)(ref_x + width), (float)(ref_x + width), (float)ref_x};
    const float corner_v[4] = {(float)ref_y, (float)ref_y, (float)(ref_y + height), (float)(ref_y + height)};

    float min_x = ER_TRANSFORM_UNBOUNDED, max_x = -ER_TRANSFORM_UNBOUNDED, min_y = ER_TRANSFORM_UNBOUNDED,
          max_y = -ER_TRANSFORM_UNBOUNDED;
    bool found_any = false;

    for (int corner_index = 0; corner_index < 4; corner_index++)
    {
        const float projected_w =
            homography[6] * corner_u[corner_index] + homography[7] * corner_v[corner_index] + homography[8];
        if (!(projected_w > 0.0f)) /* behind the viewer, or NaN */
            continue;
        const float screen_x =
            (homography[0] * corner_u[corner_index] + homography[1] * corner_v[corner_index] + homography[2])
            / projected_w;
        const float screen_y =
            (homography[3] * corner_u[corner_index] + homography[4] * corner_v[corner_index] + homography[5])
            / projected_w;
        if (screen_x != screen_x || screen_y != screen_y)
            continue;
        if (!found_any || screen_x < min_x)
            min_x = screen_x;
        if (!found_any || screen_x > max_x)
            max_x = screen_x;
        if (!found_any || screen_y < min_y)
            min_y = screen_y;
        if (!found_any || screen_y > max_y)
            max_y = screen_y;
        found_any = true;
    }

    if (!found_any)
    {
        *out_x = *out_y = *out_width = *out_height = 0;
        return;
    }

    /* Expand by 1 pixel on all sides.  At near-zero rotation angles the projected
     * corners land very close to integer coordinates; ceil rounding of those values
     * can produce an AABB that is 1 pixel short of the source, causing the last
     * row/column to back-project just outside the source buffer and appear as a
     * transparent gap.  The 1-pixel border always has valid source pixels behind it
     * (the back-projected coordinate is still inside the source), stabilising the
     * apparent size across frames and eliminating the edge-jagging artefact. */
    xform_box_axis(min_x, max_x, 1, out_x, out_width);
    xform_box_axis(min_y, max_y, 1, out_y, out_height);
}

bool er_transform_map_point_3d(
    const float inverse_homography[9], int screen_x, int screen_y, int* layout_x, int* layout_y)
{
    const float point_x = (float)screen_x;
    const float point_y = (float)screen_y;
    const float projected_w = inverse_homography[6] * point_x + inverse_homography[7] * point_y + inverse_homography[8];
    if (!(projected_w > 0.0f)) /* behind the viewer, or NaN */
        return false;
    const float mapped_x =
        (inverse_homography[0] * point_x + inverse_homography[1] * point_y + inverse_homography[2]) / projected_w;
    const float mapped_y =
        (inverse_homography[3] * point_x + inverse_homography[4] * point_y + inverse_homography[5]) / projected_w;
    if (mapped_x != mapped_x || mapped_y != mapped_y)
        return false;
    *layout_x = (int)xform_clamp(mapped_x);
    *layout_y = (int)xform_clamp(mapped_y);
    return true;
}

#if ERUI_TRANSFORMS_FULL
/**
 * @brief Back-projects the source scratch through an inverse homography and blends it out.
 *
 * Shared by the end-of-capture blit and the banded replay blit; the emit region is bounded
 * to the active scissor (see xform_emit_bounds).
 */
static void xform_emit_3d(int source_x,
                          int source_y,
                          int source_width,
                          int source_height,
                          const float inverse_homography[9],
                          int destination_x,
                          int destination_y,
                          int destination_width,
                          int destination_height)
{
    int emit_x0, emit_y0, emit_x1, emit_y1;
    if (!xform_emit_bounds(destination_x,
                           destination_y,
                           destination_width,
                           destination_height,
                           &emit_x0,
                           &emit_y0,
                           &emit_x1,
                           &emit_y1))
        return;

    for (int output_y = emit_y0; output_y < emit_y1; output_y++)
    {
        const float screen_y = (float)output_y;

        for (int segment_x = emit_x0; segment_x < emit_x1; segment_x += ERUI_TRANSFORM_WIDTH)
        {
            const int segment_width =
                (emit_x1 - segment_x) < ERUI_TRANSFORM_WIDTH ? (emit_x1 - segment_x) : ERUI_TRANSFORM_WIDTH;
            uint32_t any_nonzero = 0u;
            for (int output_column = 0; output_column < segment_width; output_column++)
            {
                const float screen_x = (float)(segment_x + output_column);

                /* Back-project screen point through the inverse homography. */
                const float projected_w =
                    inverse_homography[6] * screen_x + inverse_homography[7] * screen_y + inverse_homography[8];
                if (!(projected_w > 0.0f)) /* behind the viewer, or NaN */
                {
                    xrowbuf()[output_column] = 0u;
                    continue;
                }
                const float sample_x =
                    (inverse_homography[0] * screen_x + inverse_homography[1] * screen_y + inverse_homography[2])
                        / projected_w
                    - (float)source_x;
                const float sample_y =
                    (inverse_homography[3] * screen_x + inverse_homography[4] * screen_y + inverse_homography[5])
                        / projected_w
                    - (float)source_y;

                if (!(sample_x >= -1.0f && sample_x < (float)source_width && sample_y >= -1.0f
                      && sample_y < (float)source_height))
                {
                    xrowbuf()[output_column] = 0u;
                    continue;
                }

                const uint32_t out = er_bilerp(xsrc(), source_width, source_height, sample_x, sample_y);
                xrowbuf()[output_column] = out;
                any_nonzero |= out;
            }
            if (any_nonzero)
                er_blit_blend(xrowbuf(),
                              segment_width * (int)sizeof(uint32_t),
                              ER_ALPHA_OPAQUE,
                              segment_x,
                              output_y,
                              segment_width,
                              1);
        }
    }
}
#endif /* ERUI_TRANSFORMS_FULL */

void er_transform_source_end_blit_3d(int source_x,
                                     int source_y,
                                     int source_width,
                                     int source_height,
                                     const float inverse_homography[9],
                                     int destination_x,
                                     int destination_y,
                                     int destination_width,
                                     int destination_height)
{
#if ERUI_TRANSFORMS_FULL
    if (!xc()->active)
        return;

    er_pop_clip_rect(); /* remove the capture's clip reset — output goes through the outer stack */
    er_scratch_pop_base();
    xc()->active = false;
    /* Re-apply the inherited draw alpha for the output blend (see xc()->saved_alpha). */
    er_set_draw_alpha(xc()->saved_alpha);

    if (destination_width <= 0 || destination_height <= 0)
        return;

    xform_emit_3d(source_x,
                  source_y,
                  source_width,
                  source_height,
                  inverse_homography,
                  destination_x,
                  destination_y,
                  destination_width,
                  destination_height);
#else
    (void)source_x;
    (void)source_y;
    (void)source_width;
    (void)source_height;
    (void)inverse_homography;
    (void)destination_x;
    (void)destination_y;
    (void)destination_width;
    (void)destination_height;
#endif
}

void er_transform_source_replay_blit_3d(int source_x,
                                        int source_y,
                                        int source_width,
                                        int source_height,
                                        const float inverse_homography[9],
                                        int destination_x,
                                        int destination_y,
                                        int destination_width,
                                        int destination_height)
{
#if ERUI_TRANSFORMS_FULL
    if (xc()->active || destination_width <= 0 || destination_height <= 0)
        return;
    xform_emit_3d(source_x,
                  source_y,
                  source_width,
                  source_height,
                  inverse_homography,
                  destination_x,
                  destination_y,
                  destination_width,
                  destination_height);
#else
    (void)source_x;
    (void)source_y;
    (void)source_width;
    (void)source_height;
    (void)inverse_homography;
    (void)destination_x;
    (void)destination_y;
    (void)destination_width;
    (void)destination_height;
#endif
}
#endif /* ERUI_3D_TRANSFORMS */

void er_transform_compute_matrix(const ERNode* node,
                                 int ref_x,
                                 int ref_y,
                                 int width,
                                 int height,
                                 float* matrix_a,
                                 float* matrix_b,
                                 float* matrix_c,
                                 float* matrix_d,
                                 float* matrix_translate_x,
                                 float* matrix_translate_y)
{
    const float scale_x = (node->transform_scale_x == 0.0f) ? 1.0f : node->transform_scale_x;
    const float scale_y = (node->transform_scale_y == 0.0f) ? 1.0f : node->transform_scale_y;
    const float theta = node->transform_rotate_z * ER_DEG2RAD;
    const float cos_theta = cosf(theta);
    const float sin_theta = sinf(theta);

    /* 2×2 rotation/scale matrix:
     *   x' = scale_x*cos(θ)*x − scale_y*sin(θ)*y
     *   y' = scale_x*sin(θ)*x + scale_y*cos(θ)*y  */
    *matrix_a = scale_x * cos_theta;
    *matrix_b = scale_x * sin_theta;
    *matrix_c = -scale_y * sin_theta;
    *matrix_d = scale_y * cos_theta;

    /* Transform origin in layout/render space. */
    const float origin_fraction_x = (node->transform_origin_x < 0.0f) ? 0.5f : node->transform_origin_x;
    const float origin_fraction_y = (node->transform_origin_y < 0.0f) ? 0.5f : node->transform_origin_y;
    const float origin_x = (float)ref_x + origin_fraction_x * (float)width;
    const float origin_y = (float)ref_y + origin_fraction_y * (float)height;

    /* Total translation: pivot back + explicit translate − M * pivot. */
    *matrix_translate_x = origin_x + node->transform_translate_x - (*matrix_a * origin_x + *matrix_c * origin_y);
    *matrix_translate_y = origin_y + node->transform_translate_y - (*matrix_b * origin_x + *matrix_d * origin_y);
}

bool er_transform_invert(float matrix_a,
                         float matrix_b,
                         float matrix_c,
                         float matrix_d,
                         float matrix_translate_x,
                         float matrix_translate_y,
                         float* inverse_a,
                         float* inverse_b,
                         float* inverse_c,
                         float* inverse_d,
                         float* inverse_translate_x,
                         float* inverse_translate_y)
{
    const float determinant = matrix_a * matrix_d - matrix_b * matrix_c;
    /* As in er_transform_homography_invert: a NaN or overflowed determinant is singular too. */
    if (!(fabsf(determinant) >= ER_TRANSFORM_DETERMINANT_EPSILON_2D && fabsf(determinant) < INFINITY))
        return false;

    const float inverse_determinant = 1.0f / determinant;
    /* Inverse translation: inverse_translate_x = (matrix_c*matrix_translate_y − matrix_d*matrix_translate_x) /
     * determinant, inverse_translate_y = (matrix_b*matrix_translate_x − matrix_a*matrix_translate_y) / determinant */
    const float result[6] = {matrix_d * inverse_determinant,
                             -matrix_b * inverse_determinant,
                             -matrix_c * inverse_determinant,
                             matrix_a * inverse_determinant,
                             (matrix_c * matrix_translate_y - matrix_d * matrix_translate_x) * inverse_determinant,
                             (matrix_b * matrix_translate_x - matrix_a * matrix_translate_y) * inverse_determinant};
    /* A usable determinant says nothing of the translation, which a huge scale about the pivot overflows while
     * a lopsided one keeps matrix_a*matrix_d finite. An inverse with a NaN or infinite coefficient has no usable map
     * either. */
    for (int coefficient_index = 0; coefficient_index < 6; coefficient_index++)
        if (!(fabsf(result[coefficient_index]) < INFINITY))
            return false;
    *inverse_a = result[0];
    *inverse_b = result[1];
    *inverse_c = result[2];
    *inverse_d = result[3];
    *inverse_translate_x = result[4];
    *inverse_translate_y = result[5];
    return true;
}

void er_transform_aabb(int ref_x,
                       int ref_y,
                       int width,
                       int height,
                       float matrix_a,
                       float matrix_b,
                       float matrix_c,
                       float matrix_d,
                       float matrix_translate_x,
                       float matrix_translate_y,
                       int* out_x,
                       int* out_y,
                       int* out_width,
                       int* out_height)
{
    /* Screen-space positions of the four corners. */
    const float corner_x[4] = {(float)ref_x, (float)(ref_x + width), (float)(ref_x + width), (float)ref_x};
    const float corner_y[4] = {(float)ref_y, (float)ref_y, (float)(ref_y + height), (float)(ref_y + height)};

    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    bool found_any = false;

    for (int corner_index = 0; corner_index < 4; corner_index++)
    {
        const float screen_x =
            matrix_a * corner_x[corner_index] + matrix_c * corner_y[corner_index] + matrix_translate_x;
        const float screen_y =
            matrix_b * corner_x[corner_index] + matrix_d * corner_y[corner_index] + matrix_translate_y;
        if (screen_x != screen_x || screen_y != screen_y) /* an infinite component times zero: no position to bound */
            continue;
        if (!found_any || screen_x < min_x)
            min_x = screen_x;
        if (!found_any || screen_x > max_x)
            max_x = screen_x;
        if (!found_any || screen_y < min_y)
            min_y = screen_y;
        if (!found_any || screen_y > max_y)
            max_y = screen_y;
        found_any = true;
    }

    if (!found_any)
    {
        *out_x = *out_y = *out_width = *out_height = 0;
        return;
    }

    xform_box_axis(min_x, max_x, 0, out_x, out_width);
    xform_box_axis(min_y, max_y, 0, out_y, out_height);
}

bool er_transform_map_point(float inverse_a,
                            float inverse_b,
                            float inverse_c,
                            float inverse_d,
                            float inverse_translate_x,
                            float inverse_translate_y,
                            int screen_x,
                            int screen_y,
                            int* layout_x,
                            int* layout_y)
{
    const float mapped_x = inverse_a * (float)screen_x + inverse_c * (float)screen_y + inverse_translate_x;
    const float mapped_y = inverse_b * (float)screen_x + inverse_d * (float)screen_y + inverse_translate_y;
    if (mapped_x != mapped_x || mapped_y != mapped_y)
        return false;
    *layout_x = (int)xform_clamp(mapped_x);
    *layout_y = (int)xform_clamp(mapped_y);
    return true;
}

/**
 * @brief Whether a node's transform can be inverted — the other half of the capture admission test.
 *
 * er_transform_source_fits() answers the size half; this answers the rest of what render_tree()
 * requires before it starts a capture, because the inverse-map blit is what puts the captured scratch
 * back on screen and a singular matrix has no inverse. Such a node degrades to painting untransformed
 * at its layout box, and — the reason this is exported — the capture then passes to whichever
 * transform is nested inside it. The damage pre-pass picks the ancestor to escalate a descendant's
 * damage to (see capturing_transform_ancestor), so it has to make that same call or it escalates to a
 * node that never captured while the pixels land somewhere else entirely.
 *
 * Answered with the identical thresholds er_transform_invert() and er_transform_homography_invert()
 * apply, on a matrix built from the same inputs — a prediction, not a guess.
 *
 * @param[in] node           Node to test.
 * @param[in] ref_x,ref_y    Layout-space origin of the node (render px/py); the 3D homography's
 *                           pivot depends on it, so pass what render_tree would.
 * @param[in] width,height   Node size in pixels.
 *
 * @return true when the transform inverts (always false without ERUI_TRANSFORMS=FULL, which has no
 *         capture path at all).
 */
bool er_transform_is_invertible(const ERNode* node, int ref_x, int ref_y, int width, int height)
{
#if ERUI_TRANSFORMS_FULL
#if ERUI_3D_TRANSFORMS
    if (er_transform_is_3d(node))
    {
        float homography[9], inverse[9];
        er_transform_compute_homography_3d(node, ref_x, ref_y, width, height, homography);
        return er_transform_homography_invert(homography, inverse);
    }
#endif
    float matrix_a, matrix_b, matrix_c, matrix_d, matrix_translate_x, matrix_translate_y, inverse_a, inverse_b,
        inverse_c, inverse_d, inverse_translate_x, inverse_translate_y;
    er_transform_compute_matrix(node,
                                ref_x,
                                ref_y,
                                width,
                                height,
                                &matrix_a,
                                &matrix_b,
                                &matrix_c,
                                &matrix_d,
                                &matrix_translate_x,
                                &matrix_translate_y);
    return er_transform_invert(matrix_a,
                               matrix_b,
                               matrix_c,
                               matrix_d,
                               matrix_translate_x,
                               matrix_translate_y,
                               &inverse_a,
                               &inverse_b,
                               &inverse_c,
                               &inverse_d,
                               &inverse_translate_x,
                               &inverse_translate_y);
#else
    (void)node;
    (void)ref_x;
    (void)ref_y;
    (void)width;
    (void)height;
    return false;
#endif
}

bool er_transform_source_fits(int width, int height)
{
#if ERUI_TRANSFORMS_FULL
    return width > 0 && height > 0 && width <= ERUI_TRANSFORM_WIDTH && height <= ERUI_TRANSFORM_HEIGHT;
#else
    (void)width;
    (void)height;
    return false;
#endif
}

bool er_transform_source_begin(int source_x, int source_y, int width, int height)
{
#if ERUI_TRANSFORMS_FULL
    if (xc()->active)
        return false;
    if (!er_transform_source_fits(width, height))
        return false;

    /* Clear only the used width × height sub-rect (node-local origin 0,0), not the whole scratch.
     * The subtree renders into rows [0,height) cols [0,width) and the inverse-map sampler only ever reads
     * that region (out-of-range neighbours are treated as transparent), so clearing it is sufficient and
     * the output is identical.
     */
    for (int row = 0; row < height; row++)
        memset(xsrc() + (size_t)row * ERUI_TRANSFORM_WIDTH, 0, (size_t)width * sizeof(uint32_t));
    er_scratch_push_base(xsrc(), ERUI_TRANSFORM_WIDTH, ERUI_TRANSFORM_HEIGHT, source_x, source_y);
    xc()->active = true;
    xc()->saved_alpha = er_get_draw_alpha();
    er_set_draw_alpha(ER_ALPHA_OPAQUE);
    /* The capture must see the whole subtree: outer scissors (including banded-composite tile
     * clips) apply to the transformed OUTPUT at emit time, not to the source. Without this a
     * band clip would truncate the source at the strip seam. */
    er_push_clip_reset();
    /* A fresh capture owns the source buffer — invalidate any previous replay cache until the
     * compositor re-notes it. */
    xc()->cache_tag = ER_TRANSFORM_NO_TAG;
    return true;
#else
    (void)source_x;
    (void)source_y;
    (void)width;
    (void)height;
    return false;
#endif
}

void er_transform_source_note(uint16_t tag, uint32_t generation)
{
#if ERUI_TRANSFORMS_FULL
    xc()->cache_tag = tag;
    xc()->cache_generation = generation;
#else
    (void)tag;
    (void)generation;
#endif
}

bool er_transform_source_is_cached(uint16_t tag, uint32_t generation)
{
#if ERUI_TRANSFORMS_FULL
    return !xc()->active && tag != ER_TRANSFORM_NO_TAG && xc()->cache_tag == tag
           && xc()->cache_generation == generation;
#else
    (void)tag;
    (void)generation;
    return false;
#endif
}

void er_transform_source_replay_blit(int source_x,
                                     int source_y,
                                     int source_width,
                                     int source_height,
                                     float inverse_a,
                                     float inverse_b,
                                     float inverse_c,
                                     float inverse_d,
                                     float inverse_translate_x,
                                     float inverse_translate_y,
                                     int destination_x,
                                     int destination_y,
                                     int destination_width,
                                     int destination_height)
{
#if ERUI_TRANSFORMS_FULL
    if (xc()->active || destination_width <= 0 || destination_height <= 0)
        return;
    xform_emit_2d(source_x,
                  source_y,
                  source_width,
                  source_height,
                  inverse_a,
                  inverse_b,
                  inverse_c,
                  inverse_d,
                  inverse_translate_x,
                  inverse_translate_y,
                  destination_x,
                  destination_y,
                  destination_width,
                  destination_height);
#else
    (void)source_x;
    (void)source_y;
    (void)source_width;
    (void)source_height;
    (void)inverse_a;
    (void)inverse_b;
    (void)inverse_c;
    (void)inverse_d;
    (void)inverse_translate_x;
    (void)inverse_translate_y;
    (void)destination_x;
    (void)destination_y;
    (void)destination_width;
    (void)destination_height;
#endif
}

void er_transform_source_end_blit(int source_x,
                                  int source_y,
                                  int source_width,
                                  int source_height,
                                  float inverse_a,
                                  float inverse_b,
                                  float inverse_c,
                                  float inverse_d,
                                  float inverse_translate_x,
                                  float inverse_translate_y,
                                  int destination_x,
                                  int destination_y,
                                  int destination_width,
                                  int destination_height)
{
#if ERUI_TRANSFORMS_FULL
    if (!xc()->active)
        return;

    er_pop_clip_rect(); /* remove the capture's clip reset — output goes through the outer stack */
    er_scratch_pop_base();
    xc()->active = false;
    /* Re-apply the inherited draw alpha for the output blend (see xc()->saved_alpha). */
    er_set_draw_alpha(xc()->saved_alpha);

    if (destination_width <= 0 || destination_height <= 0)
        return;

    xform_emit_2d(source_x,
                  source_y,
                  source_width,
                  source_height,
                  inverse_a,
                  inverse_b,
                  inverse_c,
                  inverse_d,
                  inverse_translate_x,
                  inverse_translate_y,
                  destination_x,
                  destination_y,
                  destination_width,
                  destination_height);
#else
    (void)source_x;
    (void)source_y;
    (void)source_width;
    (void)source_height;
    (void)inverse_a;
    (void)inverse_b;
    (void)inverse_c;
    (void)inverse_d;
    (void)inverse_translate_x;
    (void)inverse_translate_y;
    (void)destination_x;
    (void)destination_y;
    (void)destination_width;
    (void)destination_height;
#endif
}
