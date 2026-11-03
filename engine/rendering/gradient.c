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
static uint32_t s_gradient_row_pool[ERUI_RENDER_WORKERS][ERUI_MAX_IMG_ROW_PIXELS];

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
 * @param[in] row           Assembled premultiplied ARGB8888 row, @p capped_width pixels wide.
 * @param[in] x             Destination left edge in framebuffer pixels.
 * @param[in] y             Destination row in framebuffer pixels.
 * @param[in] row_index     Row index within the box, 0 = top.
 * @param[in] capped_width  Assembled width (may be less than mask->width on very wide boxes).
 */
static void mask_blit_row(const GradMask* mask, const uint32_t* row, int x, int y, int row_index, int capped_width)
{
    if (!mask->rounded)
    {
        er_blit_blend(row, capped_width * (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x, y, capped_width, 1);
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

    const int x1 = (span.x1 < capped_width) ? span.x1 : capped_width;
    if (x1 > span.x0)
        er_blit_blend(
            &row[span.x0], (x1 - span.x0) * (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x + span.x0, y, x1 - span.x0, 1);

#if ERUI_BORDER_AA
    /* Fringe pixels take the gradient colour at their own column, faded by the arc's coverage. */
    for (int step = 0, step_count = er_rrect_fringe_max(span.left_radius); step < step_count; step++)
    {
        const float coverage =
            er_rrect_fringe_cov(span.left_radius, span.left_half_width, span.left_row_distance, step);
        if (coverage <= 0.0f)
            break;
        const int fringe_x = span.x0 - 1 - step;
        if (coverage < 1.0f && fringe_x >= 0 && fringe_x < capped_width)
        {
            const uint32_t pixel =
                er_px_scale_premul(row[fringe_x], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
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
        if (coverage < 1.0f && fringe_x >= 0 && fringe_x < capped_width && fringe_x < mask->width)
        {
            const uint32_t pixel =
                er_px_scale_premul(row[fringe_x], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
            er_blit_blend(&pixel, (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x + fringe_x, y, 1, 1);
        }
    }
#endif
}

/**
 * @brief Renders a linear gradient into a rectangular framebuffer region.
 *
 * The gradient angle is measured in degrees clockwise from the top: 0° runs top→bottom,
 * 90° runs left→right.  The four corner projections onto the gradient direction vector are
 * used to normalise the per-pixel position to [0.0–1.0] regardless of angle.
 *
 * Each row is computed into grow() and flushed via er_blit_blend (premultiplied).
 *
 * @param[in] view_props  View props (gradient_angle, gradient_stop_count, gradient_stops).
 * @param[in] x           Destination left edge in framebuffer pixels.
 * @param[in] y           Destination top edge in framebuffer pixels.
 * @param[in] width       Destination width in pixels.
 * @param[in] height      Destination height in pixels.
 */
static void render_linear(const ERViewProps* view_props, int x, int y, int width, int height)
{
    if (view_props->gradient_stop_count < 2 || width <= 0 || height <= 0)
        return;

    GradMask mask;
    mask_init(&mask, view_props, width, height);

    const float angle_rad = view_props->gradient_angle * ER_DEG2RAD;
    const float direction_x = sinf(angle_rad);
    const float direction_y = cosf(angle_rad);

    /* Project all four corners onto the direction vector to find the full extent. */
    const float last_x = (float)(width - 1);
    const float last_y = (float)(height - 1);
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

    const float span = max_projection - min_projection;
    const float inverse_span = (span > GRADIENT_EPSILON) ? 1.0f / span : 0.0f;
    const int capped_width = (width <= ERUI_MAX_IMG_ROW_PIXELS) ? width : ERUI_MAX_IMG_ROW_PIXELS;

    for (int row = 0; row < height; row++)
    {
        const float row_base = (float)row * direction_y;
        for (int column = 0; column < capped_width; column++)
        {
            float position = ((float)column * direction_x + row_base - min_projection) * inverse_span;
            if (position < 0.0f)
                position = 0.0f;
            if (position > 1.0f)
                position = 1.0f;
            grow()[column] = er_gradient_premul(
                er_gradient_eval_stops(view_props->gradient_stops, (int)view_props->gradient_stop_count, position));
        }
        mask_blit_row(&mask, grow(), x, y + row, row, capped_width);
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
    if (view_props->gradient_stop_count < 2 || width <= 0 || height <= 0)
        return;

    GradMask mask;
    mask_init(&mask, view_props, width, height);

    const float center_x = (float)(width - 1) * 0.5f;
    const float center_y = (float)(height - 1) * 0.5f;
    const float radius = sqrtf(center_x * center_x + center_y * center_y);
    const float inverse_radius = (radius > GRADIENT_EPSILON) ? 1.0f / radius : 0.0f;
    const int capped_width = (width <= ERUI_MAX_IMG_ROW_PIXELS) ? width : ERUI_MAX_IMG_ROW_PIXELS;

    for (int row = 0; row < height; row++)
    {
        const float distance_y_squared = ((float)row - center_y) * ((float)row - center_y);
        for (int column = 0; column < capped_width; column++)
        {
            const float distance_x_squared = ((float)column - center_x) * ((float)column - center_x);
            float position = sqrtf(distance_x_squared + distance_y_squared) * inverse_radius;
            if (position > 1.0f)
                position = 1.0f;
            grow()[column] = er_gradient_premul(
                er_gradient_eval_stops(view_props->gradient_stops, (int)view_props->gradient_stop_count, position));
        }
        mask_blit_row(&mask, grow(), x, y + row, row, capped_width);
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
