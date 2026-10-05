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
 - Colour ramp
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Ramp entries cap: one per pixel of the gradient line up to here (8 KB per render worker). */
#define GRAD_LUT_MAX 1024

/* The ramp is premultiplied with 8 fractional bits per channel, so the ordered dither below can spread
 * the sub-level remainder over neighbouring pixels; without it a dark gradient over 1000+ px shows as 8-bit
 * steps tens of pixels wide. Each entry packs a, r, g, b as 16-bit lanes of one word (a in the top lane),
 * so a pixel is one load, one add and a byte gather. */
static uint64_t s_grad_lut_pool[ERUI_RENDER_WORKERS][GRAD_LUT_MAX];

/** @brief 4x4 Bayer matrix; a threshold is (k * 16 + 8) / 256 of a level, so flat colours stay exact. */
static const uint8_t k_bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/** @brief Broadcasts a dither threshold to all four lanes of a ramp entry. */
#define GRAD_LANES 0x0001000100010001ULL

/**
 * @brief Premultiplies a straight-alpha ARGB8888 colour into float channels (alpha, r, g, b).
 *
 * @param[in]  sa   Straight-alpha ARGB8888 colour.
 * @param[out] out  Premultiplied channels in [0, 255].
 */
static void premul_f(uint32_t sa, float out[4])
{
    const float a = (float)((sa >> 24) & 0xFFu);
    out[0] = a;
    out[1] = (float)((sa >> 16) & 0xFFu) * a / 255.0f;
    out[2] = (float)((sa >> 8) & 0xFFu) * a / 255.0f;
    out[3] = (float)(sa & 0xFFu) * a / 255.0f;
}

/**
 * @brief Builds the calling worker's colour ramp for a View gradient.
 *
 * Stops are interpolated in premultiplied space, as CSS gradients are: a fade to `transparent` keeps the
 * colour it fades from instead of darkening through transparent black.
 *
 * @param[in] vp      View props (gradient_stop_count >= 2, stops ascending).
 * @param[in] extent  Length of the gradient line in pixels; sizes the ramp (1 entry per pixel, capped).
 *
 * @return Number of entries built (2..GRAD_LUT_MAX); entry i is the colour at t = i / (n - 1).
 */
static int build_lut(const ERViewProps* vp, float extent)
{
    int n = (int)extent + 1;
    if (n < 2)
        n = 2;
    if (n > GRAD_LUT_MAX)
        n = GRAD_LUT_MAX;

    uint64_t* lut = s_grad_lut_pool[er_render_worker_id()];
    const ERGradientStop* st = vp->gradient_stops;
    const int last = (int)vp->gradient_stop_count - 1;
    int seg = 0;
    for (int i = 0; i < n; i++)
    {
        const float t = (float)i / (float)(n - 1);
        float c[4];
        if (t <= st[0].position)
            premul_f(st[0].color, c);
        else if (t >= st[last].position)
            premul_f(st[last].color, c);
        else
        {
            while (t > st[seg + 1].position)
                seg++;
            float c0[4], c1[4];
            premul_f(st[seg].color, c0);
            premul_f(st[seg + 1].color, c1);
            const float span = st[seg + 1].position - st[seg].position;
            const float lt = (span > 0.0f) ? (t - st[seg].position) / span : 1.0f;
            for (int k = 0; k < 4; k++)
                c[k] = c0[k] + (c1[k] - c0[k]) * lt;
        }
        const uint64_t a = (uint64_t)(c[0] * 256.0f + 0.5f);
        uint64_t e = a << 48;
        for (int k = 1; k < 4; k++)
        {
            const uint64_t v = (uint64_t)(c[k] * 256.0f + 0.5f);
            /* Keep colour <= alpha so every dithered pixel is a valid premultiplied value. */
            e |= (v < a ? v : a) << (48 - 16 * k);
        }
        lut[i] = e;
    }
    return n;
}

/**
 * @brief Quantizes a dithered ramp entry (entry + threshold * GRAD_LANES) to premultiplied ARGB8888.
 *
 * Thresholds are in 1/256 of a level, from k_bayer4. The same threshold on every channel preserves
 * colour <= alpha, and no lane carries: each stays below 65536.
 *
 * @param[in] e  Ramp entry plus its broadcast threshold (8.8 fixed point per 16-bit lane).
 * @return Premultiplied ARGB8888 pixel.
 */
static inline uint32_t dither_px(uint64_t e)
{
    const uint64_t v = e >> 8;
    return (uint32_t)((v & 0xFFu) | ((v >> 8) & 0xFF00u) | ((v >> 16) & 0xFF0000u) | ((v >> 24) & 0xFF000000u));
}

/*----------------------------------------------------------------------------------------------------------------------
 - Damage window
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief The part of a gradient box the current clip lets through, in box coordinates.
 *
 * A small repaint over a full-screen gradient would otherwise rebuild every row and column of it only to
 * have er_blit_blend scissor them away.
 */
typedef struct
{
    int c0; /**< First column. */
    int c1; /**< One past the last column (never beyond the row buffer). */
    int r0; /**< First row. */
    int r1; /**< One past the last row. */
} GradWindow;

/**
 * @brief Intersects the box with the active clip rect.
 *
 * @param[out] win       Receives the visible window.
 * @param[in]  x         Box left edge in framebuffer pixels.
 * @param[in]  y         Box top edge in framebuffer pixels.
 * @param[in]  capped_w  Box width capped to the row buffer.
 * @param[in]  h         Box height.
 * @return False when nothing of the box is visible.
 */
static bool window_init(GradWindow* win, int x, int y, int capped_w, int h)
{
    win->c0 = 0;
    win->c1 = capped_w;
    win->r0 = 0;
    win->r1 = h;
    int cx, cy, cw, ch;
    if (er_get_clip_rect(&cx, &cy, &cw, &ch))
    {
        if (cx - x > win->c0)
            win->c0 = cx - x;
        if (cx + cw - x < win->c1)
            win->c1 = cx + cw - x;
        if (cy - y > win->r0)
            win->r0 = cy - y;
        if (cy + ch - y < win->r1)
            win->r1 = cy + ch - y;
    }
    return win->c0 < win->c1 && win->r0 < win->r1;
}

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
 * @brief Blits the windowed part of one assembled gradient row, clipped to the rounded-rect silhouette.
 *
 * Rows clear of the corner arcs blit whole; rows the arcs cut into blit only their covered span, then
 * fade the anti-aliased fringe pixels beside it (matching er_rrect_fill_corners' fringe walk, so the
 * gradient's corner is the same shape a solid background would have painted).
 *
 * @param[in] m        Silhouette to clip to.
 * @param[in] win      Visible window; only row[win->c0 .. win->c1) was assembled.
 * @param[in] row      Assembled premultiplied ARGB8888 row, indexed by box column.
 * @param[in] x        Destination left edge of the box in framebuffer pixels.
 * @param[in] y        Destination row in framebuffer pixels.
 * @param[in] row_idx  Row index within the box, 0 = top.
 */
static void mask_blit_row(const GradMask* m, const GradWindow* win, const uint32_t* row, int x, int y, int row_idx)
{
    if (!m->rounded)
    {
        er_blit_blend(
            &row[win->c0], (win->c1 - win->c0) * (int)sizeof(uint32_t), 255, x + win->c0, y, win->c1 - win->c0, 1);
        return;
    }

    ERRRectRow rr;
    er_rrect_row(m->w, m->h, m->r_tl, m->r_tr, m->r_br, m->r_bl, row_idx, &rr);

    const int x0 = (rr.x0 > win->c0) ? rr.x0 : win->c0;
    const int x1 = (rr.x1 < win->c1) ? rr.x1 : win->c1;
    if (x1 > x0)
        er_blit_blend(&row[x0], (x1 - x0) * (int)sizeof(uint32_t), 255, x + x0, y, x1 - x0, 1);

#if ERUI_BORDER_AA
    /* Fringe pixels take the gradient colour at their own column, faded by the arc's coverage. */
    for (int k = 0, kmax = er_rrect_fringe_max(rr.l_r); k < kmax; k++)
    {
        const float cov = er_rrect_fringe_cov(rr.l_r, rr.l_dx, rr.l_dy, k);
        if (cov <= 0.0f)
            break;
        const int ax = rr.x0 - 1 - k;
        if (cov < 1.0f && ax >= win->c0 && ax < win->c1)
        {
            const uint32_t p = er_px_scale_premul(row[ax], (uint32_t)(cov * 255.0f + 0.5f));
            er_blit_blend(&p, (int)sizeof(uint32_t), 255, x + ax, y, 1, 1);
        }
    }
    for (int k = 0, kmax = er_rrect_fringe_max(rr.r_r); k < kmax; k++)
    {
        const float cov = er_rrect_fringe_cov(rr.r_r, rr.r_dx, rr.r_dy, k);
        if (cov <= 0.0f)
            break;
        const int ax = rr.x1 + k;
        if (cov < 1.0f && ax >= win->c0 && ax < win->c1 && ax < m->w)
        {
            const uint32_t p = er_px_scale_premul(row[ax], (uint32_t)(cov * 255.0f + 0.5f));
            er_blit_blend(&p, (int)sizeof(uint32_t), 255, x + ax, y, 1, 1);
        }
    }
#endif
}

/**
 * @brief Assembles row[c0 .. c1) of a linear gradient from the ramp.
 *
 * @param[out] buf    Row buffer, indexed by box column.
 * @param[in]  lut    Ramp from build_lut.
 * @param[in]  n      Ramp entries.
 * @param[in]  f      Ramp index at column c0, 16.16 fixed point.
 * @param[in]  step   Ramp index change per column, 16.16 fixed point.
 * @param[in]  bayer  The row's k_bayer4 line.
 * @param[in]  x      Box left edge in framebuffer pixels (keys the dither column).
 * @param[in]  c0     First column.
 * @param[in]  c1     One past the last column.
 */
static void assemble_row(
    uint32_t* buf, const uint64_t* lut, int n, int32_t f, int32_t step, const uint8_t* bayer, int x, int c0, int c1)
{
    /* The dither threshold cycles every 4 columns: hoist the four lane-broadcast thresholds. */
    uint64_t thr[4];
    for (int k = 0; k < 4; k++)
        thr[k] = ((uint64_t)bayer[(x + c0 + k) & 3] * 16u + 8u) * GRAD_LANES;

    /* A ramp that runs purely along y is one colour per row: only the 4-pixel dither cycle varies. */
    const int end = (step == 0 && c1 - c0 > 4) ? c0 + 4 : c1;
    for (int col = c0; col < end; col++, f += step)
    {
        int idx = (f + 32768) >> 16;
        if (idx < 0)
            idx = 0;
        else if (idx >= n)
            idx = n - 1;
        buf[col] = dither_px(lut[idx] + thr[(col - c0) & 3]);
    }
    for (int col = end; col < c1; col++)
        buf[col] = buf[col - 4];
}

/**
 * @brief Resolves a linear gradient's screen-space unit direction (y down).
 *
 * gradient_angle is degrees clockwise from the top with 0° running top→bottom and 90° left→right (CSS's
 * `<angle>` is this plus 180°). A gradient_corner instead aims the line as CSS `to <corner>` does:
 * perpendicular to the diagonal joining the two neighbouring corners, which depends on the box's aspect.
 *
 * @param[in]  vp   View props (gradient_corner, gradient_angle).
 * @param[in]  w    Box width in pixels.
 * @param[in]  h    Box height in pixels.
 * @param[out] ddx  Direction x component.
 * @param[out] ddy  Direction y component.
 */
static void linear_direction(const ERViewProps* vp, int w, int h, float* ddx, float* ddy)
{
    const float len = sqrtf((float)w * (float)w + (float)h * (float)h);
    const float sx = (float)h / len, sy = (float)w / len;
    switch (vp->gradient_corner)
    {
        case ER_GRADIENT_CORNER_TOP_RIGHT:
            *ddx = sx;
            *ddy = -sy;
            return;
        case ER_GRADIENT_CORNER_BOTTOM_RIGHT:
            *ddx = sx;
            *ddy = sy;
            return;
        case ER_GRADIENT_CORNER_BOTTOM_LEFT:
            *ddx = -sx;
            *ddy = sy;
            return;
        case ER_GRADIENT_CORNER_TOP_LEFT:
            *ddx = -sx;
            *ddy = -sy;
            return;
        default:
            /* Snapped so a side direction is exactly axis-aligned (cosf(90°) is not 0): render_linear
             * reuses rows only when the ramp runs purely along x. */
            *ddx = sinf(vp->gradient_angle * ER_DEG2RAD);
            *ddy = cosf(vp->gradient_angle * ER_DEG2RAD);
            if (fabsf(*ddx) < 1e-6f)
                *ddx = 0.0f;
            if (fabsf(*ddy) < 1e-6f)
                *ddy = 0.0f;
            return;
    }
}

/**
 * @brief Renders a linear gradient into a rectangular framebuffer region.
 *
 * The four corner projections onto the direction vector normalise the per-pixel parameter t to
 * [0.0–1.0] regardless of angle — the same gradient line length CSS uses — with the first and last pixel
 * centres landing exactly on t = 0 and t = 1.  Only the rows and columns inside the active clip are
 * assembled; each row is flushed via er_blit_blend (premultiplied).
 *
 * @param[in] vp  View props (gradient_corner, gradient_angle, gradient_stop_count, gradient_stops).
 * @param[in] x   Destination left edge in framebuffer pixels.
 * @param[in] y   Destination top edge in framebuffer pixels.
 * @param[in] w   Destination width in pixels.
 * @param[in] h   Destination height in pixels.
 */
static void render_linear(const ERViewProps* vp, int x, int y, int w, int h)
{
    if (vp->gradient_stop_count < 2 || w <= 0 || h <= 0)
        return;

    const int capped_w = (w <= ERUI_MAX_IMG_ROW_PIXELS) ? w : ERUI_MAX_IMG_ROW_PIXELS;
    GradWindow win;
    if (!window_init(&win, x, y, capped_w, h))
        return;

    GradMask mask;
    mask_init(&mask, vp, w, h);

    float ddx, ddy;
    linear_direction(vp, w, h, &ddx, &ddy);

    /* Project all four corners onto the direction vector to find the full extent. */
    const float wf = (float)(w - 1);
    const float hf = (float)(h - 1);
    const float p10 = wf * ddx;
    const float p01 = hf * ddy;
    const float p11 = wf * ddx + hf * ddy;
    const float min_p = fminf(fminf(0.0f, p10), fminf(p01, p11));
    const float max_p = fmaxf(fmaxf(0.0f, p10), fmaxf(p01, p11));

    const float span = max_p - min_p;
    const int n = build_lut(vp, span);
    const uint64_t* lut = s_grad_lut_pool[er_render_worker_id()];
    /* Ramp index per pixel step, in 16.16 fixed point. */
    const float to_idx = (span > 1e-6f) ? (float)(n - 1) / span : 0.0f;
    const int32_t step = (int32_t)lrintf(ddx * to_idx * 65536.0f);

    uint32_t* buf = grow();
    if (ddy == 0.0f)
    {
        /* A ramp that runs purely along x repeats every 4 rows (the dither cycle): assemble each phase
         * once and blit it to all of its rows. The blits never overlap, so their order does not matter. */
        const int32_t f = (int32_t)lrintf(((float)win.c0 * ddx - min_p) * to_idx * 65536.0f);
        for (int first = win.r0; first < win.r0 + 4 && first < win.r1; first++)
        {
            assemble_row(buf, lut, n, f, step, k_bayer4[(y + first) & 3], x, win.c0, win.c1);
            for (int row = first; row < win.r1; row += 4)
                mask_blit_row(&mask, &win, buf, x, y + row, row);
        }
        return;
    }
    for (int row = win.r0; row < win.r1; row++)
    {
        const int32_t f = (int32_t)lrintf(((float)win.c0 * ddx + (float)row * ddy - min_p) * to_idx * 65536.0f);
        assemble_row(buf, lut, n, f, step, k_bayer4[(y + row) & 3], x, win.c0, win.c1);
        mask_blit_row(&mask, &win, buf, x, y + row, row);
    }
}

#if ERUI_GRADIENT_RADIAL

/**
 * @brief Renders a radial gradient into a rectangular framebuffer region.
 *
 * A circle centred on the rectangle whose radius reaches the farthest corner pixel — CSS
 * `radial-gradient(circle farthest-corner at center, …)` — so the full stop range is always visible
 * inside the rect.  Only the rows and columns inside the active clip are assembled; each row is
 * flushed via er_blit_blend (premultiplied).
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

    const int capped_w = (w <= ERUI_MAX_IMG_ROW_PIXELS) ? w : ERUI_MAX_IMG_ROW_PIXELS;
    GradWindow win;
    if (!window_init(&win, x, y, capped_w, h))
        return;

    GradMask mask;
    mask_init(&mask, vp, w, h);

    const float cx = (float)(w - 1) * 0.5f;
    const float cy = (float)(h - 1) * 0.5f;
    const float r = sqrtf(cx * cx + cy * cy);
    const int n = build_lut(vp, r);
    const uint64_t* lut = s_grad_lut_pool[er_render_worker_id()];
    const float to_idx = (r > 1e-6f) ? (float)(n - 1) / r : 0.0f;

    uint32_t* buf = grow();
    for (int row = win.r0; row < win.r1; row++)
    {
        const uint8_t* bayer = k_bayer4[(y + row) & 3];
        const float dy2 = ((float)row - cy) * ((float)row - cy);
        for (int col = win.c0; col < win.c1; col++)
        {
            const float dx = (float)col - cx;
            int idx = (int)(sqrtf(dx * dx + dy2) * to_idx + 0.5f);
            if (idx >= n)
                idx = n - 1;
            buf[col] = dither_px(lut[idx] + ((uint64_t)bayer[(x + col) & 3] * 16u + 8u) * GRAD_LANES);
        }
        mask_blit_row(&mask, &win, buf, x, y + row, row);
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
