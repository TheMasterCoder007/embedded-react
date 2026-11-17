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
 *   - 1 (RGB565): the canonical framebuffer is itself RGB565. Compositing unpacks the dst to 8-bit, blends
 *     in full precision, and packs back to 565. Present is then a plain 565→565 copy of the dirty
 *     region into the panel framebuffer — the per-frame ARGB→RGB565 CONVERSION PASS IS GONE, the
 *     canonical framebuffer is half the size (2 B/px), and per-pixel compositing bandwidth roughly halves.
 *     This is the device default (the panel is physically 565, so this only moves the 565 rounding
 *     a step earlier; quality is effectively unchanged for flat UI).
 *   - 0 (ARGB8888): the canonical framebuffer is ARGB8888 and present converts the dirty region to RGB565.
 *     Full-precision intermediate (no per-blend banding); useful as an A/B reference or on hosts
 *     that are not PSRAM-bandwidth-bound.
 *
 * DIRECT MODE (ER_LCD_DIRECT, default 1): when the canonical format is RGB565, the display rotation
 * is 0, and the panel exposes 2–3 rotating framebuffers, the engine composites STRAIGHT into the panel
 * framebuffers — no canonical framebuffer and no present copy. The engine's multi-buffer damage
 * replay (er_set_display_buffer_count / er_display_present) repaints whatever each buffer missed
 * while the others were displayed. Present is then just a cache writeback of the dirty window plus
 * a buffer flip; with three framebuffers (num_fbs=3) there is always a swapped-out buffer ready to
 * draw into, so the flip never blocks. The canonical-framebuffer path below remains for rotated configs,
 * ARGB8888 canonical builds, and single-framebuffer panels.
 *
 * ROTATION is the engine's (er_set_display_rotation): the canonical framebuffer has the logical shape,
 * and present turns each region into the panel framebuffer with er_rotate_copy().
 *
 * Board specifics (RGB timings, pins, the CH422G expander for reset/backlight) live in the host that
 * creates the panel — this file only needs the panel handle.
 */

#include "er_scene.h" /* er_set_display_buffer_count / er_display_present (direct mode) */
#include "esp32_lcd_backend.h"
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

/** @brief 1 = use the ESP32-S3 PIE 128-bit SIMD blend routines (8 px/iteration) for the hot
 *         source-over paths, verified against the scalar reference by a self-test at init.
 *         Requires the RGB565 canonical format; 0 = always use the scalar C loops. */
#ifndef ER_LCD_PIE
#define ER_LCD_PIE 1
#endif
#if ER_LCD_PIE && ER_LCD_FB_RGB565 && defined(CONFIG_IDF_TARGET_ESP32S3)
#define ER_LCD_PIE_ACTIVE 1
#include "pie_blend.h"
#else
#define ER_LCD_PIE_ACTIVE 0
#endif

/** @brief 1 = 2x2 ordered dithering in the SIMD blend paths (default): the RGB565 quantization
 *         error becomes a fine, spatially stable checkerboard instead of a frame-to-frame
 *         shimmer on animated fades. 0 = uniform round-to-nearest. PIE paths only. */
#ifndef ER_LCD_DITHER
#define ER_LCD_DITHER 1
#endif

/** @brief Full scale of an 8-bit color channel. */
#define CHANNEL_MAX 255U

/** @brief Added before dividing by CHANNEL_MAX so the result rounds to nearest. */
#define CHANNEL_ROUND 127U

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 255U

/** @brief The alpha byte of an opaque ARGB8888 pixel. */
#define ARGB_OPAQUE_ALPHA 0xFF000000U

/** @brief Panel framebuffer counts the backend can rotate through. */
#define DOUBLE_BUFFER_COUNT 2
#define TRIPLE_BUFFER_COUNT 3

/** @brief Presents drawn full-screen at start, one per double-buffered framebuffer. */
#define INITIAL_FULL_PRESENTS 2

/** @brief Row staging: one slot per CPU core, aligned for the PIE SIMD loads. */
#define STAGE_CORE_COUNT 2
#define STAGE_ALIGNMENT 16

/** @brief Bytes in a kilobyte, for the log lines. */
#define BYTES_PER_KB 1024

/*----------------------------------------------------------------------------------------------------------------------
 - Pixel-format abstraction (canonical framebuffer)
 ---------------------------------------------------------------------------------------------------------------------*/

#if ER_LCD_FB_RGB565

typedef uint16_t fbpx_t; /**< Canonical framebuffer pixel: RGB565. */

/** @brief Packs an opaque ARGB8888 (0xFFRRGGBB) into an RGB565 framebuffer pixel. */
static inline fbpx_t fb_store(uint32_t argb)
{
    const uint16_t red = (uint16_t)(((argb >> 16) & 0xF8U) << 8);
    const uint16_t green = (uint16_t)(((argb >> 8) & 0xFCU) << 3);
    const uint16_t blue = (uint16_t)((argb & 0xF8U) >> 3);
    return (uint16_t)(red | green | blue);
}

/** @brief Unpacks an RGB565 framebuffer pixel to opaque ARGB8888, replicating high bits for accuracy. */
static inline uint32_t fb_load(fbpx_t pixel)
{
    const uint32_t red5 = (pixel >> 11) & 0x1FU;
    const uint32_t green6 = (pixel >> 5) & 0x3FU;
    const uint32_t blue5 = pixel & 0x1FU;
    const uint32_t red = (red5 << 3) | (red5 >> 2);
    const uint32_t green = (green6 << 2) | (green6 >> 4);
    const uint32_t blue = (blue5 << 3) | (blue5 >> 2);
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}

#define FB_CLEAR_PX ((fbpx_t)0x0000) /* opaque black */

#else /* ARGB8888 canonical */

typedef uint32_t fbpx_t; /**< Canonical framebuffer pixel: ARGB8888 (0xFFRRGGBB). */

static inline fbpx_t fb_store(uint32_t argb)
{
    return ARGB_OPAQUE_ALPHA | (argb & 0x00FFFFFFU);
}

static inline uint32_t fb_load(fbpx_t pixel)
{
    return pixel;
}

/** @brief Converts one ARGB8888 canonical pixel to RGB565. */
static inline uint16_t fb_to_565_px(fbpx_t pixel)
{
    const uint16_t red = (uint16_t)(((pixel >> 16) & 0xF8U) << 8);
    const uint16_t green = (uint16_t)(((pixel >> 8) & 0xFCU) << 3);
    const uint16_t blue = (uint16_t)((pixel & 0xF8U) >> 3);
    return (uint16_t)(red | green | blue);
}

/** @brief Converts n ARGB8888 canonical pixels to an RGB565 output row. */
static inline void fb_to_565_row(uint16_t* destination, const fbpx_t* source, int count)
{
    for (int index = 0; index < count; index++)
    {
        destination[index] = fb_to_565_px(source[index]);
    }
}

#define FB_CLEAR_PX ((fbpx_t)ARGB_OPAQUE_ALPHA) /* opaque black */

#endif /* ER_LCD_FB_RGB565 */

/*----------------------------------------------------------------------------------------------------------------------
 - State
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* TAG = "er-lcd";

typedef struct
{
    esp_lcd_panel_handle_t panel;
    int width;        /**< Logical (canonical framebuffer) width — what the engine composites into. */
    int height;       /**< Logical (canonical framebuffer) height. */
    int panel_width;  /**< Panel width as it scans (= width/height swapped for 90/270). */
    int panel_height; /**< Panel height as it scans. */
    fbpx_t*
        framebuffer; /**< Composite target: the canonical framebuffer, or (direct mode) the back panel framebuffer. */
#if !ER_LCD_FB_RGB565
    uint16_t* shadow; /**< Logical-shape RGB565 copy of the canonical framebuffer, what er_rotate_copy() reads. */
#endif
    uint16_t* panel_framebuffers[TRIPLE_BUFFER_COUNT]; /**< The panel's RGB565 framebuffers. */
    int panel_framebuffer_count; /**< Number of panel framebuffers (1 to 3); 1 = present draws the live one. */
    int back;                    /**< Index of the off-screen framebuffer to draw next. */
    bool direct;                 /**< Direct mode: the engine composites straight into the panel framebuffers and the
                                      engine's multi-buffer damage replay keeps them all current — no canonical framebuffer,
                                      no present copy. Requires RGB565 canonical format, rotation 0, and 2+ panel framebuffers. */
    SemaphoreHandle_t flip_done; /**< Given at each frame boundary: all earlier flips have taken effect. */
    int pending_flips;           /**< Flips requested since the last confirmed frame boundary. */
    /* Current dirty box, logical: inclusive min corner, EXCLUSIVE max corner (matches ERRect).
     * dirty_x_end <= dirty_x0 means "empty". */
    int dirty_x0, dirty_y0, dirty_x_end, dirty_y_end;
    /* Previous present's dirty box: unioned in so the back buffer (two presents stale) catches up. */
    ERRect previous_damage;
    bool previous_has_damage;
    int present_count; /**< First two presents draw full-screen to initialise both framebuffers. */
    /* Persistent overlay: a snapshot (see er_esp32_lcd_overlay_capture) stamped back wherever the app
     * draws over it, and presented as its own tight region after each capture — not unioned into (and
     * ballooning) the app's dirty box. */
    fbpx_t* overlay_cache;
    int overlay_capacity; /**< Allocated capacity of overlay_cache in pixels. */
    int overlay_x, overlay_y, overlay_width, overlay_height;
    bool overlay_active;
    int overlay_presents_due; /**< Panel framebuffers still missing the last capture. */
    /* Saved dirty box around an overlay draw (er_esp32_lcd_overlay_begin): drawing the overlay into
     * the framebuffer to snapshot it must not leave the overlay's rect in the APP dirty box, or it
     * would balloon the app's per-frame flush (overlay corner unioned with far-away app damage). */
    int saved_dirty_x0, saved_dirty_y0, saved_dirty_x_end, saved_dirty_y_end;
} ERLcdBackend;

static ERLcdBackend s_lcd;

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
/* Multi-core rendering: render workers on both cores call the blit callbacks concurrently, so
 * the tiny shared read-modify-writes (the flip gate, the dirty bounding box) sit inside this
 * spinlock. Uncontended cost is a few cycles per blit — noise next to the pixel work. */
static portMUX_TYPE s_blit_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void flip_gate(void)
{
    if (!s_lcd.direct)
    {
        return;
    }
    taskENTER_CRITICAL(&s_blit_mux);
    const bool must_wait = (s_lcd.pending_flips >= s_lcd.panel_framebuffer_count - 1);
    taskEXIT_CRITICAL(&s_blit_mux);
    if (!must_wait)
    {
        return;
    }
    if (s_lcd.flip_done)
    {
        if (xSemaphoreTake(s_lcd.flip_done, portMAX_DELAY) == pdTRUE)
        {
            taskENTER_CRITICAL(&s_blit_mux);
            s_lcd.pending_flips = 0; /* a frame boundary passed: earlier flips are live */
            taskEXIT_CRITICAL(&s_blit_mux);
        }
    }
}

/** @brief Clamps a rect to the framebuffer and returns false if fully off-screen. */
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

/** @brief Grows the dirty bounding box to include the given (already-clipped) rect.
 *
 * Serialized: concurrent render workers would otherwise lose min/max updates and under-flush. */
static void mark_dirty(int x, int y, int width, int height)
{
    taskENTER_CRITICAL(&s_blit_mux);
    if (x < s_lcd.dirty_x0)
        s_lcd.dirty_x0 = x;
    if (y < s_lcd.dirty_y0)
        s_lcd.dirty_y0 = y;
    if (x + width > s_lcd.dirty_x_end)
        s_lcd.dirty_x_end = x + width;
    if (y + height > s_lcd.dirty_y_end)
        s_lcd.dirty_y_end = y + height;
    taskEXIT_CRITICAL(&s_blit_mux);
}

/** @brief Moves the dirty box into @p box (logical) and empties it; false when it was empty. */
static bool take_dirty(ERRect* box)
{
    const bool has_damage = s_lcd.dirty_x_end > s_lcd.dirty_x0 && s_lcd.dirty_y_end > s_lcd.dirty_y0;
    box->x = s_lcd.dirty_x0;
    box->y = s_lcd.dirty_y0;
    box->width = s_lcd.dirty_x_end - s_lcd.dirty_x0;
    box->height = s_lcd.dirty_y_end - s_lcd.dirty_y0;
    s_lcd.dirty_x0 = s_lcd.width;
    s_lcd.dirty_y0 = s_lcd.height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
    return has_damage;
}

/** @brief Grows @p into to cover @p rect; an empty @p into (width 0) becomes @p rect. */
static void union_rect(ERRect* into, const ERRect* rect)
{
    if (into->width <= 0 || into->height <= 0)
    {
        *into = *rect;
        return;
    }
    const int right = into->x + into->width > rect->x + rect->width ? into->x + into->width : rect->x + rect->width;
    const int bottom =
        into->y + into->height > rect->y + rect->height ? into->y + into->height : rect->y + rect->height;
    into->x = into->x < rect->x ? into->x : rect->x;
    into->y = into->y < rect->y ? into->y : rect->y;
    into->width = right - into->x;
    into->height = bottom - into->y;
}

/** @brief The overlay's rect, logical. */
static ERRect overlay_rect(void)
{
    const ERRect rect = {s_lcd.overlay_x, s_lcd.overlay_y, s_lcd.overlay_width, s_lcd.overlay_height};
    return rect;
}

/** @brief Reports whether the app region covers any of the overlay, so it drew over it. */
static bool region_covers_overlay(const ERRect* region)
{
    return s_lcd.overlay_active && region->width > 0 && region->height > 0
           && region->x < s_lcd.overlay_x + s_lcd.overlay_width && s_lcd.overlay_x < region->x + region->width
           && region->y < s_lcd.overlay_y + s_lcd.overlay_height && s_lcd.overlay_y < region->y + region->height;
}

/** @brief Stamps the overlay snapshot back over the framebuffer, where the app may have drawn since. */
static void stamp_overlay(void)
{
    for (int row = 0; row < s_lcd.overlay_height; row++)
    {
        memcpy(s_lcd.framebuffer + (size_t)(s_lcd.overlay_y + row) * s_lcd.width + s_lcd.overlay_x,
               s_lcd.overlay_cache + (size_t)row * s_lcd.overlay_width,
               (size_t)s_lcd.overlay_width * sizeof(fbpx_t));
    }
}

/**
 * @brief Copies a logical region of the canonical framebuffer into a panel framebuffer, turned.
 *
 * @param[out]    destination  The panel framebuffer.
 * @param[in]     region       The region, logical.
 * @param[in,out] bounds       Grown to cover the region's panel rect.
 */
static void present_region(uint16_t* destination, const ERRect* region, ERRect* bounds)
{
#if ER_LCD_FB_RGB565
    const uint16_t* source = s_lcd.framebuffer;
#else
    for (int row = 0; row < region->height; row++)
    {
        const size_t offset = (size_t)(region->y + row) * s_lcd.width + region->x;
        fb_to_565_row(s_lcd.shadow + offset, s_lcd.framebuffer + offset, region->width);
    }
    const uint16_t* source = s_lcd.shadow;
#endif
    ERRect panel_rect = *region;
    er_display_rect_to_panel(&panel_rect);
    er_rotate_copy(source,
                   s_lcd.width * (int)sizeof(uint16_t),
                   destination,
                   s_lcd.panel_width * (int)sizeof(uint16_t),
                   (int)sizeof(uint16_t),
                   &panel_rect);
    union_rect(bounds, &panel_rect);
}

/**
 * @brief Composites one premultiplied ARGB8888 source pixel over an opaque dst pixel (8-bit precision).
 *
 * out_rgb = src_rgb + dst_rgb * (255 - src_a) / 255   (premultiplied "over"; dst alpha is implicitly 1)
 *
 * @param[in] destination  Existing pixel unpacked to opaque ARGB8888 (0xFFRRGGBB).
 * @param[in] source_pixel   Premultiplied source pixel (0xAARRGGBB, RGB already scaled by alpha).
 *
 * @return The composited opaque pixel (0xFFRRGGBB), ready to pack via fb_store.
 */
/**
 * @brief Fast source-over of a premultiplied ARGB8888 pixel onto an opaque canonical pixel.
 *
 * Paired-channel arithmetic: R+B blend in one 32-bit multiply, G in another — 2 multiplies
 * per pixel instead of ~6, at the cost of ≤1 LSB darkening (inverse_alpha = 255 - source_alpha with >>8), which is
 * below the RGB565 quantization step. This is the per-pixel core of the composited-strip →
 * framebuffer emit, the hottest loop on PSRAM-framebuffer boards.
 */
static inline fbpx_t over_premul_fast(fbpx_t destination, uint32_t source_pixel)
{
    const uint32_t source_alpha = source_pixel >> 24;
    if (source_alpha == ALPHA_OPAQUE)
    {
        return fb_store(source_pixel);
    }
    if (source_alpha == 0U)
    {
        return destination;
    }
    const uint32_t inverse_alpha = CHANNEL_MAX - source_alpha;
    const uint32_t destination_argb = fb_load(destination);
    const uint32_t out_red_blue =
        ((source_pixel & 0x00FF00FFU) + (((destination_argb & 0x00FF00FFU) * inverse_alpha >> 8) & 0x00FF00FFU))
        & 0x00FF00FFU;
    const uint32_t out_green =
        ((source_pixel & 0x0000FF00U) + (((destination_argb & 0x0000FF00U) * inverse_alpha >> 8) & 0x0000FF00U))
        & 0x0000FF00U;
    return fb_store(ARGB_OPAQUE_ALPHA | out_red_blue | out_green);
}

/** @brief Scales a premultiplied ARGB8888 pixel (all four channels) by alpha/255, paired. */
static inline uint32_t scale_premul_fast(uint32_t source_pixel, uint32_t alpha)
{
    /* a1 in [0,256]: 255→256 keeps full-opacity lossless. */
    const uint32_t scaled_alpha = alpha + (alpha >> 7);
    const uint32_t alpha_green = ((source_pixel >> 8) & 0x00FF00FFU) * scaled_alpha; /* A and G lanes */
    const uint32_t red_blue = (source_pixel & 0x00FF00FFU) * scaled_alpha;           /* R and B lanes */
    return ((alpha_green & 0xFF00FF00U)) | ((red_blue >> 8) & 0x00FF00FFU);
}

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
    const uint32_t destination_red = (destination >> 16) & 0xFFU;
    const uint32_t destination_green = (destination >> 8) & 0xFFU;
    const uint32_t destination_blue = destination & 0xFFU;
    const uint32_t source_red = (source_pixel >> 16) & 0xFFU;
    const uint32_t source_green = (source_pixel >> 8) & 0xFFU;
    const uint32_t source_blue = source_pixel & 0xFFU;
    const uint32_t red = source_red + (destination_red * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t green = source_green + (destination_green * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    const uint32_t blue = source_blue + (destination_blue * inverse_alpha + CHANNEL_ROUND) / CHANNEL_MAX;
    return ARGB_OPAQUE_ALPHA | (red << 16) | (green << 8) | blue;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Backend callbacks
 ---------------------------------------------------------------------------------------------------------------------*/

/* Row staging buffer (internal DRAM): the canonical framebuffer lives in PSRAM, where per-pixel
 * read-modify-write stalls ~90 cycles/px. Blends copy the destination row in with a burst memcpy,
 * blend against internal memory, and burst the result back — several times faster than in-place.
 * 16-byte aligned so the PIE SIMD routines can run on it directly. */
#define ER_ROW_STAGE_MAX 1024
/* One staging slot per CPU core: render workers are core-pinned, so indexing by the core id gives
 * every concurrent blit its own buffers with no locking (slot 0 doubles as the single-core slot). */
__attribute__((aligned(STAGE_ALIGNMENT))) static fbpx_t s_row_stage_pool[STAGE_CORE_COUNT][ER_ROW_STAGE_MAX];

/** @brief This core's row staging buffer. */
static inline fbpx_t* row_stage(void)
{
    return s_row_stage_pool[xPortGetCoreID()];
}
#if ER_LCD_PIE_ACTIVE
/* Aligned source staging for the SIMD path when a source row is not 16-byte aligned. */
__attribute__((aligned(STAGE_ALIGNMENT))) static uint32_t s_pie_source_pool[STAGE_CORE_COUNT][ER_ROW_STAGE_MAX];

/** @brief This core's SIMD source staging buffer. */
static inline uint32_t* pie_src_stage(void)
{
    return s_pie_source_pool[xPortGetCoreID()];
}
static bool s_pie = false; /* PIE routines enabled (self-test passed at init) */
#endif

/** @brief Fills a rect with a straight-alpha ARGB8888 color, composited over the framebuffer. */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    flip_gate();
    if (!clip_rect(&x, &y, &width, &height))
    {
        return;
    }
    /* Premultiply the straight-alpha fill color, then composite like copy/blend. */
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
            /* Translucent fill: stage the row internally (see blend_cb). */
            fbpx_t* stage = (width <= ER_ROW_STAGE_MAX) ? row_stage() : destination_row;
            if (stage != destination_row)
                memcpy(stage, destination_row, (size_t)width * sizeof(fbpx_t));
            int first_scalar_column = 0;
#if ER_LCD_PIE_ACTIVE
            if (alpha != 0U && s_pie && stage == row_stage() && width >= ER_PIE_LANES)
            {
                const int group_count = width / ER_PIE_LANES;
                er_pie_fill_row_565(stage, source_pixel, group_count);

                for (int column = group_count * ER_PIE_LANES; column < width; column++)
                {
                    stage[column] = er_pie_blend_pixel_565(
                        stage[column], source_pixel, ER_PIE_ALPHA_OPAQUE, ER_PIE_FILL_MATH, column);
                }
                first_scalar_column = width;
            }
#endif
            for (int column = first_scalar_column; column < width; column++)
            {
                stage[column] = over_premul_fast(stage[column], source_pixel);
            }
            if (stage != destination_row)
                memcpy(destination_row, stage, (size_t)width * sizeof(fbpx_t));
        }
    }
    mark_dirty(x, y, width, height);
}

/** @brief Copies a premultiplied ARGB8888 buffer into the framebuffer (premultiplied over). */
static void copy_cb(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    flip_gate();
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
            const uint32_t source_pixel = source_row[column];
            const uint32_t source_alpha = source_pixel >> 24;
            if (source_alpha == ALPHA_OPAQUE)
            {
                destination_row[column] = fb_store(source_pixel);
            }
            else if (source_alpha != 0U)
            {
                destination_row[column] = over_premul_fast(destination_row[column], source_pixel);
            }
        }
    }
    mark_dirty(x, y, width, height);
}

/** @brief Copies a KNOWN-FULLY-OPAQUE source in its native format into the framebuffer (replace).
 *
 *  The engine only routes buffers its registration-time opacity scan proved opaque (RGB565 is
 *  opaque by construction), so there is no per-pixel alpha branch and no destination read. On the
 *  RGB565 canonical framebuffer a 565 source row is a straight burst memcpy — the whole point of
 *  the 16-bit bake: a full-screen background repaint is h row copies, not ~1M CPU conversions. */
static void copy_fmt_cb(
    const void* source, int source_stride_bytes, ERImageFormat format, int x, int y, int width, int height, void* ctx)
{
    (void)ctx;
    flip_gate();
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
#if ER_LCD_FB_RGB565
            memcpy(destination_row, source_row, (size_t)width * sizeof(fbpx_t));
#else
            for (int column = 0; column < width; column++)
            {
                /* Expand 565 → opaque ARGB canonical pixel (bit replication, matching the engine). */
                const uint32_t red5 = (source_row[column] >> 11) & 0x1FU;
                const uint32_t green6 = (source_row[column] >> 5) & 0x3FU;
                const uint32_t blue5 = source_row[column] & 0x1FU;
                destination_row[column] = ARGB_OPAQUE_ALPHA | (((red5 << 3) | (red5 >> 2)) << 16)
                                          | (((green6 << 2) | (green6 >> 4)) << 8) | ((blue5 << 3) | (blue5 >> 2));
            }
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
    flip_gate();
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
        /* Stage the destination row in internal DRAM: one burst in, blend internally, one
         * burst out — instead of a ~90-cycle PSRAM read-modify-write per pixel. */
        fbpx_t* stage = (width <= ER_ROW_STAGE_MAX) ? row_stage() : destination_row;
        if (stage != destination_row)
            memcpy(stage, destination_row, (size_t)width * sizeof(fbpx_t));
        int first_scalar_column = 0;
#if ER_LCD_PIE_ACTIVE
        /* SIMD path: 8 px per iteration on the aligned stage row, plus a matching scalar mop-up
         * for the tail. A misaligned source row is burst-copied to the aligned staging first. */
        if (s_pie && stage == row_stage() && width >= ER_PIE_LANES)
        {
            const uint32_t* simd_source = source_row;
            if (((uintptr_t)source_row & (STAGE_ALIGNMENT - 1U)) != 0U)
            {
                memcpy(pie_src_stage(), source_row, (size_t)width * sizeof(uint32_t));
                simd_source = pie_src_stage();
            }
            const int group_count = width / ER_PIE_LANES;
            /* Dither phase = (x + y) parity of the row's first pixel, so the checkerboard is
             * anchored to screen coordinates (spatially stable across frames and regions). */
            const int dither_phase = ER_LCD_DITHER ? ((x + y + row) & 1) : ER_PIE_DITHER_UNIFORM;
            er_pie_blend_row_565(stage, simd_source, group_count, (uint8_t)global_alpha, dither_phase);

            for (int column = group_count * ER_PIE_LANES; column < width; column++)
            {
                stage[column] = er_pie_blend_pixel_565(
                    stage[column], simd_source[column], (uint8_t)global_alpha, dither_phase, column);
            }
            first_scalar_column = width;
        }
#endif
        if (global_alpha == ALPHA_OPAQUE)
        {
            /* Full-alpha path — the common case for composited strips, where most pixels are
             * fully opaque: no global-alpha scaling, opaque pixels skip the dst load. */
            for (int column = first_scalar_column; column < width; column++)
            {
                const uint32_t source_pixel = source_row[column];
                const uint32_t source_alpha = source_pixel >> 24;
                if (source_alpha == ALPHA_OPAQUE)
                {
                    stage[column] = fb_store(source_pixel);
                }
                else if (source_alpha != 0U)
                {
                    stage[column] = over_premul_fast(stage[column], source_pixel);
                }
            }
        }
        else
        {
            for (int column = first_scalar_column; column < width; column++)
            {
                /* Scale the premultiplied source by the global alpha (paired lanes), then over. */
                const uint32_t source_pixel = source_row[column];
                if (source_pixel == 0U)
                    continue;
                stage[column] = over_premul_fast(stage[column], scale_premul_fast(source_pixel, global_alpha));
            }
        }
        if (stage != destination_row)
            memcpy(destination_row, stage, (size_t)width * sizeof(fbpx_t));
    }
    mark_dirty(x, y, width, height);
}

bool er_esp32_lcd_pie_enabled(void)
{
#if ER_LCD_PIE_ACTIVE
    return s_pie;
#else
    return false;
#endif
}

/** @brief Pushes the painted (dirty) region to the panel, re-compositing the overlay. */
void er_esp32_lcd_present(void)
{
    ERRect damage;
    const bool has_damage = take_dirty(&damage);

    if (s_lcd.direct)
    {
        /* Direct mode: the engine composited straight into the back panel framebuffer, and its
         * multi-buffer damage replay already repainted everything this buffer missed while it was
         * off-screen — the dirty box therefore covers every pixel written since this buffer was
         * last flipped, and is exactly the cache-writeback window draw_bitmap needs. Rotation is 0,
         * so logical and panel coordinates agree. */
        const bool overlay_due = s_lcd.overlay_active && s_lcd.overlay_presents_due > 0;
        if (!has_damage && !overlay_due)
        {
            return; /* nothing changed: no flip, and the engine's buffer ring must not advance */
        }
        ERRect flush = has_damage ? damage : (ERRect){0, 0, 0, 0};
        if (overlay_due || (has_damage && region_covers_overlay(&damage)))
        {
            flip_gate(); /* the stamp writes the back buffer too */
            stamp_overlay();
        }
        if (overlay_due)
        {
            const ERRect overlay = overlay_rect();
            union_rect(&flush, &overlay);
            s_lcd.overlay_presents_due--;
        }

        /* A frame boundary confirms every earlier flip: drain the grant (if any) BEFORE queuing
         * this flip so the pending count only tracks flips the panel hasn't absorbed yet. */
        if (s_lcd.flip_done && xSemaphoreTake(s_lcd.flip_done, 0) == pdTRUE)
        {
            s_lcd.pending_flips = 0;
        }

        /* Flip: draw_bitmap with a panel-owned buffer writes back the cache over the given window
         * and switches the scan-out buffer at the next frame boundary. The next frame's first
         * write into the new back buffer gates on flip_gate() — with three framebuffers that
         * effectively never blocks; with two it waits up to one refresh. */
        esp_lcd_panel_draw_bitmap(
            s_lcd.panel, flush.x, flush.y, flush.x + flush.width, flush.y + flush.height, s_lcd.framebuffer);
        s_lcd.pending_flips++;
        s_lcd.back = (s_lcd.back + 1) % s_lcd.panel_framebuffer_count;
        s_lcd.framebuffer = (fbpx_t*)s_lcd.panel_framebuffers[s_lcd.back];
        er_display_present(); /* the next commit targets the new back buffer's damage debt */
        return;
    }

    const bool page_flips = s_lcd.panel_framebuffer_count > 1;
    /* First two presents with page flips draw the whole frame so both framebuffers start valid. */
    const bool full_frame = page_flips && s_lcd.present_count < INITIAL_FULL_PRESENTS;

    /* App region to push. With page flips the back framebuffer is two presents stale, so union the
     * current and previous dirty boxes; with one framebuffer present draws the live one, so only the
     * current box is needed. */
    ERRect region = {0, 0, 0, 0};
    if (full_frame)
    {
        region = (ERRect){0, 0, s_lcd.width, s_lcd.height};
        s_lcd.present_count++;
    }
    else
    {
        if (has_damage)
        {
            region = damage;
        }
        if (page_flips && s_lcd.previous_has_damage)
        {
            union_rect(&region, &s_lcd.previous_damage);
        }
    }
    s_lcd.previous_damage = damage;
    s_lcd.previous_has_damage = has_damage;

    const bool has_region = region.width > 0 && region.height > 0;
    const bool overlay_due = s_lcd.overlay_active && s_lcd.overlay_presents_due > 0;
    if (!has_region && !overlay_due)
    {
        return;
    }

    uint16_t* destination = s_lcd.panel_framebuffers[page_flips ? s_lcd.back : 0];
    ERRect bounds = {0, 0, 0, 0}; /* panel coordinates */
    /* Where the app drew over the overlay, put it back before the region goes out. */
    if (region_covers_overlay(&region))
    {
        stamp_overlay();
    }
    if (has_region)
    {
        present_region(destination, &region, &bounds);
    }
    /* A fresh capture goes out as its own tight region, not unioned into the app's, so a small corner
     * overlay never enlarges the app copy. */
    if (overlay_due)
    {
        const ERRect overlay = overlay_rect();
        present_region(destination, &overlay, &bounds);
        s_lcd.overlay_presents_due--;
    }
    /* draw_bitmap with a panel-owned buffer copies nothing: it writes the cache back over the window
     * and, with page flips, switches the scan-out buffer at the next frame boundary. */
    esp_lcd_panel_draw_bitmap(
        s_lcd.panel, bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height, destination);
    if (page_flips)
    {
        s_lcd.back ^= 1;
    }
}

/** @brief Saves the current dirty box before an overlay is drawn into the framebuffer (see capture). */
void er_esp32_lcd_overlay_begin(void)
{
    s_lcd.saved_dirty_x0 = s_lcd.dirty_x0;
    s_lcd.saved_dirty_y0 = s_lcd.dirty_y0;
    s_lcd.saved_dirty_x_end = s_lcd.dirty_x_end;
    s_lcd.saved_dirty_y_end = s_lcd.dirty_y_end;
}

/** @brief Snapshots a framebuffer rect as the persistent overlay kept on top of the app. */
void er_esp32_lcd_overlay_capture(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0 || !clip_rect(&x, &y, &width, &height))
    {
        s_lcd.overlay_active = false;
        /* Still roll back the dirty box: the overlay was drawn (dirtying its rect) but is composited
         * separately, so it must not enlarge the app's flush. */
        s_lcd.dirty_x0 = s_lcd.saved_dirty_x0;
        s_lcd.dirty_y0 = s_lcd.saved_dirty_y0;
        s_lcd.dirty_x_end = s_lcd.saved_dirty_x_end;
        s_lcd.dirty_y_end = s_lcd.saved_dirty_y_end;
        return;
    }
    const int pixel_count = width * height;
    if (pixel_count > s_lcd.overlay_capacity)
    {
        fbpx_t* new_cache = (fbpx_t*)heap_caps_malloc((size_t)pixel_count * sizeof(fbpx_t), MALLOC_CAP_SPIRAM);
        if (!new_cache)
        {
            s_lcd.overlay_active = false;
            return;
        }
        if (s_lcd.overlay_cache)
        {
            heap_caps_free(s_lcd.overlay_cache);
        }
        s_lcd.overlay_cache = new_cache;
        s_lcd.overlay_capacity = pixel_count;
    }
    for (int row = 0; row < height; row++)
    {
        memcpy(s_lcd.overlay_cache + (size_t)row * width,
               s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x,
               (size_t)width * sizeof(fbpx_t));
    }
    s_lcd.overlay_x = x;
    s_lcd.overlay_y = y;
    s_lcd.overlay_width = width;
    s_lcd.overlay_height = height;
    s_lcd.overlay_active = true;
    s_lcd.overlay_presents_due = s_lcd.panel_framebuffer_count > 1 ? s_lcd.panel_framebuffer_count : 1;

    /* Roll the dirty box back to its pre-overlay state: the overlay's rect is composited every present
     * from the cache, so it must not stay in (and balloon) the app's per-frame dirty region. */
    s_lcd.dirty_x0 = s_lcd.saved_dirty_x0;
    s_lcd.dirty_y0 = s_lcd.saved_dirty_y0;
    s_lcd.dirty_x_end = s_lcd.saved_dirty_x_end;
    s_lcd.dirty_y_end = s_lcd.saved_dirty_y_end;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief ISR callback: the previously displayed framebuffer is now safe to draw into. */
static bool
frame_buf_complete_cb(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* event_data, void* user_ctx)
{
    (void)panel;
    (void)event_data;
    (void)user_ctx;
    BaseType_t higher_priority_woken = pdFALSE;
    if (s_lcd.flip_done)
    {
        xSemaphoreGiveFromISR(s_lcd.flip_done, &higher_priority_woken);
    }
    return higher_priority_woken == pdTRUE;
}

bool er_esp32_lcd_backend_init(esp_lcd_panel_handle_t panel)
{
    int width = 0;
    int height = 0;
    if (!er_get_display_size(&width, &height))
    {
        ESP_LOGE(TAG, "no display size: call er_set_display_rotation() first");
        return false;
    }
    const ERDisplayRotation rotation = er_get_display_rotation();
    const bool swapped = rotation == ER_ROTATION_90 || rotation == ER_ROTATION_270;
    s_lcd.panel = panel;
    s_lcd.width = width; /* logical (canonical framebuffer) */
    s_lcd.height = height;
    s_lcd.panel_width = swapped ? height : width;
    s_lcd.panel_height = swapped ? width : height;
    s_lcd.dirty_x0 = width;
    s_lcd.dirty_y0 = height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
    s_lcd.previous_has_damage = false;
    s_lcd.present_count = 0;
    s_lcd.overlay_active = false;

    /* Tear-free path: if the panel exposes rotating framebuffers (num_fbs 2 or 3), draw the
     * off-screen one and let draw_bitmap swap it in at vsync. With one, present draws the live
     * framebuffer (can tear). */
    void* framebuffer0 = NULL;
    void* framebuffer1 = NULL;
    void* framebuffer2 = NULL;
    s_lcd.panel_framebuffer_count = 0;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel, TRIPLE_BUFFER_COUNT, &framebuffer0, &framebuffer1, &framebuffer2)
            == ESP_OK
        && framebuffer0 && framebuffer1 && framebuffer2 && framebuffer0 != framebuffer1 && framebuffer0 != framebuffer2
        && framebuffer1 != framebuffer2)
    {
        s_lcd.panel_framebuffer_count = TRIPLE_BUFFER_COUNT;
    }
    else if (esp_lcd_rgb_panel_get_frame_buffer(panel, DOUBLE_BUFFER_COUNT, &framebuffer0, &framebuffer1) == ESP_OK
             && framebuffer0 && framebuffer1 && framebuffer0 != framebuffer1)
    {
        s_lcd.panel_framebuffer_count = DOUBLE_BUFFER_COUNT;
        framebuffer2 = NULL;
    }
    else if (esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &framebuffer0) == ESP_OK && framebuffer0)
    {
        s_lcd.panel_framebuffer_count = 1;
        framebuffer1 = NULL;
        framebuffer2 = NULL;
    }
    else
    {
        ESP_LOGE(TAG, "the panel has no framebuffer");
        return false;
    }
    s_lcd.panel_framebuffers[0] = (uint16_t*)framebuffer0;
    s_lcd.panel_framebuffers[1] = (uint16_t*)framebuffer1;
    s_lcd.panel_framebuffers[2] = (uint16_t*)framebuffer2;
    s_lcd.back = s_lcd.panel_framebuffer_count > 1 ? 1 : 0; /* with page flips fb0 shows first; draw into fb1 */

    /* Direct mode: composite straight into the panel framebuffers. Needs the canonical format to BE
     * the panel format (RGB565), an identity rotation (blits land at panel coordinates), and two or
     * more panel framebuffers. The engine's multi-buffer damage replay repaints whatever each buffer
     * missed while the other was displayed, so no canonical framebuffer is required at all. */
    s_lcd.direct = (ER_LCD_DIRECT != 0) && (ER_LCD_FB_RGB565 != 0) && rotation == ER_ROTATION_0
                   && s_lcd.panel_framebuffer_count > 1;

    if (s_lcd.direct)
    {
        /* The flip must be sequenced: after draw_bitmap queues a buffer switch, the swapped-out
         * buffer only becomes safe to draw into at the next frame boundary. Without that signal,
         * direct mode would tear — fall back to the canonical path instead. */
        s_lcd.flip_done = xSemaphoreCreateBinary();
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
            .on_frame_buf_complete = frame_buf_complete_cb,
        };
        if (!s_lcd.flip_done || esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, NULL) != ESP_OK)
        {
            ESP_LOGW(TAG, "frame_buf_complete callback unavailable; using the canonical-fb path");
            if (s_lcd.flip_done)
            {
                vSemaphoreDelete(s_lcd.flip_done);
                s_lcd.flip_done = NULL;
            }
            s_lcd.direct = false;
        }
    }

    /* The copy path flips between two framebuffers, each two presents stale, which the previous
     * damage box covers. A third goes unused there. */
    if (!s_lcd.direct && s_lcd.panel_framebuffer_count > DOUBLE_BUFFER_COUNT)
    {
        s_lcd.panel_framebuffer_count = DOUBLE_BUFFER_COUNT;
        s_lcd.panel_framebuffers[2] = NULL;
    }

    if (s_lcd.direct)
    {
        /* All panel framebuffers start as the composite target; clear them to opaque black. */
        for (int index = 0; index < s_lcd.panel_framebuffer_count; index++)
        {
            memset(s_lcd.panel_framebuffers[index], 0, (size_t)width * height * sizeof(uint16_t));
        }
        /* Push fb0 full-screen once: writes the cleared pixels back through the cache and makes
         * fb0 the displayed buffer, so the panel shows black instead of allocation garbage. */
        esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, s_lcd.panel_framebuffers[0]);
        s_lcd.framebuffer = (fbpx_t*)s_lcd.panel_framebuffers[s_lcd.back];
        s_lcd.pending_flips = 0;
    }
    else
    {
        s_lcd.framebuffer = (fbpx_t*)heap_caps_malloc((size_t)width * height * sizeof(fbpx_t), MALLOC_CAP_SPIRAM);
        if (!s_lcd.framebuffer)
        {
            ESP_LOGE(TAG,
                     "framebuffer alloc failed (need %d KB PSRAM)",
                     (int)((size_t)width * height * sizeof(fbpx_t) / BYTES_PER_KB));
            return false;
        }
        /* Clear the framebuffer to opaque black so the first frame composites over a known background. */
        for (size_t index = 0; index < (size_t)width * height; index++)
        {
            s_lcd.framebuffer[index] = FB_CLEAR_PX;
        }
#if !ER_LCD_FB_RGB565
        s_lcd.shadow = (uint16_t*)heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_lcd.shadow)
        {
            ESP_LOGE(TAG, "RGB565 shadow alloc failed");
            heap_caps_free(s_lcd.framebuffer);
            s_lcd.framebuffer = NULL;
            return false;
        }
#endif
    }

    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.copy_rect_format = copy_fmt_cb;
    backend.blend_rect = blend_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL; /* engine doesn't auto-present; the host calls er_esp32_lcd_present() */
    backend.context = NULL;
    embedded_renderer_set_backend(&backend);
    /* Direct mode rotates the panel framebuffers; tell the engine so each commit repaints the
     * target buffer's accumulated damage debt (see er_set_display_buffer_count in er_scene.h). */
    er_set_display_buffer_count(s_lcd.direct ? s_lcd.panel_framebuffer_count : 1);

#if ER_LCD_PIE_ACTIVE
    /* Verify the SIMD blends against the scalar reference before trusting them. */
    s_pie = er_pie_blend_selftest();
    ESP_LOGI(TAG, "PIE SIMD blend: %s", s_pie ? "self-test passed, enabled" : "SELF-TEST FAILED, using C loops");
#endif

    ESP_LOGI(TAG,
             "LCD backend ready: logical %dx%d, panel %dx%d, rotation %d°, %s %s",
             width,
             height,
             s_lcd.panel_width,
             s_lcd.panel_height,
             (int)rotation,
             s_lcd.direct ? "DIRECT to panel fbs," : "canonical fb",
             s_lcd.direct       ? "no canonical fb"
             : ER_LCD_FB_RGB565 ? "RGB565"
                                : "ARGB8888");
    return true;
}
