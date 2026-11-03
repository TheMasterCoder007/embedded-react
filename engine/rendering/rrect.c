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

#include "rrect.h"
#include "renderer_internal.h"
#include <math.h>

/* ERUI_BORDER_AA is defaulted in rrect.h, so every rasterizer that masks to a rounded edge agrees. */

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief ERRRectBorder::style values. */
#define BORDER_STYLE_SOLID 0
#define BORDER_STYLE_DASHED 1

/** @brief Nominal dash and dot patterns in pixels, before they are stretched to fit the perimeter. */
#define BORDER_DASH_LENGTH 8.0f
#define BORDER_DASH_GAP 6.0f
#define BORDER_DOT_LENGTH 3.0f
#define BORDER_DOT_GAP 3.0f

/** @brief Full scale of mix_argb()'s blend position. */
#define MIX_POSITION_SCALE 256

/** @brief atan_ratio()'s polynomial: pi/4 and its two correction terms. */
#define ATAN_RATIO_QUARTER_PI 0.7853981634f
#define ATAN_RATIO_POLY_A 0.2447f
#define ATAN_RATIO_POLY_B 0.0663f

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Scales ONLY the alpha channel of a straight-alpha ARGB8888 color by a coverage byte.
 *
 * The straight-alpha counterpart of er_px_scale_premul(): the colour channels are left alone because
 * they are not premultiplied. Applying the premultiplied variant here would darken the colour as well
 * as fade it.
 *
 * @param[in] argb      Straight-alpha ARGB8888 color.
 * @param[in] coverage  Multiplier in the range [0, 255].
 *
 * @return Color with its alpha channel scaled by coverage/255.
 */
static uint32_t scale_straight_alpha(uint32_t argb, uint8_t coverage)
{
    uint32_t alpha = ((argb >> 24) * (uint32_t)coverage + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
    return (argb & 0x00FFFFFFU) | (alpha << 24);
}

/**
 * @brief Fills one horizontal span [x0, x1) at scanline row_y.
 *
 * @param[in] argb   Straight-alpha ARGB8888 color.
 * @param[in] row_y  Y coordinate of the scanline.
 * @param[in] x0     Leftmost pixel, inclusive.
 * @param[in] x1     Rightmost pixel, exclusive.
 */
static void fill_span(uint32_t argb, int row_y, int x0, int x1)
{
    if (x1 > x0)
        er_blit_fill(argb, x0, row_y, x1 - x0, 1);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_rrect_fill(uint32_t argb, int x, int y, int width, int height, int radius)
{
    if (width <= 0 || height <= 0 || (argb >> 24) == 0)
        return;

    if (radius <= 0)
    {
        er_blit_fill(argb, x, y, width, height);
        return;
    }

    /* Clamp so corner arc centers never cross; produces a pill / capsule shape. */
    int corner_radius = radius;
    if (2 * corner_radius >= width)
        corner_radius = (width - 1) / 2;
    if (2 * corner_radius >= height)
        corner_radius = (height - 1) / 2;

    if (corner_radius <= 0)
    {
        er_blit_fill(argb, x, y, width, height);
        return;
    }

    /* Middle strip — full width, no corner rounding. */
    int middle_height = height - 2 * corner_radius;
    if (middle_height > 0)
        er_blit_fill(argb, x, y + corner_radius, width, middle_height);

    /*
     * Corner rows: iterate corner_row from 1 (one step into the corner region, just inside
     * the straight middle band) up to corner_radius (the outermost corner row).
     *
     * Arc center (left): (x + corner_radius, y + corner_radius) for the top corners.
     * Arc center (right): (x + width - 1 - corner_radius, y + corner_radius).
     *
     * For a scanline corner_row rows above the horizontal center line:
     *   exact_half_width = sqrt(corner_radius² − corner_row²)  — arc half-width (float)
     *   half_width       = floor(exact_half_width)             — last fully-inside column offset
     *
     * Interior span: [x + corner_radius − half_width,  x + width − corner_radius + half_width)
     * AA edge pixels (when ERUI_BORDER_AA): columns are stepped outward from the
     *   interior edge (step = 0, 1, 2, ...) until SDF coverage drops to zero.
     *   Pixel centre at a step has offset (half_width + step + 0.5, corner_row - 0.5) from the arc
     *   centre; coverage = corner_radius + 0.5 - distance.  Typically 1-3 iterations; up to
     *   O(sqrt(corner_radius)) at the arc apex.
     */
    for (int corner_row = 1; corner_row <= corner_radius; corner_row++)
    {
        /*
         * Compute the solid-span half-width using pixel-centre SDF sampling,
         * consistent with the AA fringe loop below.
         */
        float sample_radius = (float)corner_radius - 0.5f;
        float sample_y = (float)corner_row - 0.5f;
        /* At the last row the two squares are equal, and a fused multiply-subtract can leave a tiny negative. */
        const float chord_squared = sample_radius * sample_radius - sample_y * sample_y;
        float exact_half_width = chord_squared > 0.0f ? sqrtf(chord_squared) : 0.0f;
        int half_width = (int)(exact_half_width + 0.5f); /* round to nearest */
        int x0 = x + corner_radius - half_width;
        int x1 = x + width - corner_radius + half_width;

        int top_row_y = y + corner_radius - corner_row;
        int bottom_row_y = y + height - corner_radius - 1 + corner_row;

        fill_span(argb, top_row_y, x0, x1);
        if (bottom_row_y != top_row_y)
            fill_span(argb, bottom_row_y, x0, x1);

#if ERUI_BORDER_AA
        for (int step = 0, step_count = er_rrect_fringe_max(corner_radius); step < step_count; step++)
        {
            const float coverage = er_rrect_fringe_cov(corner_radius, half_width, corner_row, step);
            if (coverage <= 0.0f)
                break;
            if (coverage < 1.0f)
            {
                uint32_t fringe_color = scale_straight_alpha(argb, (uint8_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
                int fringe_left_x = x0 - 1 - step;
                int fringe_right_x = x1 + step;

                if (fringe_left_x >= x)
                {
                    er_blit_fill(fringe_color, fringe_left_x, top_row_y, 1, 1);
                    if (bottom_row_y != top_row_y)
                        er_blit_fill(fringe_color, fringe_left_x, bottom_row_y, 1, 1);
                }
                if (fringe_right_x < x + width)
                {
                    er_blit_fill(fringe_color, fringe_right_x, top_row_y, 1, 1);
                    if (bottom_row_y != top_row_y)
                        er_blit_fill(fringe_color, fringe_right_x, bottom_row_y, 1, 1);
                }
            }
        }
#endif
    }
}

/**
 * @brief Computes the solid arc half-width at distance row_distance from a corner arc centre.
 *
 * Uses the same pixel-centre SDF sampling as er_rrect_fill so that
 * er_rrect_fill_corners and er_rrect_fill produce identical edges when all four
 * radii are equal.
 *
 * @param[in] radius        Corner radius in pixels.
 * @param[in] row_distance  Distance from arc centre row (1 = outermost corner row).
 *
 * @return Number of solid pixels inward from the arc centre column.
 */
static int corner_dx(int radius, int row_distance)
{
    float sample_radius = (float)radius - 0.5f;
    float sample_y = (float)row_distance - 0.5f;
    float chord_squared = sample_radius * sample_radius - sample_y * sample_y;
    if (chord_squared < 0.0f)
        return 0;
    return (int)(sqrtf(chord_squared) + 0.5f);
}

void er_rrect_clamp_radii(int width,
                          int height,
                          int* radius_top_left,
                          int* radius_top_right,
                          int* radius_bottom_right,
                          int* radius_bottom_left)
{
    /* Degenerate box: no area, so no corners to round. Handled first so the clamps below can divide
     * and scale by width and height freely. */
    if (width <= 0 || height <= 0)
    {
        *radius_top_left = *radius_top_right = *radius_bottom_right = *radius_bottom_left = 0;
        return;
    }

    /* A negative radius is meaningless — square that corner off. This has to happen BEFORE the
     * uniform test below, for two reasons: it decides which clamp runs (a stray negative would drop
     * an otherwise-uniform set into the mixed path), and the proportional scaling there multiplies by
     * the radius, so a negative would survive it and come back out negative. Reachable from an app:
     * borderTopLeftRadius 10 with borderRadius -5 resolves to (10, -5, -5, -5). */
    if (*radius_top_left < 0)
        *radius_top_left = 0;
    if (*radius_top_right < 0)
        *radius_top_right = 0;
    if (*radius_bottom_right < 0)
        *radius_bottom_right = 0;
    if (*radius_bottom_left < 0)
        *radius_bottom_left = 0;

    if (*radius_top_left == *radius_top_right && *radius_top_right == *radius_bottom_right
        && *radius_bottom_right == *radius_bottom_left)
    {
        /* Uniform: keep opposite arcs tangent, the same clamp er_rrect_fill applies, so a uniform
         * radius produces one shape no matter which of the two fills paints it. Cannot go negative:
         * the input is >= 0 and width, height are >= 1, so (width - 1) / 2 is >= 0 too. */
        int radius = *radius_top_left;
        if (2 * radius >= width)
            radius = (width - 1) / 2;
        if (2 * radius >= height)
            radius = (height - 1) / 2;
        *radius_top_left = *radius_top_right = *radius_bottom_right = *radius_bottom_left = radius;
        return;
    }

    /* Mixed: scale each opposing pair down proportionally so neighbouring arcs do not overlap. */
    if (*radius_top_left + *radius_top_right > width)
    {
        int pair_sum = *radius_top_left + *radius_top_right;
        *radius_top_left = *radius_top_left * width / pair_sum;
        *radius_top_right = *radius_top_right * width / pair_sum;
    }
    if (*radius_bottom_left + *radius_bottom_right > width)
    {
        int pair_sum = *radius_bottom_left + *radius_bottom_right;
        *radius_bottom_left = *radius_bottom_left * width / pair_sum;
        *radius_bottom_right = *radius_bottom_right * width / pair_sum;
    }
    if (*radius_top_left + *radius_bottom_left > height)
    {
        int pair_sum = *radius_top_left + *radius_bottom_left;
        *radius_top_left = *radius_top_left * height / pair_sum;
        *radius_bottom_left = *radius_bottom_left * height / pair_sum;
    }
    if (*radius_top_right + *radius_bottom_right > height)
    {
        int pair_sum = *radius_top_right + *radius_bottom_right;
        *radius_top_right = *radius_top_right * height / pair_sum;
        *radius_bottom_right = *radius_bottom_right * height / pair_sum;
    }
}

void er_rrect_row(int width,
                  int height,
                  int radius_top_left,
                  int radius_top_right,
                  int radius_bottom_right,
                  int radius_bottom_left,
                  int row,
                  ERRRectRow* out)
{
    out->x0 = 0;
    out->x1 = width;
    out->left_radius = out->left_half_width = out->left_row_distance = 0;
    out->right_radius = out->right_half_width = out->right_row_distance = 0;

    /* Left edge: which corner arc (if any) cuts into this row. */
    if (row < radius_top_left)
    {
        out->left_radius = radius_top_left;
        out->left_row_distance = radius_top_left - row;
    }
    else if (radius_bottom_left > 0 && row >= height - radius_bottom_left)
    {
        out->left_radius = radius_bottom_left;
        out->left_row_distance = row - (height - radius_bottom_left) + 1;
    }
    if (out->left_radius > 0)
    {
        out->left_half_width = corner_dx(out->left_radius, out->left_row_distance);
        out->x0 = out->left_radius - out->left_half_width;
    }

    /* Right edge. */
    if (row < radius_top_right)
    {
        out->right_radius = radius_top_right;
        out->right_row_distance = radius_top_right - row;
    }
    else if (radius_bottom_right > 0 && row >= height - radius_bottom_right)
    {
        out->right_radius = radius_bottom_right;
        out->right_row_distance = row - (height - radius_bottom_right) + 1;
    }
    if (out->right_radius > 0)
    {
        out->right_half_width = corner_dx(out->right_radius, out->right_row_distance);
        out->x1 = width - out->right_radius + out->right_half_width;
    }
}

float er_rrect_fringe_cov(int radius, int half_width, int row_distance, int step)
{
    const float sample_y = (float)row_distance - 0.5f;
    const float sample_x = (float)half_width + (float)step + 0.5f;
    return (float)radius + 0.5f - sqrtf(sample_x * sample_x + sample_y * sample_y);
}

int er_rrect_fringe_len(int radius, int half_width, int row_distance)
{
#if ERUI_BORDER_AA
    if (radius <= 0)
        return 0;
    int step = 0;
    while (step < er_rrect_fringe_max(radius) && er_rrect_fringe_cov(radius, half_width, row_distance, step) > 0.0f)
        step++;
    return step;
#else
    (void)radius;
    (void)half_width;
    (void)row_distance;
    return 0;
#endif
}

void er_rrect_fill_corners(uint32_t argb,
                           int x,
                           int y,
                           int width,
                           int height,
                           int radius_top_left,
                           int radius_top_right,
                           int radius_bottom_right,
                           int radius_bottom_left)
{
    if (width <= 0 || height <= 0 || (argb >> 24) == 0)
        return;

    er_rrect_clamp_radii(width, height, &radius_top_left, &radius_top_right, &radius_bottom_right, &radius_bottom_left);

    for (int row = 0; row < height; row++)
    {
        ERRRectRow span;
        er_rrect_row(
            width, height, radius_top_left, radius_top_right, radius_bottom_right, radius_bottom_left, row, &span);

        fill_span(argb, y + row, x + span.x0, x + span.x1);

#if ERUI_BORDER_AA
        /* AA fringe on the left corner edge. */
        for (int step = 0, step_count = er_rrect_fringe_max(span.left_radius); step < step_count; step++)
        {
            const float coverage =
                er_rrect_fringe_cov(span.left_radius, span.left_half_width, span.left_row_distance, step);
            if (coverage <= 0.0f)
                break;
            if (coverage < 1.0f)
            {
                const int fringe_x = x + span.x0 - 1 - step;
                if (fringe_x >= x)
                    er_blit_fill(scale_straight_alpha(argb, (uint8_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f)),
                                 fringe_x,
                                 y + row,
                                 1,
                                 1);
            }
        }

        /* AA fringe on the right corner edge. */
        for (int step = 0, step_count = er_rrect_fringe_max(span.right_radius); step < step_count; step++)
        {
            const float coverage =
                er_rrect_fringe_cov(span.right_radius, span.right_half_width, span.right_row_distance, step);
            if (coverage <= 0.0f)
                break;
            if (coverage < 1.0f)
            {
                const int fringe_x = x + span.x1 + step;
                if (fringe_x < x + width)
                    er_blit_fill(scale_straight_alpha(argb, (uint8_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f)),
                                 fringe_x,
                                 y + row,
                                 1,
                                 1);
            }
        }
#endif
    }
}

/** @brief Subtracts an inset from a corner radius without going negative. */
static int inset_radius(int radius, int first_edge, int second_edge)
{
    const int thicker_edge =
        (first_edge > second_edge) ? first_edge : second_edge; /* the thicker of the two edges meeting at this corner */
    return (radius > thicker_edge) ? radius - thicker_edge : 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Band shading: which edge owns a pixel, and whether a dash pattern paints it there
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Everything a band pixel needs to be shaded: its owning edge, and the dash phase there.
 *
 * Two questions get asked per pixel. WHICH EDGE — adjacent colours have to meet on a diagonal
 * through each corner, not on a horizontal or vertical cut, or a two-colour border shows an obvious
 * step where the colours change. AND IS THE DASH ON — a dash pattern runs ALONG the border, which on
 * a rounded rect means measuring arc length around eight segments (four straight runs, four quarter
 * arcs) rather than stepping x or y, so the dashes flow through the corners instead of stopping at
 * them.
 *
 * A single-colour solid border answers both trivially and takes the `uniform` fast path.
 */
typedef struct
{
    uint8_t style;     /**< 0 = solid (the perimeter walk is skipped entirely). */
    bool uniform;      /**< One colour, no dashes: whole runs emit in a single fill. */
    bool one_color;    /**< All four edges the same colour: no mitre to resolve. */
    int width, height; /**< Outer box size. */
    int radius_top_left, radius_top_right, radius_bottom_right, radius_bottom_left; /**< Clamped outer radii. */
    int band_left, band_top, band_right, band_bottom;          /**< Per-edge band widths, clamped to >= 0. */
    uint32_t left_color, top_color, right_color, bottom_color; /**< Per-edge colours. */
    float mitre_scale_top_left, mitre_scale_top_right, mitre_scale_bottom_right,
        mitre_scale_bottom_left; /**< 1 / |(width, width)| per corner: scales a mitre cross product to pixels. */
    float segment_start[8];      /**< Arc-length at the start of each segment, clockwise from (radius_top_left, 0). */
    float period;                /**< One dash + gap. */
    float on_length;             /**< Painted share of a period. */
} BandCtx;

/**
 * @brief Mixes two straight-alpha ARGB8888 colours channel by channel.
 *
 * @param[in] from      Colour at position 0.
 * @param[in] to        Colour at position 256.
 * @param[in] position  Blend position in [0, 256].
 *
 * @return The blended colour.
 */
static uint32_t mix_argb(uint32_t from, uint32_t to, int position)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const int from_channel = (int)((from >> shift) & 0xFFu);
        const int to_channel = (int)((to >> shift) & 0xFFu);
        out |= (uint32_t)(from_channel + (to_channel - from_channel) * position / MIX_POSITION_SCALE) << shift;
    }
    return out;
}

/**
 * @brief Blends the two colours meeting at a mitre, by how far the pixel sits across the seam.
 *
 * Without this the mitre is the one hard edge in the whole rasterizer — every arc it meets is
 * anti-aliased — and at 45 degrees a hard diagonal staircases badly, reading as the two colours
 * interlocking rather than meeting on a line.
 *
 * @param[in] negative_color  Colour on the negative side of the seam.
 * @param[in] positive_color  Colour on the positive side.
 * @param[in] distance        Signed distance from the pixel centre to the seam, in pixels, positive toward
 *                            @p positive_color.
 *
 * @return The colour for this pixel.
 */
static uint32_t mitre_mix(uint32_t negative_color, uint32_t positive_color, float distance)
{
    if (negative_color == positive_color)
        return positive_color;
#if ERUI_BORDER_AA
    const float coverage = distance + 0.5f;
    if (coverage <= 0.0f)
        return negative_color;
    if (coverage >= 1.0f)
        return positive_color;
    return mix_argb(negative_color, positive_color, (int)(coverage * (float)MIX_POSITION_SCALE + 0.5f));
#else
    return (distance > 0.0f) ? positive_color : negative_color;
#endif
}

/** @brief 1 / |(first_width, second_width)|, or 0 when the corner has no band at all. */
static float mitre_norm(int first_width, int second_width)
{
    const float length = sqrtf((float)(first_width * first_width + second_width * second_width));
    return (length > 0.0f) ? (1.0f / length) : 0.0f;
}

/**
 * @brief atan(numerator/denominator) for non-negative arguments, to about 0.0015 rad.
 *
 * Accurate to a fraction of a pixel at any radius a border is drawn at, and a handful of multiplies
 * rather than a libm call — this runs per band pixel inside the corner arcs, on parts without an FPU.
 *
 * @param[in] numerator    Numerator, >= 0.
 * @param[in] denominator  Denominator, >= 0.
 *
 * @return The angle in radians, within [0, pi/2].
 */
static float atan_ratio(float numerator, float denominator)
{
    if (denominator <= 0.0f)
        return (numerator > 0.0f) ? ER_HALF_PI : 0.0f;
    const bool steep = (numerator > denominator);
    const float ratio = steep ? (denominator / numerator) : (numerator / denominator);
    const float angle =
        ratio * (ATAN_RATIO_QUARTER_PI - (ratio - 1.0f) * (ATAN_RATIO_POLY_A + ATAN_RATIO_POLY_B * ratio));
    return steep ? (ER_HALF_PI - angle) : angle;
}

/**
 * @brief Measures the rounded perimeter and fits the dash pattern to it.
 *
 * The nominal pattern (8-on/6-off dashed, 3-on/3-off dotted) is stretched slightly so a whole number
 * of periods spans the perimeter — otherwise the walk closes on a stub dash at the top-left corner.
 *
 * @param[out] band                 Receives the shading context.
 * @param[in]  border               Per-edge widths, colours and style.
 * @param[in]  width                Outer width in pixels.
 * @param[in]  height               Outer height in pixels.
 * @param[in]  radius_top_left      Clamped top-left radius.
 * @param[in]  radius_top_right     Clamped top-right radius.
 * @param[in]  radius_bottom_right  Clamped bottom-right radius.
 * @param[in]  radius_bottom_left   Clamped bottom-left radius.
 */
static void band_init(BandCtx* band,
                      const ERRRectBorder* border,
                      int width,
                      int height,
                      int radius_top_left,
                      int radius_top_right,
                      int radius_bottom_right,
                      int radius_bottom_left)
{
    const uint8_t style = border->style;
    band->style = style;
    band->width = width;
    band->height = height;
    band->radius_top_left = radius_top_left;
    band->radius_top_right = radius_top_right;
    band->radius_bottom_right = radius_bottom_right;
    band->radius_bottom_left = radius_bottom_left;
    band->band_left = (border->left > 0) ? border->left : 0;
    band->band_top = (border->top > 0) ? border->top : 0;
    band->band_right = (border->right > 0) ? border->right : 0;
    band->band_bottom = (border->bottom > 0) ? border->bottom : 0;
    band->left_color = border->left_color;
    band->top_color = border->top_color;
    band->right_color = border->right_color;
    band->bottom_color = border->bottom_color;
    band->one_color = (border->left_color == border->top_color) && (border->top_color == border->right_color)
                      && (border->right_color == border->bottom_color);
    band->uniform = (style == BORDER_STYLE_SOLID) && band->one_color;
    band->mitre_scale_top_left = mitre_norm(band->band_left, band->band_top);
    band->mitre_scale_top_right = mitre_norm(band->band_right, band->band_top);
    band->mitre_scale_bottom_right = mitre_norm(band->band_right, band->band_bottom);
    band->mitre_scale_bottom_left = mitre_norm(band->band_left, band->band_bottom);
    if (style == BORDER_STYLE_SOLID)
        return;

    const float top = (float)(width - radius_top_left - radius_top_right) > 0.0f
                          ? (float)(width - radius_top_left - radius_top_right)
                          : 0.0f;
    const float right = (float)(height - radius_top_right - radius_bottom_right) > 0.0f
                            ? (float)(height - radius_top_right - radius_bottom_right)
                            : 0.0f;
    const float bottom = (float)(width - radius_bottom_right - radius_bottom_left) > 0.0f
                             ? (float)(width - radius_bottom_right - radius_bottom_left)
                             : 0.0f;
    const float left = (float)(height - radius_bottom_left - radius_top_left) > 0.0f
                           ? (float)(height - radius_bottom_left - radius_top_left)
                           : 0.0f;

    /* Clockwise from the top edge's left end. */
    band->segment_start[0] = 0.0f;                         /* top edge, left -> right */
    band->segment_start[1] = band->segment_start[0] + top; /* top-right arc */
    band->segment_start[2] =
        band->segment_start[1] + ER_HALF_PI * (float)radius_top_right; /* right edge, top -> bottom */
    band->segment_start[3] = band->segment_start[2] + right;           /* bottom-right arc */
    band->segment_start[4] =
        band->segment_start[3] + ER_HALF_PI * (float)radius_bottom_right; /* bottom edge, right -> left */
    band->segment_start[5] = band->segment_start[4] + bottom;             /* bottom-left arc */
    band->segment_start[6] =
        band->segment_start[5] + ER_HALF_PI * (float)radius_bottom_left; /* left edge, bottom -> top */
    band->segment_start[7] = band->segment_start[6] + left;              /* top-left arc */
    const float perimeter = band->segment_start[7] + ER_HALF_PI * (float)radius_top_left;

    const float dash_length = (style == BORDER_STYLE_DASHED) ? BORDER_DASH_LENGTH : BORDER_DOT_LENGTH;
    const float gap_length = (style == BORDER_STYLE_DASHED) ? BORDER_DASH_GAP : BORDER_DOT_GAP;
    const float nominal_period = dash_length + gap_length;
    float periods = (perimeter > 0.0f) ? (perimeter / nominal_period) : 1.0f;
    periods = (float)(int)(periods + 0.5f); /* round to a whole number so the pattern closes */
    if (periods < 1.0f)
        periods = 1.0f;
    band->period = (perimeter > 0.0f) ? (perimeter / periods) : nominal_period;
    band->on_length = band->period * (dash_length / nominal_period);
}

/**
 * @brief Picks which edge owns a band pixel.
 *
 * Adjacent colours meet on the line the two band widths imply — 45 degrees when they are equal,
 * tilted toward the thinner edge otherwise, matching how the web splits a border corner. Inside a
 * ROUNDED corner that line is radial from the arc centre; the comparison there reads inverted next
 * to the square case, because those pixels sit up-and-left of the centre rather than down-and-right
 * of a corner point.
 *
 * @param[in] band     Shading context.
 * @param[in] pixel_x  Pixel column, relative to the box's left edge.
 * @param[in] pixel_y  Pixel row, relative to the box's top edge.
 *
 * @return The owning edge's colour.
 */
static uint32_t band_color(const BandCtx* band, int pixel_x, int pixel_y)
{
    if (band->one_color)
        return band->top_color;

    /* Pixel centre: the seam is a real line, so it has to be measured from the centre of the pixel
     * to fade correctly rather than snapping to the grid. */
    const float center_x = (float)pixel_x + 0.5f;
    const float center_y = (float)pixel_y + 0.5f;

    /* Top-left. The rounded case measures from the arc centre, which is why its terms read inverted
     * next to the square one: those pixels sit up-and-left of the centre, not down-and-right of a
     * corner point. Positive distance is the top edge's side. */
    if (band->radius_top_left > 0)
    {
        if (pixel_x < band->radius_top_left && pixel_y < band->radius_top_left)
            return mitre_mix(band->left_color,
                             band->top_color,
                             ((float)band->band_top * ((float)band->radius_top_left - center_y)
                              - (float)band->band_left * ((float)band->radius_top_left - center_x))
                                 * band->mitre_scale_top_left);
    }
    else if (pixel_x < band->band_left && pixel_y < band->band_top)
    {
        return mitre_mix(band->left_color,
                         band->top_color,
                         ((float)band->band_top * center_x - (float)band->band_left * center_y)
                             * band->mitre_scale_top_left);
    }

    /* Top-right. */
    if (band->radius_top_right > 0)
    {
        if (pixel_x >= band->width - band->radius_top_right && pixel_y < band->radius_top_right)
            return mitre_mix(band->right_color,
                             band->top_color,
                             ((float)band->band_top * ((float)band->radius_top_right - center_y)
                              - (float)band->band_right * (center_x - (float)(band->width - band->radius_top_right)))
                                 * band->mitre_scale_top_right);
    }
    else if (pixel_x >= band->width - band->band_right && pixel_y < band->band_top)
    {
        return mitre_mix(band->right_color,
                         band->top_color,
                         ((float)band->band_top * ((float)band->width - center_x) - (float)band->band_right * center_y)
                             * band->mitre_scale_top_right);
    }

    /* Bottom-right. */
    if (band->radius_bottom_right > 0)
    {
        if (pixel_x >= band->width - band->radius_bottom_right && pixel_y >= band->height - band->radius_bottom_right)
            return mitre_mix(band->right_color,
                             band->bottom_color,
                             ((float)band->band_bottom * (center_y - (float)(band->height - band->radius_bottom_right))
                              - (float)band->band_right * (center_x - (float)(band->width - band->radius_bottom_right)))
                                 * band->mitre_scale_bottom_right);
    }
    else if (pixel_x >= band->width - band->band_right && pixel_y >= band->height - band->band_bottom)
    {
        return mitre_mix(band->right_color,
                         band->bottom_color,
                         ((float)band->band_bottom * ((float)band->width - center_x)
                          - (float)band->band_right * ((float)band->height - center_y))
                             * band->mitre_scale_bottom_right);
    }

    /* Bottom-left. */
    if (band->radius_bottom_left > 0)
    {
        if (pixel_x < band->radius_bottom_left && pixel_y >= band->height - band->radius_bottom_left)
            return mitre_mix(band->left_color,
                             band->bottom_color,
                             ((float)band->band_bottom * (center_y - (float)(band->height - band->radius_bottom_left))
                              - (float)band->band_left * ((float)band->radius_bottom_left - center_x))
                                 * band->mitre_scale_bottom_left);
    }
    else if (pixel_x < band->band_left && pixel_y >= band->height - band->band_bottom)
    {
        return mitre_mix(
            band->left_color,
            band->bottom_color,
            ((float)band->band_bottom * center_x - (float)band->band_left * ((float)band->height - center_y))
                * band->mitre_scale_bottom_left);
    }

    /* Clear of every corner: the pixel hugs exactly one straight edge. */
    const int distance_top = pixel_y;
    const int distance_bottom = band->height - 1 - pixel_y;
    const int distance_left = pixel_x;
    const int distance_right = band->width - 1 - pixel_x;
    if (distance_top <= distance_bottom && distance_top <= distance_left && distance_top <= distance_right)
        return band->top_color;
    if (distance_right <= distance_bottom && distance_right <= distance_left)
        return band->right_color;
    if (distance_bottom <= distance_left)
        return band->bottom_color;
    return band->left_color;
}

/**
 * @brief Maps a band pixel to its distance along the rounded perimeter.
 *
 * @param[in] band     Shading context.
 * @param[in] pixel_x  Pixel column, relative to the box's left edge.
 * @param[in] pixel_y  Pixel row, relative to the box's top edge.
 *
 * @return Arc length from the start of the top edge, clockwise.
 */
static float perimeter_s(const BandCtx* band, int pixel_x, int pixel_y)
{
    /* Corner arcs, each measured from where the preceding straight edge ends. */
    if (pixel_x < band->radius_top_left && pixel_y < band->radius_top_left)
        return band->segment_start[7]
               + (float)band->radius_top_left
                     * atan_ratio((float)(band->radius_top_left - pixel_y), (float)(band->radius_top_left - pixel_x));
    if (pixel_x >= band->width - band->radius_top_right && pixel_y < band->radius_top_right)
        return band->segment_start[1]
               + (float)band->radius_top_right
                     * atan_ratio((float)(pixel_x - (band->width - band->radius_top_right)),
                                  (float)(band->radius_top_right - pixel_y));
    if (pixel_x >= band->width - band->radius_bottom_right && pixel_y >= band->height - band->radius_bottom_right)
        return band->segment_start[3]
               + (float)band->radius_bottom_right
                     * atan_ratio((float)(pixel_y - (band->height - band->radius_bottom_right)),
                                  (float)(pixel_x - (band->width - band->radius_bottom_right)));
    if (pixel_x < band->radius_bottom_left && pixel_y >= band->height - band->radius_bottom_left)
        return band->segment_start[5]
               + (float)band->radius_bottom_left
                     * atan_ratio((float)(band->radius_bottom_left - pixel_x),
                                  (float)(pixel_y - (band->height - band->radius_bottom_left)));

    /* Straight runs: a band pixel clear of the arcs belongs to whichever edge it hugs. */
    const int distance_top = pixel_y;
    const int distance_bottom = band->height - 1 - pixel_y;
    const int distance_left = pixel_x;
    const int distance_right = band->width - 1 - pixel_x;
    if (distance_top <= distance_bottom && distance_top <= distance_left && distance_top <= distance_right)
        return band->segment_start[0] + (float)(pixel_x - band->radius_top_left);
    if (distance_right <= distance_bottom && distance_right <= distance_left)
        return band->segment_start[2] + (float)(pixel_y - band->radius_top_right);
    if (distance_bottom <= distance_left)
        return band->segment_start[4] + (float)((band->width - band->radius_bottom_right) - pixel_x);
    return band->segment_start[6] + (float)((band->height - band->radius_bottom_left) - pixel_y);
}

/** @brief True when the dash pattern paints the band at this pixel (always true for a solid border). */
static bool dash_on(const BandCtx* band, int pixel_x, int pixel_y)
{
    if (band->style == BORDER_STYLE_SOLID)
        return true;
    return fmodf(perimeter_s(band, pixel_x, pixel_y), band->period) < band->on_length;
}

/**
 * @brief Emits one horizontal band run, split wherever the owning edge or the dash phase changes.
 *
 * A single-colour solid border emits the whole run in one fill; otherwise the run is walked pixel by
 * pixel and equal neighbours are coalesced, so a corner that changes colour mid-run still costs only
 * the two fills it needs.
 *
 * @param[in] band        Shading context.
 * @param[in] x           Box left edge in framebuffer pixels.
 * @param[in] absolute_y  Scanline in framebuffer pixels.
 * @param[in] row         Scanline relative to the box's top edge.
 * @param[in] x0          Run start, relative to the box's left edge.
 * @param[in] x1          Run end (exclusive), relative to the box's left edge.
 */
static void band_run(const BandCtx* band, int x, int absolute_y, int row, int x0, int x1)
{
    if (x1 <= x0)
        return;
    if (band->uniform)
    {
        if (band->left_color >> 24)
            fill_span(band->left_color, absolute_y, x + x0, x + x1);
        return;
    }
    int run_start = -1;
    uint32_t run_color = 0;
    for (int column = x0; column <= x1; column++)
    {
        uint32_t color = 0;
        bool painted = false;
        if (column < x1)
        {
            color = band_color(band, column, row);
            painted = ((color >> 24) != 0) && dash_on(band, column, row);
        }
        if (run_start >= 0 && (!painted || color != run_color))
        {
            fill_span(run_color, absolute_y, x + run_start, x + column);
            run_start = -1;
        }
        if (painted && run_start < 0)
        {
            run_start = column;
            run_color = color;
        }
    }
}

void er_rrect_fill_ring_edges(int x,
                              int y,
                              int width,
                              int height,
                              int radius_top_left,
                              int radius_top_right,
                              int radius_bottom_right,
                              int radius_bottom_left,
                              const ERRRectBorder* border)
{
    if (width <= 0 || height <= 0 || !border)
        return;
    if (border->left <= 0 && border->top <= 0 && border->right <= 0 && border->bottom <= 0)
        return;
    if ((border->left_color >> 24) == 0 && (border->top_color >> 24) == 0 && (border->right_color >> 24) == 0
        && (border->bottom_color >> 24) == 0)
        return;

    er_rrect_clamp_radii(width, height, &radius_top_left, &radius_top_right, &radius_bottom_right, &radius_bottom_left);

    /* The inset shape the band is hollowed out by. Corners stay concentric by shrinking each radius
     * by the thicker of the two edges meeting there; a band thick enough to consume the interior
     * leaves no hole at all. */
    const int band_left = (border->left > 0) ? border->left : 0;
    const int band_top = (border->top > 0) ? border->top : 0;
    const int band_right = (border->right > 0) ? border->right : 0;
    const int band_bottom = (border->bottom > 0) ? border->bottom : 0;
    const int inner_width = width - band_left - band_right;
    const int inner_height = height - band_top - band_bottom;
    const bool has_inner = (inner_width > 0 && inner_height > 0);
    int inner_radius_top_left = inset_radius(radius_top_left, band_left, band_top);
    int inner_radius_top_right = inset_radius(radius_top_right, band_right, band_top);
    int inner_radius_bottom_right = inset_radius(radius_bottom_right, band_right, band_bottom);
    int inner_radius_bottom_left = inset_radius(radius_bottom_left, band_left, band_bottom);
    if (has_inner)
        er_rrect_clamp_radii(inner_width,
                             inner_height,
                             &inner_radius_top_left,
                             &inner_radius_top_right,
                             &inner_radius_bottom_right,
                             &inner_radius_bottom_left);

    BandCtx band;
    band_init(&band, border, width, height, radius_top_left, radius_top_right, radius_bottom_right, radius_bottom_left);

    for (int row = 0; row < height; row++)
    {
        ERRRectRow outer;
        er_rrect_row(
            width, height, radius_top_left, radius_top_right, radius_bottom_right, radius_bottom_left, row, &outer);

        const int inner_row = row - band_top;
        const bool inner_here = has_inner && inner_row >= 0 && inner_row < inner_height;

        if (!inner_here)
        {
            /* A row above or below the interior: band all the way across. */
            band_run(&band, x, y + row, row, outer.x0, outer.x1);
        }
        else
        {
            ERRRectRow inner;
            er_rrect_row(inner_width,
                         inner_height,
                         inner_radius_top_left,
                         inner_radius_top_right,
                         inner_radius_bottom_right,
                         inner_radius_bottom_left,
                         inner_row,
                         &inner);
            inner.x0 += band_left; /* inset-relative -> outer-relative; the arc fields stay radius-relative */
            inner.x1 += band_left;

            /* Left band: outer edge inward, stopping short of the interior's own fringe. */
            const int inner_fringe_left =
                er_rrect_fringe_len(inner.left_radius, inner.left_half_width, inner.left_row_distance);
            band_run(&band, x, y + row, row, outer.x0, inner.x0 - inner_fringe_left);

            /* Right band: mirror. */
            const int inner_fringe_right =
                er_rrect_fringe_len(inner.right_radius, inner.right_half_width, inner.right_row_distance);
            band_run(&band, x, y + row, row, inner.x1 + inner_fringe_right, outer.x1);

#if ERUI_BORDER_AA
            /* Inner edge: these pixels are partly inside the interior, so the band keeps only the
             * share the interior does NOT cover. */
            for (int step = 0; step < inner_fringe_left; step++)
            {
                const float coverage =
                    er_rrect_fringe_cov(inner.left_radius, inner.left_half_width, inner.left_row_distance, step);
                const int fringe_x = inner.x0 - 1 - step;
                if (coverage >= 1.0f || fringe_x < outer.x0 || fringe_x >= outer.x1 || !dash_on(&band, fringe_x, row))
                    continue;
                const uint32_t fringe_color = band_color(&band, fringe_x, row);
                if (fringe_color >> 24)
                    er_blit_fill(
                        scale_straight_alpha(fringe_color, (uint8_t)((1.0f - coverage) * ER_CHANNEL_MAX_FLOAT + 0.5f)),
                        x + fringe_x,
                        y + row,
                        1,
                        1);
            }
            for (int step = 0; step < inner_fringe_right; step++)
            {
                const float coverage =
                    er_rrect_fringe_cov(inner.right_radius, inner.right_half_width, inner.right_row_distance, step);
                const int fringe_x = inner.x1 + step;
                if (coverage >= 1.0f || fringe_x < outer.x0 || fringe_x >= outer.x1 || !dash_on(&band, fringe_x, row))
                    continue;
                const uint32_t fringe_color = band_color(&band, fringe_x, row);
                if (fringe_color >> 24)
                    er_blit_fill(
                        scale_straight_alpha(fringe_color, (uint8_t)((1.0f - coverage) * ER_CHANNEL_MAX_FLOAT + 0.5f)),
                        x + fringe_x,
                        y + row,
                        1,
                        1);
            }
#endif
        }

#if ERUI_BORDER_AA
        /* Outer edge: the same fringe a filled rounded rect lays down, so the ring's silhouette
         * matches a solid fill's exactly. */
        for (int step = 0, step_count = er_rrect_fringe_max(outer.left_radius); step < step_count; step++)
        {
            const float coverage =
                er_rrect_fringe_cov(outer.left_radius, outer.left_half_width, outer.left_row_distance, step);
            if (coverage <= 0.0f)
                break;
            const int fringe_x = outer.x0 - 1 - step;
            if (coverage >= 1.0f || fringe_x < 0 || !dash_on(&band, fringe_x, row))
                continue;
            const uint32_t fringe_color = band_color(&band, fringe_x, row);
            if (fringe_color >> 24)
                er_blit_fill(scale_straight_alpha(fringe_color, (uint8_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f)),
                             x + fringe_x,
                             y + row,
                             1,
                             1);
        }
        for (int step = 0, step_count = er_rrect_fringe_max(outer.right_radius); step < step_count; step++)
        {
            const float coverage =
                er_rrect_fringe_cov(outer.right_radius, outer.right_half_width, outer.right_row_distance, step);
            if (coverage <= 0.0f)
                break;
            const int fringe_x = outer.x1 + step;
            if (coverage >= 1.0f || fringe_x >= width || !dash_on(&band, fringe_x, row))
                continue;
            const uint32_t fringe_color = band_color(&band, fringe_x, row);
            if (fringe_color >> 24)
                er_blit_fill(scale_straight_alpha(fringe_color, (uint8_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f)),
                             x + fringe_x,
                             y + row,
                             1,
                             1);
        }
#endif
    }
}

void er_rrect_fill_ring(uint32_t argb,
                        int x,
                        int y,
                        int width,
                        int height,
                        int radius_top_left,
                        int radius_top_right,
                        int radius_bottom_right,
                        int radius_bottom_left,
                        int band_width)
{
    const ERRRectBorder border = {band_width, band_width, band_width, band_width, argb, argb, argb, argb, 0};
    er_rrect_fill_ring_edges(
        x, y, width, height, radius_top_left, radius_top_right, radius_bottom_right, radius_bottom_left, &border);
}

void er_rrect_fill_bordered(
    uint32_t background_argb, uint32_t border_argb, int border_width, int x, int y, int width, int height, int radius)
{
    if (border_width <= 0 || (border_argb >> 24) == 0)
    {
        er_rrect_fill(background_argb, x, y, width, height, radius);
        return;
    }

    const int inner_x = x + border_width;
    const int inner_y = y + border_width;
    const int inner_width = width - 2 * border_width;
    const int inner_height = height - 2 * border_width;
    const int inner_radius = radius > border_width ? radius - border_width : 0;

    if ((background_argb >> 24) == ER_ALPHA_OPAQUE)
    {
        /* Opaque background: fill the whole shape in the border colour and paint the background back
         * over the inset. Cheaper than stroking a ring, and the background hides every border pixel
         * it needs to. */
        er_rrect_fill(border_argb, x, y, width, height, radius);
        if (inner_width > 0 && inner_height > 0)
            er_rrect_fill(background_argb, inner_x, inner_y, inner_width, inner_height, inner_radius);
        return;
    }

    /* Anything less than opaque — a translucent background, the transparent default, or a gradient
     * already painted into this box — cannot cover a filled border shape, so stroke the band only. */
    er_rrect_fill_ring(border_argb, x, y, width, height, radius, radius, radius, radius, border_width);
    if ((background_argb >> 24) != 0 && inner_width > 0 && inner_height > 0)
        er_rrect_fill(background_argb, inner_x, inner_y, inner_width, inner_height, inner_radius);
}
