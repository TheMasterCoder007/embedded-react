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

#include "shadow.h"
#include "er_limits.h"
#include "renderer_internal.h"
#include <string.h>

#if ERUI_SHADOWS

/* Android-style `elevation`: one number stands in for offset, opacity and blur. React Native maps it to
 * the platform's own elevation shadow, so there is no exact formula to copy — these are the slopes that
 * make an elevation ladder (1, 2, 4, 8, ...) look like Material's, with the blur radius taken as the
 * elevation itself. The opacity cap stops a large elevation turning the shadow into a black box. */
#define SHADOW_ELEVATION_OFFSET_PER_DP 0.33f /**< Downward offset in px per unit of elevation. */
#define SHADOW_ELEVATION_OPACITY_BASE 0.2f   /**< Opacity at the lowest elevation. */
#define SHADOW_ELEVATION_OPACITY_SLOPE 0.01f /**< Extra opacity per unit of elevation. */
#define SHADOW_ELEVATION_OPACITY_MAX 0.5f    /**< Ceiling on the synthesised opacity. */

/** @brief Opaque black, the colour of an elevation shadow and of one with no shadowColor. */
#define SHADOW_DEFAULT_COLOR 0xFF000000U

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Alpha-only working buffer: the blurred shape lives here before being tinted and blitted. */
static uint8_t s_alpha[ERUI_SCRATCH_HEIGHT][ERUI_SCRATCH_WIDTH];

/* Running-sum accumulator shared by hblur() and vblur(). Sized for the longer dimension. */
static uint16_t s_blur_sums[ERUI_SCRATCH_WIDTH >= ERUI_SCRATCH_HEIGHT ? ERUI_SCRATCH_WIDTH : ERUI_SCRATCH_HEIGHT];

/* One row of premultiplied ARGB8888 pixels assembled before each er_blit_blend() call. */
static uint32_t s_row_buffer[ERUI_SCRATCH_WIDTH];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fills the node silhouette into the shadow alpha buffer at offset (blur_radius, blur_radius).
 *
 * Pixels inside the rounded rectangle are written as opaque; corner pixels that fall
 * outside the arc are written as 0.  All other cells in the active region must be
 * cleared to 0 by the caller before this is called.
 *
 * @param[in] blur_radius  Blur radius; the shape is placed at column/row offset blur_radius in s_alpha.
 * @param[in] node_width   Node width in pixels.
 * @param[in] node_height  Node height in pixels.
 * @param[in] corner       Corner radius of the rounded rectangle in pixels.
 */
static void fill_shape(int blur_radius, int node_width, int node_height, int corner)
{
    if (corner < 0)
        corner = 0;
    const int max_corner = (node_width < node_height ? node_width : node_height) / 2;
    if (corner > max_corner)
        corner = max_corner;

    for (int row = 0; row < node_height; row++)
    {
        for (int column = 0; column < node_width; column++)
        {
            int inside = 1;
            if (corner > 0)
            {
                const int in_left = column < corner;
                const int in_right = column >= node_width - corner;
                const int in_top = row < corner;
                const int in_bottom = row >= node_height - corner;

                if ((in_left || in_right) && (in_top || in_bottom))
                {
                    const int center_x = in_left ? corner : node_width - 1 - corner;
                    const int center_y = in_top ? corner : node_height - 1 - corner;
                    const int distance_x = column - center_x;
                    const int distance_y = row - center_y;
                    inside = (distance_x * distance_x + distance_y * distance_y) <= corner * corner;
                }
            }
            s_alpha[blur_radius + row][blur_radius + column] = inside ? ER_ALPHA_OPAQUE : 0U;
        }
    }
}

/**
 * @brief Applies one horizontal box-blur pass to the active region of s_alpha.
 *
 * Pixels outside [0, active_width) are treated as 0 (zero-padded boundary).  Results are
 * written back in-place via the s_blur_sums accumulator so the pass is a true single
 * scan without read-after-write aliasing.
 *
 * @param[in] active_width   Width of the active region in s_alpha (columns 0..active_width-1).
 * @param[in] active_height  Height of the active region in s_alpha (rows 0..active_height-1).
 * @param[in] blur_radius    Box half-radius; the blur window width is 2 * blur_radius + 1.
 */
static void hblur(int active_width, int active_height, int blur_radius)
{
    const int window = 2 * blur_radius + 1;
    for (int row = 0; row < active_height; row++)
    {
        uint8_t* const row_alpha = s_alpha[row];
        uint16_t sum = 0;

        /* Pre-load elements that are already inside the window at col = 0.
         * With zero-padding, elements at indices < 0 contribute 0 and are skipped. */
        for (int preload_column = 0; preload_column < blur_radius && preload_column < active_width; preload_column++)
            sum += row_alpha[preload_column];

        for (int column = 0; column < active_width; column++)
        {
            const int right = column + blur_radius;
            sum += (right < active_width) ? row_alpha[right] : 0U;
            s_blur_sums[column] = sum;
            const int left = column - blur_radius;
            sum -= (left >= 0) ? row_alpha[left] : 0U;
        }
        for (int column = 0; column < active_width; column++)
            row_alpha[column] = (uint8_t)(s_blur_sums[column] / (uint16_t)window);
    }
}

/**
 * @brief Applies one vertical box-blur pass to the active region of s_alpha.
 *
 * Pixels outside [0, active_height) are treated as 0 (zero-padded boundary).
 *
 * @param[in] active_width   Width of the active region in s_alpha.
 * @param[in] active_height  Height of the active region in s_alpha (rows 0..active_height-1).
 * @param[in] blur_radius    Box half-radius; the blur window height is 2 * blur_radius + 1.
 */
static void vblur(int active_width, int active_height, int blur_radius)
{
    const int window = 2 * blur_radius + 1;
    for (int column = 0; column < active_width; column++)
    {
        uint16_t sum = 0;

        for (int preload_row = 0; preload_row < blur_radius && preload_row < active_height; preload_row++)
            sum += s_alpha[preload_row][column];

        for (int row = 0; row < active_height; row++)
        {
            const int below = row + blur_radius;
            sum += (below < active_height) ? s_alpha[below][column] : 0U;
            s_blur_sums[row] = sum;
            const int above = row - blur_radius;
            sum -= (above >= 0) ? s_alpha[above][column] : 0U;
        }
        for (int row = 0; row < active_height; row++)
            s_alpha[row][column] = (uint8_t)(s_blur_sums[row] / (uint16_t)window);
    }
}

#endif /* ERUI_SHADOWS */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_shadow_render(const ERViewProps* view_props, int x, int y, int width, int height)
{
#if ERUI_SHADOWS
    /* Resolve shadow parameters from explicit props or from elevation. */
    uint32_t color;
    float offset_x, offset_y, opacity;
    int radius;

    if (view_props->shadow_opacity > 0.0f)
    {
        color = view_props->shadow_color ? view_props->shadow_color : SHADOW_DEFAULT_COLOR;
        offset_x = view_props->shadow_offset_x;
        offset_y = view_props->shadow_offset_y;
        opacity = view_props->shadow_opacity > 1.0f ? 1.0f : view_props->shadow_opacity;
        radius = (int)view_props->shadow_radius;
    }
    else if (view_props->elevation > 0)
    {
        /* Android-style elevation: synthesise a downward black shadow. */
        color = SHADOW_DEFAULT_COLOR;
        offset_x = 0.0f;
        offset_y = (float)view_props->elevation * SHADOW_ELEVATION_OFFSET_PER_DP;
        opacity = SHADOW_ELEVATION_OPACITY_BASE + (float)view_props->elevation * SHADOW_ELEVATION_OPACITY_SLOPE;
        if (opacity > SHADOW_ELEVATION_OPACITY_MAX)
            opacity = SHADOW_ELEVATION_OPACITY_MAX;
        radius = (int)view_props->elevation;
    }
    else
    {
        return; /* No shadow requested. */
    }

    if (width <= 0 || height <= 0)
        return;

    /* Skip gracefully when the node alone already exceeds the scratch buffer. */
    if (width > ERUI_SCRATCH_WIDTH || height > ERUI_SCRATCH_HEIGHT)
        return;

    /* Clamp blur radius so the padded shadow fits inside the scratch buffer. */
    int max_radius_x = (ERUI_SCRATCH_WIDTH - width) / 2;
    int max_radius_y = (ERUI_SCRATCH_HEIGHT - height) / 2;
    if (radius > max_radius_x)
        radius = max_radius_x;
    if (radius > max_radius_y)
        radius = max_radius_y;
    if (radius < 0)
        radius = 0;

    const int active_width = width + 2 * radius;   /* active shadow buffer width  */
    const int active_height = height + 2 * radius; /* active shadow buffer height */

    /* World-space top-left of the shadow (includes blur spread and offset). */
    const int shadow_x = x + (int)offset_x - radius;
    const int shadow_y = y + (int)offset_y - radius;

    /* Clear the active region. */
    for (int row = 0; row < active_height; row++)
        memset(s_alpha[row], 0, (size_t)active_width);

    /* Rasterise the node silhouette at position (radius, radius) in the buffer. */
    fill_shape(radius, width, height, (int)view_props->border_radius);

    /* Two-pass separable box blur approximates a Gaussian soft shadow. */
    if (radius > 0)
    {
        hblur(active_width, active_height, radius);
        vblur(active_width, active_height, radius);
    }

    /* Build premultiplied ARGB pixels and blit one row at a time to avoid a
     * second large static buffer.  The shadow_color alpha multiplies with
     * shadow_opacity to produce the effective base alpha. */
    const uint8_t color_alpha = (uint8_t)((color >> 24) & 0xFFU);
    const uint8_t color_red = (uint8_t)((color >> 16) & 0xFFU);
    const uint8_t color_green = (uint8_t)((color >> 8) & 0xFFU);
    const uint8_t color_blue = (uint8_t)(color & 0xFFU);
    const uint32_t shadow_alpha = (uint32_t)(opacity * ER_CHANNEL_MAX_FLOAT + 0.5f);
    const uint32_t base_alpha = shadow_alpha * (uint32_t)color_alpha / ER_CHANNEL_MAX;

    const int stride = (int)((size_t)active_width * sizeof(uint32_t));
    for (int row = 0; row < active_height; row++)
    {
        for (int column = 0; column < active_width; column++)
        {
            const uint32_t pixel_alpha = (uint32_t)s_alpha[row][column] * base_alpha / ER_CHANNEL_MAX;
            const uint32_t premultiplied_red = (uint32_t)color_red * pixel_alpha / ER_CHANNEL_MAX;
            const uint32_t premultiplied_green = (uint32_t)color_green * pixel_alpha / ER_CHANNEL_MAX;
            const uint32_t premultiplied_blue = (uint32_t)color_blue * pixel_alpha / ER_CHANNEL_MAX;
            s_row_buffer[column] =
                (pixel_alpha << 24U) | (premultiplied_red << 16U) | (premultiplied_green << 8U) | premultiplied_blue;
        }
        er_blit_blend(s_row_buffer, stride, ER_ALPHA_OPAQUE, shadow_x, shadow_y + row, active_width, 1);
    }
#else
    (void)view_props;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
#endif
}
