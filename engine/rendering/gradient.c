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

#include "gradient.h"
#include "er_limits.h"
#include "renderer_internal.h"
#include "rrect.h" /* rounded-corner geometry, shared so the mask matches the fill pixel for pixel */
#include <math.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A span or radius at or below this has no direction to spread the stops along. */
#define GRADIENT_EPSILON 1e-6f

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Single-row premultiplied ARGB8888 scratch buffer used during gradient rasterization. */
/* One assembled row per render worker: cheap enough to duplicate, and it keeps gradient
 * backgrounds parallel-safe (see the multi-core render fork in compositor.c). */
static uint32_t s_gradient_row_pool[ERUI_RENDER_WORKERS][ERUI_MAX_IMAGE_ROW_PIXELS];

/** @brief The calling worker's gradient row buffer. */
static inline uint32_t* grow(void)
{
    return s_gradient_row_pool[er_render_worker_id()];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Linearly interpolates between two straight-alpha ARGB8888 colors.
 *
 * @param[in] from      Color at fraction 0.0.
 * @param[in] to        Color at fraction 1.0.
 * @param[in] fraction  Blend factor [0.0–1.0].
 *
 * @return Interpolated straight-alpha ARGB8888 color.
 */
static uint32_t lerp_argb(uint32_t from, uint32_t to, float fraction)
{
    /* Infinite stop positions, which a package can give, make fraction a NaN or send it past the ends. */
    if (!(fraction > 0.0f))
        fraction = 0.0f;
    else if (fraction > 1.0f)
        fraction = 1.0f;
    const int from_alpha = (int)((from >> 24) & 0xFFu), to_alpha = (int)((to >> 24) & 0xFFu);
    const int from_red = (int)((from >> 16) & 0xFFu), to_red = (int)((to >> 16) & 0xFFu);
    const int from_green = (int)((from >> 8) & 0xFFu), to_green = (int)((to >> 8) & 0xFFu);
    const int from_blue = (int)(from & 0xFFu), to_blue = (int)(to & 0xFFu);
    const uint32_t alpha = (uint32_t)(from_alpha + (int)((float)(to_alpha - from_alpha) * fraction));
    const uint32_t red = (uint32_t)(from_red + (int)((float)(to_red - from_red) * fraction));
    const uint32_t green = (uint32_t)(from_green + (int)((float)(to_green - from_green) * fraction));
    const uint32_t blue = (uint32_t)(from_blue + (int)((float)(to_blue - from_blue) * fraction));
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

/**
 * @brief Evaluates the gradient color at a scalar position.
 *
 * Finds the pair of stops that brackets position and lerps between them. Returns the first stop
 * color for a position below stops[0].position and the last stop color for one above the final position.
 *
 * @param[in] stops     Color stop array sorted by ascending position.
 * @param[in] count     Number of entries in stops.
 * @param[in] position  Gradient parameter [0.0–1.0].
 *
 * @return Straight-alpha ARGB8888 color at position.
 */
uint32_t er_gradient_eval_stops(const ERGradientStop* stops, int count, float position)
{
    if (count <= 0)
        return 0u;
    if (position <= stops[0].position || count == 1)
        return stops[0].color;
    if (position >= stops[count - 1].position)
        return stops[count - 1].color;
    for (int stop_index = 0; stop_index < count - 1; stop_index++)
    {
        if (position <= stops[stop_index + 1].position)
        {
            const float span = stops[stop_index + 1].position - stops[stop_index].position;
            const float stop_fraction = (span > 0.0f) ? (position - stops[stop_index].position) / span : 0.0f;
            return lerp_argb(stops[stop_index].color, stops[stop_index + 1].color, stop_fraction);
        }
    }
    return stops[count - 1].color;
}

/**
 * @brief Converts a straight-alpha ARGB8888 color to premultiplied ARGB8888.
 *
 * Required before writing pixels into buffers consumed by er_blit_blend().
 *
 * @param[in] straight_argb  Straight-alpha ARGB8888 color.
 *
 * @return Premultiplied ARGB8888 equivalent.
 */
uint32_t er_gradient_premul(uint32_t straight_argb)
{
    const uint32_t alpha = (straight_argb >> 24) & 0xFFu;
    const uint32_t red = (((straight_argb >> 16) & 0xFFu) * alpha) / ER_CHANNEL_MAX;
    const uint32_t green = (((straight_argb >> 8) & 0xFFu) * alpha) / ER_CHANNEL_MAX;
    const uint32_t blue = ((straight_argb & 0xFFu) * alpha) / ER_CHANNEL_MAX;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

#if ERUI_GRADIENT

/*----------------------------------------------------------------------------------------------------------------------
 - Rounded-corner masking
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief The rounded-rect silhouette a gradient's rows are masked to.
 *
 * A gradient is a background: it must stop at the same edge the node's background_color would have,
 * or its square corners poke out past the rounded ones (invisible at radius 4, obvious by radius 12).
 * The radii are resolved and clamped exactly as render_view_bg does, so the mask and the border ring
 * drawn on top of it describe one shape.
 */
typedef struct
{
    int width;               /**< Box width — the coordinate space of the row spans. */
    int height;              /**< Box height. */
    int radius_top_left;     /**< Clamped corner radii. */
    int radius_top_right;    /**< @see radius_top_left */
    int radius_bottom_right; /**< @see radius_top_left */
    int radius_bottom_left;  /**< @see radius_top_left */
    bool rounded;            /**< False when every radius is 0: rows blit whole, at no extra cost. */
} GradMask;

/**
 * @brief Resolves a node's corner radii into the mask its gradient rows are clipped to.
 *
 * @param[out] mask        Receives the silhouette.
 * @param[in]  view_props  View props carrying border_radius and the per-corner overrides.
 * @param[in]  width       Box width in pixels.
 * @param[in]  height      Box height in pixels.
 */
static void mask_init(GradMask* mask, const ERViewProps* view_props, int width, int height)
{
    /* Per-corner radius, falling back to the uniform one — the same resolution render_view_bg uses. */
    mask->radius_top_left = view_props->border_top_left_radius > 0 ? (int)view_props->border_top_left_radius
                                                                   : (int)view_props->border_radius;
    mask->radius_top_right = view_props->border_top_right_radius > 0 ? (int)view_props->border_top_right_radius
                                                                     : (int)view_props->border_radius;
    mask->radius_bottom_right = view_props->border_bottom_right_radius > 0 ? (int)view_props->border_bottom_right_radius
                                                                           : (int)view_props->border_radius;
    mask->radius_bottom_left = view_props->border_bottom_left_radius > 0 ? (int)view_props->border_bottom_left_radius
                                                                         : (int)view_props->border_radius;
    er_rrect_clamp_radii(width,
                         height,
                         &mask->radius_top_left,
                         &mask->radius_top_right,
                         &mask->radius_bottom_right,
                         &mask->radius_bottom_left);
    mask->width = width;
    mask->height = height;
    mask->rounded = (mask->radius_top_left > 0 || mask->radius_top_right > 0 || mask->radius_bottom_right > 0
                     || mask->radius_bottom_left > 0);
}

/**
 * @brief Blits one assembled gradient row, clipped to the rounded-rect silhouette.
 *
 * Rows clear of the corner arcs blit whole; rows the arcs cut into blit only their covered span, then
 * fade the anti-aliased fringe pixels beside it (matching er_rrect_fill_corners' fringe walk, so the
 * gradient's corner is the same shape a solid background would have painted).
 *
 * @param[in] mask          Silhouette to clip to.
 * @param[in] row           Assembled premultiplied ARGB8888 row: the box's columns [first_column, end_column).
 * @param[in] x             The box's left edge in framebuffer pixels.
 * @param[in] y             Destination row in framebuffer pixels.
 * @param[in] row_index     Row index within the box, 0 = top.
 * @param[in] first_column  First assembled column of the box.
 * @param[in] end_column    One past the last assembled column.
 */
static void
mask_blit_row(const GradMask* mask, const uint32_t* row, int x, int y, int row_index, int first_column, int end_column)
{
    if (!mask->rounded)
    {
        er_blit_blend(row,
                      (end_column - first_column) * (int)sizeof(uint32_t),
                      ER_ALPHA_OPAQUE,
                      x + first_column,
                      y,
                      end_column - first_column,
                      1);
        return;
    }

    ERRRectRow span;
    er_rrect_row(mask->width,
                 mask->height,
                 mask->radius_top_left,
                 mask->radius_top_right,
                 mask->radius_bottom_right,
                 mask->radius_bottom_left,
                 row_index,
                 &span);

    const int x0 = (span.x0 > first_column) ? span.x0 : first_column;
    const int x1 = (span.x1 < end_column) ? span.x1 : end_column;
    if (x1 > x0)
        er_blit_blend(
            &row[x0 - first_column], (x1 - x0) * (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x + x0, y, x1 - x0, 1);

#if ERUI_BORDER_AA
    /* Fringe pixels take the gradient colour at their own column, faded by the arc's coverage. */
    for (int step = 0, step_count = er_rrect_fringe_max(span.left_radius); step < step_count; step++)
    {
        const float coverage =
            er_rrect_fringe_cov(span.left_radius, span.left_half_width, span.left_row_distance, step);
        if (coverage <= 0.0f)
            break;
        const int fringe_x = span.x0 - 1 - step;
        if (coverage < 1.0f && fringe_x >= first_column && fringe_x < end_column)
        {
            const uint32_t pixel =
                er_px_scale_premul(row[fringe_x - first_column], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
            er_blit_blend(&pixel, (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x + fringe_x, y, 1, 1);
        }
    }
    for (int step = 0, step_count = er_rrect_fringe_max(span.right_radius); step < step_count; step++)
    {
        const float coverage =
            er_rrect_fringe_cov(span.right_radius, span.right_half_width, span.right_row_distance, step);
        if (coverage <= 0.0f)
            break;
        const int fringe_x = span.x1 + step;
        if (coverage < 1.0f && fringe_x >= first_column && fringe_x < end_column)
        {
            const uint32_t pixel =
                er_px_scale_premul(row[fringe_x - first_column], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
            er_blit_blend(&pixel, (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x + fringe_x, y, 1, 1);
        }
    }
#endif
}

/*----------------------------------------------------------------------------------------------------------------------
 - The colour ramp
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A ramp position of 1.0. Positions are fixed point so a pixel costs no float math. */
#define RAMP_ONE 65536

/** @brief Fraction bits a channel keeps between two stops, for the dither to round. */
#define RAMP_FRACTION_BITS 12

/** @brief Fraction bits of a linear gradient's per-column position step. */
#define RAMP_STEP_BITS 30

/** @brief A view's stops, premultiplied and in fixed point. */
typedef struct
{
    uint32_t color[ER_GRADIENT_MAX_STOPS];       /**< Premultiplied ARGB8888. */
    int32_t position[ER_GRADIENT_MAX_STOPS];     /**< Ascending, 0 to RAMP_ONE. */
    uint32_t inverse[ER_GRADIENT_MAX_STOPS - 1]; /**< Scales an offset into the span after a stop to a fraction. */
    int count;
} GradRamp;

/** @brief Ordered-dither thresholds, 0 to 63. */
static const uint8_t k_dither[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

/**
 * @brief The dither threshold of a framebuffer pixel, below one channel step.
 *
 * Keyed to the framebuffer rather than the box, so a partial repaint lands on the same pattern.
 */
static inline uint32_t dither_at(int x, int y)
{
    return (uint32_t)k_dither[y & 7][x & 7] << (RAMP_FRACTION_BITS - 6);
}

/**
 * @brief Builds a view's ramp.
 *
 * Stops are premultiplied before they are interpolated, so a fade to transparent keeps its hue. A position
 * below an earlier one is raised to it.
 *
 * @return false when the view has fewer than two stops.
 */
static bool ramp_init(GradRamp* ramp, const ERViewProps* view_props)
{
    int count = (int)view_props->gradient_stop_count;
    if (count > ER_GRADIENT_MAX_STOPS)
        count = ER_GRADIENT_MAX_STOPS;
    if (count < 2)
        return false;

    int32_t previous = 0;
    for (int stop_index = 0; stop_index < count; stop_index++)
    {
        /* A package can give a NaN or infinite position. */
        const float position = view_props->gradient_stops[stop_index].position;
        int32_t fixed = !(position > 0.0f) ? 0 : (position >= 1.0f ? RAMP_ONE : (int32_t)(position * RAMP_ONE + 0.5f));
        if (fixed < previous)
            fixed = previous;
        previous = fixed;
        ramp->position[stop_index] = fixed;
        ramp->color[stop_index] = er_gradient_premul(view_props->gradient_stops[stop_index].color);
    }
    for (int stop_index = 0; stop_index < count - 1; stop_index++)
    {
        const uint32_t span = (uint32_t)(ramp->position[stop_index + 1] - ramp->position[stop_index]);
        ramp->inverse[stop_index] = span ? ((1u << RAMP_FRACTION_BITS) << 16) / span : 0u;
    }
    ramp->count = count;
    return true;
}

/**
 * @brief The ramp's premultiplied colour at a position, dithered.
 *
 * @param[in] ramp       The ramp.
 * @param[in] position   0 to RAMP_ONE; anything outside takes the nearest end's colour.
 * @param[in] threshold  The pixel's dither_at().
 */
static inline uint32_t ramp_color(const GradRamp* ramp, int32_t position, uint32_t threshold)
{
    const int last = ramp->count - 1;
    if (position <= ramp->position[0])
        return ramp->color[0];
    if (position >= ramp->position[last])
        return ramp->color[last];

    int stop_index = 0;
    while (position >= ramp->position[stop_index + 1])
        stop_index++;

    const int32_t fraction =
        (int32_t)(((uint32_t)(position - ramp->position[stop_index]) * ramp->inverse[stop_index]) >> 16);
    const uint32_t from = ramp->color[stop_index];
    const uint32_t to = ramp->color[stop_index + 1];
    uint32_t pixel = 0u;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const int32_t from_channel = (int32_t)((from >> shift) & 0xFFu);
        const int32_t to_channel = (int32_t)((to >> shift) & 0xFFu);
        const int32_t channel = (from_channel << RAMP_FRACTION_BITS) + (to_channel - from_channel) * fraction;
        pixel |= (((uint32_t)channel + threshold) >> RAMP_FRACTION_BITS) << shift;
    }
    return pixel;
}

/**
 * @brief The part of a box the active clip leaves, in the box's own columns and rows.
 *
 * Only what is inside it gets assembled: a small damage rect over a large gradient costs its own pixels.
 *
 * @return false when the clip leaves nothing.
 */
static bool
clip_box(int x, int y, int width, int height, int* first_column, int* end_column, int* first_row, int* end_row)
{
    *first_column = 0;
    *end_column = width;
    *first_row = 0;
    *end_row = height;

    int clip_x, clip_y, clip_width, clip_height;
    if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height))
    {
        if (clip_x - x > *first_column)
            *first_column = clip_x - x;
        if (clip_x + clip_width - x < *end_column)
            *end_column = clip_x + clip_width - x;
        if (clip_y - y > *first_row)
            *first_row = clip_y - y;
        if (clip_y + clip_height - y < *end_row)
            *end_row = clip_y + clip_height - y;
    }
    if (*end_column - *first_column > ERUI_MAX_IMAGE_ROW_PIXELS)
        *end_column = *first_column + ERUI_MAX_IMAGE_ROW_PIXELS;
    return *end_column > *first_column && *end_row > *first_row;
}

/**
 * @brief Renders a linear gradient into a rectangular framebuffer region.
 *
 * The gradient angle is measured in degrees clockwise from the top: 0° runs top→bottom,
 * 90° runs left→right.  With a gradient_corner the direction comes from the box instead, so the two other
 * corners share the middle colour whatever the aspect ratio.  The four corner projections onto the direction
 * vector normalise the per-pixel position to [0.0–1.0].
 *
 * Each row is computed into grow() and flushed via er_blit_blend (premultiplied).
 *
 * @param[in] view_props  View props (gradient_angle, gradient_corner, gradient_stop_count, gradient_stops).
 * @param[in] x           Destination left edge in framebuffer pixels.
 * @param[in] y           Destination top edge in framebuffer pixels.
 * @param[in] width       Destination width in pixels.
 * @param[in] height      Destination height in pixels.
 */
static void render_linear(const ERViewProps* view_props, int x, int y, int width, int height)
{
    GradRamp ramp;
    int first_column, end_column, first_row, end_row;
    if (width <= 0 || height <= 0 || !ramp_init(&ramp, view_props)
        || !clip_box(x, y, width, height, &first_column, &end_column, &first_row, &end_row))
        return;

    GradMask mask;
    mask_init(&mask, view_props, width, height);

    const float last_x = (float)(width - 1);
    const float last_y = (float)(height - 1);
    float direction_x, direction_y;
    if (view_props->gradient_corner != ER_GRADIENT_CORNER_NONE)
    {
        const float sign_x = (view_props->gradient_corner == ER_GRADIENT_CORNER_TOP_RIGHT
                              || view_props->gradient_corner == ER_GRADIENT_CORNER_BOTTOM_RIGHT)
                                 ? 1.0f
                                 : -1.0f;
        const float sign_y = (view_props->gradient_corner == ER_GRADIENT_CORNER_BOTTOM_RIGHT
                              || view_props->gradient_corner == ER_GRADIENT_CORNER_BOTTOM_LEFT)
                                 ? 1.0f
                                 : -1.0f;
        /* Perpendicular to the other diagonal. A box one pixel across has no diagonal to follow. */
        const bool flat = (last_x <= 0.0f || last_y <= 0.0f);
        direction_x = sign_x * (flat ? 1.0f : last_y);
        direction_y = sign_y * (flat ? 1.0f : last_x);
    }
    else
    {
        const float angle_rad = view_props->gradient_angle * ER_DEG2RAD;
        direction_x = sinf(angle_rad);
        direction_y = cosf(angle_rad);
    }

    /* Project all four corners onto the direction vector to find the full extent. */
    const float projection00 = 0.0f;
    const float projection10 = last_x * direction_x;
    const float projection01 = last_y * direction_y;
    const float projection11 = last_x * direction_x + last_y * direction_y;
    float min_projection = projection00, max_projection = projection00;
    if (projection10 < min_projection)
        min_projection = projection10;
    if (projection10 > max_projection)
        max_projection = projection10;
    if (projection01 < min_projection)
        min_projection = projection01;
    if (projection01 > max_projection)
        max_projection = projection01;
    if (projection11 < min_projection)
        min_projection = projection11;
    if (projection11 > max_projection)
        max_projection = projection11;

    /* A NaN angle leaves the span a NaN, and the gradient its first colour. */
    const float span = max_projection - min_projection;
    const float scale = (span > GRADIENT_EPSILON) ? (float)(1 << RAMP_STEP_BITS) / span : 0.0f;
    const int32_t step = (int32_t)lroundf(direction_x * scale);
    const int32_t half = 1 << (RAMP_STEP_BITS - 16 - 1);
    uint32_t* const pixels = grow();

    for (int row = first_row; row < end_row; row++)
    {
        int32_t position = (int32_t)lroundf(((float)row * direction_y - min_projection) * scale) + first_column * step;
        const uint8_t* dither = k_dither[(y + row) & 7];
        for (int column = first_column; column < end_column; column++, position += step)
        {
            pixels[column - first_column] = ramp_color(&ramp,
                                                       (position + half) >> (RAMP_STEP_BITS - 16),
                                                       (uint32_t)dither[(x + column) & 7] << (RAMP_FRACTION_BITS - 6));
        }
        mask_blit_row(&mask, pixels, x, y + row, row, first_column, end_column);
    }
}

#if ERUI_GRADIENT_RADIAL

/**
 * @brief Renders a radial gradient into a rectangular framebuffer region.
 *
 * The gradient origin is the rectangle centre.  The radius is the Euclidean distance from
 * the centre to the farthest corner, so position 1.0 is reached at exactly the corner pixels and
 * the full stop range is always visible inside the rect.
 *
 * Each row is computed into grow() and flushed via er_blit_blend (premultiplied).
 *
 * @param[in] view_props  View props (gradient_stop_count, gradient_stops).
 * @param[in] x           Destination left edge in framebuffer pixels.
 * @param[in] y           Destination top edge in framebuffer pixels.
 * @param[in] width       Destination width in pixels.
 * @param[in] height      Destination height in pixels.
 */
static void render_radial(const ERViewProps* view_props, int x, int y, int width, int height)
{
    GradRamp ramp;
    int first_column, end_column, first_row, end_row;
    if (width <= 0 || height <= 0 || !ramp_init(&ramp, view_props)
        || !clip_box(x, y, width, height, &first_column, &end_column, &first_row, &end_row))
        return;

    GradMask mask;
    mask_init(&mask, view_props, width, height);

    const float center_x = (float)(width - 1) * 0.5f;
    const float center_y = (float)(height - 1) * 0.5f;
    const float radius = sqrtf(center_x * center_x + center_y * center_y);
    const float scale = (radius > GRADIENT_EPSILON) ? (float)RAMP_ONE / radius : 0.0f;
    uint32_t* const pixels = grow();

    for (int row = first_row; row < end_row; row++)
    {
        const float distance_y_squared = ((float)row - center_y) * ((float)row - center_y);
        for (int column = first_column; column < end_column; column++)
        {
            const float distance_x_squared = ((float)column - center_x) * ((float)column - center_x);
            pixels[column - first_column] =
                ramp_color(&ramp,
                           (int32_t)(sqrtf(distance_x_squared + distance_y_squared) * scale + 0.5f),
                           dither_at(x + column, y + row));
        }
        mask_blit_row(&mask, pixels, x, y + row, row, first_column, end_column);
    }
}

#endif /* ERUI_GRADIENT_RADIAL */

#endif /* ERUI_GRADIENT */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_gradient_render(const ERViewProps* view_props, int x, int y, int width, int height)
{
#if ERUI_GRADIENT
    if (!view_props || view_props->gradient_type == ER_GRADIENT_NONE)
        return;
    if (view_props->gradient_type == ER_GRADIENT_LINEAR)
    {
        render_linear(view_props, x, y, width, height);
        return;
    }
#if ERUI_GRADIENT_RADIAL
    if (view_props->gradient_type == ER_GRADIENT_RADIAL)
        render_radial(view_props, x, y, width, height);
#endif
#else
    (void)view_props;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
#endif
}
