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
#include "renderer_internal.h"
#include "rrect.h" /* rounded-corner geometry, shared so the mask matches the fill pixel for pixel */
#include <math.h>

#ifndef ERUI_MAX_IMG_ROW_PIXELS
#define ERUI_MAX_IMG_ROW_PIXELS 800
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define GRAD_PI 3.14159265358979323846f

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Single-row premultiplied ARGB8888 scratch buffer used during gradient rasterization. */
/* One assembled row per render worker: cheap enough to duplicate, and it keeps gradient
 * backgrounds parallel-safe (see the multi-core render fork in compositor.c). */
static uint32_t s_grad_row_pool[ERUI_RENDER_WORKERS][ERUI_MAX_IMG_ROW_PIXELS];

/** @brief The calling worker's gradient row buffer. */
static inline uint32_t* grow(void)
{
    return s_grad_row_pool[er_render_worker_id()];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Linearly interpolates between two straight-alpha ARGB8888 colors.
 *
 * @param[in] c0  Color at t = 0.0.
 * @param[in] c1  Color at t = 1.0.
 * @param[in] t   Blend factor [0.0–1.0].
 *
 * @return Interpolated straight-alpha ARGB8888 color.
 */
static uint32_t lerp_argb(uint32_t c0, uint32_t c1, float t)
{
    const int a0 = (int)((c0 >> 24) & 0xFFu), a1 = (int)((c1 >> 24) & 0xFFu);
    const int r0 = (int)((c0 >> 16) & 0xFFu), r1 = (int)((c1 >> 16) & 0xFFu);
    const int g0 = (int)((c0 >> 8) & 0xFFu), g1 = (int)((c1 >> 8) & 0xFFu);
    const int b0 = (int)(c0 & 0xFFu), b1 = (int)(c1 & 0xFFu);
    const uint32_t a = (uint32_t)(a0 + (int)((float)(a1 - a0) * t));
    const uint32_t r = (uint32_t)(r0 + (int)((float)(r1 - r0) * t));
    const uint32_t g = (uint32_t)(g0 + (int)((float)(g1 - g0) * t));
    const uint32_t b = (uint32_t)(b0 + (int)((float)(b1 - b0) * t));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/**
 * @brief Evaluates the gradient color at a scalar parameter t.
 *
 * Finds the pair of stops that brackets t and lerps between them. Returns the first stop
 * color for t below stops[0].position and the last stop color for t above the final position.
 *
 * @param[in] stops  Color stop array sorted by ascending position.
 * @param[in] count  Number of entries in stops.
 * @param[in] t      Gradient parameter [0.0–1.0].
 *
 * @return Straight-alpha ARGB8888 color at position t.
 */
uint32_t er_gradient_eval_stops(const ERGradientStop* stops, int count, float t)
{
    if (count <= 0)
        return 0u;
    if (t <= stops[0].position || count == 1)
        return stops[0].color;
    if (t >= stops[count - 1].position)
        return stops[count - 1].color;
    for (int i = 0; i < count - 1; i++)
    {
        if (t <= stops[i + 1].position)
        {
            const float span = stops[i + 1].position - stops[i].position;
            const float lt = (span > 0.0f) ? (t - stops[i].position) / span : 0.0f;
            return lerp_argb(stops[i].color, stops[i + 1].color, lt);
        }
    }
    return stops[count - 1].color;
}

/**
 * @brief Converts a straight-alpha ARGB8888 color to premultiplied ARGB8888.
 *
 * Required before writing pixels into buffers consumed by er_blit_blend().
 *
 * @param[in] sa  Straight-alpha ARGB8888 color.
 *
 * @return Premultiplied ARGB8888 equivalent.
 */
uint32_t er_gradient_premul(uint32_t sa)
{
    const uint32_t a = (sa >> 24) & 0xFFu;
    const uint32_t r = (((sa >> 16) & 0xFFu) * a) / 255u;
    const uint32_t g = (((sa >> 8) & 0xFFu) * a) / 255u;
    const uint32_t b = ((sa & 0xFFu) * a) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
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
    int w;        /**< Box width — the coordinate space of the row spans. */
    int h;        /**< Box height. */
    int r_tl;     /**< Clamped corner radii. */
    int r_tr;     /**< @see r_tl */
    int r_br;     /**< @see r_tl */
    int r_bl;     /**< @see r_tl */
    bool rounded; /**< False when every radius is 0: rows blit whole, at no extra cost. */
} GradMask;

/**
 * @brief Resolves a node's corner radii into the mask its gradient rows are clipped to.
 *
 * @param[out] m   Receives the silhouette.
 * @param[in]  vp  View props carrying border_radius and the per-corner overrides.
 * @param[in]  w   Box width in pixels.
 * @param[in]  h   Box height in pixels.
 */
static void mask_init(GradMask* m, const ERViewProps* vp, int w, int h)
{
    /* Per-corner radius, falling back to the uniform one — the same resolution render_view_bg uses. */
    m->r_tl = vp->border_tl_radius > 0 ? (int)vp->border_tl_radius : (int)vp->border_radius;
    m->r_tr = vp->border_tr_radius > 0 ? (int)vp->border_tr_radius : (int)vp->border_radius;
    m->r_br = vp->border_br_radius > 0 ? (int)vp->border_br_radius : (int)vp->border_radius;
    m->r_bl = vp->border_bl_radius > 0 ? (int)vp->border_bl_radius : (int)vp->border_radius;
    er_rrect_clamp_radii(w, h, &m->r_tl, &m->r_tr, &m->r_br, &m->r_bl);
    m->w = w;
    m->h = h;
    m->rounded = (m->r_tl > 0 || m->r_tr > 0 || m->r_br > 0 || m->r_bl > 0);
}

/**
 * @brief Scales a premultiplied ARGB8888 pixel by an anti-aliasing coverage byte.
 *
 * Every channel scales, not just alpha: the value is premultiplied, so leaving the colour channels
 * alone would brighten the fringe instead of fading it.
 *
 * @param[in] p    Premultiplied ARGB8888 pixel.
 * @param[in] cov  Coverage in [0, 255].
 *
 * @return The pixel scaled by cov/255, still premultiplied.
 */
static uint32_t premul_scale(uint32_t p, uint32_t cov)
{
    const uint32_t a = (((p >> 24) & 0xFFu) * cov + 127u) / 255u;
    const uint32_t r = (((p >> 16) & 0xFFu) * cov + 127u) / 255u;
    const uint32_t g = (((p >> 8) & 0xFFu) * cov + 127u) / 255u;
    const uint32_t b = ((p & 0xFFu) * cov + 127u) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/**
 * @brief Blits one assembled gradient row, clipped to the rounded-rect silhouette.
 *
 * Rows clear of the corner arcs blit whole; rows the arcs cut into blit only their covered span, then
 * fade the anti-aliased fringe pixels beside it (matching er_rrect_fill_corners' fringe walk, so the
 * gradient's corner is the same shape a solid background would have painted).
 *
 * @param[in] m         Silhouette to clip to.
 * @param[in] row       Assembled premultiplied ARGB8888 row, @p capped_w pixels wide.
 * @param[in] x         Destination left edge in framebuffer pixels.
 * @param[in] y         Destination row in framebuffer pixels.
 * @param[in] row_idx   Row index within the box, 0 = top.
 * @param[in] capped_w  Assembled width (may be less than m->w on very wide boxes).
 */
static void mask_blit_row(const GradMask* m, const uint32_t* row, int x, int y, int row_idx, int capped_w)
{
    if (!m->rounded)
    {
        er_blit_blend(row, capped_w * (int)sizeof(uint32_t), 255, x, y, capped_w, 1);
        return;
    }

    ERRRectRow rr;
    er_rrect_row(m->w, m->h, m->r_tl, m->r_tr, m->r_br, m->r_bl, row_idx, &rr);

    const int x1 = (rr.x1 < capped_w) ? rr.x1 : capped_w;
    if (x1 > rr.x0)
        er_blit_blend(&row[rr.x0], (x1 - rr.x0) * (int)sizeof(uint32_t), 255, x + rr.x0, y, x1 - rr.x0, 1);

#if ERUI_BORDER_AA
    /* Fringe pixels take the gradient colour at their own column, faded by the arc's coverage. */
    if (rr.l_r > 0)
    {
        for (int k = 0;; k++)
        {
            const float cov = er_rrect_fringe_cov(rr.l_r, rr.l_dx, rr.l_dy, k);
            if (cov <= 0.0f)
                break;
            const int ax = rr.x0 - 1 - k;
            if (cov < 1.0f && ax >= 0 && ax < capped_w)
            {
                const uint32_t p = premul_scale(row[ax], (uint32_t)(cov * 255.0f + 0.5f));
                er_blit_blend(&p, (int)sizeof(uint32_t), 255, x + ax, y, 1, 1);
            }
        }
    }
    if (rr.r_r > 0)
    {
        for (int k = 0;; k++)
        {
            const float cov = er_rrect_fringe_cov(rr.r_r, rr.r_dx, rr.r_dy, k);
            if (cov <= 0.0f)
                break;
            const int ax = rr.x1 + k;
            if (cov < 1.0f && ax >= 0 && ax < capped_w && ax < m->w)
            {
                const uint32_t p = premul_scale(row[ax], (uint32_t)(cov * 255.0f + 0.5f));
                er_blit_blend(&p, (int)sizeof(uint32_t), 255, x + ax, y, 1, 1);
            }
        }
    }
#endif
}

/**
 * @brief Renders a linear gradient into a rectangular framebuffer region.
 *
 * The gradient angle is measured in degrees clockwise from the top: 0° runs top→bottom,
 * 90° runs left→right.  The four corner projections onto the gradient direction vector are
 * used to normalise the per-pixel parameter t to [0.0–1.0] regardless of angle.
 *
 * Each row is computed into grow() and flushed via er_blit_blend (premultiplied).
 *
 * @param[in] vp  View props (gradient_angle, gradient_stop_count, gradient_stops).
 * @param[in] x   Destination left edge in framebuffer pixels.
 * @param[in] y   Destination top edge in framebuffer pixels.
 * @param[in] w   Destination width in pixels.
 * @param[in] h   Destination height in pixels.
 */
static void render_linear(const ERViewProps* vp, int x, int y, int w, int h)
{
    if (vp->gradient_stop_count < 2 || w <= 0 || h <= 0)
        return;

    GradMask mask;
    mask_init(&mask, vp, w, h);

    const float angle_rad = vp->gradient_angle * (GRAD_PI / 180.0f);
    const float ddx = sinf(angle_rad);
    const float ddy = cosf(angle_rad);

    /* Project all four corners onto the direction vector to find the full extent. */
    const float wf = (float)(w - 1);
    const float hf = (float)(h - 1);
    const float p00 = 0.0f;
    const float p10 = wf * ddx;
    const float p01 = hf * ddy;
    const float p11 = wf * ddx + hf * ddy;
    float min_p = p00, max_p = p00;
    if (p10 < min_p)
        min_p = p10;
    if (p10 > max_p)
        max_p = p10;
    if (p01 < min_p)
        min_p = p01;
    if (p01 > max_p)
        max_p = p01;
    if (p11 < min_p)
        min_p = p11;
    if (p11 > max_p)
        max_p = p11;

    const float span = max_p - min_p;
    const float inv_span = (span > 1e-6f) ? 1.0f / span : 0.0f;
    const int capped_w = (w <= ERUI_MAX_IMG_ROW_PIXELS) ? w : ERUI_MAX_IMG_ROW_PIXELS;

    for (int row = 0; row < h; row++)
    {
        const float row_base = (float)row * ddy;
        for (int col = 0; col < capped_w; col++)
        {
            float t = ((float)col * ddx + row_base - min_p) * inv_span;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            grow()[col] =
                er_gradient_premul(er_gradient_eval_stops(vp->gradient_stops, (int)vp->gradient_stop_count, t));
        }
        mask_blit_row(&mask, grow(), x, y + row, row, capped_w);
    }
}

#if ERUI_GRADIENT_RADIAL

/**
 * @brief Renders a radial gradient into a rectangular framebuffer region.
 *
 * The gradient origin is the rectangle centre.  The radius is the Euclidean distance from
 * the centre to the farthest corner, so t = 1.0 is reached at exactly the corner pixels and
 * the full stop range is always visible inside the rect.
 *
 * Each row is computed into grow() and flushed via er_blit_blend (premultiplied).
 *
 * @param[in] vp  View props (gradient_stop_count, gradient_stops).
 * @param[in] x   Destination left edge in framebuffer pixels.
 * @param[in] y   Destination top edge in framebuffer pixels.
 * @param[in] w   Destination width in pixels.
 * @param[in] h   Destination height in pixels.
 */
static void render_radial(const ERViewProps* vp, int x, int y, int w, int h)
{
    if (vp->gradient_stop_count < 2 || w <= 0 || h <= 0)
        return;

    GradMask mask;
    mask_init(&mask, vp, w, h);

    const float cx = (float)(w - 1) * 0.5f;
    const float cy = (float)(h - 1) * 0.5f;
    const float r = sqrtf(cx * cx + cy * cy);
    const float inv_r = (r > 1e-6f) ? 1.0f / r : 0.0f;
    const int capped_w = (w <= ERUI_MAX_IMG_ROW_PIXELS) ? w : ERUI_MAX_IMG_ROW_PIXELS;

    for (int row = 0; row < h; row++)
    {
        const float dy2 = ((float)row - cy) * ((float)row - cy);
        for (int col = 0; col < capped_w; col++)
        {
            const float dx2 = ((float)col - cx) * ((float)col - cx);
            float t = sqrtf(dx2 + dy2) * inv_r;
            if (t > 1.0f)
                t = 1.0f;
            grow()[col] =
                er_gradient_premul(er_gradient_eval_stops(vp->gradient_stops, (int)vp->gradient_stop_count, t));
        }
        mask_blit_row(&mask, grow(), x, y + row, row, capped_w);
    }
}

#endif /* ERUI_GRADIENT_RADIAL */

#endif /* ERUI_GRADIENT */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_gradient_render(const ERViewProps* vp, int x, int y, int w, int h)
{
#if ERUI_GRADIENT
    if (!vp || vp->gradient_type == ER_GRADIENT_NONE)
        return;
    if (vp->gradient_type == ER_GRADIENT_LINEAR)
    {
        render_linear(vp, x, y, w, h);
        return;
    }
#if ERUI_GRADIENT_RADIAL
    if (vp->gradient_type == ER_GRADIENT_RADIAL)
        render_radial(vp, x, y, w, h);
#endif
#else
    (void)vp;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#endif
}
