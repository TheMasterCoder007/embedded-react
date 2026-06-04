/*
 * ESP32 LCD render backend — board-agnostic software framebuffer + RGB565 flush.
 *
 * The engine renders ARGB8888 (straight-alpha fills, premultiplied copy/blend). This backend keeps a
 * persistent ARGB8888 framebuffer in PSRAM, composites every fill/copy/blend into it (premultiplied
 * over an opaque background), tracks the dirty bounding box, and on frame_ready converts just that
 * region to RGB565 and pushes it to the esp_lcd panel. The panel keeps its own framebuffer that its
 * LCD peripheral DMAs to the display, so only the changed region needs to be copied each frame.
 *
 * Board specifics (RGB timings, pins, the CH422G expander for reset/backlight) live in the host that
 * creates the panel — this file only needs the panel handle.
 */

#include "esp32_lcd_backend.h"
#include "native_renderer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* TAG = "er-lcd";

typedef struct
{
    esp_lcd_panel_handle_t panel;
    int w;
    int h;
    uint32_t* fb;   /**< ARGB8888 framebuffer (PSRAM), w*h. Stores opaque 0xFFRRGGBB after compositing. */
    uint16_t* line; /**< RGB565 staging buffer for the flush (PSRAM), w*h. */
    /* Dirty bounding box (inclusive); x1 < x0 means "empty". */
    int dx0, dy0, dx1, dy1;
} ERLcdBackend;

static ERLcdBackend s_be;

/*----------------------------------------------------------------------------------------------------------------------
 - Helpers
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Clamps a rect to the framebuffer and returns false if fully off-screen. */
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

/** @brief Grows the dirty bounding box to include the given (already-clipped) rect. */
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

/**
 * @brief Composites one premultiplied ARGB8888 source pixel over an opaque dst pixel.
 *
 * out_rgb = src_rgb + dst_rgb * (255 - src_a) / 255   (premultiplied "over"; dst alpha is implicitly 1)
 *
 * @param[in] dst  Existing framebuffer pixel (0xFFRRGGBB).
 * @param[in] sp   Premultiplied source pixel (0xAARRGGBB, RGB already scaled by alpha).
 *
 * @return The composited opaque pixel.
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
    const uint32_t dr = (dst >> 16) & 0xFFU;
    const uint32_t dg = (dst >> 8) & 0xFFU;
    const uint32_t db = dst & 0xFFU;
    const uint32_t sr = (sp >> 16) & 0xFFU;
    const uint32_t sg = (sp >> 8) & 0xFFU;
    const uint32_t sb = sp & 0xFFU;
    const uint32_t r = sr + (dr * inv + 127U) / 255U;
    const uint32_t g = sg + (dg * inv + 127U) / 255U;
    const uint32_t b = sb + (db * inv + 127U) / 255U;
    return 0xFF000000U | (r << 16) | (g << 8) | b;
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
    /* Premultiply the straight-alpha fill color, then composite like copy/blend. */
    const uint32_t a = argb >> 24;
    const uint32_t pr = (((argb >> 16) & 0xFFU) * a + 127U) / 255U;
    const uint32_t pg = (((argb >> 8) & 0xFFU) * a + 127U) / 255U;
    const uint32_t pb = ((argb & 0xFFU) * a + 127U) / 255U;
    const uint32_t sp = (a << 24) | (pr << 16) | (pg << 8) | pb;

    for (int row = 0; row < h; row++)
    {
        uint32_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        if (a == 255U)
        {
            const uint32_t opaque = 0xFF000000U | (argb & 0x00FFFFFFU);
            for (int col = 0; col < w; col++)
            {
                d[col] = opaque;
            }
        }
        else
        {
            for (int col = 0; col < w; col++)
            {
                d[col] = over_premul(d[col], sp);
            }
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Copies a premultiplied ARGB8888 buffer into the framebuffer (premultiplied over). */
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
        uint32_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        for (int col = 0; col < w; col++)
        {
            d[col] = over_premul(d[col], s[col]);
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
        uint32_t* d = s_be.fb + (size_t)(y + row) * s_be.w + x;
        for (int col = 0; col < w; col++)
        {
            /* Scale the premultiplied source by the global alpha (both rgb and a), then over. */
            const uint32_t p = s[col];
            const uint32_t sa = ((p >> 24) * ga + 127U) / 255U;
            const uint32_t sr = (((p >> 16) & 0xFFU) * ga + 127U) / 255U;
            const uint32_t sg = (((p >> 8) & 0xFFU) * ga + 127U) / 255U;
            const uint32_t sb = ((p & 0xFFU) * ga + 127U) / 255U;
            d[col] = over_premul(d[col], (sa << 24) | (sr << 16) | (sg << 8) | sb);
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Converts the dirty region to RGB565 and pushes it to the panel framebuffer. */
void er_esp32_lcd_present(void)
{
    if (s_be.dx1 < s_be.dx0 || s_be.dy1 < s_be.dy0)
    {
        return; /* nothing changed */
    }
    const int x0 = s_be.dx0;
    const int y0 = s_be.dy0;
    const int rw = s_be.dx1 - s_be.dx0 + 1;
    const int rh = s_be.dy1 - s_be.dy0 + 1;

    /* Pack the dirty region as RGB565 (row-tight) into the staging buffer. */
    for (int row = 0; row < rh; row++)
    {
        const uint32_t* s = s_be.fb + (size_t)(y0 + row) * s_be.w + x0;
        uint16_t* o = s_be.line + (size_t)row * rw;
        for (int col = 0; col < rw; col++)
        {
            const uint32_t p = s[col];
            const uint16_t r = (uint16_t)(((p >> 16) & 0xF8U) << 8);
            const uint16_t g = (uint16_t)(((p >> 8) & 0xFCU) << 3);
            const uint16_t b = (uint16_t)((p & 0xF8U) >> 3);
            o[col] = (uint16_t)(r | g | b);
        }
    }
    esp_lcd_panel_draw_bitmap(s_be.panel, x0, y0, x0 + rw, y0 + rh, s_be.line);

    /* Reset the dirty box. */
    s_be.dx0 = s_be.w;
    s_be.dy0 = s_be.h;
    s_be.dx1 = -1;
    s_be.dy1 = -1;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_esp32_lcd_backend_init(esp_lcd_panel_handle_t panel, int width, int height)
{
    s_be.panel = panel;
    s_be.w = width;
    s_be.h = height;
    s_be.dx0 = width;
    s_be.dy0 = height;
    s_be.dx1 = -1;
    s_be.dy1 = -1;

    s_be.fb = (uint32_t*)heap_caps_malloc((size_t)width * height * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_be.line = (uint16_t*)heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_be.fb || !s_be.line)
    {
        ESP_LOGE(TAG, "framebuffer alloc failed (need %d KB PSRAM)", (int)((size_t)width * height * 6 / 1024));
        return false;
    }
    /* Clear the framebuffer to opaque black so the first frame composites over a known background. */
    for (size_t i = 0; i < (size_t)width * height; i++)
    {
        s_be.fb[i] = 0xFF000000U;
    }

    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL; /* engine doesn't auto-present; the host calls er_esp32_lcd_present() */
    backend.ctx = NULL;
    embedded_renderer_set_backend(&backend);

    ESP_LOGI(TAG,
             "LCD backend ready: %dx%d (fb %d KB + rgb565 %d KB in PSRAM)",
             width,
             height,
             (int)((size_t)width * height * 4 / 1024),
             (int)((size_t)width * height * 2 / 1024));
    return true;
}
