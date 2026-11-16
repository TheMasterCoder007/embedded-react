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

/*
 * SPI-LCD render backend for small MCUs (RP2040-class) — single RGB565 framebuffer, dirty-rect flush.
 *
 * The pico-sdk counterpart of backends/esp32-spi-lcd, stripped further: no RTOS, no DMA bounce, no
 * banding — a 240x240 RGB565 framebuffer is only 112.5 KB, which the RP2040's 264 KB SRAM holds with
 * room for the engine's pools. The engine hands ARGB8888 sources (straight-alpha fills, premultiplied
 * copy/blend); fill/copy/blend composite over the framebuffer at full 8-bit precision, then present()
 * streams the dirty bounding box to the panel through two board-supplied callbacks (set_window +
 * write_pixels — see pico_spi_lcd_backend.h). Pixels are STORED byte-swapped (big-endian RGB565, the
 * MSB-first order SPI panels take), so rows go from framebuffer to wire untouched.
 *
 * No rotation (the panel's MADCTL handles orientation), no double-buffer, no overlay.
 */

#include "native_renderer.h"
#include "pico_spi_lcd_backend.h"

#include <stdlib.h>
#include <string.h>

/** @brief Full scale of an 8-bit color channel. */
#define CHANNEL_MAX 255U

/** @brief Added before dividing by CHANNEL_MAX so the result rounds to nearest. */
#define CHANNEL_ROUND 127U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 255U

/** @brief The alpha byte of an opaque ARGB8888 pixel. */
#define ARGB_OPAQUE_ALPHA 0xFF000000U

/**
 * @brief 1 = ordered dither where ARGB8888 is reduced to RGB565, so a slow colour change shows no steps.
 * 0 = truncate.
 */
#ifndef ER_LCD_DITHER
#define ER_LCD_DITHER 0
#endif

#define LCD_DITHER_ACTIVE ER_LCD_DITHER

/*----------------------------------------------------------------------------------------------------------------------
 - Pixel helpers (full-precision compositing; pack to / unpack from the panel's byte order)
 ---------------------------------------------------------------------------------------------------------------------*/

/*
 * fb_store packs ARGB8888 to RGB565 and byte-swaps so the framebuffer bytes are already in wire order
 * (SPI sends MSB first; MIPI-DCS 16-bit color is high byte first). fb_load is the exact inverse so
 * compositing read-back (alpha blends that sample the destination) stays correct.
 */
static inline uint16_t fb_store(uint32_t argb)
{
    const uint16_t rgb =
        (uint16_t)((((argb >> 16) & 0xF8U) << 8) | (((argb >> 8) & 0xFCU) << 3) | ((argb & 0xF8U) >> 3));
    return (uint16_t)__builtin_bswap16(rgb);
}

static inline uint32_t fb_load(uint16_t pixel)
{
    const uint16_t rgb = __builtin_bswap16(pixel);
    const uint32_t red5 = (rgb >> 11) & 0x1FU, green6 = (rgb >> 5) & 0x3FU, blue5 = rgb & 0x1FU;
    const uint32_t red = (red5 << 3) | (red5 >> 2), green = (green6 << 2) | (green6 >> 4),
                   blue = (blue5 << 3) | (blue5 >> 2);
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}

/*
 * Ordered dither for the 8-bit to RGB565 step. dither_argb() moves a pixel's channels so that the
 * plain truncating pack in fb_store() lands on the dithered level.
 *
 * Per channel: v - (v >> 5) + threshold for red and blue (threshold 0 to 7), v - (v >> 6) + threshold
 * for green (0 to 3). The subtraction keeps the sum within 8 bits, and takes a value fb_load() returned
 * back to its own level whatever the threshold, so pixels already in the framebuffer are stable.
 */
#if LCD_DITHER_ACTIVE

/** @brief Side of the threshold matrix, a power of two. */
#define DITHER_SIZE 4

/** @brief A 0 to 15 matrix entry as the three channel thresholds, in ARGB8888 channel positions. */
#define DITHER_CELL(level) ((((level) >> 1) << 16) | (((level) >> 2) << 8) | ((level) >> 1))

/** @brief The matrix entries with the lowest and the highest thresholds. */
#define DITHER_LOWEST DITHER_CELL(0U)
#define DITHER_HIGHEST DITHER_CELL(15U)

static const uint32_t k_dither[DITHER_SIZE][DITHER_SIZE] = {
    {DITHER_CELL(0U), DITHER_CELL(8U), DITHER_CELL(2U), DITHER_CELL(10U)},
    {DITHER_CELL(12U), DITHER_CELL(4U), DITHER_CELL(14U), DITHER_CELL(6U)},
    {DITHER_CELL(3U), DITHER_CELL(11U), DITHER_CELL(1U), DITHER_CELL(9U)},
    {DITHER_CELL(15U), DITHER_CELL(7U), DITHER_CELL(13U), DITHER_CELL(5U)},
};

/** @brief Applies packed channel thresholds to a pixel. */
static inline uint32_t dither_apply(uint32_t argb, uint32_t thresholds)
{
    return argb - ((argb >> 5) & 0x00070007U) - ((argb >> 6) & 0x00000300U) + thresholds;
}

/** @brief Dithers a pixel for its place on the screen, so a partial repaint lands on the same pattern. */
static inline uint32_t dither_argb(uint32_t argb, int screen_x, int screen_y)
{
    return dither_apply(argb, k_dither[screen_y & (DITHER_SIZE - 1)][screen_x & (DITHER_SIZE - 1)]);
}

#else

static inline uint32_t dither_argb(uint32_t argb, int screen_x, int screen_y)
{
    (void)screen_x;
    (void)screen_y;
    return argb;
}

#endif

/**
 * @brief Composites a premultiplied ARGB8888 source over an opaque ARGB8888 destination (8-bit precision).
 * out_rgb = src_rgb + dst_rgb * (255 - src_a) / 255.
 */
static inline uint32_t over_premul(uint32_t destination, uint32_t source_pixel)
{
    const uint32_t source_alpha = source_pixel >> 24;
    if (source_alpha == ALPHA_OPAQUE)
    {
        return ARGB_OPAQUE_ALPHA | (source_pixel & 0x00FFFFFFU);
    }
    if (source_alpha == 0U)
    {
        return destination;
    }
    const uint32_t inverse_alpha = CHANNEL_MAX - source_alpha;
    const uint32_t destination_red = (destination >> 16) & 0xFFU, destination_green = (destination >> 8) & 0xFFU,
                   destination_blue = destination & 0xFFU;
    const uint32_t source_red = (source_pixel >> 16) & 0xFFU, source_green = (source_pixel >> 8) & 0xFFU,
                   source_blue = source_pixel & 0xFFU;
    const uint32_t red = source_red + (destination_red * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t green = source_green + (destination_green * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t blue = source_blue + (destination_blue * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    ErPicoLcdPanelOps ops;
    int width;
    int height;
    uint16_t* framebuffer;  /**< Canonical framebuffer, panel byte order. */
    int dirty_x0, dirty_y0; /**< Dirty box, inclusive min corner. */
    int dirty_x_end,
        dirty_y_end;    /**< Dirty box, EXCLUSIVE max corner (matches ERRect); dirty_x_end <= dirty_x0 means empty. */
    bool first_present; /**< First present pushes the whole frame. */
} ErPicoLcdBackend;

static ErPicoLcdBackend s_lcd;

/** @brief Clamps a rect to the framebuffer; returns false if fully off-screen. */
static bool clip_rect(int* x, int* y, int* width, int* height)
{
    if (*x < 0)
    {
        *width += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *height += *y;
        *y = 0;
    }
    if (*x + *width > s_lcd.width)
    {
        *width = s_lcd.width - *x;
    }
    if (*y + *height > s_lcd.height)
    {
        *height = s_lcd.height - *y;
    }
    return (*width > 0 && *height > 0);
}

/** @brief Grows the dirty box to include an (already-clipped) rect. */
static void mark_dirty(int x, int y, int width, int height)
{
    if (x < s_lcd.dirty_x0)
        s_lcd.dirty_x0 = x;
    if (y < s_lcd.dirty_y0)
        s_lcd.dirty_y0 = y;
    if (x + width > s_lcd.dirty_x_end)
        s_lcd.dirty_x_end = x + width;
    if (y + height > s_lcd.dirty_y_end)
        s_lcd.dirty_y_end = y + height;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Backend callbacks
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Fills a rect with a straight-alpha ARGB8888 color, composited over the framebuffer. */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    if (!clip_rect(&x, &y, &width, &height))
    {
        return;
    }
    const uint32_t alpha = argb >> 24;
    const uint32_t premultiplied_red = (((argb >> 16) & 0xFFU) * alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t premultiplied_green = (((argb >> 8) & 0xFFU) * alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t premultiplied_blue = ((argb & 0xFFU) * alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t source_pixel =
        (alpha << 24) | (premultiplied_red << 16) | (premultiplied_green << 8) | premultiplied_blue;
#if LCD_DITHER_ACTIVE
    /* A colour between two RGB565 levels fills as a pattern of both. */
    const uint16_t opaque_pixel = fb_store(dither_apply(argb, DITHER_LOWEST));
    if (alpha == ALPHA_OPAQUE && fb_store(dither_apply(argb, DITHER_HIGHEST)) != opaque_pixel)
    {
        for (int row = 0; row < height; row++)
        {
            uint16_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
            uint16_t pattern[DITHER_SIZE]; /* the row's four pixels, starting at x */
            for (int phase = 0; phase < DITHER_SIZE; phase++)
            {
                pattern[phase] = fb_store(dither_argb(argb, x + phase, y + row));
            }
            int column = 0;
            for (; column + DITHER_SIZE <= width; column += DITHER_SIZE)
            {
                destination_row[column] = pattern[0];
                destination_row[column + 1] = pattern[1];
                destination_row[column + 2] = pattern[2];
                destination_row[column + 3] = pattern[3];
            }
            for (; column < width; column++)
            {
                destination_row[column] = pattern[column & (DITHER_SIZE - 1)];
            }
        }
        mark_dirty(x, y, width, height);
        return;
    }
#else
    const uint16_t opaque_pixel = fb_store(ARGB_OPAQUE_ALPHA | (argb & 0x00FFFFFFU));
#endif

    for (int row = 0; row < height; row++)
    {
        uint16_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
        if (alpha == ALPHA_OPAQUE)
        {
            for (int column = 0; column < width; column++)
            {
                destination_row[column] = opaque_pixel;
            }
        }
        else
        {
            for (int column = 0; column < width; column++)
            {
                destination_row[column] = fb_store(
                    dither_argb(over_premul(fb_load(destination_row[column]), source_pixel), x + column, y + row));
            }
        }
    }
    mark_dirty(x, y, width, height);
}

/** @brief Copies a premultiplied ARGB8888 buffer over the framebuffer. */
static void copy_cb(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const int original_x = x, original_y = y;
    if (!clip_rect(&x, &y, &width, &height))
    {
        return;
    }
    const int skip_x = x - original_x;
    const int skip_y = y - original_y;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)((const uint8_t*)source + (size_t)(skip_y + row) * source_stride_bytes) + skip_x;
        uint16_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
        for (int column = 0; column < width; column++)
        {
            destination_row[column] = fb_store(
                dither_argb(over_premul(fb_load(destination_row[column]), source_row[column]), x + column, y + row));
        }
    }
    mark_dirty(x, y, width, height);
}

/** @brief Copies a KNOWN-FULLY-OPAQUE source in its native format into the framebuffer (replace).
 *
 *  The engine only routes buffers its registration-time opacity scan proved opaque (RGB565 is opaque
 *  by construction), so pixels replace outright: no fb_load read-back and no over_premul math. The
 *  framebuffer stores wire-order (byte-swapped) RGB565, so a 565 source is one bswap16 per pixel — far cheaper
 *  than the blend path's load/composite/store, which is what makes 16-bit-baked backgrounds cheap on
 *  an RP2040-class CPU. */
static void copy_fmt_cb(
    const void* source, int source_stride_bytes, ERImageFormat format, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    const int original_x = x, original_y = y;
    if (!clip_rect(&x, &y, &width, &height))
    {
        return;
    }
    const int skip_x = x - original_x;
    const int skip_y = y - original_y;
    if (format == ER_IMAGE_RGB565)
    {
        for (int row = 0; row < height; row++)
        {
            const uint16_t* source_row =
                (const uint16_t*)((const uint8_t*)source + (size_t)(skip_y + row) * source_stride_bytes) + skip_x;
            uint16_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
            for (int column = 0; column < width; column++)
            {
                destination_row[column] = (uint16_t)__builtin_bswap16(source_row[column]);
            }
        }
    }
    else
    {
        for (int row = 0; row < height; row++)
        {
            const uint32_t* source_row =
                (const uint32_t*)((const uint8_t*)source + (size_t)(skip_y + row) * source_stride_bytes) + skip_x;
            uint16_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
            for (int column = 0; column < width; column++)
            {
                /* opaque contract: no alpha inspection */
                destination_row[column] = fb_store(dither_argb(source_row[column], x + column, y + row));
            }
        }
    }
    mark_dirty(x, y, width, height);
}

/** @brief Blends a premultiplied ARGB8888 buffer over the framebuffer at a global alpha. */
static void
blend_cb(const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    if (alpha == 0U)
    {
        return;
    }
    const int original_x = x, original_y = y;
    if (!clip_rect(&x, &y, &width, &height))
    {
        return;
    }
    const int skip_x = x - original_x;
    const int skip_y = y - original_y;
    const uint32_t global_alpha = alpha;
    for (int row = 0; row < height; row++)
    {
        const uint32_t* source_row =
            (const uint32_t*)((const uint8_t*)source + (size_t)(skip_y + row) * source_stride_bytes) + skip_x;
        uint16_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
        if (global_alpha == ALPHA_OPAQUE)
        {
            /* Scaling by 255/255 is the identity, so take the row as it stands. Worth its own loop
             * because the engine's row emitters — the vector AA rows, gradients, shadows, transforms —
             * all blend at 255: otherwise every one of their pixels pays four multiply-and-divides
             * that cannot change its value, and an opaque one pays a framebuffer read it never uses. */
            for (int column = 0; column < width; column++)
            {
                const uint32_t source_pixel = source_row[column];
                const uint32_t source_alpha = source_pixel >> 24;
                if (source_alpha == 0U)
                    continue; /* transparent: the destination is left alone */
                const uint32_t blended = (source_alpha == ALPHA_OPAQUE)
                                             ? source_pixel
                                             : over_premul(fb_load(destination_row[column]), source_pixel);
                destination_row[column] = fb_store(dither_argb(blended, x + column, y + row));
            }
            continue;
        }
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = ((source_pixel >> 24) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            const uint32_t source_red = (((source_pixel >> 16) & 0xFFU) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            const uint32_t source_green = (((source_pixel >> 8) & 0xFFU) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            const uint32_t source_blue = ((source_pixel & 0xFFU) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            destination_row[column] = fb_store(
                dither_argb(over_premul(fb_load(destination_row[column]),
                                        (source_alpha << 24) | (source_red << 16) | (source_green << 8) | source_blue),
                            x + column,
                            y + row));
        }
    }
    mark_dirty(x, y, width, height);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Present + init
 ---------------------------------------------------------------------------------------------------------------------*/

void er_pico_spi_lcd_present(void)
{
    int x0, y0, x1, y1;
    if (s_lcd.first_present)
    {
        x0 = 0;
        y0 = 0;
        x1 = s_lcd.width - 1;
        y1 = s_lcd.height - 1;
        s_lcd.first_present = false;
    }
    else if (s_lcd.dirty_y_end > s_lcd.dirty_y0 && s_lcd.dirty_x_end > s_lcd.dirty_x0)
    {
        /* set_window takes an INCLUSIVE panel window (CASET/RASET), so convert on the way out. */
        x0 = s_lcd.dirty_x0;
        y0 = s_lcd.dirty_y0;
        x1 = s_lcd.dirty_x_end - 1;
        y1 = s_lcd.dirty_y_end - 1;
    }
    else
    {
        return; /* nothing changed */
    }

    s_lcd.ops.set_window(x0, y0, x1, y1);
    const int box_width = x1 - x0 + 1;
    if (box_width == s_lcd.width)
    {
        /* Full-width box: the rows are one contiguous run in the framebuffer — a single write. */
        s_lcd.ops.write_pixels(s_lcd.framebuffer + (size_t)y0 * s_lcd.width, (size_t)box_width * (y1 - y0 + 1));
    }
    else
    {
        for (int row = y0; row <= y1; row++)
        {
            s_lcd.ops.write_pixels(s_lcd.framebuffer + (size_t)row * s_lcd.width + x0, (size_t)box_width);
        }
    }

    s_lcd.dirty_x0 = s_lcd.width;
    s_lcd.dirty_y0 = s_lcd.height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
}

bool er_pico_spi_lcd_backend_init(const ErPicoLcdPanelOps* ops, int width, int height)
{
    if (!ops || !ops->set_window || !ops->write_pixels || width <= 0 || height <= 0)
    {
        return false;
    }
    s_lcd.ops = *ops;
    s_lcd.width = width;
    s_lcd.height = height;
    s_lcd.dirty_x0 = width;
    s_lcd.dirty_y0 = height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
    s_lcd.first_present = true;

    const size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    s_lcd.framebuffer = (uint16_t*)malloc(bytes);
    if (!s_lcd.framebuffer)
    {
        return false;
    }
    memset(s_lcd.framebuffer, 0, bytes); /* opaque black (0x0000 byte-swapped is still 0) */

    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.copy_rect_format = copy_fmt_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL;
    backend.context = NULL;
    embedded_renderer_set_backend(&backend);
    return true;
}
