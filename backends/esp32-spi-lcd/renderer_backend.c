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
 * Lean SPI-LCD render backend — single internal-RAM framebuffer, full-width band flush.
 *
 * The no-PSRAM counterpart to backends/esp32-lcd (which keeps its buffers in PSRAM and supports RGB
 * double-buffering/rotation). Stripped to what a plain ESP32-WROOM-32 with only internal DRAM affords:
 *
 *   - ONE canonical framebuffer in internal RAM. The engine hands ARGB8888 sources (straight-alpha
 *     fills, premultiplied copy/blend); fill/copy/blend composite over the framebuffer in full 8-bit precision.
 *     PIXEL FORMAT is selectable (ER_SPI_LCD_FB8): RGB565 (2 B/px, default) or RGB332 (1 B/px). On the
 *     original ESP32 the internal DRAM is FRAGMENTED — the largest contiguous block is ~110 KB — so a
 *     full 240x320 RGB565 framebuffer (150 KB) does NOT fit; RGB332 (75 KB) does. Quality drops (256 colors,
 *     banding on anti-aliased text) but it renders. RGB565 is for boards with a big enough block.
 *   - Present pushes the dirty box as FULL-WIDTH bands through a SMALL DMA bounce buffer (RGB565). The
 *     band's rows are contiguous in the framebuffer, so each chunk is one memcpy/convert into the bounce, then
 *     draw_bitmap DMAs it. The big framebuffer itself need not be DMA-able.
 *
 * No rotation (the panel's MADCTL handles orientation), no double-buffer, no overlay.
 */

#include "esp32_spi_lcd_backend.h"
#include "native_renderer.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

/** @brief 0 = RGB565 canonical framebuffer (2 B/px); 1 = RGB332 (1 B/px — fits a small internal-RAM block). */
#ifndef ER_SPI_LCD_FB8
#define ER_SPI_LCD_FB8 0
#endif

/** @brief Rows per DMA bounce chunk. The big framebuffer needn't be DMA-able — only this small buffer. */
#ifndef ER_SPI_LCD_BOUNCE_ROWS
#define ER_SPI_LCD_BOUNCE_ROWS 24
#endif

/**
 * @brief Banded rendering: render in horizontal strips through a small RGB565 band buffer instead of a
 * full-screen canonical framebuffer. The panel's own GRAM retains everything outside the dirty rows, so
 * only ER_LCD_BANDED_ROWS rows live in RAM at once — the way a no-PSRAM panel affords full 16-bit color.
 * The engine drives the strips (band_begin/band_flush below); each strip is full screen width so the
 * band buffer flushes straight to the panel via one draw_bitmap (no bounce). Implies RGB565 — the whole
 * point is 16-bit color — so ER_SPI_LCD_FB8 is ignored when banded.
 */
#ifndef ER_LCD_BANDED
#define ER_LCD_BANDED 0
#endif

/** @brief Rows per band buffer when ER_LCD_BANDED is enabled (RAM = screen width * rows * 2 bytes). */
#ifndef ER_LCD_BANDED_ROWS
#define ER_LCD_BANDED_ROWS 40
#endif

/** @brief RGB332 canonical pixel only applies to the full-framebuffer FB8 path; banded is always RGB565. */
#if ER_SPI_LCD_FB8 && !ER_LCD_BANDED
#define ER_SPI_LCD_FB332 1
#else
#define ER_SPI_LCD_FB332 0
#endif

/**
 * @brief Panel color order for the RGB565 path: 1 = byte-swap + BGR, 0 = standard RGB565.
 *
 * Some SPI panels display a stored RGB565 word as BGR(byteswap(word)) rather than the standard layout —
 * notably the DIYmall "Cheap Yellow Display" ST7789, which wants the two color bytes swapped AND red/blue
 * in BGR order. Set this per board (the CYD example does). Has no effect on the RGB332 path.
 */
#ifndef ER_SPI_LCD_SWAP_BGR
#define ER_SPI_LCD_SWAP_BGR 0
#endif

static const char* TAG = "er-spi-lcd";

/** @brief Full scale of an 8-bit color channel. */
#define CHANNEL_MAX 255U

/** @brief Added before dividing by CHANNEL_MAX so the result rounds to nearest. */
#define CHANNEL_ROUND 127U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 255U

/** @brief The alpha byte of an opaque ARGB8888 pixel. */
#define ARGB_OPAQUE_ALPHA 0xFF000000U

/** @brief Band buffers in banded mode: one composites while the other transfers. */
#define BAND_BANK_COUNT 2

/** @brief Bytes in a kilobyte, for the log lines. */
#define BYTES_PER_KB 1024

/*----------------------------------------------------------------------------------------------------------------------
 - Canonical-pixel helpers (full-precision compositing; pack to the canonical format; convert to RGB565)
 ---------------------------------------------------------------------------------------------------------------------*/

#if ER_SPI_LCD_FB332

typedef uint8_t fbpx_t; /**< RGB332: RRRGGGBB. */

static inline fbpx_t fb_store(uint32_t argb)
{
    return (uint8_t)(((argb >> 16) & 0xE0U) | (((argb >> 8) & 0xE0U) >> 3) | ((argb & 0xC0U) >> 6));
}
static inline uint32_t fb_load(fbpx_t pixel)
{
    uint32_t red = pixel & 0xE0U;
    uint32_t green = (pixel << 3) & 0xE0U;
    uint32_t blue = (pixel << 6) & 0xC0U;
    red |= red >> 3;
    red |= red >> 6; /* replicate to fill 8 bits */
    green |= green >> 3;
    green |= green >> 6;
    blue |= blue >> 2;
    blue |= blue >> 4;
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}
/** @brief Converts a row of canonical pixels to RGB565 (the panel format). */
static inline void fb_to_565_row(uint16_t* destination, const fbpx_t* source, int count)
{
    for (int index = 0; index < count; index++)
    {
        const uint32_t argb = fb_load(source[index]);
        destination[index] =
            (uint16_t)((((argb >> 16) & 0xF8U) << 8) | (((argb >> 8) & 0xFCU) << 3) | ((argb & 0xF8U) >> 3));
    }
}

#else /* RGB565 canonical */

typedef uint16_t fbpx_t;

/*
 * fb_store / fb_load pack to (and unpack from) the panel's pixel order. With ER_SPI_LCD_SWAP_BGR the
 * stored word is pre-compensated for a panel that displays it as BGR(byteswap(word)) — pack BGR
 * (B5 [15:11], G6 [10:5], R5 [4:0]) then byte-swap — so the panel's transform lands on the intended
 * color. Otherwise standard RGB565. fb_load is the exact inverse of fb_store so compositing read-back
 * (alpha blends that sample the destination) stays correct.
 */
#if ER_SPI_LCD_SWAP_BGR
static inline fbpx_t fb_store(uint32_t argb)
{
    const uint16_t bgr = (uint16_t)(((argb & 0xF8U) << 8)             /* B5 -> [15:11] */
                                    | (((argb >> 8) & 0xFCU) << 3)    /* G6 -> [10:5]  */
                                    | (((argb >> 16) & 0xF8U) >> 3)); /* R5 -> [4:0]   */
    return (fbpx_t)__builtin_bswap16(bgr);
}
static inline uint32_t fb_load(fbpx_t pixel)
{
    const uint16_t bgr = __builtin_bswap16((uint16_t)pixel);
    const uint32_t blue5 = (bgr >> 11) & 0x1FU, green6 = (bgr >> 5) & 0x3FU, red5 = bgr & 0x1FU;
    const uint32_t red = (red5 << 3) | (red5 >> 2), green = (green6 << 2) | (green6 >> 4),
                   blue = (blue5 << 3) | (blue5 >> 2);
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}
#else
static inline fbpx_t fb_store(uint32_t argb)
{
    return (uint16_t)((((argb >> 16) & 0xF8U) << 8) | (((argb >> 8) & 0xFCU) << 3) | ((argb & 0xF8U) >> 3));
}
static inline uint32_t fb_load(fbpx_t pixel)
{
    const uint32_t red5 = (pixel >> 11) & 0x1FU, green6 = (pixel >> 5) & 0x3FU, blue5 = pixel & 0x1FU;
    const uint32_t red = (red5 << 3) | (red5 >> 2), green = (green6 << 2) | (green6 >> 4),
                   blue = (blue5 << 3) | (blue5 >> 2);
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}
#endif
static inline void fb_to_565_row(uint16_t* destination, const fbpx_t* source, int count)
{
    memcpy(destination, source, (size_t)count * sizeof(uint16_t)); /* already in the panel's pixel order */
}

#endif

#if ER_SPI_LCD_FB332 || ER_SPI_LCD_SWAP_BGR
/** @brief Repacks a standard RGB565 word (R5 [15:11], G6 [10:5], B5 [4:0]) into the framebuffer's stored form. */
static inline fbpx_t fb_store_565(uint16_t rgb)
{
#if ER_SPI_LCD_FB332
    /* Keep the top 3/3/2 bits of each channel. */
    return (fbpx_t)(((rgb >> 8) & 0xE0U) | ((rgb >> 6) & 0x1CU) | ((rgb >> 3) & 0x03U));
#else
    /* Swap red/blue then byte-swap — the same pre-compensation fb_store applies (see above). */
    const uint16_t bgr = (uint16_t)(((rgb & 0x001FU) << 11) | (rgb & 0x07E0U) | ((rgb >> 11) & 0x1FU));
    return (fbpx_t)__builtin_bswap16(bgr);
#endif
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
    esp_lcd_panel_handle_t panel;
    int width;
    int height;
    int framebuffer_rows; /**< Rows in the canonical framebuffer: height (full-screen) or ER_LCD_BANDED_ROWS (banded).
                           */
    fbpx_t* framebuffer;  /**< Canonical framebuffer (full-framebuffer path) or the band buffer CURRENTLY being
                      composited (banded path = bank[current_bank]). All draw callbacks target this. */
    uint16_t* bounce;     /**< Small DMA-capable RGB565 staging buffer (full-framebuffer path only; NULL banded). */
    SemaphoreHandle_t transfer_done; /**< Given by the panel's color-trans-done ISR; waited on before buffer reuse. */
    int dirty_x0, dirty_y0;          /**< Dirty box, inclusive min corner. */
    int dirty_x_end,
        dirty_y_end;    /**< Dirty box, EXCLUSIVE max corner (matches ERRect); dirty_x_end <= dirty_x0 means empty. */
    bool first_present; /**< First present pushes the whole frame. */
    int strip_x, strip_y, strip_width,
        strip_height;              /**< Banded: screen-space rect of the strip currently being composited. */
    fbpx_t* bank[BAND_BANK_COUNT]; /**< Banded: two ping-pong band buffers — compose the next strip into one while
                          the panel DMAs the previous from the other (overlaps CPU + SPI). */
    int current_bank;              /**< Banded: index of the bank currently being composited (0/1). */
    bool strip_in_flight;          /**< Banded: a strip's draw_bitmap DMA is outstanding (must be waited on). */
} ERSpiLcdBackend;

static ERSpiLcdBackend s_lcd;

/** @brief Panel color-transfer-done ISR: signals that a band's DMA finished and the bounce is free. */
static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* event_data, void* ctx)
{
    (void)io;
    (void)event_data;
    (void)ctx;
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd.transfer_done, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

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
    if (*y + *height > s_lcd.framebuffer_rows)
    {
        *height = s_lcd.framebuffer_rows - *y;
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
    const fbpx_t opaque_pixel = fb_store(ARGB_OPAQUE_ALPHA | (argb & 0x00FFFFFFU));

    for (int row = 0; row < height; row++)
    {
        fbpx_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
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
                destination_row[column] = fb_store(over_premul(fb_load(destination_row[column]), source_pixel));
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
        fbpx_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
        for (int column = 0; column < width; column++)
        {
            destination_row[column] = fb_store(over_premul(fb_load(destination_row[column]), source_row[column]));
        }
    }
    mark_dirty(x, y, width, height);
}

/** @brief Copies a KNOWN-FULLY-OPAQUE source in its native format into the framebuffer (replace).
 *
 *  The engine only routes buffers its registration-time opacity scan proved opaque (RGB565 is opaque
 *  by construction), so pixels replace outright: no fb_load read-back and no over_premul math — the
 *  fast path that makes 16-bit-baked backgrounds cheap on a blend-bound panel. An RGB565 source row
 *  is a straight memcpy when the framebuffer stores standard RGB565; with ER_SPI_LCD_SWAP_BGR or RGB332 it is
 *  a 16-bit repack per pixel — still far cheaper than the blend path's load/composite/store. */
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
            fbpx_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
#if ER_SPI_LCD_FB332 || ER_SPI_LCD_SWAP_BGR
            for (int column = 0; column < width; column++)
            {
                destination_row[column] = fb_store_565(source_row[column]);
            }
#else
            memcpy(destination_row, source_row, (size_t)width * sizeof(fbpx_t));
#endif
        }
    }
    else
    {
        for (int row = 0; row < height; row++)
        {
            const uint32_t* source_row =
                (const uint32_t*)((const uint8_t*)source + (size_t)(skip_y + row) * source_stride_bytes) + skip_x;
            fbpx_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
            for (int column = 0; column < width; column++)
            {
                destination_row[column] = fb_store(source_row[column]); /* opaque contract: no alpha inspection */
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
        fbpx_t* destination_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
        for (int column = 0; column < width; column++)
        {
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = ((source_pixel >> 24) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            const uint32_t source_red = (((source_pixel >> 16) & 0xFFU) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            const uint32_t source_green = (((source_pixel >> 8) & 0xFFU) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            const uint32_t source_blue = ((source_pixel & 0xFFU) * global_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
            destination_row[column] =
                fb_store(over_premul(fb_load(destination_row[column]),
                                     (source_alpha << 24) | (source_red << 16) | (source_green << 8) | source_blue));
        }
    }
    mark_dirty(x, y, width, height);
}

#if ER_LCD_BANDED
/**
 * @brief Begins a strip: records its screen rect and clears the band buffer rows it will fill.
 *
 * The engine emits the strip's fill/copy/blend ops with band-local Y, so they composite into the band
 * buffer; band_flush_cb() then DMAs it to the panel. Strips are full screen width (strip_x = 0, strip_width = width),
 * so the band buffer's first strip_height rows are tightly packed and flush directly without a bounce buffer.
 *
 * @param[in] x    Strip left edge in screen space (0).
 * @param[in] y    Strip top edge in screen space.
 * @param[in] width    Strip width in pixels (screen width).
 * @param[in] height    Strip height in pixels (<= ER_LCD_BANDED_ROWS).
 * @param[in] ctx  Unused.
 */
static void band_begin_cb(int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    s_lcd.framebuffer = s_lcd.bank[s_lcd.current_bank];
    s_lcd.strip_x = x;
    s_lcd.strip_y = y;
    s_lcd.strip_width = width;
    s_lcd.strip_height = height;
    for (int row = 0; row < height; row++)
    {
        memset(s_lcd.framebuffer + (size_t)row * s_lcd.width + x, 0, (size_t)width * sizeof(fbpx_t)); /* opaque black */
    }
}

/**
 * @brief Flushes the current strip to the panel WITHOUT blocking, overlapping its DMA with the next strip.
 *
 * Ping-pong: we first wait for the PREVIOUS strip's DMA (on the other bank) to finish — by now the engine
 * has spent the whole compositing of THIS strip overlapping it, so the wait is usually instant — which
 * frees that bank for the next strip. We then kick off this strip's draw_bitmap (async; the band buffer is
 * RGB565 + DMA-capable so it goes straight to the panel) and flip `current_bank` so the next band_begin composites
 * into the now-free bank while this one transfers. Net: repaint time ≈ max(compose, transfer) per strip,
 * not their sum — the visible top-to-bottom "wave" becomes a single smooth sweep instead of a stepped one.
 *
 * @param[in] ctx  Unused.
 */
static void band_flush_cb(void* ctx)
{
    (void)ctx;
    if (s_lcd.strip_width <= 0 || s_lcd.strip_height <= 0)
    {
        return;
    }
    if (s_lcd.strip_in_flight)
    {
        xSemaphoreTake(s_lcd.transfer_done, portMAX_DELAY); /* previous strip's DMA done → its bank is free to reuse */
    }
    esp_lcd_panel_draw_bitmap(s_lcd.panel,
                              s_lcd.strip_x,
                              s_lcd.strip_y,
                              s_lcd.strip_x + s_lcd.strip_width,
                              s_lcd.strip_y + s_lcd.strip_height,
                              s_lcd.framebuffer);
    s_lcd.strip_in_flight = true;
    s_lcd.current_bank ^= 1; /* next strip composites into the other bank while this one DMAs */
}
#endif /* ER_LCD_BANDED */

/*----------------------------------------------------------------------------------------------------------------------
 - Present + init
 ---------------------------------------------------------------------------------------------------------------------*/

void er_esp32_spi_lcd_present(void)
{
#if ER_LCD_BANDED
    /* Banded mode flushes each strip inside band_flush() during er_commit(); nothing to do per frame. */
    return;
#else
    int y0, y1;
    if (s_lcd.first_present)
    {
        y0 = 0;
        y1 = s_lcd.height - 1;
        s_lcd.first_present = false;
    }
    else if (s_lcd.dirty_y_end > s_lcd.dirty_y0 && s_lcd.dirty_x_end > s_lcd.dirty_x0)
    {
        /* The row loop below is inclusive of y1, so convert off the exclusive end here. */
        y0 = s_lcd.dirty_y0;
        y1 = s_lcd.dirty_y_end - 1;
    }
    else
    {
        return; /* nothing changed */
    }

    /* Push the full-width band [0..width-1] x [y0..y1] in DMA-sized chunks: each chunk's rows are contiguous
     * in the framebuffer, so one convert/copy fills the RGB565 bounce, then draw_bitmap DMAs it to the panel. */
    for (int chunk_y = y0; chunk_y <= y1; chunk_y += ER_SPI_LCD_BOUNCE_ROWS)
    {
        const int rows = (chunk_y + ER_SPI_LCD_BOUNCE_ROWS - 1 <= y1) ? ER_SPI_LCD_BOUNCE_ROWS : (y1 - chunk_y + 1);
        fb_to_565_row(s_lcd.bounce, s_lcd.framebuffer + (size_t)chunk_y * s_lcd.width, rows * s_lcd.width);
        esp_lcd_panel_draw_bitmap(s_lcd.panel, 0, chunk_y, s_lcd.width, chunk_y + rows, s_lcd.bounce);
        /* draw_bitmap is async (SPI DMA); wait for THIS band before overwriting the shared bounce. */
        xSemaphoreTake(s_lcd.transfer_done, portMAX_DELAY);
    }

    s_lcd.dirty_x0 = s_lcd.width;
    s_lcd.dirty_y0 = s_lcd.height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
#endif /* !ER_LCD_BANDED */
}

bool er_esp32_spi_lcd_backend_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, int width, int height)
{
    s_lcd.panel = panel;
    s_lcd.width = width;
    s_lcd.height = height;
    s_lcd.dirty_x0 = width;
    s_lcd.dirty_y0 = height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
    s_lcd.first_present = true;

    /* Transfer-done signalling so present() can safely reuse the one bounce buffer between bands. */
    s_lcd.transfer_done = xSemaphoreCreateBinary();
    if (!s_lcd.transfer_done)
    {
        ESP_LOGE(TAG, "semaphore alloc failed");
        return false;
    }
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = on_trans_done};
    esp_lcd_panel_io_register_event_callbacks(io, &callbacks, NULL);

#if ER_LCD_BANDED
    /* Banded: only small RGB565 band buffers live in RAM; the panel's own GRAM retains the rest of the
     * frame. TWO ping-pong buffers (both DMA-capable, flushed straight to the panel — no bounce): the
     * engine composites the next strip into one while the panel DMAs the previous from the other. */
    s_lcd.framebuffer_rows = ER_LCD_BANDED_ROWS;
    const size_t bytes = (size_t)width * (size_t)ER_LCD_BANDED_ROWS * sizeof(fbpx_t);
    for (int bank_index = 0; bank_index < BAND_BANK_COUNT; bank_index++)
    {
        s_lcd.bank[bank_index] = (fbpx_t*)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_lcd.bank[bank_index])
        {
            ESP_LOGE(TAG,
                     "band buffer %d alloc failed (need 2 x %d KB internal DMA RAM)",
                     bank_index,
                     (int)(bytes / BYTES_PER_KB));
            return false;
        }
        memset(s_lcd.bank[bank_index], 0, bytes); /* opaque black */
    }
    s_lcd.current_bank = 0;
    s_lcd.strip_in_flight = false;
    s_lcd.framebuffer = s_lcd.bank[0];
    s_lcd.bounce = NULL;
#else
    /* Canonical framebuffer: MALLOC_CAP_8BIT = byte-addressable internal RAM. MUST NOT be plain
     * MALLOC_CAP_INTERNAL — that can return instruction-RAM (IRAM, 0x4008xxxx) which only allows
     * 32-bit word access, and the byte/16-bit framebuffer accesses would fault (LoadStoreError). 8BIT also
     * picks the data-bus "D/IRAM" block, which is the largest byte-addressable region here. */
    s_lcd.framebuffer_rows = height;
    const size_t bytes = (size_t)width * height * sizeof(fbpx_t);
    s_lcd.framebuffer = (fbpx_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!s_lcd.framebuffer)
    {
        ESP_LOGE(TAG, "framebuffer alloc failed (need %d KB internal RAM)", (int)(bytes / BYTES_PER_KB));
        return false;
    }
    memset(s_lcd.framebuffer, 0, bytes); /* opaque black */

    /* Small DMA-capable RGB565 bounce buffer for the per-band flush. */
    const size_t bounce_bytes = (size_t)width * ER_SPI_LCD_BOUNCE_ROWS * sizeof(uint16_t);
    s_lcd.bounce = (uint16_t*)heap_caps_malloc(bounce_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_lcd.bounce)
    {
        ESP_LOGE(TAG, "bounce buffer alloc failed (need %d KB internal DMA RAM)", (int)(bounce_bytes / BYTES_PER_KB));
        heap_caps_free(s_lcd.framebuffer);
        s_lcd.framebuffer = NULL;
        return false;
    }
#endif

    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.copy_rect_format = copy_fmt_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL;
    backend.context = NULL;
#if ER_LCD_BANDED
    backend.band_height = ER_LCD_BANDED_ROWS;
    backend.band_begin = band_begin_cb;
    backend.band_flush = band_flush_cb;
#endif
    embedded_renderer_set_backend(&backend);

#if ER_LCD_BANDED
    ESP_LOGI(TAG,
             "SPI LCD backend ready: %dx%d, RGB565 BANDED (2 x %d-row ping-pong buffers, %d KB DMA RAM) — panel GRAM "
             "retains the frame",
             width,
             height,
             ER_LCD_BANDED_ROWS,
             (int)(BAND_BANK_COUNT * bytes / BYTES_PER_KB));
#else
    ESP_LOGI(TAG,
             "SPI LCD backend ready: %dx%d, %s fb in internal RAM (%d KB) + %d KB DMA bounce",
             width,
             height,
             ER_SPI_LCD_FB332 ? "RGB332" : "RGB565",
             (int)(bytes / BYTES_PER_KB),
             (int)(bounce_bytes / BYTES_PER_KB));
#endif
    return true;
}
