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

void er_rrect_clamp_radii(int w, int h, int* r_tl, int* r_tr, int* r_br, int* r_bl)
{
    if (*r_tl == *r_tr && *r_tr == *r_br && *r_br == *r_bl)
    {
        /* Uniform: keep opposite arcs tangent, the same clamp er_rrect_fill applies, so a uniform
         * radius produces one shape no matter which of the two fills paints it. */
        int r = *r_tl;
        if (2 * r >= w)
            r = (w - 1) / 2;
        if (2 * r >= h)
            r = (h - 1) / 2;
        if (r < 0)
            r = 0;
        *r_tl = *r_tr = *r_br = *r_bl = r;
        return;
    }

    /* Mixed: scale each opposing pair down proportionally so neighbouring arcs do not overlap. */
    if (*r_tl + *r_tr > w)
    {
        int s = *r_tl + *r_tr;
        *r_tl = *r_tl * w / s;
        *r_tr = *r_tr * w / s;
    }
    if (*r_bl + *r_br > w)
    {
        int s = *r_bl + *r_br;
        *r_bl = *r_bl * w / s;
        *r_br = *r_br * w / s;
    }
    if (*r_tl + *r_bl > h)
    {
        int s = *r_tl + *r_bl;
        *r_tl = *r_tl * h / s;
        *r_bl = *r_bl * h / s;
    }
    if (*r_tr + *r_br > h)
    {
        int s = *r_tr + *r_br;
        *r_tr = *r_tr * h / s;
        *r_br = *r_br * h / s;
    }
}

void er_rrect_row(int w, int h, int r_tl, int r_tr, int r_br, int r_bl, int row, ERRRectRow* out)
{
    out->x0 = 0;
    out->x1 = w;
    out->l_r = out->l_dx = out->l_dy = 0;
    out->r_r = out->r_dx = out->r_dy = 0;

    /* Left edge: which corner arc (if any) cuts into this row. */
    if (row < r_tl)
    {
        out->l_r = r_tl;
        out->l_dy = r_tl - row;
    }
    else if (r_bl > 0 && row >= h - r_bl)
    {
        out->l_r = r_bl;
        out->l_dy = row - (h - r_bl) + 1;
    }
    if (out->l_r > 0)
    {
        out->l_dx = corner_dx(out->l_r, out->l_dy);
        out->x0 = out->l_r - out->l_dx;
    }

    /* Right edge. */
    if (row < r_tr)
    {
        out->r_r = r_tr;
        out->r_dy = r_tr - row;
    }
    else if (r_br > 0 && row >= h - r_br)
    {
        out->r_r = r_br;
        out->r_dy = row - (h - r_br) + 1;
    }
    if (out->r_r > 0)
    {
        out->r_dx = corner_dx(out->r_r, out->r_dy);
        out->x1 = w - out->r_r + out->r_dx;
    }
}

float er_rrect_fringe_cov(int r, int dx, int dy, int k)
{
    const float cy = (float)dy - 0.5f;
    const float cx = (float)dx + (float)k + 0.5f;
    return (float)r + 0.5f - sqrtf(cx * cx + cy * cy);
}

void er_rrect_fill_corners(uint32_t argb, int x, int y, int w, int h, int r_tl, int r_tr, int r_br, int r_bl)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0)
        return;

    er_rrect_clamp_radii(w, h, &r_tl, &r_tr, &r_br, &r_bl);

    for (int row = 0; row < h; row++)
    {
        ERRRectRow rr;
        er_rrect_row(w, h, r_tl, r_tr, r_br, r_bl, row, &rr);

        fill_span(argb, y + row, x + rr.x0, x + rr.x1);

#if ERUI_BORDER_AA
        /* AA fringe on the left corner edge. */
        if (rr.l_r > 0)
        {
            for (int k = 0;; k++)
            {
                const float cov = er_rrect_fringe_cov(rr.l_r, rr.l_dx, rr.l_dy, k);
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    const int ax = x + rr.x0 - 1 - k;
                    if (ax >= x)
                        er_blit_fill(scale_alpha(argb, (uint8_t)(cov * 255.0f + 0.5f)), ax, y + row, 1, 1);
                }
            }
        }

        /* AA fringe on the right corner edge. */
        if (rr.r_r > 0)
        {
            for (int k = 0;; k++)
            {
                const float cov = er_rrect_fringe_cov(rr.r_r, rr.r_dx, rr.r_dy, k);
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    const int ax = x + rr.x1 + k;
                    if (ax < x + w)
                        er_blit_fill(scale_alpha(argb, (uint8_t)(cov * 255.0f + 0.5f)), ax, y + row, 1, 1);
                }
            }
        }
#endif
    }
}

/**
 * @brief Counts the anti-aliased fringe pixels stepping outward from a corner's solid edge.
 *
 * The ring needs this up front: its solid band has to stop where the inset shape's fringe begins,
 * which the fill paths never need to know because they walk the fringe after painting their span.
 *
 * @param[in] r   Corner arc radius; 0 (a straight edge) has no fringe.
 * @param[in] dx  Solid half-width at this row.
 * @param[in] dy  Row distance from the arc centre.
 *
 * @return Number of fringe pixels, 0 when ERUI_BORDER_AA is off.
 */
static int fringe_len(int r, int dx, int dy)
{
#if ERUI_BORDER_AA
    if (r <= 0)
        return 0;
    int k = 0;
    while (er_rrect_fringe_cov(r, dx, dy, k) > 0.0f) /* strictly decreasing in k, so this terminates */
        k++;
    return k;
#else
    (void)r;
    (void)dx;
    (void)dy;
    return 0;
#endif
}

/** @brief Subtracts an inset from a corner radius without going negative. */
static int inset_radius(int r, int a, int b)
{
    const int d = (a > b) ? a : b; /* the thicker of the two edges meeting at this corner */
    return (r > d) ? r - d : 0;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Dashed / dotted borders: arc-length stepping around the rounded perimeter
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_RRECT_HALF_PI 1.57079632679f

/**
 * @brief Precomputed perimeter walk for stippling a border band.
 *
 * A dash pattern has to run ALONG the border, which on a rounded rect means measuring arc length
 * around eight segments — four straight runs and four quarter arcs — rather than stepping x or y.
 * Every band pixel is mapped back to its distance along that path and painted only where the
 * pattern is "on", so the dashes flow through the corners instead of stopping at them.
 */
typedef struct
{
    uint8_t style;              /**< 0 = solid (the walk is skipped entirely). */
    int w, h;                   /**< Outer box size. */
    int r_tl, r_tr, r_br, r_bl; /**< Clamped outer radii. */
    float seg[8];               /**< Arc-length at the start of each segment, clockwise from (r_tl, 0). */
    float period;               /**< One dash + gap. */
    float on_len;               /**< Painted share of a period. */
} DashCtx;

/**
 * @brief atan(a/b) for non-negative arguments, to about 0.0015 rad.
 *
 * Accurate to a fraction of a pixel at any radius a border is drawn at, and a handful of multiplies
 * rather than a libm call — this runs per band pixel inside the corner arcs, on parts without an FPU.
 *
 * @param[in] a  Numerator, >= 0.
 * @param[in] b  Denominator, >= 0.
 *
 * @return The angle in radians, within [0, pi/2].
 */
static float atan_ratio(float a, float b)
{
    if (b <= 0.0f)
        return (a > 0.0f) ? ER_RRECT_HALF_PI : 0.0f;
    const bool steep = (a > b);
    const float z = steep ? (b / a) : (a / b);
    const float t = z * (0.7853981634f - (z - 1.0f) * (0.2447f + 0.0663f * z));
    return steep ? (ER_RRECT_HALF_PI - t) : t;
}

/**
 * @brief Measures the rounded perimeter and fits the dash pattern to it.
 *
 * The nominal pattern (8-on/6-off dashed, 3-on/3-off dotted) is stretched slightly so a whole number
 * of periods spans the perimeter — otherwise the walk closes on a stub dash at the top-left corner.
 *
 * @param[out] d      Receives the walk.
 * @param[in]  style  0 = solid, 1 = dashed, 2 = dotted.
 * @param[in]  w      Outer width in pixels.
 * @param[in]  h      Outer height in pixels.
 * @param[in]  r_tl   Clamped top-left radius.
 * @param[in]  r_tr   Clamped top-right radius.
 * @param[in]  r_br   Clamped bottom-right radius.
 * @param[in]  r_bl   Clamped bottom-left radius.
 */
static void dash_init(DashCtx* d, uint8_t style, int w, int h, int r_tl, int r_tr, int r_br, int r_bl)
{
    d->style = style;
    d->w = w;
    d->h = h;
    d->r_tl = r_tl;
    d->r_tr = r_tr;
    d->r_br = r_br;
    d->r_bl = r_bl;
    if (style == 0)
        return;

    const float top = (float)(w - r_tl - r_tr) > 0.0f ? (float)(w - r_tl - r_tr) : 0.0f;
    const float right = (float)(h - r_tr - r_br) > 0.0f ? (float)(h - r_tr - r_br) : 0.0f;
    const float bottom = (float)(w - r_br - r_bl) > 0.0f ? (float)(w - r_br - r_bl) : 0.0f;
    const float left = (float)(h - r_bl - r_tl) > 0.0f ? (float)(h - r_bl - r_tl) : 0.0f;

    /* Clockwise from the top edge's left end. */
    d->seg[0] = 0.0f;                                       /* top edge, left -> right */
    d->seg[1] = d->seg[0] + top;                            /* top-right arc */
    d->seg[2] = d->seg[1] + ER_RRECT_HALF_PI * (float)r_tr; /* right edge, top -> bottom */
    d->seg[3] = d->seg[2] + right;                          /* bottom-right arc */
    d->seg[4] = d->seg[3] + ER_RRECT_HALF_PI * (float)r_br; /* bottom edge, right -> left */
    d->seg[5] = d->seg[4] + bottom;                         /* bottom-left arc */
    d->seg[6] = d->seg[5] + ER_RRECT_HALF_PI * (float)r_bl; /* left edge, bottom -> top */
    d->seg[7] = d->seg[6] + left;                           /* top-left arc */
    const float perim = d->seg[7] + ER_RRECT_HALF_PI * (float)r_tl;

    const float on = (style == 1) ? 8.0f : 3.0f;
    const float off = (style == 1) ? 6.0f : 3.0f;
    const float nominal = on + off;
    float periods = (perim > 0.0f) ? (perim / nominal) : 1.0f;
    periods = (float)(int)(periods + 0.5f); /* round to a whole number so the pattern closes */
    if (periods < 1.0f)
        periods = 1.0f;
    d->period = (perim > 0.0f) ? (perim / periods) : nominal;
    d->on_len = d->period * (on / nominal);
}

/**
 * @brief Maps a band pixel to its distance along the rounded perimeter.
 *
 * @param[in] d   The walk.
 * @param[in] px  Pixel column, relative to the box's left edge.
 * @param[in] py  Pixel row, relative to the box's top edge.
 *
 * @return Arc length from the start of the top edge, clockwise.
 */
static float perimeter_s(const DashCtx* d, int px, int py)
{
    /* Corner arcs, each measured from where the preceding straight edge ends. */
    if (px < d->r_tl && py < d->r_tl)
        return d->seg[7] + (float)d->r_tl * atan_ratio((float)(d->r_tl - py), (float)(d->r_tl - px));
    if (px >= d->w - d->r_tr && py < d->r_tr)
        return d->seg[1] + (float)d->r_tr * atan_ratio((float)(px - (d->w - d->r_tr)), (float)(d->r_tr - py));
    if (px >= d->w - d->r_br && py >= d->h - d->r_br)
        return d->seg[3] + (float)d->r_br * atan_ratio((float)(py - (d->h - d->r_br)), (float)(px - (d->w - d->r_br)));
    if (px < d->r_bl && py >= d->h - d->r_bl)
        return d->seg[5] + (float)d->r_bl * atan_ratio((float)(d->r_bl - px), (float)(py - (d->h - d->r_bl)));

    /* Straight runs: a band pixel clear of the arcs belongs to whichever edge it hugs. */
    const int dt = py;
    const int db = d->h - 1 - py;
    const int dl = px;
    const int dr = d->w - 1 - px;
    if (dt <= db && dt <= dl && dt <= dr)
        return d->seg[0] + (float)(px - d->r_tl);
    if (dr <= db && dr <= dl)
        return d->seg[2] + (float)(py - d->r_tr);
    if (db <= dl)
        return d->seg[4] + (float)((d->w - d->r_br) - px);
    return d->seg[6] + (float)((d->h - d->r_bl) - py);
}

/** @brief True when the dash pattern paints the band at this pixel (always true for a solid border). */
static bool dash_on(const DashCtx* d, int px, int py)
{
    if (d->style == 0)
        return true;
    return fmodf(perimeter_s(d, px, py), d->period) < d->on_len;
}

/**
 * @brief Emits one horizontal band run, broken into the dash pattern's painted stretches.
 *
 * @param[in] d      The walk; a solid border emits the run in one call.
 * @param[in] argb   Band colour.
 * @param[in] x      Box left edge in framebuffer pixels.
 * @param[in] y_abs  Scanline in framebuffer pixels.
 * @param[in] row    Scanline relative to the box's top edge.
 * @param[in] x0     Run start, relative to the box's left edge.
 * @param[in] x1     Run end (exclusive), relative to the box's left edge.
 */
static void band_run(const DashCtx* d, uint32_t argb, int x, int y_abs, int row, int x0, int x1)
{
    if (x1 <= x0 || (argb >> 24) == 0)
        return;
    if (d->style == 0)
    {
        fill_span(argb, y_abs, x + x0, x + x1);
        return;
    }
    int run = -1;
    for (int i = x0; i <= x1; i++)
    {
        const bool on = (i < x1) && dash_on(d, i, row);
        if (on && run < 0)
        {
            run = i;
        }
        else if (!on && run >= 0)
        {
            fill_span(argb, y_abs, x + run, x + i);
            run = -1;
        }
    }
}

void er_rrect_fill_ring_edges(
    int x, int y, int w, int h, int r_tl, int r_tr, int r_br, int r_bl, const ERRRectBorder* b)
{
    if (w <= 0 || h <= 0 || !b)
        return;
    if (b->l <= 0 && b->t <= 0 && b->r <= 0 && b->bo <= 0)
        return;
    if ((b->cl >> 24) == 0 && (b->ct >> 24) == 0 && (b->cr >> 24) == 0 && (b->cb >> 24) == 0)
        return;

    er_rrect_clamp_radii(w, h, &r_tl, &r_tr, &r_br, &r_bl);

    /* The inset shape the band is hollowed out by. Corners stay concentric by shrinking each radius
     * by the thicker of the two edges meeting there; a band thick enough to consume the interior
     * leaves no hole at all. */
    const int bl = (b->l > 0) ? b->l : 0;
    const int bt = (b->t > 0) ? b->t : 0;
    const int br = (b->r > 0) ? b->r : 0;
    const int bb = (b->bo > 0) ? b->bo : 0;
    const int iw = w - bl - br;
    const int ih = h - bt - bb;
    const bool has_inner = (iw > 0 && ih > 0);
    int ir_tl = inset_radius(r_tl, bl, bt);
    int ir_tr = inset_radius(r_tr, br, bt);
    int ir_br = inset_radius(r_br, br, bb);
    int ir_bl = inset_radius(r_bl, bl, bb);
    if (has_inner)
        er_rrect_clamp_radii(iw, ih, &ir_tl, &ir_tr, &ir_br, &ir_bl);

    DashCtx dash;
    dash_init(&dash, b->style, w, h, r_tl, r_tr, r_br, r_bl);

    for (int row = 0; row < h; row++)
    {
        ERRRectRow o;
        er_rrect_row(w, h, r_tl, r_tr, r_br, r_bl, row, &o);

        /* Which edge owns this row's band. Top and bottom claim their full width; the side edges own
         * only the rows between them — the same split the four straight edge rects used to make, so
         * per-edge colours land where they always did. */
        const uint32_t cl = (row < bt) ? b->ct : ((row >= h - bb) ? b->cb : b->cl);
        const uint32_t cr = (row < bt) ? b->ct : ((row >= h - bb) ? b->cb : b->cr);

        const int irow = row - bt;
        const bool inner_here = has_inner && irow >= 0 && irow < ih;

        if (!inner_here)
        {
            /* A row above or below the interior: band all the way across. */
            band_run(&dash, cl, x, y + row, row, o.x0, o.x1);
        }
        else
        {
            ERRRectRow in;
            er_rrect_row(iw, ih, ir_tl, ir_tr, ir_br, ir_bl, irow, &in);
            in.x0 += bl; /* inset-relative -> outer-relative; the arc fields stay radius-relative */
            in.x1 += bl;

            /* Left band: outer edge inward, stopping short of the interior's own fringe. */
            const int nl = fringe_len(in.l_r, in.l_dx, in.l_dy);
            band_run(&dash, cl, x, y + row, row, o.x0, in.x0 - nl);

            /* Right band: mirror. */
            const int nr = fringe_len(in.r_r, in.r_dx, in.r_dy);
            band_run(&dash, cr, x, y + row, row, in.x1 + nr, o.x1);

#if ERUI_BORDER_AA
            /* Inner edge: these pixels are partly inside the interior, so the band keeps only the
             * share the interior does NOT cover. */
            if (cl >> 24)
            {
                for (int k = 0; k < nl; k++)
                {
                    const float cov = er_rrect_fringe_cov(in.l_r, in.l_dx, in.l_dy, k);
                    const int ax = in.x0 - 1 - k;
                    if (cov < 1.0f && ax >= o.x0 && ax < o.x1 && dash_on(&dash, ax, row))
                        er_blit_fill(scale_alpha(cl, (uint8_t)((1.0f - cov) * 255.0f + 0.5f)), x + ax, y + row, 1, 1);
                }
            }
            if (cr >> 24)
            {
                for (int k = 0; k < nr; k++)
                {
                    const float cov = er_rrect_fringe_cov(in.r_r, in.r_dx, in.r_dy, k);
                    const int ax = in.x1 + k;
                    if (cov < 1.0f && ax >= o.x0 && ax < o.x1 && dash_on(&dash, ax, row))
                        er_blit_fill(scale_alpha(cr, (uint8_t)((1.0f - cov) * 255.0f + 0.5f)), x + ax, y + row, 1, 1);
                }
            }
#endif
        }

#if ERUI_BORDER_AA
        /* Outer edge: the same fringe a filled rounded rect lays down, so the ring's silhouette
         * matches a solid fill's exactly. */
        if (o.l_r > 0 && (cl >> 24))
        {
            for (int k = 0;; k++)
            {
                const float cov = er_rrect_fringe_cov(o.l_r, o.l_dx, o.l_dy, k);
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    const int ax = o.x0 - 1 - k;
                    if (ax >= 0 && dash_on(&dash, ax, row))
                        er_blit_fill(scale_alpha(cl, (uint8_t)(cov * 255.0f + 0.5f)), x + ax, y + row, 1, 1);
                }
            }
        }
        if (o.r_r > 0 && (cr >> 24))
        {
            for (int k = 0;; k++)
            {
                const float cov = er_rrect_fringe_cov(o.r_r, o.r_dx, o.r_dy, k);
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    const int ax = o.x1 + k;
                    if (ax < w && dash_on(&dash, ax, row))
                        er_blit_fill(scale_alpha(cr, (uint8_t)(cov * 255.0f + 0.5f)), x + ax, y + row, 1, 1);
                }
            }
        }
#endif
    }
}

void er_rrect_fill_ring(uint32_t argb, int x, int y, int w, int h, int r_tl, int r_tr, int r_br, int r_bl, int bw)
{
    const ERRRectBorder b = {bw, bw, bw, bw, argb, argb, argb, argb, 0};
    er_rrect_fill_ring_edges(x, y, w, h, r_tl, r_tr, r_br, r_bl, &b);
}

void er_rrect_fill_bordered(
    uint32_t bg_argb, uint32_t border_argb, int border_w, int x, int y, int w, int h, int radius)
{
    if (border_w <= 0 || (border_argb >> 24) == 0)
    {
        er_rrect_fill(bg_argb, x, y, w, h, radius);
        return;
    }

    const int ix = x + border_w;
    const int iy = y + border_w;
    const int iw = w - 2 * border_w;
    const int ih = h - 2 * border_w;
    const int ir = radius > border_w ? radius - border_w : 0;

    if ((bg_argb >> 24) == 0xFFu)
    {
        /* Opaque background: fill the whole shape in the border colour and paint the background back
         * over the inset. Cheaper than stroking a ring, and the background hides every border pixel
         * it needs to. */
        er_rrect_fill(border_argb, x, y, w, h, radius);
        if (iw > 0 && ih > 0)
            er_rrect_fill(bg_argb, ix, iy, iw, ih, ir);
        return;
    }

    /* Anything less than opaque — a translucent background, the transparent default, or a gradient
     * already painted into this box — cannot cover a filled border shape, so stroke the band only. */
    er_rrect_fill_ring(border_argb, x, y, w, h, radius, radius, radius, radius, border_w);
    if ((bg_argb >> 24) != 0 && iw > 0 && ih > 0)
        er_rrect_fill(bg_argb, ix, iy, iw, ih, ir);
}
