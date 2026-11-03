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
 * DIRECT MODE (ER_LCD_DIRECT, default 1): when the canonical format is RGB565, rotation is 0, and
 * the panel exposes 2–3 rotating framebuffers, the engine composites STRAIGHT into the panel
 * framebuffers — no canonical framebuffer and no present copy. The engine's multi-buffer damage
 * replay (er_set_display_buffer_count / er_display_present) repaints whatever each buffer missed
 * while the others were displayed. Present is then just a cache writeback of the dirty window plus
 * a buffer flip; with three framebuffers (num_fbs=3) there is always a swapped-out buffer ready to
 * draw into, so the flip never blocks. The canonical-framebuffer path below remains for rotated configs,
 * ARGB8888 canonical builds, and single-framebuffer (copy mode) panels.
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

/** @brief Emits n canonical pixels to an RGB565 output row — a straight copy (already 565). */
static inline void fb_to_565_row(uint16_t* destination, const fbpx_t* source, int count)
{
    memcpy(destination, source, (size_t)count * sizeof(uint16_t));
}

/** @brief Converts one canonical pixel to RGB565 (already 565 — identity). Used by rotated present. */
static inline uint16_t fb_to_565_px(fbpx_t pixel)
{
    return (uint16_t)pixel;
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

/** @brief Converts one ARGB8888 canonical pixel to RGB565. Used by rotated present. */
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
    int panel_width;  /**< Physical panel width (= width/height swapped for 90/270). */
    int panel_height; /**< Physical panel height. */
    int rotation;     /**< Clockwise display rotation: 0, 90, 180, or 270. */
    fbpx_t*
        framebuffer; /**< Composite target: the canonical framebuffer, or (direct mode) the back panel framebuffer. */
    uint16_t* panel_framebuffers[TRIPLE_BUFFER_COUNT]; /**< The panel's rotating RGB565 framebuffers;
                                                          panel_framebuffers[0] == NULL = copy mode. */
    int panel_framebuffer_count; /**< Number of panel framebuffers in panel_framebuffers (2 or 3; 0 in copy mode). */
    int back;                    /**< Index of the off-screen framebuffer to draw next. */
    uint16_t* line;              /**< RGB565 staging buffer (copy mode only, when double buffering is unavailable). */
    bool direct;                 /**< Direct mode: the engine composites straight into the panel framebuffers and the
                                      engine's multi-buffer damage replay keeps them all current — no canonical framebuffer,
                                      no present copy. Requires RGB565 canonical format, rotation 0, and 2+ panel framebuffers. */
    SemaphoreHandle_t flip_done; /**< Given at each frame boundary: all earlier flips have taken effect. */
    int pending_flips;           /**< Flips requested since the last confirmed frame boundary. */
    /* Current dirty box: inclusive min corner, EXCLUSIVE max corner (matches ERRect). dirty_x_end <= dirty_x0
     * means "empty". The panel's own draw_bitmap window is exclusive too; the inclusive locals in
     * present() exist only because rot_box()/present_region() work in inclusive corners. */
    int dirty_x0, dirty_y0, dirty_x_end, dirty_y_end;
    /* Previous present's dirty box: unioned in so the back buffer (two presents stale) catches up. */
    int previous_dirty_x0, previous_dirty_y0, previous_dirty_x_end, previous_dirty_y_end;
    int present_count; /**< First two presents draw full-screen to initialise both framebuffers. */
    /* Persistent overlay: an RGB565 snapshot (see er_esp32_lcd_overlay_capture) re-composited on top
     * of every present, so a small always-on overlay stays consistent across both framebuffers and is
     * flushed as its own tight region — not unioned into (and ballooning) the app's dirty box. */
    uint16_t* overlay_cache;
    int overlay_capacity; /**< Allocated capacity of overlay_cache in pixels. */
    int overlay_x, overlay_y, overlay_width, overlay_height;
    bool overlay_active;
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

/** @brief Maps a logical (canonical-framebuffer) pixel to its physical (panel) position under the rotation. */
static inline void rot_point(int logical_x, int logical_y, int* physical_x, int* physical_y)
{
    switch (s_lcd.rotation)
    {
        case 90:
            *physical_x = s_lcd.panel_width - 1 - logical_y;
            *physical_y = logical_x;
            break;
        case 180:
            *physical_x = s_lcd.panel_width - 1 - logical_x;
            *physical_y = s_lcd.panel_height - 1 - logical_y;
            break;
        case 270:
            *physical_x = logical_y;
            *physical_y = s_lcd.panel_height - 1 - logical_x;
            break;
        default:
            *physical_x = logical_x;
            *physical_y = logical_y;
            break;
    }
}

/** @brief Maps a logical box (inclusive) to its physical bounding box (still axis-aligned). */
static void
rot_box(int x0, int y0, int x1, int y1, int* physical_x0, int* physical_y0, int* physical_x1, int* physical_y1)
{
    int first_x, first_y, second_x, second_y;
    rot_point(x0, y0, &first_x, &first_y);
    rot_point(x1, y1, &second_x, &second_y);
    *physical_x0 = first_x < second_x ? first_x : second_x;
    *physical_y0 = first_y < second_y ? first_y : second_y;
    *physical_x1 = first_x > second_x ? first_x : second_x;
    *physical_y1 = first_y > second_y ? first_y : second_y;
}

/**
 * @brief Writes the logical region [x0..x1]x[y0..y1] of the canonical framebuffer into a physical 565 buffer.
 *
 * destination has row stride destination_stride pixels, and its element (0,0) is physical
 * (destination_origin_x, destination_origin_y). At 0° this is the fast per-row 565 copy; otherwise each pixel is placed
 * at its rotated physical position.
 */
static void present_region(uint16_t* destination,
                           int destination_stride,
                           int destination_origin_x,
                           int destination_origin_y,
                           int x0,
                           int y0,
                           int x1,
                           int y1)
{
    if (s_lcd.rotation == 0)
    {
        const int region_width = x1 - x0 + 1;
        for (int logical_y = y0; logical_y <= y1; logical_y++)
        {
            const fbpx_t* source_row = s_lcd.framebuffer + (size_t)logical_y * s_lcd.width + x0;
            uint16_t* output_row = destination + (size_t)(logical_y - destination_origin_y) * destination_stride
                                   + (x0 - destination_origin_x);
            fb_to_565_row(output_row, source_row, region_width);
        }
        return;
    }
    for (int logical_y = y0; logical_y <= y1; logical_y++)
    {
        const fbpx_t* source_row = s_lcd.framebuffer + (size_t)logical_y * s_lcd.width;
        for (int logical_x = x0; logical_x <= x1; logical_x++)
        {
            int physical_x, physical_y;
            rot_point(logical_x, logical_y, &physical_x, &physical_y);
            destination[(size_t)(physical_y - destination_origin_y) * destination_stride
                        + (physical_x - destination_origin_x)] = fb_to_565_px(source_row[logical_x]);
        }
    }
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

/** @brief Composites the cached RGB565 overlay (logical coords) onto the physical panel buffer. */
static void blit_overlay(uint16_t* destination)
{
    if (s_lcd.rotation == 0)
    {
        for (int row = 0; row < s_lcd.overlay_height; row++)
        {
            const uint16_t* source_row = s_lcd.overlay_cache + (size_t)row * s_lcd.overlay_width;
            uint16_t* output_row = destination + (size_t)(s_lcd.overlay_y + row) * s_lcd.panel_width + s_lcd.overlay_x;
            memcpy(output_row, source_row, (size_t)s_lcd.overlay_width * sizeof(uint16_t));
        }
        return;
    }
    for (int row = 0; row < s_lcd.overlay_height; row++)
    {
        const int logical_y = s_lcd.overlay_y + row;
        const uint16_t* source_row = s_lcd.overlay_cache + (size_t)row * s_lcd.overlay_width;
        for (int column = 0; column < s_lcd.overlay_width; column++)
        {
            int physical_x, physical_y;
            rot_point(s_lcd.overlay_x + column, logical_y, &physical_x, &physical_y);
            destination[(size_t)physical_y * s_lcd.panel_width + physical_x] = source_row[column];
        }
    }
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
                    stage[column] =
                        er_pie_blend_px_565(stage[column], source_pixel, ER_PIE_ALPHA_OPAQUE, ER_PIE_FILL_MATH, column);
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
    if (format == ER_IMG_RGB565)
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
                stage[column] = er_pie_blend_px_565(
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
    if (s_lcd.direct)
    {
        /* Direct mode: the engine composited straight into the back panel framebuffer, and its
         * multi-buffer damage replay already repainted everything this buffer missed while it was
         * off-screen — the dirty box therefore covers every pixel written since this buffer was
         * last flipped, and is exactly the cache-writeback window draw_bitmap needs. */
        bool has_damage = (s_lcd.dirty_x_end > s_lcd.dirty_x0 && s_lcd.dirty_y_end > s_lcd.dirty_y0);
        int x0 = s_lcd.dirty_x0, y0 = s_lcd.dirty_y0, x1 = s_lcd.dirty_x_end - 1, y1 = s_lcd.dirty_y_end - 1;
        s_lcd.dirty_x0 = s_lcd.width;
        s_lcd.dirty_y0 = s_lcd.height;
        s_lcd.dirty_x_end = 0;
        s_lcd.dirty_y_end = 0;
        if (!has_damage && !s_lcd.overlay_active)
        {
            return; /* nothing changed: no flip, and the engine's buffer ring must not advance */
        }

        uint16_t* destination_framebuffer = (uint16_t*)s_lcd.framebuffer;
        /* Re-stamp the persistent overlay onto this buffer (its own tight region). */
        if (s_lcd.overlay_active)
        {
            flip_gate(); /* the stamp writes the back buffer too */
            blit_overlay(destination_framebuffer);
            if (!has_damage)
            {
                x0 = s_lcd.overlay_x;
                y0 = s_lcd.overlay_y;
                x1 = s_lcd.overlay_x + s_lcd.overlay_width - 1;
                y1 = s_lcd.overlay_y + s_lcd.overlay_height - 1;
            }
            else
            {
                if (s_lcd.overlay_x < x0)
                    x0 = s_lcd.overlay_x;
                if (s_lcd.overlay_y < y0)
                    y0 = s_lcd.overlay_y;
                if (s_lcd.overlay_x + s_lcd.overlay_width - 1 > x1)
                    x1 = s_lcd.overlay_x + s_lcd.overlay_width - 1;
                if (s_lcd.overlay_y + s_lcd.overlay_height - 1 > y1)
                    y1 = s_lcd.overlay_y + s_lcd.overlay_height - 1;
            }
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
        esp_lcd_panel_draw_bitmap(s_lcd.panel, x0, y0, x1 + 1, y1 + 1, destination_framebuffer);
        s_lcd.pending_flips++;
        s_lcd.back = (s_lcd.back + 1) % s_lcd.panel_framebuffer_count;
        s_lcd.framebuffer = (fbpx_t*)s_lcd.panel_framebuffers[s_lcd.back];
        er_display_present(); /* the next commit targets the new back buffer's damage debt */
        return;
    }

    const bool double_buffered = (s_lcd.panel_framebuffers[0] != NULL);
    /* First two presents in double-buffer mode draw the whole frame so both framebuffers start valid. */
    const bool full_frame = double_buffered && s_lcd.present_count < INITIAL_FULL_PRESENTS;

    /* App dirty region to push. In double-buffer mode the back framebuffer is two presents stale, so
     * union the current and previous dirty boxes; in copy mode draw_bitmap writes the live framebuffer,
     * so only the current box is needed. */
    bool has_damage = false;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    if (full_frame)
    {
        x0 = 0;
        y0 = 0;
        x1 = s_lcd.width - 1;
        y1 = s_lcd.height - 1;
        has_damage = true;
    }
    else
    {
        if (s_lcd.dirty_x_end > s_lcd.dirty_x0 && s_lcd.dirty_y_end > s_lcd.dirty_y0)
        {
            x0 = s_lcd.dirty_x0;
            y0 = s_lcd.dirty_y0;
            x1 = s_lcd.dirty_x_end - 1;
            y1 = s_lcd.dirty_y_end - 1;
            has_damage = true;
        }
        if (double_buffered && s_lcd.previous_dirty_x_end > s_lcd.previous_dirty_x0
            && s_lcd.previous_dirty_y_end > s_lcd.previous_dirty_y0)
        {
            if (!has_damage)
            {
                x0 = s_lcd.previous_dirty_x0;
                y0 = s_lcd.previous_dirty_y0;
                x1 = s_lcd.previous_dirty_x_end - 1;
                y1 = s_lcd.previous_dirty_y_end - 1;
                has_damage = true;
            }
            else
            {
                if (s_lcd.previous_dirty_x0 < x0)
                    x0 = s_lcd.previous_dirty_x0;
                if (s_lcd.previous_dirty_y0 < y0)
                    y0 = s_lcd.previous_dirty_y0;
                if (s_lcd.previous_dirty_x_end - 1 > x1)
                    x1 = s_lcd.previous_dirty_x_end - 1;
                if (s_lcd.previous_dirty_y_end - 1 > y1)
                    y1 = s_lcd.previous_dirty_y_end - 1;
            }
        }
    }

    /* Roll current → previous, then reset the current box (we've captured everything we need above). */
    s_lcd.previous_dirty_x0 = s_lcd.dirty_x0;
    s_lcd.previous_dirty_y0 = s_lcd.dirty_y0;
    s_lcd.previous_dirty_x_end = s_lcd.dirty_x_end;
    s_lcd.previous_dirty_y_end = s_lcd.dirty_y_end;
    s_lcd.dirty_x0 = s_lcd.width;
    s_lcd.dirty_y0 = s_lcd.height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
    if (full_frame)
    {
        s_lcd.present_count++;
    }
    /* Present when the app changed, OR there's an overlay to keep current on this frame's buffer. */
    if (!has_damage && !s_lcd.overlay_active)
    {
        return;
    }

    if (double_buffered)
    {
        uint16_t* destination_framebuffer = s_lcd.panel_framebuffers[s_lcd.back];
        /* App region: map the logical dirty region into the off-screen panel framebuffer with the
         * configured rotation (a plain 565→565 row copy at 0°). */
        if (has_damage)
        {
            present_region(destination_framebuffer, s_lcd.panel_width, 0, 0, x0, y0, x1, y1);
        }
        /* Overlay: re-composite the cached RGB565 panel on top (its own tight region — not unioned
         * into the app's dirty box, so a small corner overlay never enlarges the app flush). */
        if (s_lcd.overlay_active)
        {
            blit_overlay(destination_framebuffer);
        }
        /* draw_bitmap bounds are PHYSICAL panel coords: rotate the app + overlay logical boxes and
         * union them (the per-region writes above stay tight). */
        int bounds_x0 = s_lcd.panel_width, bounds_y0 = s_lcd.panel_height, bounds_x1 = -1, bounds_y1 = -1;
        if (has_damage)
        {
            rot_box(x0, y0, x1, y1, &bounds_x0, &bounds_y0, &bounds_x1, &bounds_y1);
        }
        if (s_lcd.overlay_active)
        {
            int overlay_x0, overlay_y0, overlay_x1, overlay_y1;
            rot_box(s_lcd.overlay_x,
                    s_lcd.overlay_y,
                    s_lcd.overlay_x + s_lcd.overlay_width - 1,
                    s_lcd.overlay_y + s_lcd.overlay_height - 1,
                    &overlay_x0,
                    &overlay_y0,
                    &overlay_x1,
                    &overlay_y1);
            if (overlay_x0 < bounds_x0)
                bounds_x0 = overlay_x0;
            if (overlay_y0 < bounds_y0)
                bounds_y0 = overlay_y0;
            if (overlay_x1 > bounds_x1)
                bounds_x1 = overlay_x1;
            if (overlay_y1 > bounds_y1)
                bounds_y1 = overlay_y1;
        }
        esp_lcd_panel_draw_bitmap(
            s_lcd.panel, bounds_x0, bounds_y0, bounds_x1 + 1, bounds_y1 + 1, destination_framebuffer);
        s_lcd.back ^= 1;
    }
    else
    {
        /* Copy mode: pack the (rotated) region row-tight into the staging buffer; draw_bitmap copies it
         * into the live framebuffer at the physical bounding box. */
        if (has_damage)
        {
            int physical_x0, physical_y0, physical_x1, physical_y1;
            rot_box(x0, y0, x1, y1, &physical_x0, &physical_y0, &physical_x1, &physical_y1);
            present_region(s_lcd.line, physical_x1 - physical_x0 + 1, physical_x0, physical_y0, x0, y0, x1, y1);
            esp_lcd_panel_draw_bitmap(
                s_lcd.panel, physical_x0, physical_y0, physical_x1 + 1, physical_y1 + 1, s_lcd.line);
        }
        if (s_lcd.overlay_active)
        {
            if (s_lcd.rotation == 0)
            {
                esp_lcd_panel_draw_bitmap(s_lcd.panel,
                                          s_lcd.overlay_x,
                                          s_lcd.overlay_y,
                                          s_lcd.overlay_x + s_lcd.overlay_width,
                                          s_lcd.overlay_y + s_lcd.overlay_height,
                                          s_lcd.overlay_cache);
            }
            else
            {
                int physical_x0, physical_y0, physical_x1, physical_y1;
                rot_box(s_lcd.overlay_x,
                        s_lcd.overlay_y,
                        s_lcd.overlay_x + s_lcd.overlay_width - 1,
                        s_lcd.overlay_y + s_lcd.overlay_height - 1,
                        &physical_x0,
                        &physical_y0,
                        &physical_x1,
                        &physical_y1);
                const int physical_box_width = physical_x1 - physical_x0 + 1;
                for (int row = 0; row < s_lcd.overlay_height; row++)
                {
                    const int logical_y = s_lcd.overlay_y + row;
                    const uint16_t* source_row = s_lcd.overlay_cache + (size_t)row * s_lcd.overlay_width;
                    for (int column = 0; column < s_lcd.overlay_width; column++)
                    {
                        int physical_x, physical_y;
                        rot_point(s_lcd.overlay_x + column, logical_y, &physical_x, &physical_y);
                        s_lcd.line[(size_t)(physical_y - physical_y0) * physical_box_width
                                   + (physical_x - physical_x0)] = source_row[column];
                    }
                }
                esp_lcd_panel_draw_bitmap(
                    s_lcd.panel, physical_x0, physical_y0, physical_x1 + 1, physical_y1 + 1, s_lcd.line);
            }
        }
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

/** @brief Snapshots a framebuffer rect as the persistent overlay re-composited on every present. */
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
        uint16_t* new_cache = (uint16_t*)heap_caps_malloc((size_t)pixel_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
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
        const fbpx_t* source_row = s_lcd.framebuffer + (size_t)(y + row) * s_lcd.width + x;
        uint16_t* output_row = s_lcd.overlay_cache + (size_t)row * width;
        fb_to_565_row(output_row, source_row, width);
    }
    s_lcd.overlay_x = x;
    s_lcd.overlay_y = y;
    s_lcd.overlay_width = width;
    s_lcd.overlay_height = height;
    s_lcd.overlay_active = true;

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

bool er_esp32_lcd_backend_init(esp_lcd_panel_handle_t panel, int width, int height, int rotation)
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)
    {
        ESP_LOGE(TAG, "invalid rotation %d (must be 0/90/180/270)", rotation);
        return false;
    }
    s_lcd.panel = panel;
    s_lcd.width = width; /* logical (canonical framebuffer) */
    s_lcd.height = height;
    s_lcd.rotation = rotation;
    /* Physical panel size = logical with width/height swapped for 90/270. */
    s_lcd.panel_width = (rotation == 90 || rotation == 270) ? height : width;
    s_lcd.panel_height = (rotation == 90 || rotation == 270) ? width : height;
    s_lcd.dirty_x0 = width;
    s_lcd.dirty_y0 = height;
    s_lcd.dirty_x_end = 0;
    s_lcd.dirty_y_end = 0;
    s_lcd.previous_dirty_x0 = width;
    s_lcd.previous_dirty_y0 = height;
    s_lcd.previous_dirty_x_end = 0;
    s_lcd.previous_dirty_y_end = 0;
    s_lcd.present_count = 0;
    s_lcd.overlay_active = false;

    /* Tear-free path: if the panel exposes rotating framebuffers (num_fbs 2 or 3), draw the
     * off-screen one and let draw_bitmap swap it in at vsync. Otherwise fall back to a staging
     * buffer that draw_bitmap copies into the live framebuffer (single-buffered; can tear). */
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
    if (s_lcd.panel_framebuffer_count > 0)
    {
        s_lcd.panel_framebuffers[0] = (uint16_t*)framebuffer0;
        s_lcd.panel_framebuffers[1] = (uint16_t*)framebuffer1;
        s_lcd.panel_framebuffers[2] = (uint16_t*)framebuffer2;
        s_lcd.back = 1; /* fb0 is displayed first; draw into fb1 */
        s_lcd.line = NULL;
    }
    else
    {
        s_lcd.panel_framebuffers[0] = NULL;
        s_lcd.panel_framebuffers[1] = NULL;
        s_lcd.panel_framebuffers[2] = NULL;
        s_lcd.line = (uint16_t*)heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_lcd.line)
        {
            ESP_LOGE(TAG, "staging buffer alloc failed");
            return false;
        }
    }

    /* Direct mode: composite straight into the panel framebuffers. Needs the canonical format to BE
     * the panel format (RGB565), an identity rotation (blits land at panel coordinates), and both
     * panel framebuffers. The engine's multi-buffer damage replay repaints whatever each buffer
     * missed while the other was displayed, so no canonical framebuffer is required at all. */
    s_lcd.direct =
        (ER_LCD_DIRECT != 0) && (ER_LCD_FB_RGB565 != 0) && rotation == 0 && s_lcd.panel_framebuffers[0] != NULL;

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
    }

    static EmbeddedRenderBackend backend;
    backend.fill_rect = fill_cb;
    backend.copy_rect = copy_cb;
    backend.copy_rect_fmt = copy_fmt_cb;
    backend.blend_rect = blend_cb;
    backend.wait = NULL;
    backend.frame_ready = NULL; /* engine doesn't auto-present; the host calls er_esp32_lcd_present() */
    backend.ctx = NULL;
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
             rotation,
             s_lcd.direct ? "DIRECT to panel fbs," : "canonical fb",
             s_lcd.direct       ? "no canonical fb"
             : ER_LCD_FB_RGB565 ? "RGB565"
                                : "ARGB8888");
    return true;
}
