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
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief The alpha byte of a fully opaque ARGB8888 pixel. */
#define ARGB_OPAQUE_ALPHA 0xFF000000u

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
    int x;                   /**< Box left edge in framebuffer pixels. */
    int y;                   /**< Box top edge in framebuffer pixels. */
    int width;               /**< Box width — the coordinate space of the row spans. */
    int height;              /**< Box height. */
    int radius_top_left;     /**< Clamped corner radii. */
    int radius_top_right;    /**< @see radius_top_left */
    int radius_bottom_right; /**< @see radius_top_left */
    int radius_bottom_left;  /**< @see radius_top_left */
    bool rounded;            /**< False when every radius is 0 or there is no box: rows blit whole, as before. */
} ImgMask;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A mask that clips nothing, for the paths that emit rows the mask has already let through whole. */
static const ImgMask k_square_mask = {0, 0, 0, 0, 0, 0, 0, 0, false};

/** @brief Single-row scratch buffer used when nearest-neighbor scaling is required. */
/* One assembled row per render worker: cheap enough to duplicate, and it keeps image
 * blits parallel-safe (see the multi-core render fork in compositor.c). */
static uint32_t s_row_buffer_pool[ERUI_RENDER_WORKERS][ERUI_MAX_IMAGE_ROW_PIXELS];

/** @brief The calling worker's image row buffer. */
static inline uint32_t* irow(void)
{
    return s_row_buffer_pool[er_render_worker_id()];
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
static inline uint32_t rgb565_to_argb(uint16_t pixel)
{
    const uint32_t red5 = (pixel >> 11) & 0x1Fu;
    const uint32_t green6 = (pixel >> 5) & 0x3Fu;
    const uint32_t blue5 = pixel & 0x1Fu;
    const uint32_t red = (red5 << 3) | (red5 >> 2);
    const uint32_t green = (green6 << 2) | (green6 >> 4);
    const uint32_t blue = (blue5 << 3) | (blue5 >> 2);
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}

/**
 * @brief Reads one source pixel as premultiplied ARGB8888, whatever the image's storage format.
 */
static inline uint32_t fetch_px(const ImageEntry* image, int x, int y)
{
    const size_t pixel_index = (size_t)y * (size_t)image->width + (size_t)x;
    if (image->format == ER_IMAGE_RGB565)
        return rgb565_to_argb(((const uint16_t*)image->pixels)[pixel_index]);
    return ((const uint32_t*)image->pixels)[pixel_index];
}

/**
 * @brief Applies a tint to a premultiplied ARGB8888 pixel.
 *
 * Preserves the original alpha. Replaces the RGB channels with the tint color
 * pre-multiplied by the original alpha, which is the correct behaviour for icon tinting
 * (transparent pixels remain transparent; opaque pixels become the tint color).
 *
 * @param[in] pixel       Source premultiplied ARGB8888 pixel.
 * @param[in] tint_red    Tint red channel (0–255 straight).
 * @param[in] tint_green  Tint green channel (0–255 straight).
 * @param[in] tint_blue   Tint blue channel (0–255 straight).
 *
 * @return Tinted premultiplied ARGB8888 pixel.
 */
static uint32_t apply_tint(uint32_t pixel, uint8_t tint_red, uint8_t tint_green, uint8_t tint_blue)
{
    const uint32_t alpha = (pixel >> 24) & 0xFFu;
    const uint32_t red = ((uint32_t)tint_red * alpha) / ER_CHANNEL_MAX;
    const uint32_t green = ((uint32_t)tint_green * alpha) / ER_CHANNEL_MAX;
    const uint32_t blue = ((uint32_t)tint_blue * alpha) / ER_CHANNEL_MAX;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

#if ERUI_BILINEAR_SCALE

/**
 * @brief Samples a premultiplied ARGB8888 source image at fractional pixel coordinates.
 *
 * Clamps to the source crop rectangle [source_x, source_x+source_width) × [source_y, source_y+source_height) before
 * computing the four-tap bilinear weights. Bilinear interpolation on premultiplied values is
 * mathematically exact (no alpha halo artefacts).
 *
 * @param[in] image          Source image entry (any storage format; taps are fetched as ARGB8888).
 * @param[in] source_x       Left edge of the valid crop region.
 * @param[in] source_y       Top edge of the valid crop region.
 * @param[in] source_width   Width of the valid crop region.
 * @param[in] source_height  Height of the valid crop region.
 * @param[in] sample_x       Fractional source X coordinate.
 * @param[in] sample_y       Fractional source Y coordinate.
 *
 * @return Bilinearly sampled premultiplied ARGB8888 pixel.
 */
static uint32_t bilinear_sample(const ImageEntry* image,
                                int source_x,
                                int source_y,
                                int source_width,
                                int source_height,
                                float sample_x,
                                float sample_y)
{
    /* Clamp to crop bounds. */
    const float x_min = (float)source_x, x_max = (float)(source_x + source_width - 1);
    const float y_min = (float)source_y, y_max = (float)(source_y + source_height - 1);
    if (sample_x < x_min)
        sample_x = x_min;
    if (sample_x > x_max)
        sample_x = x_max;
    if (sample_y < y_min)
        sample_y = y_min;
    if (sample_y > y_max)
        sample_y = y_max;

    const int x0 = (int)sample_x, y0 = (int)sample_y;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 > source_x + source_width - 1)
        x1 = source_x + source_width - 1;
    if (y1 > source_y + source_height - 1)
        y1 = source_y + source_height - 1;

    const float fraction_x = sample_x - (float)x0;
    const float fraction_y = sample_y - (float)y0;
    const float weight00 = (1.0f - fraction_x) * (1.0f - fraction_y);
    const float weight10 = fraction_x * (1.0f - fraction_y);
    const float weight01 = (1.0f - fraction_x) * fraction_y;
    const float weight11 = fraction_x * fraction_y;

    const uint32_t pixel00 = fetch_px(image, x0, y0);
    const uint32_t pixel10 = fetch_px(image, x1, y0);
    const uint32_t pixel01 = fetch_px(image, x0, y1);
    const uint32_t pixel11 = fetch_px(image, x1, y1);

    const uint32_t alpha = (uint32_t)(((pixel00 >> 24) & 0xFFu) * weight00 + ((pixel10 >> 24) & 0xFFu) * weight10
                                      + ((pixel01 >> 24) & 0xFFu) * weight01 + ((pixel11 >> 24) & 0xFFu) * weight11);
    const uint32_t red = (uint32_t)(((pixel00 >> 16) & 0xFFu) * weight00 + ((pixel10 >> 16) & 0xFFu) * weight10
                                    + ((pixel01 >> 16) & 0xFFu) * weight01 + ((pixel11 >> 16) & 0xFFu) * weight11);
    const uint32_t green = (uint32_t)(((pixel00 >> 8) & 0xFFu) * weight00 + ((pixel10 >> 8) & 0xFFu) * weight10
                                      + ((pixel01 >> 8) & 0xFFu) * weight01 + ((pixel11 >> 8) & 0xFFu) * weight11);
    const uint32_t blue = (uint32_t)((pixel00 & 0xFFu) * weight00 + (pixel10 & 0xFFu) * weight10
                                     + (pixel01 & 0xFFu) * weight01 + (pixel11 & 0xFFu) * weight11);
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

#endif /* ERUI_BILINEAR_SCALE */

/**
 * @brief Narrows the destination [0, width) x [0, height) placed at (x, y) to the part inside the active scissor.
 *
 * Every blit is clipped to the scissor anyway; producing only this part keeps a repaint that touches a
 * corner of a large image from converting or re-scaling all of it.
 *
 * @param[in]  x       Destination left edge in framebuffer pixels.
 * @param[in]  y       Destination top edge in framebuffer pixels.
 * @param[in]  width   Destination width in pixels.
 * @param[in]  height  Destination height in pixels.
 * @param[out] x0      First visible column, relative to x.
 * @param[out] y0      First visible row, relative to y.
 * @param[out] x1      One past the last visible column, relative to x.
 * @param[out] y1      One past the last visible row, relative to y.
 *
 * @return false when nothing of the destination is inside the scissor.
 */
static bool visible_part(int x, int y, int width, int height, int* x0, int* y0, int* x1, int* y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = width;
    *y1 = height;
    int clip_x, clip_y, clip_width, clip_height;
    if (er_get_clip_rect(&clip_x, &clip_y, &clip_width, &clip_height))
    {
        if (clip_x - x > *x0)
            *x0 = clip_x - x;
        if (clip_y - y > *y0)
            *y0 = clip_y - y;
        if (clip_x + clip_width - x < *x1)
            *x1 = clip_x + clip_width - x;
        if (clip_y + clip_height - y < *y1)
            *y1 = clip_y + clip_height - y;
    }
    return *x0 < *x1 && *y0 < *y1;
}

/**
 * @brief Resolves an Image node's corner radii into the mask its rows are clipped to.
 *
 * @param[out] mask   Receives the silhouette.
 * @param[in]  props  Image props carrying the resolved per-corner radii.
 * @param[in]  box    The node's border box; NULL leaves the mask square.
 */
static void mask_init(ImgMask* mask, const ERImageProps* props, const ERRect* box)
{
    *mask = k_square_mask;
    if (!box)
        return;
    mask->x = box->x;
    mask->y = box->y;
    mask->width = box->width;
    mask->height = box->height;
    mask->radius_top_left = props->border_top_left_radius;
    mask->radius_top_right = props->border_top_right_radius;
    mask->radius_bottom_right = props->border_bottom_right_radius;
    mask->radius_bottom_left = props->border_bottom_left_radius;
    er_rrect_clamp_radii(mask->width,
                         mask->height,
                         &mask->radius_top_left,
                         &mask->radius_top_right,
                         &mask->radius_bottom_right,
                         &mask->radius_bottom_left);
    mask->rounded = (mask->radius_top_left > 0 || mask->radius_top_right > 0 || mask->radius_bottom_right > 0
                     || mask->radius_bottom_left > 0);
}

/**
 * @brief Finds the destination rows no corner arc cuts into, which can go out unclipped.
 *
 * @param[in]  mask                Silhouette the rows are clipped to.
 * @param[in]  destination_y       Destination top edge in framebuffer pixels.
 * @param[in]  destination_height  Destination height in pixels.
 * @param[out] band0               First straight row, relative to destination_y.
 * @param[out] band1               One past the last straight row, relative to destination_y; equal to band0
 *                                 when there is none.
 */
static void mask_straight_rows(const ImgMask* mask, int destination_y, int destination_height, int* band0, int* band1)
{
    *band0 = 0;
    *band1 = destination_height;
    if (!mask->rounded)
        return;
    const int top = (mask->radius_top_left > mask->radius_top_right) ? mask->radius_top_left : mask->radius_top_right;
    const int bottom =
        (mask->radius_bottom_left > mask->radius_bottom_right) ? mask->radius_bottom_left : mask->radius_bottom_right;
    if (mask->y + top - destination_y > *band0)
        *band0 = mask->y + top - destination_y;
    if (mask->y + mask->height - bottom - destination_y < *band1)
        *band1 = mask->y + mask->height - bottom - destination_y;
    if (*band1 < *band0)
        *band1 = *band0;
}

/**
 * @brief Emits a run of premultiplied pixels, copied when the image is opaque and blended otherwise.
 *
 * @param[in] pixels       Premultiplied ARGB8888 pixels.
 * @param[in] opaque       Whether every pixel's alpha is 0xFF.
 * @param[in] x            Destination left edge in framebuffer pixels.
 * @param[in] y            Destination row in framebuffer pixels.
 * @param[in] pixel_count  Pixel count.
 */
static void emit_span(const uint32_t* pixels, bool opaque, int x, int y, int pixel_count)
{
    if (opaque)
        er_blit_copy(pixels, pixel_count * (int)sizeof(uint32_t), x, y, pixel_count, 1);
    else
        er_blit_blend(pixels, pixel_count * (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, x, y, pixel_count, 1);
}

/**
 * @brief Emits one destination row of image pixels, clipped to the rounded-rect silhouette.
 *
 * A square mask emits the row whole. Otherwise only the row's covered span goes out, then the
 * anti-aliased fringe pixels beside it, faded by the arc's coverage (the walk er_rrect_fill_corners
 * fills a background's corner with, so the image's corner is the same shape).
 *
 * @param[in] mask         Silhouette to clip to.
 * @param[in] row          Premultiplied ARGB8888 pixels for destination columns [x, x + pixel_count).
 * @param[in] opaque       Whether every pixel in @p row has alpha 0xFF.
 * @param[in] x            Destination left edge of @p row in framebuffer pixels.
 * @param[in] y            Destination row in framebuffer pixels.
 * @param[in] pixel_count  Pixel count.
 */
static void emit_row(const ImgMask* mask, const uint32_t* row, bool opaque, int x, int y, int pixel_count)
{
    if (pixel_count <= 0)
        return;
    if (!mask->rounded)
    {
        emit_span(row, opaque, x, y, pixel_count);
        return;
    }
    if (y < mask->y || y >= mask->y + mask->height)
        return;

    ERRRectRow span;
    er_rrect_row(mask->width,
                 mask->height,
                 mask->radius_top_left,
                 mask->radius_top_right,
                 mask->radius_bottom_right,
                 mask->radius_bottom_left,
                 y - mask->y,
                 &span);

    int span_start = mask->x + span.x0;
    int span_end = mask->x + span.x1;
    if (span_start < x)
        span_start = x;
    if (span_end > x + pixel_count)
        span_end = x + pixel_count;
    if (span_end > span_start)
        emit_span(&row[span_start - x], opaque, span_start, y, span_end - span_start);

#if ERUI_BORDER_AA
    for (int step = 0, step_count = er_rrect_fringe_max(span.left_radius); step < step_count; step++)
    {
        const float coverage =
            er_rrect_fringe_cov(span.left_radius, span.left_half_width, span.left_row_distance, step);
        if (coverage <= 0.0f)
            break;
        const int fringe_x = mask->x + span.x0 - 1 - step;
        if (coverage < 1.0f && fringe_x >= mask->x && fringe_x >= x && fringe_x < x + pixel_count)
        {
            const uint32_t pixel =
                er_px_scale_premul(row[fringe_x - x], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
            er_blit_blend(&pixel, (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, fringe_x, y, 1, 1);
        }
    }
    for (int step = 0, step_count = er_rrect_fringe_max(span.right_radius); step < step_count; step++)
    {
        const float coverage =
            er_rrect_fringe_cov(span.right_radius, span.right_half_width, span.right_row_distance, step);
        if (coverage <= 0.0f)
            break;
        const int fringe_x = mask->x + span.x1 + step;
        if (coverage < 1.0f && fringe_x < mask->x + mask->width && fringe_x >= x && fringe_x < x + pixel_count)
        {
            const uint32_t pixel =
                er_px_scale_premul(row[fringe_x - x], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
            er_blit_blend(&pixel, (int)sizeof(uint32_t), ER_ALPHA_OPAQUE, fringe_x, y, 1, 1);
        }
    }
#endif
}

/**
 * @brief Emits columns [visible_x0, visible_x1) of one source row at 1:1, through the mask.
 *
 * ARGB rows go out straight from the source buffer; RGB565 rows are expanded into the row scratch
 * buffer first, chunked so widths beyond its capacity still render fully.
 *
 * @param[in] image          Source image entry.
 * @param[in] mask           Silhouette to clip to.
 * @param[in] source_x       Source column drawn at destination_x.
 * @param[in] source_row     Source row.
 * @param[in] destination_x  Destination left edge in framebuffer pixels.
 * @param[in] y              Destination row in framebuffer pixels.
 * @param[in] visible_x0     First column to emit, relative to destination_x.
 * @param[in] visible_x1     One past the last column to emit, relative to destination_x.
 */
static void emit_unscaled_row(const ImageEntry* image,
                              const ImgMask* mask,
                              int source_x,
                              int source_row,
                              int destination_x,
                              int y,
                              int visible_x0,
                              int visible_x1)
{
    if (image->format == ER_IMAGE_ARGB8888)
    {
        const uint32_t* source_pixels =
            (const uint32_t*)image->pixels + (size_t)source_row * (size_t)image->width + (size_t)source_x;
        emit_row(
            mask, source_pixels + visible_x0, image->opaque, destination_x + visible_x0, y, visible_x1 - visible_x0);
        return;
    }
    const uint16_t* source_pixels =
        (const uint16_t*)image->pixels + (size_t)source_row * (size_t)image->width + (size_t)source_x;
    for (int chunk_x = visible_x0; chunk_x < visible_x1; chunk_x += ERUI_MAX_IMAGE_ROW_PIXELS)
    {
        const int chunk_width =
            (visible_x1 - chunk_x) < ERUI_MAX_IMAGE_ROW_PIXELS ? (visible_x1 - chunk_x) : ERUI_MAX_IMAGE_ROW_PIXELS;
        for (int chunk_column = 0; chunk_column < chunk_width; chunk_column++)
            irow()[chunk_column] = rgb565_to_argb(source_pixels[chunk_x + chunk_column]);
        emit_row(mask, irow(), true, destination_x + chunk_x, y, chunk_width);
    }
}

/**
 * @brief Renders a source crop at 1:1 with no tint, as one whole-rect transfer where the backend can take it.
 *
 * @param[in] image          Source image entry.
 * @param[in] source_x       Left edge of the source crop.
 * @param[in] source_y       Top edge of the source crop.
 * @param[in] destination_x  Destination left edge in framebuffer pixels.
 * @param[in] destination_y  Destination top edge in framebuffer pixels.
 * @param[in] width          Width in pixels, the same in source and destination.
 * @param[in] height         Height in pixels, the same in source and destination.
 */
static void render_unscaled(
    const ImageEntry* image, int source_x, int source_y, int destination_x, int destination_y, int width, int height)
{
    if (image->format == ER_IMAGE_ARGB8888)
    {
        /* Emit directly from the source buffer rows using the image's own row stride. An opaque
         * image goes through the format-aware entry point: with a copy_rect_format backend that is
         * one whole-rect call carrying the engine's opacity guarantee (no per-pixel alpha scan
         * backend-side); otherwise it degrades to the classic er_blit_copy. */
        const int image_stride = image->width * (int)sizeof(uint32_t);
        const uint32_t* rows =
            (const uint32_t*)image->pixels + (size_t)source_y * (size_t)image->width + (size_t)source_x;
        if (image->opaque)
            er_blit_copy_fmt(rows, image_stride, ER_IMAGE_ARGB8888, destination_x, destination_y, width, height);
        else
            er_blit_blend(rows, image_stride, ER_ALPHA_OPAQUE, destination_x, destination_y, width, height);
        return;
    }

    /* RGB565 1:1: hand the 16-bit rows straight to the backend when it can take them — one
     * whole-rect transfer (a single M2M_PFC on DMA2D-class hardware), the same one-shot cost
     * as the ARGB path. A full-screen background must NOT decay into per-row CPU expansion:
     * that is ~1M conversions plus one backend call per scanline. */
    const int rgb565_stride = image->width * (int)sizeof(uint16_t);
    const uint16_t* rgb565_rows =
        (const uint16_t*)image->pixels + (size_t)source_y * (size_t)image->width + (size_t)source_x;
    if (er_blit_copy_fmt(rgb565_rows, rgb565_stride, ER_IMAGE_RGB565, destination_x, destination_y, width, height))
        return;

    /* CPU fallback (no copy_rect_format backend, scratch capture, or inherited alpha): expand each
     * row into the scratch buffer (internal RAM — reads from the 2 B/px source are the only
     * external-memory source traffic), then emit it. */
    int visible_x0, visible_y0, visible_x1, visible_y1;
    if (!visible_part(destination_x, destination_y, width, height, &visible_x0, &visible_y0, &visible_x1, &visible_y1))
        return;
    for (int row = visible_y0; row < visible_y1; row++)
        emit_unscaled_row(image,
                          &k_square_mask,
                          source_x,
                          source_y + row,
                          destination_x,
                          destination_y + row,
                          visible_x0,
                          visible_x1);
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
 * @param[in] image               Source image entry (buffer, dimensions, format, opacity).
 * @param[in] mask                Silhouette the destination rows are clipped to.
 * @param[in] source_x            Left edge of the source crop rectangle.
 * @param[in] source_y            Top edge of the source crop rectangle.
 * @param[in] source_width        Width of the source crop rectangle.
 * @param[in] source_height       Height of the source crop rectangle.
 * @param[in] destination_x       Destination left edge in framebuffer pixels.
 * @param[in] destination_y       Destination top edge in framebuffer pixels.
 * @param[in] destination_width   Destination width in pixels.
 * @param[in] destination_height  Destination height in pixels.
 * @param[in] has_tint            Whether to apply a tint color.
 * @param[in] tint_red            Tint red channel.
 * @param[in] tint_green          Tint green channel.
 * @param[in] tint_blue           Tint blue channel.
 */
static void render_region(const ImageEntry* image,
                          const ImgMask* mask,
                          int source_x,
                          int source_y,
                          int source_width,
                          int source_height,
                          int destination_x,
                          int destination_y,
                          int destination_width,
                          int destination_height,
                          bool has_tint,
                          uint8_t tint_red,
                          uint8_t tint_green,
                          uint8_t tint_blue)
{
    if (destination_width <= 0 || destination_height <= 0 || source_width <= 0 || source_height <= 0)
        return;

    /* Fast path: source and destination regions are the same size and no tint is needed. */
    if (!has_tint && source_width == destination_width && source_height == destination_height)
    {
        /* The rows between the corner arcs keep the whole-rect transfer; only the arc rows go row by row. */
        int band0, band1;
        mask_straight_rows(mask, destination_y, destination_height, &band0, &band1);
        if (band1 > band0)
            render_unscaled(image,
                            source_x,
                            source_y + band0,
                            destination_x,
                            destination_y + band0,
                            destination_width,
                            band1 - band0);
        if (band0 == 0 && band1 == destination_height)
            return;

        int visible_x0, visible_y0, visible_x1, visible_y1;
        if (!visible_part(destination_x,
                          destination_y,
                          destination_width,
                          destination_height,
                          &visible_x0,
                          &visible_y0,
                          &visible_x1,
                          &visible_y1))
            return;
        for (int row = visible_y0; row < visible_y1; row++)
        {
            if (row >= band0 && row < band1)
                continue;
            emit_unscaled_row(
                image, mask, source_x, source_y + row, destination_x, destination_y + row, visible_x0, visible_x1);
        }
        return;
    }

    /* General path: scale and/or tint via a one-row scratch buffer. */
    const int capped_width =
        (destination_width <= ERUI_MAX_IMAGE_ROW_PIXELS) ? destination_width : ERUI_MAX_IMAGE_ROW_PIXELS;
    int visible_x0, visible_y0, visible_x1, visible_y1;
    if (!visible_part(destination_x,
                      destination_y,
                      capped_width,
                      destination_height,
                      &visible_x0,
                      &visible_y0,
                      &visible_x1,
                      &visible_y1))
        return;
    const int visible_width = visible_x1 - visible_x0;
    for (int row = visible_y0; row < visible_y1; row++)
    {
        for (int column = visible_x0; column < visible_x1; column++)
        {
#if ERUI_BILINEAR_SCALE
            /* Fractional source coords with half-pixel alignment for correct up-sampling. */
            const float sample_x =
                (float)source_x + ((float)column + 0.5f) * (float)source_width / (float)destination_width - 0.5f;
            const float sample_y =
                (float)source_y + ((float)row + 0.5f) * (float)source_height / (float)destination_height - 0.5f;
            uint32_t pixel =
                bilinear_sample(image, source_x, source_y, source_width, source_height, sample_x, sample_y);
#else
            const int source_row = source_y + (row * source_height) / destination_height;
            const int source_column = source_x + (column * source_width) / destination_width;
            uint32_t pixel = fetch_px(image, source_column, source_row);
#endif
            if (image->opaque)
                pixel |= ARGB_OPAQUE_ALPHA; /* squash bilinear float dust so the row stays exactly opaque */
            if (has_tint)
                pixel = apply_tint(pixel, tint_red, tint_green, tint_blue);
            irow()[column - visible_x0] = pixel;
        }
        /* Tint preserves alpha, so an opaque image stays opaque through every branch above. */
        emit_row(mask, irow(), image->opaque, destination_x + visible_x0, destination_y + row, visible_width);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_image_load(const char* name, const void* argb_pixels, int width, int height)
{
    image_registry_store(name, argb_pixels, width, height, ER_IMAGE_ARGB8888);
}

void er_image_load_rgb565(const char* name, const void* rgb565_pixels, int width, int height)
{
    image_registry_store(name, rgb565_pixels, width, height, ER_IMAGE_RGB565);
}

bool er_image_exists(const char* name)
{
    return name && image_registry_get(name) != NULL;
}

void er_image_clear(void)
{
    image_registry_init();
}

void er_image_render(const ERImageProps* props, int x, int y, int width, int height, const ERRect* box)
{
    if (!props || width <= 0 || height <= 0)
        return;

    const ImageEntry* image = image_registry_get(props->image_name);
    if (!image || image->width <= 0 || image->height <= 0)
        return;

    ImgMask mask;
    mask_init(&mask, props, box);

    const uint32_t tint = props->tint_color;
    const bool has_tint = (tint != 0u);
    const uint8_t tint_red = has_tint ? (uint8_t)((tint >> 16) & 0xFFu) : 0u;
    const uint8_t tint_green = has_tint ? (uint8_t)((tint >> 8) & 0xFFu) : 0u;
    const uint8_t tint_blue = has_tint ? (uint8_t)(tint & 0xFFu) : 0u;

    switch ((ERResizeMode)props->resize_mode)
    {
        default:
        case ER_RESIZE_STRETCH:
            render_region(image,
                          &mask,
                          0,
                          0,
                          image->width,
                          image->height,
                          x,
                          y,
                          width,
                          height,
                          has_tint,
                          tint_red,
                          tint_green,
                          tint_blue);
            break;

        case ER_RESIZE_COVER:
        {
            /* Scale so the image fills the destination, cropping the longer axis. */
            int crop_width, crop_height;
            if (width * image->height >= height * image->width)
            {
                crop_width = image->width;
                crop_height = (height * image->width + width / 2) / width;
            }
            else
            {
                crop_height = image->height;
                crop_width = (width * image->height + height / 2) / height;
            }
            if (crop_width < 1)
                crop_width = 1;
            if (crop_height < 1)
                crop_height = 1;
            const int crop_x = (image->width - crop_width) / 2;
            const int crop_y = (image->height - crop_height) / 2;
            render_region(image,
                          &mask,
                          crop_x,
                          crop_y,
                          crop_width,
                          crop_height,
                          x,
                          y,
                          width,
                          height,
                          has_tint,
                          tint_red,
                          tint_green,
                          tint_blue);
            break;
        }

        case ER_RESIZE_CONTAIN:
        {
            /* Scale to fit entirely inside the destination, preserving aspect ratio. */
            int scaled_width, scaled_height;
            if (width * image->height <= height * image->width)
            {
                scaled_width = width;
                scaled_height = (width * image->height + image->width / 2) / image->width;
            }
            else
            {
                scaled_height = height;
                scaled_width = (height * image->width + image->height / 2) / image->height;
            }
            if (scaled_width < 1)
                scaled_width = 1;
            if (scaled_height < 1)
                scaled_height = 1;
            const int offset_x = (width - scaled_width) / 2;
            const int offset_y = (height - scaled_height) / 2;
            render_region(image,
                          &mask,
                          0,
                          0,
                          image->width,
                          image->height,
                          x + offset_x,
                          y + offset_y,
                          scaled_width,
                          scaled_height,
                          has_tint,
                          tint_red,
                          tint_green,
                          tint_blue);
            break;
        }

        case ER_RESIZE_CENTER:
        {
            /* Display at original size, centered; clip to node bounds. */
            const int source_x = (image->width > width) ? (image->width - width) / 2 : 0;
            const int source_y = (image->height > height) ? (image->height - height) / 2 : 0;
            const int visible_width = (image->width < width) ? image->width : width;
            const int visible_height = (image->height < height) ? image->height : height;
            const int offset_x = (image->width < width) ? (width - image->width) / 2 : 0;
            const int offset_y = (image->height < height) ? (height - image->height) / 2 : 0;
            render_region(image,
                          &mask,
                          source_x,
                          source_y,
                          visible_width,
                          visible_height,
                          x + offset_x,
                          y + offset_y,
                          visible_width,
                          visible_height,
                          has_tint,
                          tint_red,
                          tint_green,
                          tint_blue);
            break;
        }

        case ER_RESIZE_REPEAT:
        {
            /* Tile the image at original size across the destination rect. */
            for (int tile_y = 0; tile_y < height; tile_y += image->height)
            {
                const int tile_height = (tile_y + image->height <= height) ? image->height : (height - tile_y);
                for (int tile_x = 0; tile_x < width; tile_x += image->width)
                {
                    const int tile_width = (tile_x + image->width <= width) ? image->width : (width - tile_x);
                    render_region(image,
                                  &mask,
                                  0,
                                  0,
                                  tile_width,
                                  tile_height,
                                  x + tile_x,
                                  y + tile_y,
                                  tile_width,
                                  tile_height,
                                  has_tint,
                                  tint_red,
                                  tint_green,
                                  tint_blue);
                }
            }
            break;
        }
    }
}
