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

/*----------------------------------------------------------------------------------------------------------------------
 - Macros
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_BORDER_AA
#define ERUI_BORDER_AA 1
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Scales the alpha channel of a straight-alpha ARGB8888 color by a coverage byte.
 *
 * @param[in] argb      Straight-alpha ARGB8888 color.
 * @param[in] coverage  Multiplier in the range [0, 255].
 *
 * @return Color with its alpha channel scaled by coverage/255.
 */
static uint32_t scale_alpha(uint32_t argb, uint8_t coverage)
{
    uint32_t a = ((argb >> 24) * (uint32_t)coverage + 127U) / 255U;
    return (argb & 0x00FFFFFFU) | (a << 24);
}

/**
 * @brief Fills one horizontal span [x0, x1) at scanline py.
 *
 * @param[in] argb  Straight-alpha ARGB8888 color.
 * @param[in] py    Y coordinate of the scanline.
 * @param[in] x0    Leftmost pixel, inclusive.
 * @param[in] x1    Rightmost pixel, exclusive.
 */
static void fill_span(uint32_t argb, int py, int x0, int x1)
{
    if (x1 > x0)
        er_blit_fill(argb, x0, py, x1 - x0, 1);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_rrect_fill(uint32_t argb, int x, int y, int w, int h, int radius)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0)
        return;

    if (radius <= 0)
    {
        er_blit_fill(argb, x, y, w, h);
        return;
    }

    /* Clamp so corner arc centers never cross; produces a pill / capsule shape. */
    int r = radius;
    if (2 * r >= w)
        r = (w - 1) / 2;
    if (2 * r >= h)
        r = (h - 1) / 2;

    if (r <= 0)
    {
        er_blit_fill(argb, x, y, w, h);
        return;
    }

    /* Middle strip — full width, no corner rounding. */
    int mid_h = h - 2 * r;
    if (mid_h > 0)
        er_blit_fill(argb, x, y + r, w, mid_h);

    /*
     * Corner rows: iterate dy from 1 (one step into the corner region, just inside
     * the straight middle band) up to r (the outermost corner row).
     *
     * Arc center (left): (x + r, y + r) for the top corners.
     * Arc center (right): (x + w - 1 - r, y + r).
     *
     * For a scanline at distance dy above the horizontal center line:
     *   dx_f = sqrt(r² − dy²)                   — arc half-width (float)
     *   dx   = floor(dx_f)                       — last fully-inside column offset
     *
     * Interior span: [x + r − dx,  x + w − r + dx)
     * AA edge pixels (when ERUI_BORDER_AA): columns are stepped outward from the
     *   interior edge (k = 0, 1, 2, ...) until SDF coverage drops to zero.
     *   Pixel centre at step k has offset (dx + k + 0.5, dy - 0.5) from the arc
     *   centre; coverage = r + 0.5 - dist.  Typically 1-3 iterations; up to
     *   O(sqrt(r)) at the arc apex.
     */
    for (int dy = 1; dy <= r; dy++)
    {
        /*
         * Compute the solid-span half-width using pixel-centre SDF sampling,
         * consistent with the AA fringe loop below.
         */
        float r05 = (float)r - 0.5f;
        float cy0 = (float)dy - 0.5f;
        float dx_f = sqrtf(r05 * r05 - cy0 * cy0);
        int dx = (int)(dx_f + 0.5f); /* round to nearest */
        int x0 = x + r - dx;
        int x1 = x + w - r + dx;

        int top_py = y + r - dy;
        int bot_py = y + h - r - 1 + dy;

        fill_span(argb, top_py, x0, x1);
        if (bot_py != top_py)
            fill_span(argb, bot_py, x0, x1);

#if ERUI_BORDER_AA
        {
            float cy = (float)dy - 0.5f;
            for (int k = 0;; k++)
            {
                float cx = (float)dx + (float)k + 0.5f;
                float dist = sqrtf(cx * cx + cy * cy);
                float cov = (float)r + 0.5f - dist;
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    uint32_t aa = scale_alpha(argb, (uint8_t)(cov * 255.0f + 0.5f));
                    int ax_l = x0 - 1 - k;
                    int ax_r = x1 + k;

                    if (ax_l >= x)
                    {
                        er_blit_fill(aa, ax_l, top_py, 1, 1);
                        if (bot_py != top_py)
                            er_blit_fill(aa, ax_l, bot_py, 1, 1);
                    }
                    if (ax_r < x + w)
                    {
                        er_blit_fill(aa, ax_r, top_py, 1, 1);
                        if (bot_py != top_py)
                            er_blit_fill(aa, ax_r, bot_py, 1, 1);
                    }
                }
            }
        }
#endif
    }
}

/**
 * @brief Computes the solid arc half-width at distance dy from a corner arc centre.
 *
 * Uses the same pixel-centre SDF sampling as er_rrect_fill so that
 * er_rrect_fill_corners and er_rrect_fill produce identical edges when all four
 * radii are equal.
 *
 * @param[in] r   Corner radius in pixels.
 * @param[in] dy  Distance from arc centre row (1 = outermost corner row).
 *
 * @return Number of solid pixels inward from the arc centre column.
 */
static int corner_dx(int r, int dy)
{
    float r05 = (float)r - 0.5f;
    float cy = (float)dy - 0.5f;
    float val = r05 * r05 - cy * cy;
    if (val < 0.0f)
        return 0;
    return (int)(sqrtf(val) + 0.5f);
}

void er_rrect_fill_corners(uint32_t argb, int x, int y, int w, int h, int r_tl, int r_tr, int r_br, int r_bl)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0)
        return;

    /* Clamp so opposite corner arcs do not overlap horizontally or vertically. */
    if (r_tl + r_tr > w)
    {
        int s = r_tl + r_tr;
        r_tl = r_tl * w / s;
        r_tr = r_tr * w / s;
    }
    if (r_bl + r_br > w)
    {
        int s = r_bl + r_br;
        r_bl = r_bl * w / s;
        r_br = r_br * w / s;
    }
    if (r_tl + r_bl > h)
    {
        int s = r_tl + r_bl;
        r_tl = r_tl * h / s;
        r_bl = r_bl * h / s;
    }
    if (r_tr + r_br > h)
    {
        int s = r_tr + r_br;
        r_tr = r_tr * h / s;
        r_br = r_br * h / s;
    }

    for (int row = 0; row < h; row++)
    {
        int left_x = x;
        int right_x = x + w;

        /* Resolve left-edge corner radius for this row. */
        int left_r = 0;
        int left_dy = 0;
        if (row < r_tl)
        {
            left_r = r_tl;
            left_dy = r_tl - row;
        }
        else if (r_bl > 0 && row >= h - r_bl)
        {
            left_r = r_bl;
            left_dy = row - (h - r_bl) + 1;
        }

        int left_dx = 0;
        if (left_r > 0)
        {
            left_dx = corner_dx(left_r, left_dy);
            left_x = x + left_r - left_dx;
        }

        /* Resolve right-edge corner radius for this row. */
        int right_r = 0;
        int right_dy = 0;
        if (row < r_tr)
        {
            right_r = r_tr;
            right_dy = r_tr - row;
        }
        else if (r_br > 0 && row >= h - r_br)
        {
            right_r = r_br;
            right_dy = row - (h - r_br) + 1;
        }

        int right_dx = 0;
        if (right_r > 0)
        {
            right_dx = corner_dx(right_r, right_dy);
            right_x = x + w - right_r + right_dx;
        }

        fill_span(argb, y + row, left_x, right_x);

#if ERUI_BORDER_AA
        /* AA fringe on the left corner edge. */
        if (left_r > 0)
        {
            float cy = (float)left_dy - 0.5f;
            for (int k = 0;; k++)
            {
                float cx = (float)left_dx + (float)k + 0.5f;
                float dist = sqrtf(cx * cx + cy * cy);
                float cov = (float)left_r + 0.5f - dist;
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    int ax = x + left_r - left_dx - 1 - k;
                    if (ax >= x)
                        er_blit_fill(scale_alpha(argb, (uint8_t)(cov * 255.0f + 0.5f)), ax, y + row, 1, 1);
                }
            }
        }

        /* AA fringe on the right corner edge. */
        if (right_r > 0)
        {
            float cy = (float)right_dy - 0.5f;
            for (int k = 0;; k++)
            {
                float cx = (float)right_dx + (float)k + 0.5f;
                float dist = sqrtf(cx * cx + cy * cy);
                float cov = (float)right_r + 0.5f - dist;
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    int ax = x + w - right_r + right_dx + k;
                    if (ax < x + w)
                        er_blit_fill(scale_alpha(argb, (uint8_t)(cov * 255.0f + 0.5f)), ax, y + row, 1, 1);
                }
            }
        }
#endif
    }
}

void er_rrect_border_edge(uint32_t argb, uint8_t style, int x, int y, int w, int h, int horizontal)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0)
        return;
    if (style == 0)
    {
        er_blit_fill(argb, x, y, w, h);
        return;
    }
    /* style == 1 → dashed (8 px on, 6 px off); style == 2 → dotted (3 px on, 3 px off). */
    const int on = (style == 1) ? 8 : 3;
    const int off = (style == 1) ? 6 : 3;
    const int step = on + off;
    const int span = horizontal ? w : h;
    for (int pos = 0; pos < span; pos += step)
    {
        int fill = on < (span - pos) ? on : (span - pos);
        if (fill <= 0)
            break;
        if (horizontal)
            er_blit_fill(argb, x + pos, y, fill, h);
        else
            er_blit_fill(argb, x, y + pos, w, fill);
    }
}

void er_rrect_fill_bordered(
    uint32_t bg_argb, uint32_t border_argb, int border_w, int x, int y, int w, int h, int radius)
{
    if (border_w > 0 && (border_argb >> 24) != 0)
    {
        er_rrect_fill(border_argb, x, y, w, h, radius);

        int ix = x + border_w;
        int iy = y + border_w;
        int iw = w - 2 * border_w;
        int ih = h - 2 * border_w;
        int ir = radius > border_w ? radius - border_w : 0;

        if (iw > 0 && ih > 0)
            er_rrect_fill(bg_argb, ix, iy, iw, ih, ir);
    }
    else
    {
        er_rrect_fill(bg_argb, x, y, w, h, radius);
    }
}
