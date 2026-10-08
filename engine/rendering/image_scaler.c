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
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Single-row scratch buffer used when nearest-neighbor scaling is required. */
/* One assembled row per render worker: cheap enough to duplicate, and it keeps image
 * blits parallel-safe (see the multi-core render fork in compositor.c). */
static uint32_t s_row_buf_pool[ERUI_RENDER_WORKERS][ERUI_MAX_IMG_ROW_PIXELS];

/**
 * @brief A rounded-rect silhouette the image is clipped to: the destination box and its clamped radii.
 */
typedef struct
{
    int x, y, w, h;
    int r_tl, r_tr, r_br, r_bl;
} ImageMask;

/** The calling worker's silhouette while er_image_render() draws a rounded image; NULL otherwise. */
static const ImageMask* s_mask[ERUI_RENDER_WORKERS];

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
 * @brief Blits one row of image pixels clipped to the rounded silhouette, as a rounded background is.
 *
 * The covered span blits as it is; the anti-aliased fringe pixels beside it are faded by the arc's
 * coverage (the walk er_rrect_fill_corners and the gradient mask make), so an image corner has the
 * shape a background with the same radius would have.
 *
 * @param[in] m       Silhouette.
 * @param[in] row     Premultiplied pixels; row[i] lands on column dst_x + i.
 * @param[in] dst_x   Destination column of row[0].
 * @param[in] y       Destination row.
 * @param[in] n       Pixels in the row.
 * @param[in] opaque  The pixels are all opaque: the span replaces the destination.
 */
static void mask_row(const ImageMask* m, const uint32_t* row, int dst_x, int y, int n, bool opaque)
{
    ERRRectRow rr;
    er_rrect_row(m->w, m->h, m->r_tl, m->r_tr, m->r_br, m->r_bl, y - m->y, &rr);
    const int s0 = (m->x + rr.x0 > dst_x) ? m->x + rr.x0 : dst_x;
    const int s1 = (m->x + rr.x1 < dst_x + n) ? m->x + rr.x1 : dst_x + n;
    if (s1 > s0)
    {
        if (opaque)
            er_blit_copy(&row[s0 - dst_x], (s1 - s0) * (int)sizeof(uint32_t), s0, y, s1 - s0, 1);
        else
            er_blit_blend(&row[s0 - dst_x], (s1 - s0) * (int)sizeof(uint32_t), 255, s0, y, s1 - s0, 1);
    }
#if ERUI_BORDER_AA
    for (int k = 0, kmax = er_rrect_fringe_max(rr.l_r); k < kmax; k++)
    {
        const float cov = er_rrect_fringe_cov(rr.l_r, rr.l_dx, rr.l_dy, k);
        if (cov <= 0.0f)
            break;
        const int ax = m->x + rr.x0 - 1 - k;
        if (cov < 1.0f && ax >= dst_x && ax < dst_x + n)
        {
            const uint32_t p = er_px_scale_premul(row[ax - dst_x], (uint32_t)(cov * 255.0f + 0.5f));
            er_blit_blend(&p, (int)sizeof(uint32_t), 255, ax, y, 1, 1);
        }
    }
    for (int k = 0, kmax = er_rrect_fringe_max(rr.r_r); k < kmax; k++)
    {
        const float cov = er_rrect_fringe_cov(rr.r_r, rr.r_dx, rr.r_dy, k);
        if (cov <= 0.0f)
            break;
        const int ax = m->x + rr.x1 + k;
        if (cov < 1.0f && ax >= dst_x && ax < dst_x + n && ax < m->x + m->w)
        {
            const uint32_t p = er_px_scale_premul(row[ax - dst_x], (uint32_t)(cov * 255.0f + 0.5f));
            er_blit_blend(&p, (int)sizeof(uint32_t), 255, ax, y, 1, 1);
        }
    }
#endif
}

/**
 * @brief Blits rows of image pixels clipped to the rounded silhouette: the rows clear of the corner
 * arcs as one rectangle, the rows the arcs cut into one by one.
 *
 * @param[in] m       Silhouette.
 * @param[in] rows    First row's pixels; rows[i] lands on column dst_x + i.
 * @param[in] stride  Row stride in pixels.
 * @param[in] dst_x   Destination column of each row's first pixel.
 * @param[in] dst_y   Destination row of the first row.
 * @param[in] n       Pixels per row.
 * @param[in] count   Rows.
 * @param[in] opaque  The pixels are all opaque.
 */
static void
mask_rows(const ImageMask* m, const uint32_t* rows, int stride, int dst_x, int dst_y, int n, int count, bool opaque)
{
    const int top = m->y + (m->r_tl > m->r_tr ? m->r_tl : m->r_tr);
    const int bottom = m->y + m->h - (m->r_bl > m->r_br ? m->r_bl : m->r_br);
    for (int i = 0; i < count;)
    {
        const int y = dst_y + i;
        if (y < top || y >= bottom)
        {
            mask_row(m, rows + (size_t)i * (size_t)stride, dst_x, y, n, opaque);
            i++;
            continue;
        }
        const int run = ((bottom - y) < (count - i)) ? (bottom - y) : (count - i);
        if (opaque)
            er_blit_copy(rows + (size_t)i * (size_t)stride, stride * (int)sizeof(uint32_t), dst_x, y, n, run);
        else
            er_blit_blend(rows + (size_t)i * (size_t)stride, stride * (int)sizeof(uint32_t), 255, dst_x, y, n, run);
        i += run;
    }
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
 * @param[in] img         Source image entry (buffer, dimensions, format, opacity).
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
        if (img->format == ER_IMG_ARGB8888)
        {
            /* Emit directly from the source buffer rows using the image's own row stride. An opaque
             * image goes through the format-aware entry point: with a copy_rect_fmt backend that is
             * one whole-rect call carrying the engine's opacity guarantee (no per-pixel alpha scan
             * backend-side); otherwise it degrades to the classic er_blit_copy. */
            const int img_stride = img->w * (int)sizeof(uint32_t);
            const uint32_t* rows = (const uint32_t*)img->buf + (size_t)src_y * (size_t)img->w + (size_t)src_x;
            const ImageMask* const m = s_mask[er_render_worker_id()];
            if (m)
            {
                int vx0, vy0, vx1, vy1;
                if (visible_part(dst_x, dst_y, dst_w, dst_h, &vx0, &vy0, &vx1, &vy1))
                    mask_rows(m,
                              rows + (size_t)vy0 * (size_t)img->w + (size_t)vx0,
                              img->w,
                              dst_x + vx0,
                              dst_y + vy0,
                              vx1 - vx0,
                              vy1 - vy0,
                              img->opaque);
                return;
            }
            if (img->opaque)
                er_blit_copy_fmt(rows, img_stride, ER_IMG_ARGB8888, dst_x, dst_y, dst_w, dst_h);
            else
                er_blit_blend(rows, img_stride, 255, dst_x, dst_y, dst_w, dst_h);
            return;
        }

        /* RGB565 1:1: hand the 16-bit rows straight to the backend when it can take them — one
         * whole-rect transfer (a single M2M_PFC on DMA2D-class hardware), the same one-shot cost
         * as the ARGB path. A full-screen background must NOT decay into per-row CPU expansion:
         * that is ~1M conversions plus one backend call per scanline. */
        const int stride565 = img->w * (int)sizeof(uint16_t);
        const uint16_t* rows565 = (const uint16_t*)img->buf + (size_t)src_y * (size_t)img->w + (size_t)src_x;
        const ImageMask* const m565 = s_mask[er_render_worker_id()];
        if (!m565 && er_blit_copy_fmt(rows565, stride565, ER_IMG_RGB565, dst_x, dst_y, dst_w, dst_h))
            return;

        /* CPU fallback (no copy_rect_fmt backend, scratch capture, or inherited alpha): expand each
         * row into the scratch buffer (internal RAM — reads from the 2 B/px source are the only
         * external-memory source traffic), then emit it. Chunked horizontally so widths beyond the
         * scratch capacity still render fully. */
        int vx0, vy0, vx1, vy1;
        if (!visible_part(dst_x, dst_y, dst_w, dst_h, &vx0, &vy0, &vx1, &vy1))
            return;
        for (int dy = vy0; dy < vy1; dy++)
        {
            const uint16_t* srow = rows565 + (size_t)dy * (size_t)img->w;
            for (int cx = vx0; cx < vx1; cx += ERUI_MAX_IMG_ROW_PIXELS)
            {
                const int cw = (vx1 - cx) < ERUI_MAX_IMG_ROW_PIXELS ? (vx1 - cx) : ERUI_MAX_IMG_ROW_PIXELS;
                for (int dx = 0; dx < cw; dx++)
                    irow()[dx] = rgb565_to_argb(srow[cx + dx]);
                if (m565)
                    mask_row(m565, irow(), dst_x + cx, dst_y + dy, cw, true);
                else
                    er_blit_copy(irow(), cw * (int)sizeof(uint32_t), dst_x + cx, dst_y + dy, cw, 1);
            }
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
        const ImageMask* const m = s_mask[er_render_worker_id()];
        if (m)
            mask_row(m, irow(), dst_x + vx0, dst_y + dy, vis_w, img->opaque);
        else if (img->opaque)
            er_blit_copy(irow(), vis_w * (int)sizeof(uint32_t), dst_x + vx0, dst_y + dy, vis_w, 1);
        else
            er_blit_blend(irow(), vis_w * (int)sizeof(uint32_t), 255, dst_x + vx0, dst_y + dy, vis_w, 1);
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

void er_image_render(const ERImageProps* props, int x, int y, int w, int h)
{
    if (!props || w <= 0 || h <= 0)
        return;

    const ImageEntry* img = image_registry_get(props->image_name);
    if (!img || img->w <= 0 || img->h <= 0)
        return;

    const uint32_t tint = props->tint_color;
    const bool has_tint = (tint != 0u);
    const uint8_t tr = has_tint ? (uint8_t)((tint >> 16) & 0xFFu) : 0u;
    const uint8_t tg = has_tint ? (uint8_t)((tint >> 8) & 0xFFu) : 0u;
    const uint8_t tb = has_tint ? (uint8_t)(tint & 0xFFu) : 0u;

    /* A rounded image is clipped to its box's silhouette; a square one takes the unmasked paths. */
    ImageMask mask = {x, y, w, h, props->radius_tl, props->radius_tr, props->radius_br, props->radius_bl};
    er_rrect_clamp_radii(w, h, &mask.r_tl, &mask.r_tr, &mask.r_br, &mask.r_bl);
    const int worker = er_render_worker_id();
    s_mask[worker] = (mask.r_tl > 0 || mask.r_tr > 0 || mask.r_br > 0 || mask.r_bl > 0) ? &mask : NULL;

    switch ((ERResizeMode)props->resize_mode)
    {
        default:
        case ER_RESIZE_STRETCH:
            render_region(img, 0, 0, img->w, img->h, x, y, w, h, has_tint, tr, tg, tb);
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
            render_region(img, crop_x, crop_y, crop_w, crop_h, x, y, w, h, has_tint, tr, tg, tb);
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
            render_region(img, 0, 0, img->w, img->h, x + off_x, y + off_y, scaled_w, scaled_h, has_tint, tr, tg, tb);
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
            render_region(img, src_x, src_y, vis_w, vis_h, x + off_x, y + off_y, vis_w, vis_h, has_tint, tr, tg, tb);
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
                    render_region(img, 0, 0, tile_w, tile_h, x + tx, y + ty, tile_w, tile_h, has_tint, tr, tg, tb);
                }
            }
            break;
        }
    }
    s_mask[worker] = NULL;
}
