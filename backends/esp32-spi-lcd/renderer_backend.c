/*
 * Lean SPI-LCD render backend — single internal-RAM framebuffer, full-width band flush.
 *
 * The no-PSRAM counterpart to backends/esp32-lcd (which keeps its buffers in PSRAM and supports RGB
 * double-buffering/rotation). Stripped to what a plain ESP32-WROOM-32 with only internal DRAM affords:
 *
 *   - ONE canonical framebuffer in internal RAM. The engine hands ARGB8888 sources (straight-alpha
 *     fills, premultiplied copy/blend); fill/copy/blend composite over the fb in full 8-bit precision.
 *     PIXEL FORMAT is selectable (ER_SPI_LCD_FB8): RGB565 (2 B/px, default) or RGB332 (1 B/px). On the
 *     original ESP32 the internal DRAM is FRAGMENTED — the largest contiguous block is ~110 KB — so a
 *     full 240x320 RGB565 fb (150 KB) does NOT fit; RGB332 (75 KB) does. Quality drops (256 colors,
 *     banding on anti-aliased text) but it renders. RGB565 is for boards with a big enough block.
 *   - Present pushes the dirty box as FULL-WIDTH bands through a SMALL DMA bounce buffer (RGB565). The
 *     band's rows are contiguous in the fb, so each chunk is one memcpy/convert into the bounce, then
 *     draw_bitmap DMAs it. The big fb itself need not be DMA-able.
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

/** @brief 0 = RGB565 canonical fb (2 B/px); 1 = RGB332 (1 B/px — fits a small internal-RAM block). */
#ifndef ER_SPI_LCD_FB8
#define ER_SPI_LCD_FB8 0
#endif

/** @brief Rows per DMA bounce chunk. The big framebuffer needn't be DMA-able — only this small buffer. */
#ifndef ER_SPI_LCD_BOUNCE_ROWS
#define ER_SPI_LCD_BOUNCE_ROWS 24
#endif

static const char* TAG = "er-spi-lcd";

/*----------------------------------------------------------------------------------------------------------------------
 - Canonical-pixel helpers (full-precision compositing; pack to the canonical format; convert to RGB565)
 ---------------------------------------------------------------------------------------------------------------------*/

#if ER_SPI_LCD_FB8

typedef uint8_t fbpx_t; /**< RGB332: RRRGGGBB. */

static inline fbpx_t fb_store(uint32_t argb)
{
    return (uint8_t)(((argb >> 16) & 0xE0U) | (((argb >> 8) & 0xE0U) >> 3) | ((argb & 0xC0U) >> 6));
}
static inline uint32_t fb_load(fbpx_t p)
{
    uint32_t r = p & 0xE0U;
    uint32_t g = (p << 3) & 0xE0U;
    uint32_t b = (p << 6) & 0xC0U;
    r |= r >> 3;
    r |= r >> 6; /* replicate to fill 8 bits */
    g |= g >> 3;
    g |= g >> 6;
    b |= b >> 2;
    b |= b >> 4;
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}
/** @brief Converts a row of canonical pixels to RGB565 (the panel format). */
static inline void fb_to_565_row(uint16_t* dst, const fbpx_t* src, int n)
{
    for (int i = 0; i < n; i++)
    {
        const uint32_t a = fb_load(src[i]);
        dst[i] = (uint16_t)((((a >> 16) & 0xF8U) << 8) | (((a >> 8) & 0xFCU) << 3) | ((a & 0xF8U) >> 3));
    }
}

#else /* RGB565 canonical */

typedef uint16_t fbpx_t;

static inline fbpx_t fb_store(uint32_t argb)
{
    return (uint16_t)((((argb >> 16) & 0xF8U) << 8) | (((argb >> 8) & 0xFCU) << 3) | ((argb & 0xF8U) >> 3));
}
static inline uint32_t fb_load(fbpx_t p)
{
    const uint32_t r5 = (p >> 11) & 0x1FU, g6 = (p >> 5) & 0x3FU, b5 = p & 0x1FU;
    const uint32_t r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}
static inline void fb_to_565_row(uint16_t* dst, const fbpx_t* src, int n)
{
    memcpy(dst, src, (size_t)n * sizeof(uint16_t)); /* already 565 */
}

#endif

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
    esp_lcd_panel_handle_t panel;
    int w;
    int h;
    fbpx_t* fb;              /**< Canonical framebuffer, w*h, plain internal RAM (need not be DMA). */
    uint16_t* bounce;        /**< Small DMA-capable RGB565 staging buffer: w * ER_SPI_LCD_BOUNCE_ROWS px. */
    SemaphoreHandle_t done;  /**< Given by the panel's color-trans-done ISR; waited on before bounce reuse. */
    int dx0, dy0, dx1, dy1;  /**< Dirty bounding box (inclusive); x1 < x0 means empty. */
    bool first;              /**< First present pushes the whole frame. */
} ERSpiLcdBackend;

static ERSpiLcdBackend s_be;

/** @brief Panel color-transfer-done ISR: signals that a band's DMA finished and the bounce is free. */
static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_be.done, &hp);
    return hp == pdTRUE;
}

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
    const fbpx_t opaque_px = fb_store(0xFF000000U | (argb & 0x00FFFFFFU));

    for (int row = 0; row < h; row++)
    {
        fbpx_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
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
        fbpx_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        for (int col = 0; col < w; col++)
        {
            d[col] = fb_store(over_premul(fb_load(d[col]), s[col]));
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
        fbpx_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
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

void er_esp32_spi_lcd_present(void)
{
    int y0, y1;
    if (s_be.first)
    {
        y0 = 0;
        y1 = s_be.h - 1;
        s_be.first = false;
    }
    else if (s_be.dy1 >= s_be.dy0 && s_be.dx1 >= s_be.dx0)
    {
        y0 = s_be.dy0;
        y1 = s_be.dy1;
    }
    else
    {
        return; /* nothing changed */
    }

    /* Push the full-width band [0..w-1] x [y0..y1] in DMA-sized chunks: each chunk's rows are contiguous
     * in the fb, so one convert/copy fills the RGB565 bounce, then draw_bitmap DMAs it to the panel. */
    for (int cy = y0; cy <= y1; cy += ER_SPI_LCD_BOUNCE_ROWS)
    {
        const int rows = (cy + ER_SPI_LCD_BOUNCE_ROWS - 1 <= y1) ? ER_SPI_LCD_BOUNCE_ROWS : (y1 - cy + 1);
        fb_to_565_row(s_be.bounce, s_be.fb + (size_t)cy * s_be.w, rows * s_be.w);
        esp_lcd_panel_draw_bitmap(s_be.panel, 0, cy, s_be.w, cy + rows, s_be.bounce);
        /* draw_bitmap is async (SPI DMA); wait for THIS band before overwriting the shared bounce. */
        xSemaphoreTake(s_be.done, portMAX_DELAY);
    }

    s_be.dx0 = s_be.w;
    s_be.dy0 = s_be.h;
    s_be.dx1 = -1;
    s_be.dy1 = -1;
}

bool er_esp32_spi_lcd_backend_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, int width, int height)
{
    s_be.panel = panel;
    s_be.w = width;
    s_be.h = height;
    s_be.dx0 = width;
    s_be.dy0 = height;
    s_be.dx1 = -1;
    s_be.dy1 = -1;
    s_be.first = true;

    /* Transfer-done signalling so present() can safely reuse the one bounce buffer between bands. */
    s_be.done = xSemaphoreCreateBinary();
    if (!s_be.done)
    {
        ESP_LOGE(TAG, "semaphore alloc failed");
        return false;
    }
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);

    /* Canonical framebuffer: MALLOC_CAP_8BIT = byte-addressable internal RAM. MUST NOT be plain
     * MALLOC_CAP_INTERNAL — that can return instruction-RAM (IRAM, 0x4008xxxx) which only allows
     * 32-bit word access, and the byte/16-bit fb accesses would fault (LoadStoreError). 8BIT also
     * picks the data-bus "D/IRAM" block, which is the largest byte-addressable region here. */
    const size_t bytes = (size_t)width * height * sizeof(fbpx_t);
    s_be.fb = (fbpx_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!s_be.fb)
    {
        ESP_LOGE(TAG, "framebuffer alloc failed (need %d KB internal RAM)", (int)(bytes / 1024));
        return false;
    }
    memset(s_be.fb, 0, bytes); /* opaque black */

    /* Small DMA-capable RGB565 bounce buffer for the per-band flush. */
    const size_t bounce_bytes = (size_t)width * ER_SPI_LCD_BOUNCE_ROWS * sizeof(uint16_t);
    s_be.bounce = (uint16_t*)heap_caps_malloc(bounce_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_be.bounce)
    {
        ESP_LOGE(TAG, "bounce buffer alloc failed (need %d KB internal DMA RAM)", (int)(bounce_bytes / 1024));
        heap_caps_free(s_be.fb);
        s_be.fb = NULL;
        return false;
    }

    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL;
    backend.ctx = NULL;
    embedded_renderer_set_backend(&backend);

    ESP_LOGI(TAG,
             "SPI LCD backend ready: %dx%d, %s fb in internal RAM (%d KB) + %d KB DMA bounce",
             width,
             height,
             ER_SPI_LCD_FB8 ? "RGB332" : "RGB565",
             (int)(bytes / 1024),
             (int)(bounce_bytes / 1024));
    return true;
}
