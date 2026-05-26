#include "renderer_internal.h"
#include "rrect.h"
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
        float dx_f = sqrtf((float)(r * r - dy * dy));
        int dx = (int)dx_f;
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
            for (int k = 0; ; k++)
            {
                float cx   = (float)dx + (float)k + 0.5f;
                float dist = sqrtf(cx * cx + cy * cy);
                float cov  = (float)r + 0.5f - dist;
                if (cov <= 0.0f)
                    break;
                if (cov < 1.0f)
                {
                    uint32_t aa  = scale_alpha(argb, (uint8_t)(cov * 255.0f + 0.5f));
                    int ax_l = x0 - 1 - k;
                    int ax_r = x1     + k;

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

void er_rrect_fill_bordered(uint32_t bg_argb, uint32_t border_argb,
                            int border_w, int x, int y, int w, int h, int radius)
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
