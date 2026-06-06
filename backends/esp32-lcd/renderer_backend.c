/*
 * ESP32 LCD render backend — board-agnostic software framebuffer + RGB565 flush.
 *
 * The engine always hands this backend ARGB8888 sources (straight-alpha fills, premultiplied
 * copy/blend). The backend keeps a persistent "canonical" framebuffer in PSRAM that those
 * fill/copy/blend composite into (premultiplied over an opaque background), tracks the dirty
 * bounding box, and on present pushes just that region to the esp_lcd panel (which is always
 * RGB565).
 *
 * PIXEL FORMAT OF THE CANONICAL FRAMEBUFFER is selectable via ER_LCD_FB_RGB565 (default 1):
 *   - 1 (RGB565): the canonical fb is itself RGB565. Compositing unpacks the dst to 8-bit, blends
 *     in full precision, and packs back to 565. Present is then a plain 565→565 copy of the dirty
 *     region into the panel framebuffer — the per-frame ARGB→RGB565 CONVERSION PASS IS GONE, the
 *     canonical fb is half the size (2 B/px), and per-pixel compositing bandwidth roughly halves.
 *     This is the device default (the panel is physically 565, so this only moves the 565 rounding
 *     a step earlier; quality is effectively unchanged for flat UI).
 *   - 0 (ARGB8888): the canonical fb is ARGB8888 and present converts the dirty region to RGB565.
 *     Full-precision intermediate (no per-blend banding); useful as an A/B reference or on hosts
 *     that are not PSRAM-bandwidth-bound.
 *
 * Note on "zero-copy": the canonical fb stays the always-complete source the double-buffered,
 * incremental, read-modify-write blend model needs (each panel fb is two presents stale, so present
 * replays current ∪ previous damage to converge it). Compositing straight into the alternating panel
 * fbs — eliminating even the present copy — would require the engine to repaint the union damage into
 * the target every frame (an engine change) or accept tearing; the 565 canonical here removes the
 * conversion and halves the bandwidth/footprint, which is the bulk of the win.
 *
 * Board specifics (RGB timings, pins, the CH422G expander for reset/backlight) live in the host that
 * creates the panel — this file only needs the panel handle.
 */

#include "esp32_lcd_backend.h"
#include "native_renderer.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

/** @brief 1 = canonical framebuffer is RGB565 (no convert pass); 0 = ARGB8888 (convert at present). */
#ifndef ER_LCD_FB_RGB565
#define ER_LCD_FB_RGB565 1
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Pixel-format abstraction (canonical framebuffer)
 ---------------------------------------------------------------------------------------------------------------------*/

#if ER_LCD_FB_RGB565

typedef uint16_t fbpx_t; /**< Canonical framebuffer pixel: RGB565. */

/** @brief Packs an opaque ARGB8888 (0xFFRRGGBB) into an RGB565 framebuffer pixel. */
static inline fbpx_t fb_store(uint32_t argb)
{
    const uint16_t r = (uint16_t)(((argb >> 16) & 0xF8U) << 8);
    const uint16_t g = (uint16_t)(((argb >> 8) & 0xFCU) << 3);
    const uint16_t b = (uint16_t)((argb & 0xF8U) >> 3);
    return (uint16_t)(r | g | b);
}

/** @brief Unpacks an RGB565 framebuffer pixel to opaque ARGB8888, replicating high bits for accuracy. */
static inline uint32_t fb_load(fbpx_t p)
{
    const uint32_t r5 = (p >> 11) & 0x1FU;
    const uint32_t g6 = (p >> 5) & 0x3FU;
    const uint32_t b5 = p & 0x1FU;
    const uint32_t r = (r5 << 3) | (r5 >> 2);
    const uint32_t g = (g6 << 2) | (g6 >> 4);
    const uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000U | (r << 16) | (g << 8) | b;
}

/** @brief Emits n canonical pixels to an RGB565 output row — a straight copy (already 565). */
static inline void fb_to_565_row(uint16_t* dst, const fbpx_t* src, int n)
{
    memcpy(dst, src, (size_t)n * sizeof(uint16_t));
}

#define FB_CLEAR_PX ((fbpx_t)0x0000) /* opaque black */

#else /* ARGB8888 canonical */

typedef uint32_t fbpx_t; /**< Canonical framebuffer pixel: ARGB8888 (0xFFRRGGBB). */

static inline fbpx_t fb_store(uint32_t argb)
{
    return 0xFF000000U | (argb & 0x00FFFFFFU);
}

static inline uint32_t fb_load(fbpx_t p)
{
    return p;
}

/** @brief Converts n ARGB8888 canonical pixels to an RGB565 output row. */
static inline void fb_to_565_row(uint16_t* dst, const fbpx_t* src, int n)
{
    for (int i = 0; i < n; i++)
    {
        const uint32_t p = src[i];
        const uint16_t r = (uint16_t)(((p >> 16) & 0xF8U) << 8);
        const uint16_t g = (uint16_t)(((p >> 8) & 0xFCU) << 3);
        const uint16_t b = (uint16_t)((p & 0xF8U) >> 3);
        dst[i] = (uint16_t)(r | g | b);
    }
}

#define FB_CLEAR_PX ((fbpx_t)0xFF000000U) /* opaque black */

#endif /* ER_LCD_FB_RGB565 */

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* TAG = "er-lcd";

typedef struct
{
    esp_lcd_panel_handle_t panel;
    int w;
    int h;
    fbpx_t* fb;       /**< Canonical framebuffer (PSRAM), w*h. Authoritative full frame. */
    uint16_t* fbp[2]; /**< The panel's two RGB565 framebuffers (double-buffer mode); NULL = copy mode. */
    int back;         /**< Index of the off-screen framebuffer to draw next (double-buffer mode). */
    uint16_t* line;   /**< RGB565 staging buffer (copy mode only, when double buffering is unavailable). */
    /* Current dirty bounding box (inclusive); x1 < x0 means "empty". */
    int dx0, dy0, dx1, dy1;
    /* Previous present's dirty box: unioned in so the back buffer (two presents stale) catches up. */
    int pdx0, pdy0, pdx1, pdy1;
    int present_count; /**< First two presents draw full-screen to initialise both framebuffers. */
    /* Persistent overlay: an RGB565 snapshot (see er_esp32_lcd_overlay_capture) re-composited on top
     * of every present, so a small always-on overlay stays consistent across both framebuffers and is
     * flushed as its own tight region — not unioned into (and ballooning) the app's dirty box. */
    uint16_t* ov_cache;
    int ov_cap; /**< Allocated capacity of ov_cache in pixels. */
    int ovx, ovy, ovw, ovh;
    bool ov_active;
    /* Saved dirty box around an overlay draw (er_esp32_lcd_overlay_begin): drawing the overlay into
     * the framebuffer to snapshot it must not leave the overlay's rect in the APP dirty box, or it
     * would balloon the app's per-frame flush (overlay corner unioned with far-away app damage). */
    int sdx0, sdy0, sdx1, sdy1;
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
 * @brief Composites one premultiplied ARGB8888 source pixel over an opaque dst pixel (8-bit precision).
 *
 * out_rgb = src_rgb + dst_rgb * (255 - src_a) / 255   (premultiplied "over"; dst alpha is implicitly 1)
 *
 * @param[in] dst  Existing pixel unpacked to opaque ARGB8888 (0xFFRRGGBB).
 * @param[in] sp   Premultiplied source pixel (0xAARRGGBB, RGB already scaled by alpha).
 *
 * @return The composited opaque pixel (0xFFRRGGBB), ready to pack via fb_store.
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

/** @brief Copies the cached RGB565 overlay on top of a full-frame RGB565 destination buffer. */
static void blit_overlay(uint16_t* dst)
{
    for (int row = 0; row < s_be.ovh; row++)
    {
        const uint16_t* s = s_be.ov_cache + (size_t)row * s_be.ovw;
        uint16_t* o = dst + (size_t)(s_be.ovy + row) * s_be.w + s_be.ovx;
        memcpy(o, s, (size_t)s_be.ovw * sizeof(uint16_t));
    }
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
            /* Scale the premultiplied source by the global alpha (both rgb and a), then over. */
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

/** @brief Pushes the painted (dirty) region to the panel, re-compositing the overlay. */
void er_esp32_lcd_present(void)
{
    const bool dbl = (s_be.fbp[0] != NULL);
    /* First two presents in double-buffer mode draw the whole frame so both framebuffers start valid. */
    const bool full = dbl && s_be.present_count < 2;

    /* App dirty region to push. In double-buffer mode the back framebuffer is two presents stale, so
     * union the current and previous dirty boxes; in copy mode draw_bitmap writes the live framebuffer,
     * so only the current box is needed. */
    bool have = false;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    if (full)
    {
        x0 = 0;
        y0 = 0;
        x1 = s_be.w - 1;
        y1 = s_be.h - 1;
        have = true;
    }
    else
    {
        if (s_be.dx1 >= s_be.dx0 && s_be.dy1 >= s_be.dy0)
        {
            x0 = s_be.dx0;
            y0 = s_be.dy0;
            x1 = s_be.dx1;
            y1 = s_be.dy1;
            have = true;
        }
        if (dbl && s_be.pdx1 >= s_be.pdx0 && s_be.pdy1 >= s_be.pdy0)
        {
            if (!have)
            {
                x0 = s_be.pdx0;
                y0 = s_be.pdy0;
                x1 = s_be.pdx1;
                y1 = s_be.pdy1;
                have = true;
            }
            else
            {
                if (s_be.pdx0 < x0)
                    x0 = s_be.pdx0;
                if (s_be.pdy0 < y0)
                    y0 = s_be.pdy0;
                if (s_be.pdx1 > x1)
                    x1 = s_be.pdx1;
                if (s_be.pdy1 > y1)
                    y1 = s_be.pdy1;
            }
        }
    }

    /* Roll current → previous, then reset the current box (we've captured everything we need above). */
    s_be.pdx0 = s_be.dx0;
    s_be.pdy0 = s_be.dy0;
    s_be.pdx1 = s_be.dx1;
    s_be.pdy1 = s_be.dy1;
    s_be.dx0 = s_be.w;
    s_be.dy0 = s_be.h;
    s_be.dx1 = -1;
    s_be.dy1 = -1;
    if (full)
    {
        s_be.present_count++;
    }
    /* Present when the app changed, OR there's an overlay to keep current on this frame's buffer. */
    if (!have && !s_be.ov_active)
    {
        return;
    }

    if (dbl)
    {
        uint16_t* dst_fb = s_be.fbp[s_be.back];
        /* App region: emit straight into the off-screen framebuffer (full-width rows). In RGB565 mode
         * this is a plain 565→565 copy; in ARGB mode it converts. */
        if (have)
        {
            const int rw = x1 - x0 + 1;
            const int rh = y1 - y0 + 1;
            for (int row = 0; row < rh; row++)
            {
                const fbpx_t* s = s_be.fb + (size_t)(y0 + row) * s_be.w + x0;
                uint16_t* o = dst_fb + (size_t)(y0 + row) * s_be.w + x0;
                fb_to_565_row(o, s, rw);
            }
        }
        /* Overlay: re-composite the cached RGB565 panel on top (its own tight region — not unioned
         * into the app's dirty box, so a small corner overlay never enlarges the app flush). */
        if (s_be.ov_active)
        {
            blit_overlay(dst_fb);
        }
        /* draw_bitmap flips the whole framebuffer at vsync; the bounds are an informational hint, so
         * pass the union of the app + overlay regions (the per-region conversions above stay tight). */
        int bx0 = x0, by0 = y0, bx1 = x1, by1 = y1;
        if (s_be.ov_active)
        {
            const int ox1 = s_be.ovx + s_be.ovw - 1;
            const int oy1 = s_be.ovy + s_be.ovh - 1;
            if (!have)
            {
                bx0 = s_be.ovx;
                by0 = s_be.ovy;
                bx1 = ox1;
                by1 = oy1;
            }
            else
            {
                if (s_be.ovx < bx0)
                    bx0 = s_be.ovx;
                if (s_be.ovy < by0)
                    by0 = s_be.ovy;
                if (ox1 > bx1)
                    bx1 = ox1;
                if (oy1 > by1)
                    by1 = oy1;
            }
        }
        esp_lcd_panel_draw_bitmap(s_be.panel, bx0, by0, bx1 + 1, by1 + 1, dst_fb);
        s_be.back ^= 1;
    }
    else
    {
        /* Copy mode: pack row-tight into the staging buffer; draw_bitmap copies it into the live fb. */
        if (have)
        {
            const int rw = x1 - x0 + 1;
            const int rh = y1 - y0 + 1;
            for (int row = 0; row < rh; row++)
            {
                const fbpx_t* s = s_be.fb + (size_t)(y0 + row) * s_be.w + x0;
                uint16_t* o = s_be.line + (size_t)row * rw;
                fb_to_565_row(o, s, rw);
            }
            esp_lcd_panel_draw_bitmap(s_be.panel, x0, y0, x1 + 1, y1 + 1, s_be.line);
        }
        if (s_be.ov_active)
        {
            esp_lcd_panel_draw_bitmap(
                s_be.panel, s_be.ovx, s_be.ovy, s_be.ovx + s_be.ovw, s_be.ovy + s_be.ovh, s_be.ov_cache);
        }
    }
}

/** @brief Saves the current dirty box before an overlay is drawn into the framebuffer (see capture). */
void er_esp32_lcd_overlay_begin(void)
{
    s_be.sdx0 = s_be.dx0;
    s_be.sdy0 = s_be.dy0;
    s_be.sdx1 = s_be.dx1;
    s_be.sdy1 = s_be.dy1;
}

/** @brief Snapshots a framebuffer rect as the persistent overlay re-composited on every present. */
void er_esp32_lcd_overlay_capture(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || !clip_rect(&x, &y, &w, &h))
    {
        s_be.ov_active = false;
        /* Still roll back the dirty box: the overlay was drawn (dirtying its rect) but is composited
         * separately, so it must not enlarge the app's flush. */
        s_be.dx0 = s_be.sdx0;
        s_be.dy0 = s_be.sdy0;
        s_be.dx1 = s_be.sdx1;
        s_be.dy1 = s_be.sdy1;
        return;
    }
    const int need = w * h;
    if (need > s_be.ov_cap)
    {
        uint16_t* nc = (uint16_t*)heap_caps_malloc((size_t)need * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!nc)
        {
            s_be.ov_active = false;
            return;
        }
        if (s_be.ov_cache)
        {
            heap_caps_free(s_be.ov_cache);
        }
        s_be.ov_cache = nc;
        s_be.ov_cap = need;
    }
    for (int row = 0; row < h; row++)
    {
        const fbpx_t* s = s_be.fb + (size_t)(y + row) * s_be.w + x;
        uint16_t* o = s_be.ov_cache + (size_t)row * w;
        fb_to_565_row(o, s, w);
    }
    s_be.ovx = x;
    s_be.ovy = y;
    s_be.ovw = w;
    s_be.ovh = h;
    s_be.ov_active = true;

    /* Roll the dirty box back to its pre-overlay state: the overlay's rect is composited every present
     * from the cache, so it must not stay in (and balloon) the app's per-frame dirty region. */
    s_be.dx0 = s_be.sdx0;
    s_be.dy0 = s_be.sdy0;
    s_be.dx1 = s_be.sdx1;
    s_be.dy1 = s_be.sdy1;
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
    s_be.pdx0 = width;
    s_be.pdy0 = height;
    s_be.pdx1 = -1;
    s_be.pdy1 = -1;
    s_be.present_count = 0;
    s_be.ov_active = false;

    s_be.fb = (fbpx_t*)heap_caps_malloc((size_t)width * height * sizeof(fbpx_t), MALLOC_CAP_SPIRAM);
    if (!s_be.fb)
    {
        ESP_LOGE(
            TAG, "framebuffer alloc failed (need %d KB PSRAM)", (int)((size_t)width * height * sizeof(fbpx_t) / 1024));
        return false;
    }

    /* Tear-free path: if the panel exposes two framebuffers (num_fbs=2), draw straight into the
     * off-screen one and let draw_bitmap swap it in at vsync. Otherwise fall back to a staging buffer
     * that draw_bitmap copies into the live framebuffer (single-buffered; can tear). */
    void* fb0 = NULL;
    void* fb1 = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &fb0, &fb1) == ESP_OK && fb0 && fb1 && fb0 != fb1)
    {
        s_be.fbp[0] = (uint16_t*)fb0;
        s_be.fbp[1] = (uint16_t*)fb1;
        s_be.back = 1; /* fb0 is displayed first; draw into fb1 */
        s_be.line = NULL;
    }
    else
    {
        s_be.fbp[0] = NULL;
        s_be.fbp[1] = NULL;
        s_be.line = (uint16_t*)heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_be.line)
        {
            ESP_LOGE(TAG, "staging buffer alloc failed");
            return false;
        }
    }
    /* Clear the framebuffer to opaque black so the first frame composites over a known background. */
    for (size_t i = 0; i < (size_t)width * height; i++)
    {
        s_be.fb[i] = FB_CLEAR_PX;
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
             "LCD backend ready: %dx%d, canonical fb %s (%d KB PSRAM)",
             width,
             height,
             ER_LCD_FB_RGB565 ? "RGB565" : "ARGB8888",
             (int)((size_t)width * height * sizeof(fbpx_t) / 1024));
    return true;
}
