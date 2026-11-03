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

#include "software_backend.h"

#include "native_renderer.h"

#include <stdlib.h>
#include <string.h>

/** @brief Full scale of an 8-bit color channel. */
#define CHANNEL_MAX 255U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 255U

/** @brief Added before the divide in div255() so it rounds to nearest. */
#define DIV255_ROUND 0x80U

/** @brief Opaque black, the color a freshly-cleared framebuffer holds. */
#define OPAQUE_BLACK 0xFF000000U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal state for the software render backend.
 */
typedef struct
{
    uint32_t* framebuffer;  /**< ARGB8888 framebuffer (0xAARRGGBB), framebuffer_width * framebuffer_height pixels,
                               row-major. */
    int framebuffer_width;  /**< Framebuffer width in pixels. */
    int framebuffer_height; /**< Framebuffer height in pixels. */
} SoftCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static SoftCtx s_ctx;
static EmbeddedRenderBackend s_backend;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Rounds (x / 255) to nearest for x in [0, 65025], without a divide. */
static inline uint32_t div255(uint32_t value)
{
    value += DIV255_ROUND;
    return (value + (value >> 8)) >> 8;
}

/**
 * @brief Composites one premultiplied-ARGB source pixel over an opaque destination pixel (Porter-Duff
 *        source-over).
 *
 * The framebuffer is kept opaque, so only the color channels are blended and the result alpha stays 0xFF.
 *
 * @param[in] destination  Destination pixel (opaque ARGB8888).
 * @param[in] source_alpha   Source alpha (premultiply coverage).
 * @param[in] source_red   Premultiplied source red.
 * @param[in] source_green   Premultiplied source green.
 * @param[in] source_blue   Premultiplied source blue.
 *
 * @return The blended opaque ARGB8888 pixel.
 */
static inline uint32_t over_premul(
    uint32_t destination, uint32_t source_alpha, uint32_t source_red, uint32_t source_green, uint32_t source_blue)
{
    const uint32_t inverse_alpha = CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (destination >> 16) & 0xFFU;
    const uint32_t destination_green = (destination >> 8) & 0xFFU;
    const uint32_t destination_blue = destination & 0xFFU;
    const uint32_t red = source_red + div255(destination_red * inverse_alpha);
    const uint32_t green = source_green + div255(destination_green * inverse_alpha);
    const uint32_t blue = source_blue + div255(destination_blue * inverse_alpha);
    return 0xFF000000U | (red << 16) | (green << 8) | blue;
}

/**
 * @brief Clips a rectangle to the framebuffer bounds, advancing the source origin to match.
 *
 * @param[in,out] x   Left edge (screen space); clamped on return.
 * @param[in,out] y   Top edge (screen space); clamped on return.
 * @param[in,out] width   Width; reduced on return.
 * @param[in,out] height   Height; reduced on return.
 * @param[out]    source_x  Source column offset introduced by left/top clipping (NULL to ignore).
 * @param[out]    source_y  Source row offset introduced by left/top clipping (NULL to ignore).
 *
 * @return true if any pixels remain after clipping; false if fully off-screen.
 */
static bool clip_rect(int* x, int* y, int* width, int* height, int* source_x, int* source_y)
{
    int clipped_x = *x, clipped_y = *y, clipped_width = *width, clipped_height = *height, offset_x = 0, offset_y = 0;

    if (clipped_width <= 0 || clipped_height <= 0)
        return false;
    if (clipped_x < 0)
    {
        offset_x = -clipped_x;
        clipped_width += clipped_x;
        clipped_x = 0;
    }
    if (clipped_y < 0)
    {
        offset_y = -clipped_y;
        clipped_height += clipped_y;
        clipped_y = 0;
    }
    if (clipped_x + clipped_width > s_ctx.framebuffer_width)
        clipped_width = s_ctx.framebuffer_width - clipped_x;
    if (clipped_y + clipped_height > s_ctx.framebuffer_height)
        clipped_height = s_ctx.framebuffer_height - clipped_y;
    if (clipped_width <= 0 || clipped_height <= 0)
        return false;

    *x = clipped_x;
    *y = clipped_y;
    *width = clipped_width;
    *height = clipped_height;
    if (source_x)
        *source_x = offset_x;
    if (source_y)
        *source_y = offset_y;
    return true;
}

/**
 * @brief Fills a solid-color rectangle (straight-alpha ARGB8888) into the framebuffer.
 *
 * Opaque fills are written directly; translucent fills are source-over composited.
 *
 * @param[in] argb  Straight-alpha ARGB8888 color.
 * @param[in] x     Left edge.
 * @param[in] y     Top edge.
 * @param[in] width     Width in pixels.
 * @param[in] height     Height in pixels.
 * @param[in] ctx   Pointer to the SoftCtx.
 */
static void fill_rect_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    SoftCtx* context = ctx;
    const uint32_t alpha = (argb >> 24) & 0xFFU;
    if (alpha == 0U || !clip_rect(&x, &y, &width, &height, NULL, NULL))
        return;

    if (alpha == ALPHA_OPAQUE)
    {
        const uint32_t pixel = 0xFF000000U | (argb & 0x00FFFFFFU);
        for (int row = 0; row < height; row++)
        {
            uint32_t* destination_row = context->framebuffer + (size_t)(y + row) * context->framebuffer_width + x;
            for (int column = 0; column < width; column++)
                destination_row[column] = pixel;
        }
        return;
    }

    /* Translucent: premultiply the straight-alpha source once, then source-over each pixel. */
    const uint32_t source_red = div255(((argb >> 16) & 0xFFU) * alpha);
    const uint32_t source_green = div255(((argb >> 8) & 0xFFU) * alpha);
    const uint32_t source_blue = div255((argb & 0xFFU) * alpha);
    for (int row = 0; row < height; row++)
    {
        uint32_t* destination_row = context->framebuffer + (size_t)(y + row) * context->framebuffer_width + x;
        for (int column = 0; column < width; column++)
            destination_row[column] =
                over_premul(destination_row[column], alpha, source_red, source_green, source_blue);
    }
}

/**
 * @brief Copies a premultiplied ARGB8888 buffer into the framebuffer (source-over).
 *
 * @param[in] source              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] source_stride_bytes Row stride of source in bytes.
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] width                Width of the region in pixels.
 * @param[in] height                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SoftCtx.
 */
static void copy_rect_cb(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    SoftCtx* context = ctx;
    int source_x = 0, source_y = 0;
    if (!source || !clip_rect(&x, &y, &width, &height, &source_x, &source_y))
        return;

    const uint8_t* source_bytes = (const uint8_t*)source;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)(source_bytes + (size_t)(source_y + row) * source_stride_bytes) + source_x;
        uint32_t* destination_row = context->framebuffer + (size_t)(y + row) * context->framebuffer_width + x;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = (source_pixel >> 24) & 0xFFU;
            if (source_alpha == ALPHA_OPAQUE)
                destination_row[column] = source_pixel; /* opaque source replaces the destination */
            else if (source_alpha != 0U)
                destination_row[column] = over_premul(destination_row[column],
                                                      source_alpha,
                                                      (source_pixel >> 16) & 0xFFU,
                                                      (source_pixel >> 8) & 0xFFU,
                                                      source_pixel & 0xFFU);
        }
    }
}

/**
 * @brief Blends a premultiplied ARGB8888 buffer into the framebuffer at a global alpha.
 *
 * Each source channel (including alpha) is scaled by alpha/255, preserving the premultiplied invariant, then
 * source-over composited — matching the engine's blend formula used by the other backends.
 *
 * @param[in] source              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] source_stride_bytes Row stride of source in bytes.
 * @param[in] alpha            Global opacity (0 = transparent, 255 = opaque).
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] width                Width of the region in pixels.
 * @param[in] height                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SoftCtx.
 */
static void blend_rect_cb(
    const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    if (alpha == ALPHA_OPAQUE)
    {
        /* Nothing to scale — by 255/255 is the identity — and copy_rect_cb is the same source-over
         * with an opaque-pixel shortcut on top. Worth the redirect because the engine's own row
         * emitters (the vector AA rows, gradients, shadows, transforms) all blend at 255: without it
         * every one of their pixels pays four multiply-and-divides that cannot change its value.
         * Runs BEFORE the clip so copy_rect_cb still gets the unclipped rect to offset its source by. */
        copy_rect_cb(source, source_stride_bytes, x, y, width, height, ctx);
        return;
    }

    SoftCtx* context = ctx;
    int source_x = 0, source_y = 0;
    const uint32_t global_alpha = alpha;
    if (!source || global_alpha == 0U || !clip_rect(&x, &y, &width, &height, &source_x, &source_y))
        return;

    const uint8_t* source_bytes = (const uint8_t*)source;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)(source_bytes + (size_t)(source_y + row) * source_stride_bytes) + source_x;
        uint32_t* destination_row = context->framebuffer + (size_t)(y + row) * context->framebuffer_width + x;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = div255(((source_pixel >> 24) & 0xFFU) * global_alpha);
            if (source_alpha == 0U)
                continue;
            const uint32_t source_red = div255(((source_pixel >> 16) & 0xFFU) * global_alpha);
            const uint32_t source_green = div255(((source_pixel >> 8) & 0xFFU) * global_alpha);
            const uint32_t source_blue = div255((source_pixel & 0xFFU) * global_alpha);
            destination_row[column] =
                over_premul(destination_row[column], source_alpha, source_red, source_green, source_blue);
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_software_backend_init(int framebuffer_width, int framebuffer_height)
{
    if (framebuffer_width <= 0 || framebuffer_height <= 0)
        return false;

    s_ctx.framebuffer = malloc((size_t)framebuffer_width * (size_t)framebuffer_height * sizeof(uint32_t));
    if (!s_ctx.framebuffer)
        return false;

    s_ctx.framebuffer_width = framebuffer_width;
    s_ctx.framebuffer_height = framebuffer_height;
    er_software_clear(OPAQUE_BLACK); /* opaque black, like a freshly-cleared panel */

    s_backend.fill_rect = fill_rect_cb;
    s_backend.copy_rect = copy_rect_cb;
    s_backend.blend_rect = blend_rect_cb;
    s_backend.wait = NULL;
    s_backend.frame_ready = NULL;
    s_backend.ctx = &s_ctx;
    s_backend.band_height = 0; /* full-framebuffer path */
    s_backend.band_begin = NULL;
    s_backend.band_flush = NULL;

    embedded_renderer_set_backend(&s_backend);
    return true;
}

void er_software_backend_destroy(void)
{
    free(s_ctx.framebuffer);
    s_ctx.framebuffer = NULL;
    s_ctx.framebuffer_width = 0;
    s_ctx.framebuffer_height = 0;
}

bool er_software_backend_resize(int framebuffer_width, int framebuffer_height)
{
    if (framebuffer_width <= 0 || framebuffer_height <= 0)
        return false;

    uint32_t* resized =
        realloc(s_ctx.framebuffer, (size_t)framebuffer_width * (size_t)framebuffer_height * sizeof(uint32_t));
    if (!resized)
        return false; /* old framebuffer is left intact on failure */

    s_ctx.framebuffer = resized;
    s_ctx.framebuffer_width = framebuffer_width;
    s_ctx.framebuffer_height = framebuffer_height;
    er_software_clear(OPAQUE_BLACK);
    return true;
}

void er_software_clear(uint32_t argb)
{
    if (!s_ctx.framebuffer)
        return;
    const size_t pixel_count = (size_t)s_ctx.framebuffer_width * (size_t)s_ctx.framebuffer_height;
    for (size_t index = 0; index < pixel_count; index++)
        s_ctx.framebuffer[index] = argb;
}

uint32_t* er_software_framebuffer(void)
{
    return s_ctx.framebuffer;
}

int er_software_fb_width(void)
{
    return s_ctx.framebuffer_width;
}

int er_software_fb_height(void)
{
    return s_ctx.framebuffer_height;
}
