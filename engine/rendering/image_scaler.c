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

#include "image_scaler.h"
#include "er_limits.h"
#include "image_registry.h"
#include "renderer_internal.h"
#include "rrect.h"
#include <math.h>
#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief The rounded-rect silhouette an image's rows are clipped to.
 *
 * The node's border box with its radii clamped exactly as a background fill clamps them, so the bitmap
 * stops at the edge a background_color with the same radii would have.
 */
typedef struct
{
    int x;        /**< Box left edge in framebuffer pixels. */
    int y;        /**< Box top edge in framebuffer pixels. */
    int w;        /**< Box width — the coordinate space of the row spans. */
    int h;        /**< Box height. */
    int r_tl;     /**< Clamped corner radii. */
    int r_tr;     /**< @see r_tl */
    int r_br;     /**< @see r_tl */
    int r_bl;     /**< @see r_tl */
    bool rounded; /**< False when every radius is 0 or there is no box: rows blit whole, as before. */
} ImgMask;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A mask that clips nothing, for the paths that emit rows the mask has already let through whole. */
static const ImgMask k_square_mask = {0, 0, 0, 0, 0, 0, 0, 0, false};

/** @brief Single-row scratch buffer used when nearest-neighbor scaling is required. */
/* One assembled row per render worker: cheap enough to duplicate, and it keeps image
 * blits parallel-safe (see the multi-core render fork in compositor.c). */
static uint32_t s_row_buf_pool[ERUI_RENDER_WORKERS][ERUI_MAX_IMG_ROW_PIXELS];

/** @brief The calling worker's image row buffer. */
static inline uint32_t* irow(void)
{
    return s_row_buf_pool[er_render_worker_id()];
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Expands one RGB565 pixel to opaque premultiplied ARGB8888.
 *
 * Bit-replicates the 5/6/5 channels into 8 bits (the standard lossless-round-trip expansion),
 * so pure white/black stay exactly 0xFF/0x00.
 */
static inline uint32_t rgb565_to_argb(uint16_t p)
{
    const uint32_t r5 = (p >> 11) & 0x1Fu;
    const uint32_t g6 = (p >> 5) & 0x3Fu;
    const uint32_t b5 = p & 0x1Fu;
    const uint32_t r = (r5 << 3) | (r5 >> 2);
    const uint32_t g = (g6 << 2) | (g6 >> 4);
    const uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/**
 * @brief Reads one source pixel as premultiplied ARGB8888, whatever the image's storage format.
 */
static inline uint32_t fetch_px(const ImageEntry* img, int x, int y)
{
    const size_t idx = (size_t)y * (size_t)img->w + (size_t)x;
    if (img->format == ER_IMG_RGB565)
        return rgb565_to_argb(((const uint16_t*)img->buf)[idx]);
    return ((const uint32_t*)img->buf)[idx];
}

/**
 * @brief Applies a tint to a premultiplied ARGB8888 pixel.
 *
 * Preserves the original alpha. Replaces the RGB channels with the tint color
 * pre-multiplied by the original alpha, which is the correct behaviour for icon tinting
 * (transparent pixels remain transparent; opaque pixels become the tint color).
 *
 * @param[in] pixel  Source premultiplied ARGB8888 pixel.
 * @param[in] tr     Tint red channel (0–255 straight).
 * @param[in] tg     Tint green channel (0–255 straight).
 * @param[in] tb     Tint blue channel (0–255 straight).
 *
 * @return Tinted premultiplied ARGB8888 pixel.
 */
static uint32_t apply_tint(uint32_t pixel, uint8_t tr, uint8_t tg, uint8_t tb)
{
    const uint32_t a = (pixel >> 24) & 0xFFu;
    const uint32_t r = ((uint32_t)tr * a) / 255u;
    const uint32_t g = ((uint32_t)tg * a) / 255u;
    const uint32_t b = ((uint32_t)tb * a) / 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

#if ERUI_BILINEAR_SCALE

/**
 * @brief Samples a premultiplied ARGB8888 source image at fractional pixel coordinates.
 *
 * Clamps to the source crop rectangle [src_x, src_x+src_w) × [src_y, src_y+src_h) before
 * computing the four-tap bilinear weights. Bilinear interpolation on premultiplied values is
 * mathematically exact (no alpha halo artefacts).
 *
 * @param[in] img    Source image entry (any storage format; taps are fetched as ARGB8888).
 * @param[in] src_x  Left edge of the valid crop region.
 * @param[in] src_y  Top edge of the valid crop region.
 * @param[in] src_w  Width of the valid crop region.
 * @param[in] src_h  Height of the valid crop region.
 * @param[in] sx_f   Fractional source X coordinate.
 * @param[in] sy_f   Fractional source Y coordinate.
 *
 * @return Bilinearly sampled premultiplied ARGB8888 pixel.
 */
static uint32_t
bilinear_sample(const ImageEntry* img, int src_x, int src_y, int src_w, int src_h, float sx_f, float sy_f)
{
    /* Clamp to crop bounds. */
    const float x_min = (float)src_x, x_max = (float)(src_x + src_w - 1);
    const float y_min = (float)src_y, y_max = (float)(src_y + src_h - 1);
    if (sx_f < x_min)
        sx_f = x_min;
    if (sx_f > x_max)
        sx_f = x_max;
    if (sy_f < y_min)
        sy_f = y_min;
    if (sy_f > y_max)
        sy_f = y_max;

    const int x0 = (int)sx_f, y0 = (int)sy_f;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 > src_x + src_w - 1)
        x1 = src_x + src_w - 1;
    if (y1 > src_y + src_h - 1)
        y1 = src_y + src_h - 1;

    const float tx = sx_f - (float)x0;
    const float ty = sy_f - (float)y0;
    const float w00 = (1.0f - tx) * (1.0f - ty);
    const float w10 = tx * (1.0f - ty);
    const float w01 = (1.0f - tx) * ty;
    const float w11 = tx * ty;

    const uint32_t p00 = fetch_px(img, x0, y0);
    const uint32_t p10 = fetch_px(img, x1, y0);
    const uint32_t p01 = fetch_px(img, x0, y1);
    const uint32_t p11 = fetch_px(img, x1, y1);

    const uint32_t a = (uint32_t)(((p00 >> 24) & 0xFFu) * w00 + ((p10 >> 24) & 0xFFu) * w10
                                  + ((p01 >> 24) & 0xFFu) * w01 + ((p11 >> 24) & 0xFFu) * w11);
    const uint32_t r = (uint32_t)(((p00 >> 16) & 0xFFu) * w00 + ((p10 >> 16) & 0xFFu) * w10
                                  + ((p01 >> 16) & 0xFFu) * w01 + ((p11 >> 16) & 0xFFu) * w11);
    const uint32_t g = (uint32_t)(((p00 >> 8) & 0xFFu) * w00 + ((p10 >> 8) & 0xFFu) * w10 + ((p01 >> 8) & 0xFFu) * w01
                                  + ((p11 >> 8) & 0xFFu) * w11);
    const uint32_t b =
        (uint32_t)((p00 & 0xFFu) * w00 + (p10 & 0xFFu) * w10 + (p01 & 0xFFu) * w01 + (p11 & 0xFFu) * w11);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

#endif /* ERUI_BILINEAR_SCALE */

/**
 * @brief Narrows the destination [0, w) x [0, h) placed at (x, y) to the part inside the active scissor.
 *
 * Every blit is clipped to the scissor anyway; producing only this part keeps a repaint that touches a
 * corner of a large image from converting or re-scaling all of it.
 *
 * @param[in]  x   Destination left edge in framebuffer pixels.
 * @param[in]  y   Destination top edge in framebuffer pixels.
 * @param[in]  w   Destination width in pixels.
 * @param[in]  h   Destination height in pixels.
 * @param[out] x0  First visible column, relative to x.
 * @param[out] y0  First visible row, relative to y.
 * @param[out] x1  One past the last visible column, relative to x.
 * @param[out] y1  One past the last visible row, relative to y.
 *
 * @return false when nothing of the destination is inside the scissor.
 */
static bool visible_part(int x, int y, int w, int h, int* x0, int* y0, int* x1, int* y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = w;
    *y1 = h;
    int cx, cy, cw, ch;
    if (er_get_clip_rect(&cx, &cy, &cw, &ch))
    {
        if (cx - x > *x0)
            *x0 = cx - x;
        if (cy - y > *y0)
            *y0 = cy - y;
        if (cx + cw - x < *x1)
            *x1 = cx + cw - x;
        if (cy + ch - y < *y1)
            *y1 = cy + ch - y;
    }
    return *x0 < *x1 && *y0 < *y1;
}

/**
 * @brief Resolves an Image node's corner radii into the mask its rows are clipped to.
 *
 * @param[out] m      Receives the silhouette.
 * @param[in]  props  Image props carrying the resolved per-corner radii.
 * @param[in]  box    The node's border box; NULL leaves the mask square.
 */
static void mask_init(ImgMask* m, const ERImageProps* props, const ERRect* box)
{
    *m = k_square_mask;
    if (!box)
        return;
    m->x = box->x;
    m->y = box->y;
    m->w = box->w;
    m->h = box->h;
    m->r_tl = props->border_tl_radius;
    m->r_tr = props->border_tr_radius;
    m->r_br = props->border_br_radius;
    m->r_bl = props->border_bl_radius;
    er_rrect_clamp_radii(m->w, m->h, &m->r_tl, &m->r_tr, &m->r_br, &m->r_bl);
    m->rounded = (m->r_tl > 0 || m->r_tr > 0 || m->r_br > 0 || m->r_bl > 0);
}

/**
 * @brief Finds the destination rows no corner arc cuts into, which can go out unclipped.
 *
 * @param[in]  m       Silhouette the rows are clipped to.
 * @param[in]  dst_y   Destination top edge in framebuffer pixels.
 * @param[in]  dst_h   Destination height in pixels.
 * @param[out] band0   First straight row, relative to dst_y.
 * @param[out] band1   One past the last straight row, relative to dst_y; equal to band0 when there is none.
 */
static void mask_straight_rows(const ImgMask* m, int dst_y, int dst_h, int* band0, int* band1)
{
    *band0 = 0;
    *band1 = dst_h;
    if (!m->rounded)
        return;
    const int top = (m->r_tl > m->r_tr) ? m->r_tl : m->r_tr;
    const int bottom = (m->r_bl > m->r_br) ? m->r_bl : m->r_br;
    if (m->y + top - dst_y > *band0)
        *band0 = m->y + top - dst_y;
    if (m->y + m->h - bottom - dst_y < *band1)
        *band1 = m->y + m->h - bottom - dst_y;
    if (*band1 < *band0)
        *band1 = *band0;
}

/**
 * @brief Emits a run of premultiplied pixels, copied when the image is opaque and blended otherwise.
 *
 * @param[in] px      Premultiplied ARGB8888 pixels.
 * @param[in] opaque  Whether every pixel's alpha is 0xFF.
 * @param[in] x       Destination left edge in framebuffer pixels.
 * @param[in] y       Destination row in framebuffer pixels.
 * @param[in] n       Pixel count.
 */
static void emit_span(const uint32_t* px, bool opaque, int x, int y, int n)
{
    if (opaque)
        er_blit_copy(px, n * (int)sizeof(uint32_t), x, y, n, 1);
    else
        er_blit_blend(px, n * (int)sizeof(uint32_t), 255, x, y, n, 1);
}

/**
 * @brief Emits one destination row of image pixels, clipped to the rounded-rect silhouette.
 *
 * A square mask emits the row whole. Otherwise only the row's covered span goes out, then the
 * anti-aliased fringe pixels beside it, faded by the arc's coverage (the walk er_rrect_fill_corners
 * fills a background's corner with, so the image's corner is the same shape).
 *
 * @param[in] m       Silhouette to clip to.
 * @param[in] row     Premultiplied ARGB8888 pixels for destination columns [x, x + n).
 * @param[in] opaque  Whether every pixel in @p row has alpha 0xFF.
 * @param[in] x       Destination left edge of @p row in framebuffer pixels.
 * @param[in] y       Destination row in framebuffer pixels.
 * @param[in] n       Pixel count.
 */
static void emit_row(const ImgMask* m, const uint32_t* row, bool opaque, int x, int y, int n)
{
    if (n <= 0)
        return;
    if (!m->rounded)
    {
        emit_span(row, opaque, x, y, n);
        return;
    }
    if (y < m->y || y >= m->y + m->h)
        return;

    ERRRectRow rr;
    er_rrect_row(m->w, m->h, m->r_tl, m->r_tr, m->r_br, m->r_bl, y - m->y, &rr);

    int s0 = m->x + rr.x0;
    int s1 = m->x + rr.x1;
    if (s0 < x)
        s0 = x;
    if (s1 > x + n)
        s1 = x + n;
    if (s1 > s0)
        emit_span(&row[s0 - x], opaque, s0, y, s1 - s0);

#if ERUI_BORDER_AA
    for (int k = 0, kmax = er_rrect_fringe_max(rr.l_r); k < kmax; k++)
    {
        const float cov = er_rrect_fringe_cov(rr.l_r, rr.l_dx, rr.l_dy, k);
        if (cov <= 0.0f)
            break;
        const int ax = m->x + rr.x0 - 1 - k;
        if (cov < 1.0f && ax >= m->x && ax >= x && ax < x + n)
        {
            const uint32_t p = er_px_scale_premul(row[ax - x], (uint32_t)(cov * 255.0f + 0.5f));
            er_blit_blend(&p, (int)sizeof(uint32_t), 255, ax, y, 1, 1);
        }
    }
    for (int k = 0, kmax = er_rrect_fringe_max(rr.r_r); k < kmax; k++)
    {
        const float cov = er_rrect_fringe_cov(rr.r_r, rr.r_dx, rr.r_dy, k);
        if (cov <= 0.0f)
            break;
        const int ax = m->x + rr.x1 + k;
        if (cov < 1.0f && ax < m->x + m->w && ax >= x && ax < x + n)
        {
            const uint32_t p = er_px_scale_premul(row[ax - x], (uint32_t)(cov * 255.0f + 0.5f));
            er_blit_blend(&p, (int)sizeof(uint32_t), 255, ax, y, 1, 1);
        }
    }
#endif
}

/**
 * @brief Emits columns [vx0, vx1) of one source row at 1:1, through the mask.
 *
 * ARGB rows go out straight from the source buffer; RGB565 rows are expanded into the row scratch
 * buffer first, chunked so widths beyond its capacity still render fully.
 *
 * @param[in] img    Source image entry.
 * @param[in] m      Silhouette to clip to.
 * @param[in] src_x  Source column drawn at dst_x.
 * @param[in] sy     Source row.
 * @param[in] dst_x  Destination left edge in framebuffer pixels.
 * @param[in] y      Destination row in framebuffer pixels.
 * @param[in] vx0    First column to emit, relative to dst_x.
 * @param[in] vx1    One past the last column to emit, relative to dst_x.
 */
static void
emit_unscaled_row(const ImageEntry* img, const ImgMask* m, int src_x, int sy, int dst_x, int y, int vx0, int vx1)
{
    if (img->format == ER_IMG_ARGB8888)
    {
        const uint32_t* srow = (const uint32_t*)img->buf + (size_t)sy * (size_t)img->w + (size_t)src_x;
        emit_row(m, srow + vx0, img->opaque, dst_x + vx0, y, vx1 - vx0);
        return;
    }
    const uint16_t* srow = (const uint16_t*)img->buf + (size_t)sy * (size_t)img->w + (size_t)src_x;
    for (int cx = vx0; cx < vx1; cx += ERUI_MAX_IMG_ROW_PIXELS)
    {
        const int cw = (vx1 - cx) < ERUI_MAX_IMG_ROW_PIXELS ? (vx1 - cx) : ERUI_MAX_IMG_ROW_PIXELS;
        for (int dx = 0; dx < cw; dx++)
            irow()[dx] = rgb565_to_argb(srow[cx + dx]);
        emit_row(m, irow(), true, dst_x + cx, y, cw);
    }
}

/**
 * @brief Renders a source crop at 1:1 with no tint, as one whole-rect transfer where the backend can take it.
 *
 * @param[in] img    Source image entry.
 * @param[in] src_x  Left edge of the source crop.
 * @param[in] src_y  Top edge of the source crop.
 * @param[in] dst_x  Destination left edge in framebuffer pixels.
 * @param[in] dst_y  Destination top edge in framebuffer pixels.
 * @param[in] w      Width in pixels, the same in source and destination.
 * @param[in] h      Height in pixels, the same in source and destination.
 */
static void render_unscaled(const ImageEntry* img, int src_x, int src_y, int dst_x, int dst_y, int w, int h)
{
    if (img->format == ER_IMG_ARGB8888)
    {
        /* Emit directly from the source buffer rows using the image's own row stride. An opaque
         * image goes through the format-aware entry point: with a copy_rect_fmt backend that is
         * one whole-rect call carrying the engine's opacity guarantee (no per-pixel alpha scan
         * backend-side); otherwise it degrades to the classic er_blit_copy. */
        const int img_stride = img->w * (int)sizeof(uint32_t);
        const uint32_t* rows = (const uint32_t*)img->buf + (size_t)src_y * (size_t)img->w + (size_t)src_x;
        if (img->opaque)
            er_blit_copy_fmt(rows, img_stride, ER_IMG_ARGB8888, dst_x, dst_y, w, h);
        else
            er_blit_blend(rows, img_stride, 255, dst_x, dst_y, w, h);
        return;
    }

    /* RGB565 1:1: hand the 16-bit rows straight to the backend when it can take them — one
     * whole-rect transfer (a single M2M_PFC on DMA2D-class hardware), the same one-shot cost
     * as the ARGB path. A full-screen background must NOT decay into per-row CPU expansion:
     * that is ~1M conversions plus one backend call per scanline. */
    const int stride565 = img->w * (int)sizeof(uint16_t);
    const uint16_t* rows565 = (const uint16_t*)img->buf + (size_t)src_y * (size_t)img->w + (size_t)src_x;
    if (er_blit_copy_fmt(rows565, stride565, ER_IMG_RGB565, dst_x, dst_y, w, h))
        return;

    /* CPU fallback (no copy_rect_fmt backend, scratch capture, or inherited alpha): expand each
     * row into the scratch buffer (internal RAM — reads from the 2 B/px source are the only
     * external-memory source traffic), then emit it. */
    int vx0, vy0, vx1, vy1;
    if (!visible_part(dst_x, dst_y, w, h, &vx0, &vy0, &vx1, &vy1))
        return;
    for (int dy = vy0; dy < vy1; dy++)
        emit_unscaled_row(img, &k_square_mask, src_x, src_y + dy, dst_x, dst_y + dy, vx0, vx1);
}

/**
 * @brief Renders a source crop of an image to a destination rectangle.
 *
 * Uses bilinear sampling when ERUI_BILINEAR_SCALE is non-zero, otherwise nearest-neighbor.
 * When the source and destination sizes match and no tint is applied, the original buffer
 * rows are emitted directly without copying into the row scratch buffer.
 *
 * Fully opaque images (every source alpha 0xFF — scanned once at registration; all RGB565
 * images) are emitted through er_blit_copy instead of er_blit_blend: backends replace the
 * destination pixels outright instead of read-modify-write compositing, which is the fast
 * path for full-screen backgrounds.
 *
 * The CPU paths produce only the rows and columns inside the active scissor (see visible_part()).
 *
 * A rounded mask clips every row a corner arc cuts into; at 1:1 the rows between the arcs still go
 * out as one whole-rect transfer.
 *
 * @param[in] img         Source image entry (buffer, dimensions, format, opacity).
 * @param[in] m           Silhouette the destination rows are clipped to.
 * @param[in] src_x       Left edge of the source crop rectangle.
 * @param[in] src_y       Top edge of the source crop rectangle.
 * @param[in] src_w       Width of the source crop rectangle.
 * @param[in] src_h       Height of the source crop rectangle.
 * @param[in] dst_x       Destination left edge in framebuffer pixels.
 * @param[in] dst_y       Destination top edge in framebuffer pixels.
 * @param[in] dst_w       Destination width in pixels.
 * @param[in] dst_h       Destination height in pixels.
 * @param[in] has_tint    Whether to apply a tint color.
 * @param[in] tr          Tint red channel.
 * @param[in] tg          Tint green channel.
 * @param[in] tb          Tint blue channel.
 */
static void render_region(const ImageEntry* img,
                          const ImgMask* m,
                          int src_x,
                          int src_y,
                          int src_w,
                          int src_h,
                          int dst_x,
                          int dst_y,
                          int dst_w,
                          int dst_h,
                          bool has_tint,
                          uint8_t tr,
                          uint8_t tg,
                          uint8_t tb)
{
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
        return;

    /* Fast path: source and destination regions are the same size and no tint is needed. */
    if (!has_tint && src_w == dst_w && src_h == dst_h)
    {
        /* The rows between the corner arcs keep the whole-rect transfer; only the arc rows go row by row. */
        int band0, band1;
        mask_straight_rows(m, dst_y, dst_h, &band0, &band1);
        if (band1 > band0)
            render_unscaled(img, src_x, src_y + band0, dst_x, dst_y + band0, dst_w, band1 - band0);
        if (band0 == 0 && band1 == dst_h)
            return;

        int vx0, vy0, vx1, vy1;
        if (!visible_part(dst_x, dst_y, dst_w, dst_h, &vx0, &vy0, &vx1, &vy1))
            return;
        for (int dy = vy0; dy < vy1; dy++)
        {
            if (dy >= band0 && dy < band1)
                continue;
            emit_unscaled_row(img, m, src_x, src_y + dy, dst_x, dst_y + dy, vx0, vx1);
        }
        return;
    }

    /* General path: scale and/or tint via a one-row scratch buffer. */
    const int capped_w = (dst_w <= ERUI_MAX_IMG_ROW_PIXELS) ? dst_w : ERUI_MAX_IMG_ROW_PIXELS;
    int vx0, vy0, vx1, vy1;
    if (!visible_part(dst_x, dst_y, capped_w, dst_h, &vx0, &vy0, &vx1, &vy1))
        return;
    const int vis_w = vx1 - vx0;
    for (int dy = vy0; dy < vy1; dy++)
    {
        for (int dx = vx0; dx < vx1; dx++)
        {
#if ERUI_BILINEAR_SCALE
            /* Fractional source coords with half-pixel alignment for correct up-sampling. */
            const float sx_f = (float)src_x + ((float)dx + 0.5f) * (float)src_w / (float)dst_w - 0.5f;
            const float sy_f = (float)src_y + ((float)dy + 0.5f) * (float)src_h / (float)dst_h - 0.5f;
            uint32_t p = bilinear_sample(img, src_x, src_y, src_w, src_h, sx_f, sy_f);
#else
            const int sy = src_y + (dy * src_h) / dst_h;
            const int sx = src_x + (dx * src_w) / dst_w;
            uint32_t p = fetch_px(img, sx, sy);
#endif
            if (img->opaque)
                p |= 0xFF000000u; /* squash bilinear float dust so the row stays exactly opaque */
            if (has_tint)
                p = apply_tint(p, tr, tg, tb);
            irow()[dx - vx0] = p;
        }
        /* Tint preserves alpha, so an opaque image stays opaque through every branch above. */
        emit_row(m, irow(), img->opaque, dst_x + vx0, dst_y + dy, vis_w);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_image_load(const char* name, const void* argb_buf, int w, int h)
{
    image_registry_store(name, argb_buf, w, h, ER_IMG_ARGB8888);
}

void er_image_load_rgb565(const char* name, const void* rgb565_buf, int w, int h)
{
    image_registry_store(name, rgb565_buf, w, h, ER_IMG_RGB565);
}

bool er_image_exists(const char* name)
{
    return name && image_registry_get(name) != NULL;
}

void er_image_clear(void)
{
    image_registry_init();
}

void er_image_render(const ERImageProps* props, int x, int y, int w, int h, const ERRect* box)
{
    if (!props || w <= 0 || h <= 0)
        return;

    const ImageEntry* img = image_registry_get(props->image_name);
    if (!img || img->w <= 0 || img->h <= 0)
        return;

    ImgMask mask;
    mask_init(&mask, props, box);

    const uint32_t tint = props->tint_color;
    const bool has_tint = (tint != 0u);
    const uint8_t tr = has_tint ? (uint8_t)((tint >> 16) & 0xFFu) : 0u;
    const uint8_t tg = has_tint ? (uint8_t)((tint >> 8) & 0xFFu) : 0u;
    const uint8_t tb = has_tint ? (uint8_t)(tint & 0xFFu) : 0u;

    switch ((ERResizeMode)props->resize_mode)
    {
        default:
        case ER_RESIZE_STRETCH:
            render_region(img, &mask, 0, 0, img->w, img->h, x, y, w, h, has_tint, tr, tg, tb);
            break;

        case ER_RESIZE_COVER:
        {
            /* Scale so the image fills the destination, cropping the longer axis. */
            int crop_w, crop_h;
            if (w * img->h >= h * img->w)
            {
                crop_w = img->w;
                crop_h = (h * img->w + w / 2) / w;
            }
            else
            {
                crop_h = img->h;
                crop_w = (w * img->h + h / 2) / h;
            }
            if (crop_w < 1)
                crop_w = 1;
            if (crop_h < 1)
                crop_h = 1;
            const int crop_x = (img->w - crop_w) / 2;
            const int crop_y = (img->h - crop_h) / 2;
            render_region(img, &mask, crop_x, crop_y, crop_w, crop_h, x, y, w, h, has_tint, tr, tg, tb);
            break;
        }

        case ER_RESIZE_CONTAIN:
        {
            /* Scale to fit entirely inside the destination, preserving aspect ratio. */
            int scaled_w, scaled_h;
            if (w * img->h <= h * img->w)
            {
                scaled_w = w;
                scaled_h = (w * img->h + img->w / 2) / img->w;
            }
            else
            {
                scaled_h = h;
                scaled_w = (h * img->w + img->h / 2) / img->h;
            }
            if (scaled_w < 1)
                scaled_w = 1;
            if (scaled_h < 1)
                scaled_h = 1;
            const int off_x = (w - scaled_w) / 2;
            const int off_y = (h - scaled_h) / 2;
            render_region(
                img, &mask, 0, 0, img->w, img->h, x + off_x, y + off_y, scaled_w, scaled_h, has_tint, tr, tg, tb);
            break;
        }

        case ER_RESIZE_CENTER:
        {
            /* Display at original size, centered; clip to node bounds. */
            const int src_x = (img->w > w) ? (img->w - w) / 2 : 0;
            const int src_y = (img->h > h) ? (img->h - h) / 2 : 0;
            const int vis_w = (img->w < w) ? img->w : w;
            const int vis_h = (img->h < h) ? img->h : h;
            const int off_x = (img->w < w) ? (w - img->w) / 2 : 0;
            const int off_y = (img->h < h) ? (h - img->h) / 2 : 0;
            render_region(
                img, &mask, src_x, src_y, vis_w, vis_h, x + off_x, y + off_y, vis_w, vis_h, has_tint, tr, tg, tb);
            break;
        }

        case ER_RESIZE_REPEAT:
        {
            /* Tile the image at original size across the destination rect. */
            for (int ty = 0; ty < h; ty += img->h)
            {
                const int tile_h = (ty + img->h <= h) ? img->h : (h - ty);
                for (int tx = 0; tx < w; tx += img->w)
                {
                    const int tile_w = (tx + img->w <= w) ? img->w : (w - tx);
                    render_region(
                        img, &mask, 0, 0, tile_w, tile_h, x + tx, y + ty, tile_w, tile_h, has_tint, tr, tg, tb);
                }
            }
            break;
        }
    }
}
