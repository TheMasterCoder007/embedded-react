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
#include "image_registry.h"
#include "renderer_internal.h"
#include <math.h>
#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#ifndef ERUI_MAX_IMG_ROW_PIXELS
#define ERUI_MAX_IMG_ROW_PIXELS 800
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

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
        if (er_blit_copy_fmt(rows565, stride565, ER_IMG_RGB565, dst_x, dst_y, dst_w, dst_h))
            return;

        /* CPU fallback (no copy_rect_fmt backend, scratch capture, or inherited alpha): expand each
         * row into the scratch buffer (internal RAM — reads from the 2 B/px source are the only
         * external-memory source traffic), then emit it. Chunked horizontally so widths beyond the
         * scratch capacity still render fully. */
        for (int dy = 0; dy < dst_h; dy++)
        {
            const uint16_t* srow = rows565 + (size_t)dy * (size_t)img->w;
            for (int cx = 0; cx < dst_w; cx += ERUI_MAX_IMG_ROW_PIXELS)
            {
                const int cw = (dst_w - cx) < ERUI_MAX_IMG_ROW_PIXELS ? (dst_w - cx) : ERUI_MAX_IMG_ROW_PIXELS;
                for (int dx = 0; dx < cw; dx++)
                    irow()[dx] = rgb565_to_argb(srow[cx + dx]);
                er_blit_copy(irow(), cw * (int)sizeof(uint32_t), dst_x + cx, dst_y + dy, cw, 1);
            }
        }
        return;
    }

    /* General path: scale and/or tint via a one-row scratch buffer. */
    const int capped_w = (dst_w <= ERUI_MAX_IMG_ROW_PIXELS) ? dst_w : ERUI_MAX_IMG_ROW_PIXELS;
    for (int dy = 0; dy < dst_h; dy++)
    {
        for (int dx = 0; dx < capped_w; dx++)
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
            irow()[dx] = p;
        }
        /* Tint preserves alpha, so an opaque image stays opaque through every branch above. */
        if (img->opaque)
            er_blit_copy(irow(), capped_w * (int)sizeof(uint32_t), dst_x, dst_y + dy, capped_w, 1);
        else
            er_blit_blend(irow(), capped_w * (int)sizeof(uint32_t), 255, dst_x, dst_y + dy, capped_w, 1);
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
}
