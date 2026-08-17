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

static inline uint32_t fb_load(uint16_t p)
{
    const uint16_t rgb = __builtin_bswap16(p);
    const uint32_t r5 = (rgb >> 11) & 0x1FU, g6 = (rgb >> 5) & 0x3FU, b5 = rgb & 0x1FU;
    const uint32_t r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

/**
 * @brief Composites a premultiplied ARGB8888 source over an opaque ARGB8888 dst (8-bit precision).
 * out_rgb = src_rgb + dst_rgb * (255 - src_a) / 255.
 */
static inline uint32_t over_premul(uint32_t dst, uint32_t sp)
{
    const uint32_t sa = sp >> 24;
    if (sa == 255U)
    {
        return 0xFF000000U | (sp & 0x00FFFFFFU);
    }
    if (sa == 0U)
    {
        return dst;
    }
    const uint32_t inv = 255U - sa;
    const uint32_t dr = (dst >> 16) & 0xFFU, dg = (dst >> 8) & 0xFFU, db = dst & 0xFFU;
    const uint32_t sr = (sp >> 16) & 0xFFU, sg = (sp >> 8) & 0xFFU, sb = sp & 0xFFU;
    const uint32_t r = sr + (dr * inv + 127U) / 255U;
    const uint32_t g = sg + (dg * inv + 127U) / 255U;
    const uint32_t b = sb + (db * inv + 127U) / 255U;
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

typedef struct
{
    ErPicoLcdPanelOps ops;
    int w;
    int h;
    uint16_t* fb;           /**< Canonical framebuffer, panel byte order. */
    int dx0, dy0, dx1, dy1; /**< Dirty bounding box (inclusive); x1 < x0 means empty. */
    bool first;             /**< First present pushes the whole frame. */
} ErPicoLcdBackend;

static ErPicoLcdBackend s_be;

/** @brief Clamps a rect to the framebuffer; returns false if fully off-screen. */
static bool clip_rect(int* x, int* y, int* w, int* h)
{
    if (*x < 0)
    {
        *w += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > s_be.w)
    {
        *w = s_be.w - *x;
    }
    if (*y + *h > s_be.h)
    {
        *h = s_be.h - *y;
    }
    return (*w > 0 && *h > 0);
}

/** @brief Grows the dirty box to include an (already-clipped) rect. */
static void mark_dirty(int x, int y, int w, int h)
{
    if (x < s_be.dx0)
        s_be.dx0 = x;
    if (y < s_be.dy0)
        s_be.dy0 = y;
    if (x + w - 1 > s_be.dx1)
        s_be.dx1 = x + w - 1;
    if (y + h - 1 > s_be.dy1)
        s_be.dy1 = y + h - 1;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Backend callbacks
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Fills a rect with a straight-alpha ARGB8888 color, composited over the framebuffer. */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    if (!clip_rect(&x, &y, &w, &h))
    {
        return;
    }
    const uint32_t a = argb >> 24;
    const uint32_t pr = (((argb >> 16) & 0xFFU) * a + 127U) / 255U;
    const uint32_t pg = (((argb >> 8) & 0xFFU) * a + 127U) / 255U;
    const uint32_t pb = ((argb & 0xFFU) * a + 127U) / 255U;
    const uint32_t sp = (a << 24) | (pr << 16) | (pg << 8) | pb;
    const uint16_t opaque_px = fb_store(0xFF000000U | (argb & 0x00FFFFFFU));

    for (int row = 0; row < h; row++)
    {
        uint16_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        if (a == 255U)
        {
            for (int col = 0; col < w; col++)
            {
                d[col] = opaque_px;
            }
        }
        else
        {
            for (int col = 0; col < w; col++)
            {
                d[col] = fb_store(over_premul(fb_load(d[col]), sp));
            }
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Copies a premultiplied ARGB8888 buffer over the framebuffer. */
static void copy_cb(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const int ox = x, oy = y;
    if (!clip_rect(&x, &y, &w, &h))
    {
        return;
    }
    const int skip_x = x - ox;
    const int skip_y = y - oy;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)(skip_y + row) * src_stride_bytes) + skip_x;
        uint16_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        for (int col = 0; col < w; col++)
        {
            d[col] = fb_store(over_premul(fb_load(d[col]), s[col]));
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Copies a KNOWN-FULLY-OPAQUE source in its native format into the framebuffer (replace).
 *
 *  The engine only routes buffers its registration-time opacity scan proved opaque (RGB565 is opaque
 *  by construction), so pixels replace outright: no fb_load read-back and no over_premul math. The
 *  fb stores wire-order (byte-swapped) RGB565, so a 565 source is one bswap16 per pixel — far cheaper
 *  than the blend path's load/composite/store, which is what makes 16-bit-baked backgrounds cheap on
 *  an RP2040-class CPU. */
static void copy_fmt_cb(const void* src, int src_stride_bytes, ERImageFormat fmt, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    const int ox = x, oy = y;
    if (!clip_rect(&x, &y, &w, &h))
    {
        return;
    }
    const int skip_x = x - ox;
    const int skip_y = y - oy;
    if (fmt == ER_IMG_RGB565)
    {
        for (int row = 0; row < h; row++)
        {
            const uint16_t* s =
                (const uint16_t*)((const uint8_t*)src + (size_t)(skip_y + row) * src_stride_bytes) + skip_x;
            uint16_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
            for (int col = 0; col < w; col++)
            {
                d[col] = (uint16_t)__builtin_bswap16(s[col]);
            }
        }
    }
    else
    {
        for (int row = 0; row < h; row++)
        {
            const uint32_t* s =
                (const uint32_t*)((const uint8_t*)src + (size_t)(skip_y + row) * src_stride_bytes) + skip_x;
            uint16_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
            for (int col = 0; col < w; col++)
            {
                d[col] = fb_store(s[col]); /* opaque contract: no alpha inspection */
            }
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Blends a premultiplied ARGB8888 buffer over the framebuffer at a global alpha. */
static void blend_cb(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    if (alpha == 0U)
    {
        return;
    }
    const int ox = x, oy = y;
    if (!clip_rect(&x, &y, &w, &h))
    {
        return;
    }
    const int skip_x = x - ox;
    const int skip_y = y - oy;
    const uint32_t ga = alpha;
    for (int row = 0; row < h; row++)
    {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)src + (size_t)(skip_y + row) * src_stride_bytes) + skip_x;
        uint16_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        for (int col = 0; col < w; col++)
        {
            const uint32_t p = s[col];
            const uint32_t sa = ((p >> 24) * ga + 127U) / 255U;
            const uint32_t sr = (((p >> 16) & 0xFFU) * ga + 127U) / 255U;
            const uint32_t sg = (((p >> 8) & 0xFFU) * ga + 127U) / 255U;
            const uint32_t sb = ((p & 0xFFU) * ga + 127U) / 255U;
            d[col] = fb_store(over_premul(fb_load(d[col]), (sa << 24) | (sr << 16) | (sg << 8) | sb));
        }
    }
    mark_dirty(x, y, w, h);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Present + init
 ---------------------------------------------------------------------------------------------------------------------*/

void er_pico_spi_lcd_present(void)
{
    int x0, y0, x1, y1;
    if (s_be.first)
    {
        x0 = 0;
        y0 = 0;
        x1 = s_be.w - 1;
        y1 = s_be.h - 1;
        s_be.first = false;
    }
    else if (s_be.dy1 >= s_be.dy0 && s_be.dx1 >= s_be.dx0)
    {
        x0 = s_be.dx0;
        y0 = s_be.dy0;
        x1 = s_be.dx1;
        y1 = s_be.dy1;
    }
    else
    {
        return; /* nothing changed */
    }

    s_be.ops.set_window(x0, y0, x1, y1);
    const int rw = x1 - x0 + 1;
    if (rw == s_be.w)
    {
        /* Full-width box: the rows are one contiguous run in the framebuffer — a single write. */
        s_be.ops.write_pixels(s_be.fb + (size_t)y0 * s_be.w, (size_t)rw * (y1 - y0 + 1));
    }
    else
    {
        for (int row = y0; row <= y1; row++)
        {
            s_be.ops.write_pixels(s_be.fb + (size_t)row * s_be.w + x0, (size_t)rw);
        }
    }

    s_be.dx0 = s_be.w;
    s_be.dy0 = s_be.h;
    s_be.dx1 = -1;
    s_be.dy1 = -1;
}

bool er_pico_spi_lcd_backend_init(const ErPicoLcdPanelOps* ops, int width, int height)
{
    if (!ops || !ops->set_window || !ops->write_pixels || width <= 0 || height <= 0)
    {
        return false;
    }
    s_be.ops = *ops;
    s_be.w = width;
    s_be.h = height;
    s_be.dx0 = width;
    s_be.dy0 = height;
    s_be.dx1 = -1;
    s_be.dy1 = -1;
    s_be.first = true;

    const size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    s_be.fb = (uint16_t*)malloc(bytes);
    if (!s_be.fb)
    {
        return false;
    }
    memset(s_be.fb, 0, bytes); /* opaque black (0x0000 byte-swapped is still 0) */

    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.copy_rect_fmt = copy_fmt_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL;
    backend.ctx = NULL;
    embedded_renderer_set_backend(&backend);
    return true;
}
