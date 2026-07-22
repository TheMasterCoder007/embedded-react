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
 * DIRECT MODE (ER_LCD_DIRECT, default 1): when the canonical format is RGB565, rotation is 0, and
 * the panel exposes 2–3 rotating framebuffers, the engine composites STRAIGHT into the panel
 * framebuffers — no canonical framebuffer and no present copy. The engine's multi-buffer damage
 * replay (er_set_display_buffer_count / er_display_present) repaints whatever each buffer missed
 * while the others were displayed. Present is then just a cache writeback of the dirty window plus
 * a buffer flip; with three framebuffers (num_fbs=3) there is always a swapped-out buffer ready to
 * draw into, so the flip never blocks. The canonical-fb path below remains for rotated configs,
 * ARGB8888 canonical builds, and single-framebuffer (copy mode) panels.
 *
 * Board specifics (RGB timings, pins, the CH422G expander for reset/backlight) live in the host that
 * creates the panel — this file only needs the panel handle.
 */

#include "esp32_lcd_backend.h"
#include "er_scene.h" /* er_set_display_buffer_count / er_display_present (direct mode) */
#include "native_renderer.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

/** @brief 1 = canonical framebuffer is RGB565 (no convert pass); 0 = ARGB8888 (convert at present). */
#ifndef ER_LCD_FB_RGB565
#define ER_LCD_FB_RGB565 1
#endif

/**
 * @brief 1 = composite directly into the panel's rotating framebuffers when possible (RGB565
 *         canonical format, rotation 0, 2-3 panel framebuffers): no canonical framebuffer and no
 *         present copy — the engine's multi-buffer damage replay keeps every panel buffer current.
 *         0 = always use the canonical-framebuffer path.
 */
#ifndef ER_LCD_DIRECT
#define ER_LCD_DIRECT 1
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

/** @brief Converts one canonical pixel to RGB565 (already 565 — identity). Used by rotated present. */
static inline uint16_t fb_to_565_px(fbpx_t p)
{
    return (uint16_t)p;
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

/** @brief Converts one ARGB8888 canonical pixel to RGB565. Used by rotated present. */
static inline uint16_t fb_to_565_px(fbpx_t p)
{
    const uint16_t r = (uint16_t)(((p >> 16) & 0xF8U) << 8);
    const uint16_t g = (uint16_t)(((p >> 8) & 0xFCU) << 3);
    const uint16_t b = (uint16_t)((p & 0xF8U) >> 3);
    return (uint16_t)(r | g | b);
}

/** @brief Converts n ARGB8888 canonical pixels to an RGB565 output row. */
static inline void fb_to_565_row(uint16_t* dst, const fbpx_t* src, int n)
{
    for (int i = 0; i < n; i++)
    {
        dst[i] = fb_to_565_px(src[i]);
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
    int w;            /**< Logical (canonical fb) width — what the engine composites into. */
    int h;            /**< Logical (canonical fb) height. */
    int pw;           /**< Physical panel width (= w/h swapped for 90/270). */
    int ph;           /**< Physical panel height. */
    int rot;          /**< Clockwise display rotation: 0, 90, 180, or 270. */
    fbpx_t* fb;       /**< Composite target: the canonical fb, or (direct mode) the back panel fb. */
    uint16_t* fbp[3]; /**< The panel's rotating RGB565 framebuffers; fbp[0] == NULL = copy mode. */
    int nfb;          /**< Number of panel framebuffers in fbp (2 or 3; 0 in copy mode). */
    int back;         /**< Index of the off-screen framebuffer to draw next. */
    uint16_t* line;   /**< RGB565 staging buffer (copy mode only, when double buffering is unavailable). */
    bool direct;      /**< Direct mode: the engine composites straight into the panel framebuffers and the
                           engine's multi-buffer damage replay keeps them all current — no canonical fb,
                           no present copy. Requires RGB565 canonical format, rotation 0, and 2+ panel fbs. */
    SemaphoreHandle_t flip_done;  /**< Given at each frame boundary: all earlier flips have taken effect. */
    int pending_flips;            /**< Flips requested since the last confirmed frame boundary. */
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

/**
 * @brief Blocks until the current back framebuffer is safe to draw into (direct mode).
 *
 * A flip requested by draw_bitmap only takes effect at the next frame boundary; until then the
 * swapped-out buffer is still being scanned. The on_frame_buf_complete callback fires at every
 * frame boundary and confirms ALL flips requested before it, so with N framebuffers the back
 * buffer is unsafe only while (N-1) flips are still unconfirmed. With triple buffering this
 * effectively never blocks; with double buffering it waits up to one refresh — overlapped with
 * the frame's non-drawing work, since it runs lazily at the first write after a flip.
 */
static inline void flip_gate(void)
{
    if (!s_be.direct || s_be.pending_flips < s_be.nfb - 1)
    {
        return;
    }
    if (s_be.flip_done)
    {
        if (xSemaphoreTake(s_be.flip_done, portMAX_DELAY) == pdTRUE)
        {
            s_be.pending_flips = 0; /* a frame boundary passed: earlier flips are live */
        }
    }

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

/** @brief Maps a logical (canonical-fb) pixel to its physical (panel) position under the rotation. */
static inline void rot_point(int lx, int ly, int* px, int* py)
{
    switch (s_be.rot)
    {
        case 90:
            *px = s_be.pw - 1 - ly;
            *py = lx;
            break;
        case 180:
            *px = s_be.pw - 1 - lx;
            *py = s_be.ph - 1 - ly;
            break;
        case 270:
            *px = ly;
            *py = s_be.ph - 1 - lx;
            break;
        default:
            *px = lx;
            *py = ly;
            break;
    }
}

/** @brief Maps a logical box (inclusive) to its physical bounding box (still axis-aligned). */
static void rot_box(int x0, int y0, int x1, int y1, int* px0, int* py0, int* px1, int* py1)
{
    int ax, ay, bx, by;
    rot_point(x0, y0, &ax, &ay);
    rot_point(x1, y1, &bx, &by);
    *px0 = ax < bx ? ax : bx;
    *py0 = ay < by ? ay : by;
    *px1 = ax > bx ? ax : bx;
    *py1 = ay > by ? ay : by;
}

/**
 * @brief Writes the logical region [x0..x1]x[y0..y1] of the canonical fb into a physical 565 buffer.
 *
 * dst has row stride dst_stride pixels, and its element (0,0) is physical (dst_ox, dst_oy). At 0° this
 * is the fast per-row 565 copy; otherwise each pixel is placed at its rotated physical position.
 */
static void present_region(uint16_t* dst, int dst_stride, int dst_ox, int dst_oy, int x0, int y0, int x1, int y1)
{
    if (s_be.rot == 0)
    {
        const int rw = x1 - x0 + 1;
        for (int ly = y0; ly <= y1; ly++)
        {
            const fbpx_t* s = s_be.fb + (size_t)ly * s_be.w + x0;
            uint16_t* o = dst + (size_t)(ly - dst_oy) * dst_stride + (x0 - dst_ox);
            fb_to_565_row(o, s, rw);
        }
        return;
    }
    for (int ly = y0; ly <= y1; ly++)
    {
        const fbpx_t* s = s_be.fb + (size_t)ly * s_be.w;
        for (int lx = x0; lx <= x1; lx++)
        {
            int px, py;
            rot_point(lx, ly, &px, &py);
            dst[(size_t)(py - dst_oy) * dst_stride + (px - dst_ox)] = fb_to_565_px(s[lx]);
        }
    }
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
/**
 * @brief Fast source-over of a premultiplied ARGB8888 pixel onto an opaque canonical pixel.
 *
 * Paired-channel arithmetic: R+B blend in one 32-bit multiply, G in another — 2 multiplies
 * per pixel instead of ~6, at the cost of ≤1 LSB darkening (inv = 255-sa with >>8), which is
 * below the RGB565 quantization step. This is the per-pixel core of the composited-strip →
 * framebuffer emit, the hottest loop on PSRAM-framebuffer boards.
 */
static inline fbpx_t over_premul_fast(fbpx_t dst, uint32_t sp)
{
    const uint32_t sa = sp >> 24;
    if (sa == 255U)
    {
        return fb_store(sp);
    }
    if (sa == 0U)
    {
        return dst;
    }
    const uint32_t inv = 255U - sa;
    const uint32_t d = fb_load(dst);
    const uint32_t orb = ((sp & 0x00FF00FFU) + (((d & 0x00FF00FFU) * inv >> 8) & 0x00FF00FFU)) & 0x00FF00FFU;
    const uint32_t og = ((sp & 0x0000FF00U) + (((d & 0x0000FF00U) * inv >> 8) & 0x0000FF00U)) & 0x0000FF00U;
    return fb_store(0xFF000000U | orb | og);
}

/** @brief Scales a premultiplied ARGB8888 pixel (all four channels) by alpha/255, paired. */
static inline uint32_t scale_premul_fast(uint32_t sp, uint32_t alpha)
{
    /* a1 in [0,256]: 255→256 keeps full-opacity lossless. */
    const uint32_t a1 = alpha + (alpha >> 7);
    const uint32_t ag = ((sp >> 8) & 0x00FF00FFU) * a1;      /* A and G lanes */
    const uint32_t rb = (sp & 0x00FF00FFU) * a1;             /* R and B lanes */
    return ((ag & 0xFF00FF00U)) | ((rb >> 8) & 0x00FF00FFU);
}

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

/** @brief Composites the cached RGB565 overlay (logical coords) onto the physical panel buffer. */
static void blit_overlay(uint16_t* dst)
{
    if (s_be.rot == 0)
    {
        for (int row = 0; row < s_be.ovh; row++)
        {
            const uint16_t* s = s_be.ov_cache + (size_t)row * s_be.ovw;
            uint16_t* o = dst + (size_t)(s_be.ovy + row) * s_be.pw + s_be.ovx;
            memcpy(o, s, (size_t)s_be.ovw * sizeof(uint16_t));
        }
        return;
    }
    for (int row = 0; row < s_be.ovh; row++)
    {
        const int ly = s_be.ovy + row;
        const uint16_t* s = s_be.ov_cache + (size_t)row * s_be.ovw;
        for (int col = 0; col < s_be.ovw; col++)
        {
            int px, py;
            rot_point(s_be.ovx + col, ly, &px, &py);
            dst[(size_t)py * s_be.pw + px] = s[col];
        }
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Backend callbacks
 ---------------------------------------------------------------------------------------------------------------------*/

/* Row staging buffer (internal DRAM): the canonical framebuffer lives in PSRAM, where per-pixel
 * read-modify-write stalls ~90 cycles/px. Blends copy the destination row in with a burst memcpy,
 * blend against internal memory, and burst the result back — several times faster than in-place. */
#define ER_ROW_STAGE_MAX 1024
static fbpx_t s_row_stage[ER_ROW_STAGE_MAX];

/** @brief Fills a rect with a straight-alpha ARGB8888 color, composited over the framebuffer. */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    flip_gate();
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
            /* Translucent fill: stage the row internally (see blend_cb). */
            fbpx_t* stage = (w <= ER_ROW_STAGE_MAX) ? s_row_stage : d;
            if (stage != d)
                memcpy(stage, d, (size_t)w * sizeof(fbpx_t));
            for (int col = 0; col < w; col++)
            {
                stage[col] = over_premul_fast(stage[col], sp);
            }
            if (stage != d)
                memcpy(d, stage, (size_t)w * sizeof(fbpx_t));
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Copies a premultiplied ARGB8888 buffer into the framebuffer (premultiplied over). */
static void copy_cb(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    flip_gate();
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
            const uint32_t p = s[col];
            const uint32_t sa = p >> 24;
            if (sa == 255U)
            {
                d[col] = fb_store(p);
            }
            else if (sa != 0U)
            {
                d[col] = over_premul_fast(d[col], p);
            }
        }
    }
    mark_dirty(x, y, w, h);
}

/** @brief Blends a premultiplied ARGB8888 buffer over the framebuffer at a global alpha. */
static void blend_cb(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    (void)ctx;
    flip_gate();
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
        /* Stage the destination row in internal DRAM: one burst in, blend internally, one
         * burst out — instead of a ~90-cycle PSRAM read-modify-write per pixel. */
        fbpx_t* stage = (w <= ER_ROW_STAGE_MAX) ? s_row_stage : d;
        if (stage != d)
            memcpy(stage, d, (size_t)w * sizeof(fbpx_t));
        if (ga == 255U)
        {
            /* Full-alpha path — the common case for composited strips, where most pixels are
             * fully opaque: no global-alpha scaling, opaque pixels skip the dst load. */
            for (int col = 0; col < w; col++)
            {
                const uint32_t p = s[col];
                const uint32_t sa = p >> 24;
                if (sa == 255U)
                {
                    stage[col] = fb_store(p);
                }
                else if (sa != 0U)
                {
                    stage[col] = over_premul_fast(stage[col], p);
                }
            }
        }
        else
        {
            for (int col = 0; col < w; col++)
            {
                /* Scale the premultiplied source by the global alpha (paired lanes), then over. */
                const uint32_t p = s[col];
                if (p == 0U)
                    continue;
                stage[col] = over_premul_fast(stage[col], scale_premul_fast(p, ga));
            }
        }
        if (stage != d)
            memcpy(d, stage, (size_t)w * sizeof(fbpx_t));
    }
    mark_dirty(x, y, w, h);
}

/** @brief Pushes the painted (dirty) region to the panel, re-compositing the overlay. */
void er_esp32_lcd_present(void)
{
    if (s_be.direct)
    {
        /* Direct mode: the engine composited straight into the back panel framebuffer, and its
         * multi-buffer damage replay already repainted everything this buffer missed while it was
         * off-screen — the dirty box therefore covers every pixel written since this buffer was
         * last flipped, and is exactly the cache-writeback window draw_bitmap needs. */
        bool have = (s_be.dx1 >= s_be.dx0 && s_be.dy1 >= s_be.dy0);
        int x0 = s_be.dx0, y0 = s_be.dy0, x1 = s_be.dx1, y1 = s_be.dy1;
        s_be.dx0 = s_be.w;
        s_be.dy0 = s_be.h;
        s_be.dx1 = -1;
        s_be.dy1 = -1;
        if (!have && !s_be.ov_active)
        {
            return; /* nothing changed: no flip, and the engine's buffer ring must not advance */
        }

        uint16_t* dst_fb = (uint16_t*)s_be.fb;
        /* Re-stamp the persistent overlay onto this buffer (its own tight region). */
        if (s_be.ov_active)
        {
            flip_gate(); /* the stamp writes the back buffer too */
            blit_overlay(dst_fb);
            if (!have)
            {
                x0 = s_be.ovx;
                y0 = s_be.ovy;
                x1 = s_be.ovx + s_be.ovw - 1;
                y1 = s_be.ovy + s_be.ovh - 1;
            }
            else
            {
                if (s_be.ovx < x0)
                    x0 = s_be.ovx;
                if (s_be.ovy < y0)
                    y0 = s_be.ovy;
                if (s_be.ovx + s_be.ovw - 1 > x1)
                    x1 = s_be.ovx + s_be.ovw - 1;
                if (s_be.ovy + s_be.ovh - 1 > y1)
                    y1 = s_be.ovy + s_be.ovh - 1;
            }
        }

        /* A frame boundary confirms every earlier flip: drain the grant (if any) BEFORE queuing
         * this flip so the pending count only tracks flips the panel hasn't absorbed yet. */
        if (s_be.flip_done && xSemaphoreTake(s_be.flip_done, 0) == pdTRUE)
        {
            s_be.pending_flips = 0;
        }

        /* Flip: draw_bitmap with a panel-owned buffer writes back the cache over the given window
         * and switches the scan-out buffer at the next frame boundary. The next frame's first
         * write into the new back buffer gates on flip_gate() — with three framebuffers that
         * effectively never blocks; with two it waits up to one refresh. */
        esp_lcd_panel_draw_bitmap(s_be.panel, x0, y0, x1 + 1, y1 + 1, dst_fb);
        s_be.pending_flips++;
        s_be.back = (s_be.back + 1) % s_be.nfb;
        s_be.fb = (fbpx_t*)s_be.fbp[s_be.back];
        er_display_present(); /* the next commit targets the new back buffer's damage debt */
        return;
    }

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
        /* App region: map the logical dirty region into the off-screen panel framebuffer with the
         * configured rotation (a plain 565→565 row copy at 0°). */
        if (have)
        {
            present_region(dst_fb, s_be.pw, 0, 0, x0, y0, x1, y1);
        }
        /* Overlay: re-composite the cached RGB565 panel on top (its own tight region — not unioned
         * into the app's dirty box, so a small corner overlay never enlarges the app flush). */
        if (s_be.ov_active)
        {
            blit_overlay(dst_fb);
        }
        /* draw_bitmap bounds are PHYSICAL panel coords: rotate the app + overlay logical boxes and
         * union them (the per-region writes above stay tight). */
        int bx0 = s_be.pw, by0 = s_be.ph, bx1 = -1, by1 = -1;
        if (have)
        {
            rot_box(x0, y0, x1, y1, &bx0, &by0, &bx1, &by1);
        }
        if (s_be.ov_active)
        {
            int ax, ay, axe, aye;
            rot_box(s_be.ovx, s_be.ovy, s_be.ovx + s_be.ovw - 1, s_be.ovy + s_be.ovh - 1, &ax, &ay, &axe, &aye);
            if (ax < bx0)
                bx0 = ax;
            if (ay < by0)
                by0 = ay;
            if (axe > bx1)
                bx1 = axe;
            if (aye > by1)
                by1 = aye;
        }
        esp_lcd_panel_draw_bitmap(s_be.panel, bx0, by0, bx1 + 1, by1 + 1, dst_fb);
        s_be.back ^= 1;
    }
    else
    {
        /* Copy mode: pack the (rotated) region row-tight into the staging buffer; draw_bitmap copies it
         * into the live framebuffer at the physical bounding box. */
        if (have)
        {
            int px0, py0, px1, py1;
            rot_box(x0, y0, x1, y1, &px0, &py0, &px1, &py1);
            present_region(s_be.line, px1 - px0 + 1, px0, py0, x0, y0, x1, y1);
            esp_lcd_panel_draw_bitmap(s_be.panel, px0, py0, px1 + 1, py1 + 1, s_be.line);
        }
        if (s_be.ov_active)
        {
            if (s_be.rot == 0)
            {
                esp_lcd_panel_draw_bitmap(
                    s_be.panel, s_be.ovx, s_be.ovy, s_be.ovx + s_be.ovw, s_be.ovy + s_be.ovh, s_be.ov_cache);
            }
            else
            {
                int px0, py0, px1, py1;
                rot_box(s_be.ovx, s_be.ovy, s_be.ovx + s_be.ovw - 1, s_be.ovy + s_be.ovh - 1, &px0, &py0, &px1, &py1);
                const int pbw = px1 - px0 + 1;
                for (int row = 0; row < s_be.ovh; row++)
                {
                    const int ly = s_be.ovy + row;
                    const uint16_t* s = s_be.ov_cache + (size_t)row * s_be.ovw;
                    for (int col = 0; col < s_be.ovw; col++)
                    {
                        int px, py;
                        rot_point(s_be.ovx + col, ly, &px, &py);
                        s_be.line[(size_t)(py - py0) * pbw + (px - px0)] = s[col];
                    }
                }
                esp_lcd_panel_draw_bitmap(s_be.panel, px0, py0, px1 + 1, py1 + 1, s_be.line);
            }
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

/** @brief ISR callback: the previously displayed framebuffer is now safe to draw into. */
static bool frame_buf_complete_cb(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* edata,
                                  void* user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_woken = pdFALSE;
    if (s_be.flip_done)
    {
        xSemaphoreGiveFromISR(s_be.flip_done, &hp_woken);
    }
    return hp_woken == pdTRUE;
}

bool er_esp32_lcd_backend_init(esp_lcd_panel_handle_t panel, int width, int height, int rotation)
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
    {
        ESP_LOGE(TAG, "invalid rotation %d (must be 0/90/180/270)", rotation);
        return false;
    }
    s_be.panel = panel;
    s_be.w = width; /* logical (canonical fb) */
    s_be.h = height;
    s_be.rot = rotation;
    /* Physical panel size = logical with width/height swapped for 90/270. */
    s_be.pw = (rotation == 90 || rotation == 270) ? height : width;
    s_be.ph = (rotation == 90 || rotation == 270) ? width : height;
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

    /* Tear-free path: if the panel exposes rotating framebuffers (num_fbs 2 or 3), draw the
     * off-screen one and let draw_bitmap swap it in at vsync. Otherwise fall back to a staging
     * buffer that draw_bitmap copies into the live framebuffer (single-buffered; can tear). */
    void* fb0 = NULL;
    void* fb1 = NULL;
    void* fb2 = NULL;
    s_be.nfb = 0;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel, 3, &fb0, &fb1, &fb2) == ESP_OK && fb0 && fb1 && fb2 &&
        fb0 != fb1 && fb0 != fb2 && fb1 != fb2)
    {
        s_be.nfb = 3;
    }
    else if (esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &fb0, &fb1) == ESP_OK && fb0 && fb1 && fb0 != fb1)
    {
        s_be.nfb = 2;
        fb2 = NULL;
    }
    if (s_be.nfb > 0)
    {
        s_be.fbp[0] = (uint16_t*)fb0;
        s_be.fbp[1] = (uint16_t*)fb1;
        s_be.fbp[2] = (uint16_t*)fb2;
        s_be.back = 1; /* fb0 is displayed first; draw into fb1 */
        s_be.line = NULL;
    }
    else
    {
        s_be.fbp[0] = NULL;
        s_be.fbp[1] = NULL;
        s_be.fbp[2] = NULL;
        s_be.line = (uint16_t*)heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_be.line)
        {
            ESP_LOGE(TAG, "staging buffer alloc failed");
            return false;
        }
    }

    /* Direct mode: composite straight into the panel framebuffers. Needs the canonical format to BE
     * the panel format (RGB565), an identity rotation (blits land at panel coordinates), and both
     * panel framebuffers. The engine's multi-buffer damage replay repaints whatever each buffer
     * missed while the other was displayed, so no canonical framebuffer is required at all. */
    s_be.direct = (ER_LCD_DIRECT != 0) && (ER_LCD_FB_RGB565 != 0) && rotation == 0 && s_be.fbp[0] != NULL;

    if (s_be.direct)
    {
        /* The flip must be sequenced: after draw_bitmap queues a buffer switch, the swapped-out
         * buffer only becomes safe to draw into at the next frame boundary. Without that signal,
         * direct mode would tear — fall back to the canonical path instead. */
        s_be.flip_done = xSemaphoreCreateBinary();
        const esp_lcd_rgb_panel_event_callbacks_t cbs = {
            .on_frame_buf_complete = frame_buf_complete_cb,
        };
        if (!s_be.flip_done || esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, NULL) != ESP_OK)
        {
            ESP_LOGW(TAG, "frame_buf_complete callback unavailable; using the canonical-fb path");
            if (s_be.flip_done)
            {
                vSemaphoreDelete(s_be.flip_done);
                s_be.flip_done = NULL;
            }
            s_be.direct = false;
        }
    }

    if (s_be.direct)
    {
        /* All panel framebuffers start as the composite target; clear them to opaque black. */
        for (int i = 0; i < s_be.nfb; i++)
        {
            memset(s_be.fbp[i], 0, (size_t)width * height * sizeof(uint16_t));
        }
        /* Push fb0 full-screen once: writes the cleared pixels back through the cache and makes
         * fb0 the displayed buffer, so the panel shows black instead of allocation garbage. */
        esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, s_be.fbp[0]);
        s_be.fb = (fbpx_t*)s_be.fbp[s_be.back];
        s_be.pending_flips = 0;
    }
    else
    {
        s_be.fb = (fbpx_t*)heap_caps_malloc((size_t)width * height * sizeof(fbpx_t), MALLOC_CAP_SPIRAM);
        if (!s_be.fb)
        {
            ESP_LOGE(TAG,
                     "framebuffer alloc failed (need %d KB PSRAM)",
                     (int)((size_t)width * height * sizeof(fbpx_t) / 1024));
            return false;
        }
        /* Clear the framebuffer to opaque black so the first frame composites over a known background. */
        for (size_t i = 0; i < (size_t)width * height; i++)
        {
            s_be.fb[i] = FB_CLEAR_PX;
        }
    }

    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.blend_rect = blend_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL; /* engine doesn't auto-present; the host calls er_esp32_lcd_present() */
    backend.ctx = NULL;
    embedded_renderer_set_backend(&backend);
    /* Direct mode rotates the panel framebuffers; tell the engine so each commit repaints the
     * target buffer's accumulated damage debt (see er_set_display_buffer_count in er_scene.h). */
    er_set_display_buffer_count(s_be.direct ? s_be.nfb : 1);

    ESP_LOGI(TAG,
             "LCD backend ready: logical %dx%d, panel %dx%d, rotation %d°, %s %s",
             width,
             height,
             s_be.pw,
             s_be.ph,
             rotation,
             s_be.direct ? "DIRECT to panel fbs," : "canonical fb",
             s_be.direct        ? "no canonical fb"
             : ER_LCD_FB_RGB565 ? "RGB565"
                                : "ARGB8888");
    return true;
}
