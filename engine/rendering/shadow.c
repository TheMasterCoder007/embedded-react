#include "shadow.h"
#include "renderer_internal.h"
#include <string.h>

#ifndef ERUI_SCRATCH_W
#define ERUI_SCRATCH_W 240
#endif
#ifndef ERUI_SCRATCH_H
#define ERUI_SCRATCH_H 240
#endif

#if ERUI_SHADOWS

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Alpha-only working buffer: the blurred shape lives here before being tinted and blitted. */
static uint8_t s_alpha[ERUI_SCRATCH_H][ERUI_SCRATCH_W];

/* Running-sum accumulator shared by hblur() and vblur(). Sized for the longer dimension. */
static uint16_t s_blur_tmp[ERUI_SCRATCH_W >= ERUI_SCRATCH_H ? ERUI_SCRATCH_W : ERUI_SCRATCH_H];

/* One row of premultiplied ARGB8888 pixels assembled before each er_blit_blend() call. */
static uint32_t s_row_buf[ERUI_SCRATCH_W];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fills the node silhouette into the shadow alpha buffer at offset (r, r).
 *
 * Pixels inside the rounded rectangle are written as 255; corner pixels that fall
 * outside the arc are written as 0.  All other cells in the active region must be
 * cleared to 0 by the caller before this is called.
 *
 * @param[in] r       Blur radius; the shape is placed at column/row offset r in s_alpha.
 * @param[in] nw      Node width in pixels.
 * @param[in] nh      Node height in pixels.
 * @param[in] corner  Corner radius of the rounded rectangle in pixels.
 */
static void fill_shape(int r, int nw, int nh, int corner)
{
    if (corner < 0)
        corner = 0;
    const int max_corner = (nw < nh ? nw : nh) / 2;
    if (corner > max_corner)
        corner = max_corner;

    for (int row = 0; row < nh; row++)
    {
        for (int col = 0; col < nw; col++)
        {
            int inside = 1;
            if (corner > 0)
            {
                const int in_left = col < corner;
                const int in_right = col >= nw - corner;
                const int in_top = row < corner;
                const int in_bottom = row >= nh - corner;

                if ((in_left || in_right) && (in_top || in_bottom))
                {
                    const int cx = in_left ? corner : nw - 1 - corner;
                    const int cy = in_top ? corner : nh - 1 - corner;
                    const int dx = col - cx;
                    const int dy = row - cy;
                    inside = (dx * dx + dy * dy) <= corner * corner;
                }
            }
            s_alpha[r + row][r + col] = inside ? 255U : 0U;
        }
    }
}

/**
 * @brief Applies one horizontal box-blur pass to the active region of s_alpha.
 *
 * Pixels outside [0, sw) are treated as 0 (zero-padded boundary).  Results are
 * written back in-place via the s_blur_tmp accumulator so the pass is a true single
 * scan without read-after-write aliasing.
 *
 * @param[in] sw  Width of the active region in s_alpha (columns 0..sw-1).
 * @param[in] sh  Height of the active region in s_alpha (rows 0..sh-1).
 * @param[in] r   Box half-radius; the blur window width is 2r + 1.
 */
static void hblur(int sw, int sh, int r)
{
    const int win = 2 * r + 1;
    for (int row = 0; row < sh; row++)
    {
        uint8_t* const p = s_alpha[row];
        uint16_t sum = 0;

        /* Pre-load elements that are already inside the window at col = 0.
         * With zero-padding, elements at indices < 0 contribute 0 and are skipped. */
        for (int i = 0; i < r && i < sw; i++)
            sum += p[i];

        for (int col = 0; col < sw; col++)
        {
            const int right = col + r;
            sum += (right < sw) ? p[right] : 0U;
            s_blur_tmp[col] = sum;
            const int left = col - r;
            sum -= (left >= 0) ? p[left] : 0U;
        }
        for (int col = 0; col < sw; col++)
            p[col] = (uint8_t)(s_blur_tmp[col] / (uint16_t)win);
    }
}

/**
 * @brief Applies one vertical box-blur pass to the active region of s_alpha.
 *
 * Pixels outside [0, sh) are treated as 0 (zero-padded boundary).
 *
 * @param[in] sw  Width of the active region in s_alpha.
 * @param[in] sh  Height of the active region in s_alpha (rows 0..sh-1).
 * @param[in] r   Box half-radius; the blur window height is 2r + 1.
 */
static void vblur(int sw, int sh, int r)
{
    const int win = 2 * r + 1;
    for (int col = 0; col < sw; col++)
    {
        uint16_t sum = 0;

        for (int i = 0; i < r && i < sh; i++)
            sum += s_alpha[i][col];

        for (int row = 0; row < sh; row++)
        {
            const int below = row + r;
            sum += (below < sh) ? s_alpha[below][col] : 0U;
            s_blur_tmp[row] = sum;
            const int above = row - r;
            sum -= (above >= 0) ? s_alpha[above][col] : 0U;
        }
        for (int row = 0; row < sh; row++)
            s_alpha[row][col] = (uint8_t)(s_blur_tmp[row] / (uint16_t)win);
    }
}

#endif /* ERUI_SHADOWS */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_shadow_render(const ERViewProps* vp, int x, int y, int w, int h)
{
#if ERUI_SHADOWS
    /* Resolve shadow parameters from explicit props or from elevation. */
    uint32_t color;
    float offset_x, offset_y, opacity_f;
    int radius;

    if (vp->shadow_opacity > 0.0f)
    {
        color = vp->shadow_color ? vp->shadow_color : 0xFF000000U;
        offset_x = vp->shadow_offset_x;
        offset_y = vp->shadow_offset_y;
        opacity_f = vp->shadow_opacity > 1.0f ? 1.0f : vp->shadow_opacity;
        radius = (int)vp->shadow_radius;
    }
    else if (vp->elevation > 0)
    {
        /* Android-style elevation: synthesise a downward black shadow. */
        color = 0xFF000000U;
        offset_x = 0.0f;
        offset_y = (float)vp->elevation * 0.33f;
        opacity_f = 0.2f + (float)vp->elevation * 0.01f;
        if (opacity_f > 0.5f)
            opacity_f = 0.5f;
        radius = (int)vp->elevation;
    }
    else
    {
        return; /* No shadow requested. */
    }

    if (w <= 0 || h <= 0)
        return;

    /* Skip gracefully when the node alone already exceeds the scratch buffer. */
    if (w > ERUI_SCRATCH_W || h > ERUI_SCRATCH_H)
        return;

    /* Clamp blur radius so the padded shadow fits inside the scratch buffer. */
    int max_rx = (ERUI_SCRATCH_W - w) / 2;
    int max_ry = (ERUI_SCRATCH_H - h) / 2;
    if (radius > max_rx)
        radius = max_rx;
    if (radius > max_ry)
        radius = max_ry;
    if (radius < 0)
        radius = 0;

    const int sw = w + 2 * radius; /* active shadow buffer width  */
    const int sh = h + 2 * radius; /* active shadow buffer height */

    /* World-space top-left of the shadow (includes blur spread and offset). */
    const int sx = x + (int)offset_x - radius;
    const int sy = y + (int)offset_y - radius;

    /* Clear the active region. */
    for (int row = 0; row < sh; row++)
        memset(s_alpha[row], 0, (size_t)sw);

    /* Rasterise the node silhouette at position (radius, radius) in the buffer. */
    fill_shape(radius, w, h, (int)vp->border_radius);

    /* Two-pass separable box blur approximates a Gaussian soft shadow. */
    if (radius > 0)
    {
        hblur(sw, sh, radius);
        vblur(sw, sh, radius);
    }

    /* Build premultiplied ARGB pixels and blit one row at a time to avoid a
     * second large static buffer.  The shadow_color alpha multiplies with
     * shadow_opacity to produce the effective base alpha. */
    const uint8_t color_a = (uint8_t)((color >> 24) & 0xFFU);
    const uint8_t sc_r = (uint8_t)((color >> 16) & 0xFFU);
    const uint8_t sc_g = (uint8_t)((color >> 8) & 0xFFU);
    const uint8_t sc_b = (uint8_t)(color & 0xFFU);
    const uint32_t shad_a = (uint32_t)(opacity_f * 255.0f + 0.5f);
    const uint32_t base_a = shad_a * (uint32_t)color_a / 255U;

    const int stride = (int)((size_t)sw * sizeof(uint32_t));
    for (int row = 0; row < sh; row++)
    {
        for (int col = 0; col < sw; col++)
        {
            const uint32_t ea = (uint32_t)s_alpha[row][col] * base_a / 255U;
            const uint32_t pr = (uint32_t)sc_r * ea / 255U;
            const uint32_t pg = (uint32_t)sc_g * ea / 255U;
            const uint32_t pb = (uint32_t)sc_b * ea / 255U;
            s_row_buf[col] = (ea << 24U) | (pr << 16U) | (pg << 8U) | pb;
        }
        er_blit_blend(s_row_buf, stride, 255U, sx, sy + row, sw, 1);
    }
#else
    (void)vp;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#endif
}
