#include "image_scaler.h"
#include "image_registry.h"
#include "renderer_internal.h"
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
static uint32_t s_row_buf[ERUI_MAX_IMG_ROW_PIXELS];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

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

/**
 * @brief Renders a source crop of an image to a destination rectangle.
 *
 * Uses nearest-neighbor sampling. When the source and destination sizes match and no
 * tint is applied, the original buffer rows are blended directly without copying into
 * the row scratch buffer.
 *
 * @param[in] buf         Premultiplied ARGB8888 image data (row-major, img_w pixels wide).
 * @param[in] img_w       Total width of the source image in pixels.
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
static void render_region(const uint32_t* buf,
                          int img_w,
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

    const int img_stride = img_w * (int)sizeof(uint32_t);

    /* Fast path: source and destination regions are the same size and no tint is needed.
     * Blend directly from the source buffer rows using the image's own row stride. */
    if (!has_tint && src_w == dst_w && src_h == dst_h)
    {
        er_blit_blend(buf + (size_t)src_y * (size_t)img_w + (size_t)src_x, img_stride, 255, dst_x, dst_y, dst_w, dst_h);
        return;
    }

    /* General path: scale and/or tint via a one-row scratch buffer. */
    const int capped_w = (dst_w <= ERUI_MAX_IMG_ROW_PIXELS) ? dst_w : ERUI_MAX_IMG_ROW_PIXELS;
    for (int dy = 0; dy < dst_h; dy++)
    {
        const int sy = src_y + (dy * src_h) / dst_h;
        for (int dx = 0; dx < capped_w; dx++)
        {
            const int sx = src_x + (dx * src_w) / dst_w;
            uint32_t p = buf[(size_t)sy * (size_t)img_w + (size_t)sx];
            if (has_tint)
                p = apply_tint(p, tr, tg, tb);
            s_row_buf[dx] = p;
        }
        er_blit_blend(s_row_buf, capped_w * (int)sizeof(uint32_t), 255, dst_x, dst_y + dy, capped_w, 1);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_image_load(const char* name, const void* argb_buf, int w, int h)
{
    image_registry_store(name, argb_buf, w, h);
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
            render_region(img->buf, img->w, 0, 0, img->w, img->h, x, y, w, h, has_tint, tr, tg, tb);
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
            render_region(img->buf, img->w, crop_x, crop_y, crop_w, crop_h, x, y, w, h, has_tint, tr, tg, tb);
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
                img->buf, img->w, 0, 0, img->w, img->h, x + off_x, y + off_y, scaled_w, scaled_h, has_tint, tr, tg, tb);
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
                img->buf, img->w, src_x, src_y, vis_w, vis_h, x + off_x, y + off_y, vis_w, vis_h, has_tint, tr, tg, tb);
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
                        img->buf, img->w, 0, 0, tile_w, tile_h, x + tx, y + ty, tile_w, tile_h, has_tint, tr, tg, tb);
                }
            }
            break;
        }
    }
}
