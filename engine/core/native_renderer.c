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

#include "native_renderer.h"
#include "er_error_screen.h"
#include "er_perf.h" /* per-blit accounting (compiled to nothing when ER_PERF_STATS == 0) */
#include "er_scene.h"
#include "font_blob.h"
#include "font_registry.h"
#include "image_registry.h"
#include "renderer_internal.h"
#include <stdbool.h>
#include <stddef.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum depth of the scissor clip rectangle stack. */
#define ER_CLIP_STACK_DEPTH 8

/** @brief Origin and size of the full-extent clip er_push_clip_reset() pushes: far past any panel. */
#define ER_CLIP_RESET_ORIGIN (-(1 << 20))
#define ER_CLIP_RESET_SIZE (1 << 21)

/** @brief Pixel sizes of the buffers a blit advances through. */
#define ER_ARGB8888_PIXEL_BYTES 4
#define ER_RGB565_PIXEL_BYTES 2

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One entry in the scissor clip rectangle stack.
 */
typedef struct
{
    int x; /**< Left edge. */
    int y; /**< Top edge. */
    int width;  /**< Width in pixels. */
    int height; /**< Height in pixels. */
} ClipEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static const EmbeddedRenderBackend* g_backend = NULL;

/* Row chunk used to emit fills through the blend path while draw_alpha < 255: backend
 * fill_rect semantics for translucent colors vary (some overwrite), blend_rect is uniform. */
#define ER_FILL_ROW_CHUNK 256

/**
 * @brief Per-worker render state for the blit layer (see er_render_worker_id).
 *
 * Everything here is scoped to one worker's render traversal — pushed/popped or saved/restored
 * within a single render pass — so N workers rendering disjoint screen regions each get their own
 * copy and never contend. With the default single worker this struct is exactly the former
 * file-scope statics under one roof.
 */
typedef struct
{
    ClipEntry clip_stack[ER_CLIP_STACK_DEPTH]; /**< Scissor stack (intersected). */
    int clip_depth;
    /* Pushes dropped because the stack was full. Each pop undoes one of these before it touches
     * clip_depth, so a dropped push's matching pop never removes an outer clip still in use. Content
     * under a dropped clip is clipped only by the deepest entry that fit. */
    int clip_overflow;

    /* Banded rendering. The strip currently being rendered occupies screen rows [band_origin_y, band_origin_y +
     * band_height). This is applied at the BACKEND-EMIT boundary only — NOT via the clip stack — so it
     * bounds what lands in the band buffer without truncating offscreen-scratch (transform /
     * opacity) source rendering, which must always be complete or a transformed node split across a
     * strip seam shows an anti-aliasing gap. Each backend blit is clamped to these rows and its Y
     * translated to the 0-origin band buffer. band_height == 0 disables both (the classic
     * full-framebuffer path). */
    int band_origin_y;
    int band_height;

    /* Scratch redirect: when scratch_buffer is non-NULL all blit calls write into it. */
    uint32_t* scratch_buffer;
    int scratch_width;
    int scratch_height;
    int scratch_origin_x;
    int scratch_origin_y;

    /* Inherited draw alpha: multiplied into every blit while < 255. Set by the compositor when a
     * translucent group cannot be composited through a scratch slot (pool exhausted) — each
     * primitive is then dimmed individually, which is exact wherever siblings don't overlap.
     * Offscreen captures (opacity strips, transform sources) reset it to 255 for the capture and
     * re-apply it when the captured buffer is blended out, so it is never applied twice. */
    uint8_t draw_alpha;

    /* Assembled premultiplied row for the translucent-fill fallback (transient within one blit). */
    uint32_t fill_row[ER_FILL_ROW_CHUNK];

    /* Frame-instrumentation accumulators (er_perf.h): time inside this worker's backend callbacks and
     * the pixels they were handed. Per-worker because blits run on render-worker threads, where the
     * shared er_perf frame state must not be touched; the compositor resets them at raster begin and
     * collects the sums on the commit thread (er_blit_perf_reset / er_blit_perf_collect). Only ever
     * written when ER_PERF_STATS is on — otherwise they just sit at zero. */
    uint32_t perf_blit_us;
    uint32_t perf_blit_pixels;
} NRCtx;

/* Zero-initialized; draw_alpha is set to 255 for every worker in embedded_renderer_set_backend
 * (always called before the first render). */
static NRCtx s_worker_contexts[ERUI_RENDER_WORKERS];

/** @brief The calling worker's blit-layer context (constant &s_worker_contexts[0] in single-worker builds). */
static inline NRCtx* rc(void)
{
    return &s_worker_contexts[er_render_worker_id()];
}

/* Backend-blit accounting: brackets exactly the g_backend callback invocation (after clip and band
 * clamping), so what accumulates is the backend's own pixel-write time and the final pixel count —
 * not the engine-side routing around it. Scratch-buffer writes deliberately do NOT pass through
 * here: they are compositing work and belong to the composite bucket. Two clock reads per backend
 * call when enabled; compiled to nothing when ER_PERF_STATS == 0. */
#if ER_PERF_STATS
#define ER_BLIT_PERF_BEGIN() const uint32_t blit_start_us = er_perf_now_us()
#define ER_BLIT_PERF_END(width, height)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        NRCtx* worker_context = rc();                                                                                  \
        worker_context->perf_blit_us += er_perf_now_us() - blit_start_us;                                              \
        worker_context->perf_blit_pixels += (uint32_t)(width) * (uint32_t)(height);                                    \
    } while (0)
#else
#define ER_BLIT_PERF_BEGIN() ((void)0)
#define ER_BLIT_PERF_END(width, height) ((void)0)
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Clips a destination rectangle (in/out) to the top-most active scissor rectangle.
 *
 * Adjusts x, y, width, height in place.  Returns false when the clipped area is empty.
 *
 * @param[in,out] x       Left edge of the rectangle; updated to the clipped left edge.
 * @param[in,out] y       Top edge of the rectangle; updated to the clipped top edge.
 * @param[in,out] width   Width; updated to the clipped width (may become 0).
 * @param[in,out] height  Height; updated to the clipped height (may become 0).
 *
 * @return true if the clipped rectangle has a positive area; false if it is empty.
 */
static bool apply_clip(int* x, int* y, int* width, int* height)
{
    if (rc()->clip_depth == 0)
        return true;

    const ClipEntry* clip = &rc()->clip_stack[rc()->clip_depth - 1];
    const int right = *x + *width;
    const int bottom = *y + *height;
    const int clip_right = clip->x + clip->width;
    const int clip_bottom = clip->y + clip->height;

    const int clipped_left = *x > clip->x ? *x : clip->x;
    const int clipped_top = *y > clip->y ? *y : clip->y;
    const int clipped_right = right < clip_right ? right : clip_right;
    const int clipped_bottom = bottom < clip_bottom ? bottom : clip_bottom;

    *x = clipped_left;
    *y = clipped_top;
    *width = clipped_right - clipped_left;
    *height = clipped_bottom - clipped_top;
    return *width > 0 && *height > 0;
}

/**
 * @brief Source-over composites one premultiplied ARGB source pixel onto a destination pixel.
 *
 * out.C = src.C + dst.C * (255 - src.A) / 255, with src already premultiplied. Fully opaque
 * sources overwrite and fully transparent sources return the destination unchanged (both exact,
 * and skipping the per-channel divides). Shared by scratch_do_blend and scratch_do_copy so the
 * one blend formula lives in a single place. (scratch_do_fill keeps its own loop: a constant fill
 * color lets it hoist the premultiplied channels out of the per-pixel work.)
 */
static inline uint32_t over_premul_px(uint32_t destination_pixel, uint32_t source_pixel)
{
    const uint8_t source_alpha = (uint8_t)((source_pixel >> 24) & 0xFFU);
    if (source_alpha == 0U)
        return destination_pixel;
    if (source_alpha == ER_ALPHA_OPAQUE)
        return source_pixel;
    const uint8_t inverse_alpha = (uint8_t)(ER_CHANNEL_MAX - source_alpha);
    const uint8_t out_alpha =
        (uint8_t)(source_alpha + (uint32_t)((destination_pixel >> 24) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    const uint8_t out_red = (uint8_t)(((source_pixel >> 16) & 0xFFU)
                                      + (uint32_t)((destination_pixel >> 16) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    const uint8_t out_green =
        (uint8_t)(((source_pixel >> 8) & 0xFFU)
                  + (uint32_t)((destination_pixel >> 8) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    const uint8_t out_blue =
        (uint8_t)((source_pixel & 0xFFU) + (uint32_t)(destination_pixel & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
    return ((uint32_t)out_alpha << 24) | ((uint32_t)out_red << 16) | ((uint32_t)out_green << 8) | out_blue;
}

/**
 * @brief Source-over blends a straight-alpha ARGB fill into the active scratch buffer.
 *
 * Premultiplies the color components before blending so the scratch buffer stays in
 * premultiplied ARGB8888 throughout the compositing pass.
 *
 * @param[in] argb          Straight-alpha ARGB8888 fill color.
 * @param[in] local_x       Scratch-local left edge.
 * @param[in] local_y       Scratch-local top edge.
 * @param[in] local_width   Width in pixels.
 * @param[in] local_height  Height in pixels.
 */
static void scratch_do_fill(uint32_t argb, int local_x, int local_y, int local_width, int local_height)
{
    const uint8_t source_alpha = (uint8_t)((argb >> 24) & 0xFFU);
    if (source_alpha == 0U)
        return;

    /* Clamp once so the pixel loops below run without per-pixel bounds checks. */
    const int left = local_x < 0 ? 0 : local_x;
    const int top = local_y < 0 ? 0 : local_y;
    const int right = (local_x + local_width) > rc()->scratch_width ? rc()->scratch_width : (local_x + local_width);
    const int bottom =
        (local_y + local_height) > rc()->scratch_height ? rc()->scratch_height : (local_y + local_height);
    if (left >= right || top >= bottom)
        return;

    const uint8_t source_red = (uint8_t)((uint32_t)((argb >> 16) & 0xFFU) * source_alpha / ER_CHANNEL_MAX);
    const uint8_t source_green = (uint8_t)((uint32_t)((argb >> 8) & 0xFFU) * source_alpha / ER_CHANNEL_MAX);
    const uint8_t source_blue = (uint8_t)((uint32_t)(argb & 0xFFU) * source_alpha / ER_CHANNEL_MAX);
    const uint8_t inverse_alpha = (uint8_t)(ER_CHANNEL_MAX - source_alpha);

    if (source_alpha == ER_ALPHA_OPAQUE)
    {
        const uint32_t pixel =
            (0xFFU << 24) | ((uint32_t)source_red << 16) | ((uint32_t)source_green << 8) | source_blue;
        for (int row = top; row < bottom; row++)
        {
            uint32_t* destination_row = rc()->scratch_buffer + (size_t)row * rc()->scratch_width;
            for (int column = left; column < right; column++)
                destination_row[column] = pixel;
        }
        return;
    }

    for (int row = top; row < bottom; row++)
    {
        uint32_t* destination_row = rc()->scratch_buffer + (size_t)row * rc()->scratch_width;
        for (int column = left; column < right; column++)
        {
            const uint32_t destination_pixel = destination_row[column];
            const uint8_t out_alpha =
                (uint8_t)(source_alpha
                          + (uint32_t)((destination_pixel >> 24) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            const uint8_t out_red =
                (uint8_t)(source_red + (uint32_t)((destination_pixel >> 16) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            const uint8_t out_green =
                (uint8_t)(source_green + (uint32_t)((destination_pixel >> 8) & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            const uint8_t out_blue =
                (uint8_t)(source_blue + (uint32_t)(destination_pixel & 0xFFU) * inverse_alpha / ER_CHANNEL_MAX);
            destination_row[column] =
                ((uint32_t)out_alpha << 24) | ((uint32_t)out_red << 16) | ((uint32_t)out_green << 8) | out_blue;
        }
    }
}

/**
 * @brief Source-over blends a premultiplied ARGB source into the scratch buffer at a global alpha.
 *
 * @param[in] source        Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride        Source row stride in bytes.
 * @param[in] alpha         Global alpha scale (0 = transparent, 255 = opaque).
 * @param[in] local_x       Scratch-local left edge.
 * @param[in] local_y       Scratch-local top edge.
 * @param[in] local_width   Width in pixels.
 * @param[in] local_height  Height in pixels.
 */
static void scratch_do_blend(
    const void* source, int stride, uint8_t alpha, int local_x, int local_y, int local_width, int local_height)
{
    if (alpha == 0U)
        return;

    /* Clamp once (advancing the source pointer to match) so the pixel loops below run
     * without per-pixel bounds checks. */
    const int left = local_x < 0 ? 0 : local_x;
    const int top = local_y < 0 ? 0 : local_y;
    const int right = (local_x + local_width) > rc()->scratch_width ? rc()->scratch_width : (local_x + local_width);
    const int bottom =
        (local_y + local_height) > rc()->scratch_height ? rc()->scratch_height : (local_y + local_height);
    if (left >= right || top >= bottom)
        return;
    source = (const uint8_t*)source + (size_t)(top - local_y) * (size_t)stride
             + (size_t)(left - local_x) * ER_ARGB8888_PIXEL_BYTES;

    for (int row = top; row < bottom; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)(row - top) * (size_t)stride);
        uint32_t* destination_row = rc()->scratch_buffer + (size_t)row * rc()->scratch_width;

        for (int column = left; column < right; column++)
        {
            uint32_t source_pixel = source_row[column - left];
            if (alpha < ER_ALPHA_OPAQUE)
            {
                /* Scale all premultiplied channels by the global alpha. */
                source_pixel = ((uint32_t)((source_pixel >> 24) & 0xFFU) * alpha / ER_CHANNEL_MAX << 24)
                               | ((uint32_t)((source_pixel >> 16) & 0xFFU) * alpha / ER_CHANNEL_MAX << 16)
                               | ((uint32_t)((source_pixel >> 8) & 0xFFU) * alpha / ER_CHANNEL_MAX << 8)
                               | (uint32_t)(source_pixel & 0xFFU) * alpha / ER_CHANNEL_MAX;
            }

            if ((source_pixel >> 24) != 0U) /* skip fully-transparent source pixels */
                destination_row[column] = over_premul_px(destination_row[column], source_pixel);
        }
    }
}

/**
 * @brief Composites premultiplied ARGB pixels into the scratch buffer via source-over.
 *
 * Mirrors what every backend's copy_rect does to the framebuffer (fully opaque pixels
 * overwrite, partially transparent ones blend, fully transparent ones are skipped) so a
 * copy means the same thing whether it lands on the framebuffer or an offscreen scratch
 * capture. A prior raw-memcpy overwrite here dropped the background already painted under
 * anti-aliased glyph edges, so text composited through an opacity group or a transform
 * source picked up a dark fringe once the scratch was blended out.
 *
 * @param[in] source        Source pixel buffer (premultiplied ARGB8888).
 * @param[in] stride        Source row stride in bytes.
 * @param[in] local_x       Scratch-local left edge.
 * @param[in] local_y       Scratch-local top edge.
 * @param[in] local_width   Width in pixels.
 * @param[in] local_height  Height in pixels.
 */
static void scratch_do_copy(const void* source, int stride, int local_x, int local_y, int local_width, int local_height)
{
    /* Clamp once (advancing the source pointer to match) so the pixel loop runs unchecked. */
    const int left = local_x < 0 ? 0 : local_x;
    const int top = local_y < 0 ? 0 : local_y;
    const int right = (local_x + local_width) > rc()->scratch_width ? rc()->scratch_width : (local_x + local_width);
    const int bottom =
        (local_y + local_height) > rc()->scratch_height ? rc()->scratch_height : (local_y + local_height);
    if (left >= right || top >= bottom)
        return;
    source = (const uint8_t*)source + (size_t)(top - local_y) * (size_t)stride
             + (size_t)(left - local_x) * ER_ARGB8888_PIXEL_BYTES;

    for (int row = top; row < bottom; row++)
    {
        const uint32_t* source_row = (const uint32_t*)((const uint8_t*)source + (size_t)(row - top) * (size_t)stride);
        uint32_t* destination_row = rc()->scratch_buffer + (size_t)row * rc()->scratch_width;

        for (int column = left; column < right; column++)
        {
            const uint32_t source_pixel = source_row[column - left];
            if ((source_pixel >> 24) != 0U) /* skip fully-transparent source pixels */
                destination_row[column] = over_premul_px(destination_row[column], source_pixel);
        }
    }
}

const EmbeddedRenderBackend* er_backend(void)
{
    return g_backend;
}

void er_scratch_begin(uint32_t* buffer, int width, int height, int origin_x, int origin_y)
{
    rc()->scratch_buffer = buffer;
    rc()->scratch_width = width;
    rc()->scratch_height = height;
    rc()->scratch_origin_x = origin_x;
    rc()->scratch_origin_y = origin_y;
}

void er_scratch_end(void)
{
    rc()->scratch_buffer = NULL;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_push_clip_rect(int x, int y, int width, int height)
{
    if (rc()->clip_depth >= ER_CLIP_STACK_DEPTH)
    {
        rc()->clip_overflow++;
        return;
    }

    ClipEntry entry = {x, y, width, height};

    if (rc()->clip_depth > 0)
    {
        /* Intersect the new rect with the current top-of-stack. */
        const ClipEntry* top = &rc()->clip_stack[rc()->clip_depth - 1];
        const int right = x + width;
        const int bottom = y + height;
        const int stack_right = top->x + top->width;
        const int stack_bottom = top->y + top->height;

        entry.x = x > top->x ? x : top->x;
        entry.y = y > top->y ? y : top->y;
        const int entry_right = right < stack_right ? right : stack_right;
        const int entry_bottom = bottom < stack_bottom ? bottom : stack_bottom;
        entry.width = entry_right - entry.x;
        entry.height = entry_bottom - entry.y;
        if (entry.width < 0)
            entry.width = 0;
        if (entry.height < 0)
            entry.height = 0;
    }

    rc()->clip_stack[rc()->clip_depth++] = entry;
}

void er_pop_clip_rect(void)
{
    if (rc()->clip_overflow > 0)
        rc()->clip_overflow--;
    else if (rc()->clip_depth > 0)
        rc()->clip_depth--;
}

void er_push_clip_reset(void)
{
    if (rc()->clip_depth >= ER_CLIP_STACK_DEPTH)
    {
        rc()->clip_overflow++;
        return;
    }
    /* Full-extent entry pushed WITHOUT intersecting the current top: used by offscreen source
     * captures (transform subtree render) that must see the whole subtree regardless of outer
     * scissors — the captured OUTPUT is clipped by the restored stack when it is blended out. */
    ClipEntry entry = {ER_CLIP_RESET_ORIGIN, ER_CLIP_RESET_ORIGIN, ER_CLIP_RESET_SIZE, ER_CLIP_RESET_SIZE};
    rc()->clip_stack[rc()->clip_depth++] = entry;
}

void er_set_band(int origin_y, int height)
{
    rc()->band_origin_y = origin_y;
    rc()->band_height = height;
}

bool er_band_active(int* origin_y, int* height)
{
    if (origin_y)
        *origin_y = rc()->band_origin_y;
    if (height)
        *height = rc()->band_height;
    return rc()->band_height > 0;
}

bool er_get_clip_rect(int* x, int* y, int* width, int* height)
{
    if (rc()->clip_depth == 0)
        return false;
    const ClipEntry* clip = &rc()->clip_stack[rc()->clip_depth - 1];
    if (x)
        *x = clip->x;
    if (y)
        *y = clip->y;
    if (width)
        *width = clip->width;
    if (height)
        *height = clip->height;
    return true;
}

void er_set_draw_alpha(uint8_t alpha)
{
    rc()->draw_alpha = alpha;
}

uint8_t er_get_draw_alpha(void)
{
    return rc()->draw_alpha;
}

void er_blit_fill(uint32_t argb, int x, int y, int width, int height)
{
    if (rc()->draw_alpha < ER_ALPHA_OPAQUE)
    {
        /* Inherited-alpha fallback: emit the fill through the blend path so the color is
         * composited over existing pixels on every backend. er_blit_blend applies
         * rc()->draw_alpha itself, so the row holds the color at its ORIGINAL alpha. */
        const uint8_t alpha = (uint8_t)((argb >> 24) & 0xFFU);
        if (alpha == 0U)
            return;
        if (!apply_clip(&x, &y, &width, &height))
            return;
        const uint32_t premultiplied_red = (uint32_t)((argb >> 16) & 0xFFU) * alpha / ER_CHANNEL_MAX;
        const uint32_t premultiplied_green = (uint32_t)((argb >> 8) & 0xFFU) * alpha / ER_CHANNEL_MAX;
        const uint32_t premultiplied_blue = (uint32_t)(argb & 0xFFU) * alpha / ER_CHANNEL_MAX;
        const uint32_t premultiplied =
            ((uint32_t)alpha << 24) | (premultiplied_red << 16) | (premultiplied_green << 8) | premultiplied_blue;
        const int chunk = width < ER_FILL_ROW_CHUNK ? width : ER_FILL_ROW_CHUNK;
        for (int column = 0; column < chunk; column++)
            rc()->fill_row[column] = premultiplied;
        for (int row = 0; row < height; row++)
            for (int chunk_x = 0; chunk_x < width; chunk_x += ER_FILL_ROW_CHUNK)
            {
                const int chunk_width = (width - chunk_x) < ER_FILL_ROW_CHUNK ? (width - chunk_x) : ER_FILL_ROW_CHUNK;
                er_blit_blend(rc()->fill_row,
                              chunk_width * (int)sizeof(uint32_t),
                              ER_ALPHA_OPAQUE,
                              x + chunk_x,
                              y + row,
                              chunk_width,
                              1);
            }
        return;
    }
    if (!apply_clip(&x, &y, &width, &height))
        return;
    if (rc()->scratch_buffer)
    {
        scratch_do_fill(argb, x - rc()->scratch_origin_x, y - rc()->scratch_origin_y, width, height);
        return;
    }
    if (g_backend && g_backend->fill_rect)
    {
        if (rc()->band_height > 0)
        {
            const int top = rc()->band_origin_y, bottom = rc()->band_origin_y + rc()->band_height;
            if (y < top)
            {
                height -= top - y;
                y = top;
            }
            if (y + height > bottom)
                height = bottom - y;
            if (height <= 0)
                return;
        }
        ER_BLIT_PERF_BEGIN();
        g_backend->fill_rect(argb, x, y - rc()->band_origin_y, width, height, g_backend->ctx);
        ER_BLIT_PERF_END(width, height);
    }
}

void er_blit_copy(const void* source, int stride, int x, int y, int width, int height)
{
    if (rc()->draw_alpha < ER_ALPHA_OPAQUE)
    {
        /* Inherited-alpha fallback: an opaque copy becomes a blend at the inherited alpha
         * (er_blit_blend applies rc()->draw_alpha itself). */
        er_blit_blend(source, stride, ER_ALPHA_OPAQUE, x, y, width, height);
        return;
    }
    const int original_x = x;
    const int original_y = y;
    if (!apply_clip(&x, &y, &width, &height))
        return;
    /* Advance source pointer to account for clipped rows and columns. */
    const int clipped_columns = x - original_x;
    const int clipped_rows = y - original_y;
    source = (const uint8_t*)source + (size_t)clipped_rows * (size_t)stride
             + (size_t)clipped_columns * ER_ARGB8888_PIXEL_BYTES;
    if (rc()->scratch_buffer)
    {
        scratch_do_copy(source, stride, x - rc()->scratch_origin_x, y - rc()->scratch_origin_y, width, height);
        return;
    }
    if (g_backend && g_backend->copy_rect)
    {
        if (rc()->band_height > 0)
        {
            const int top = rc()->band_origin_y, bottom = rc()->band_origin_y + rc()->band_height;
            if (y < top)
            {
                const int skipped_rows = top - y;
                source = (const uint8_t*)source + (size_t)skipped_rows * (size_t)stride;
                height -= skipped_rows;
                y = top;
            }
            if (y + height > bottom)
                height = bottom - y;
            if (height <= 0)
                return;
        }
        ER_BLIT_PERF_BEGIN();
        g_backend->copy_rect(source, stride, x, y - rc()->band_origin_y, width, height, g_backend->ctx);
        ER_BLIT_PERF_END(width, height);
    }
}

bool er_blit_copy_fmt(const void* source, int stride, ERImageFormat format, int x, int y, int width, int height)
{
    /* The format-aware hook applies only when the backend can take the buffer as-is: no inherited
     * draw alpha to composite and no scratch capture in flight (scratch buffers are ARGB8888). */
    const bool use_format_hook =
        g_backend && g_backend->copy_rect_fmt && rc()->draw_alpha == ER_ALPHA_OPAQUE && !rc()->scratch_buffer;
    if (!use_format_hook)
    {
        if (format != ER_IMG_ARGB8888)
            return false; /* caller expands rows on the CPU */
        er_blit_copy(source, stride, x, y, width, height);
        return true;
    }

    const int bytes_per_pixel = (format == ER_IMG_RGB565) ? ER_RGB565_PIXEL_BYTES : ER_ARGB8888_PIXEL_BYTES;
    const int original_x = x;
    const int original_y = y;
    if (!apply_clip(&x, &y, &width, &height))
        return true; /* fully clipped: nothing to draw */
    source = (const uint8_t*)source + (size_t)(y - original_y) * (size_t)stride
             + (size_t)(x - original_x) * (size_t)bytes_per_pixel;
    if (rc()->band_height > 0)
    {
        const int top = rc()->band_origin_y, bottom = rc()->band_origin_y + rc()->band_height;
        if (y < top)
        {
            const int skipped_rows = top - y;
            source = (const uint8_t*)source + (size_t)skipped_rows * (size_t)stride;
            height -= skipped_rows;
            y = top;
        }
        if (y + height > bottom)
            height = bottom - y;
        if (height <= 0)
            return true;
    }
    ER_BLIT_PERF_BEGIN();
    g_backend->copy_rect_fmt(source, stride, format, x, y - rc()->band_origin_y, width, height, g_backend->ctx);
    ER_BLIT_PERF_END(width, height);
    return true;
}

void er_blit_blend(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height)
{
    if (rc()->draw_alpha < ER_ALPHA_OPAQUE)
    {
        alpha = (uint8_t)((uint32_t)alpha * rc()->draw_alpha / ER_CHANNEL_MAX);
        if (alpha == 0U)
            return;
    }
    const int original_x = x;
    const int original_y = y;
    if (!apply_clip(&x, &y, &width, &height))
        return;
    const int clipped_columns = x - original_x;
    const int clipped_rows = y - original_y;
    source = (const uint8_t*)source + (size_t)clipped_rows * (size_t)stride
             + (size_t)clipped_columns * ER_ARGB8888_PIXEL_BYTES;
    if (rc()->scratch_buffer)
    {
        scratch_do_blend(source, stride, alpha, x - rc()->scratch_origin_x, y - rc()->scratch_origin_y, width, height);
        return;
    }
    if (g_backend && g_backend->blend_rect)
    {
        if (rc()->band_height > 0)
        {
            const int top = rc()->band_origin_y, bottom = rc()->band_origin_y + rc()->band_height;
            if (y < top)
            {
                const int skipped_rows = top - y;
                source = (const uint8_t*)source + (size_t)skipped_rows * (size_t)stride;
                height -= skipped_rows;
                y = top;
            }
            if (y + height > bottom)
                height = bottom - y;
            if (height <= 0)
                return;
        }
        ER_BLIT_PERF_BEGIN();
        g_backend->blend_rect(source, stride, alpha, x, y - rc()->band_origin_y, width, height, g_backend->ctx);
        ER_BLIT_PERF_END(width, height);
    }
}

void er_blit_perf_reset(void)
{
    for (int worker = 0; worker < ERUI_RENDER_WORKERS; worker++)
    {
        s_worker_contexts[worker].perf_blit_us = 0U;
        s_worker_contexts[worker].perf_blit_pixels = 0U;
    }
}

void er_blit_perf_collect(uint32_t* microseconds, uint32_t* pixels)
{
    uint32_t total_us = 0U;
    uint32_t total_pixels = 0U;
    for (int worker = 0; worker < ERUI_RENDER_WORKERS; worker++)
    {
        total_us += s_worker_contexts[worker].perf_blit_us;
        total_pixels += s_worker_contexts[worker].perf_blit_pixels;
    }
    if (microseconds)
        *microseconds = total_us;
    if (pixels)
        *pixels = total_pixels;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void embedded_renderer_set_backend(const EmbeddedRenderBackend* backend)
{
    g_backend = backend;
    /* Reset every worker's blit-layer context to a fresh state (draw_alpha's non-zero default
     * lives here rather than in an initializer so a re-set backend also re-arms it). */
    for (int worker = 0; worker < ERUI_RENDER_WORKERS; worker++)
    {
        NRCtx* context = &s_worker_contexts[worker];
        context->clip_depth = 0;
        context->clip_overflow = 0;
        context->band_origin_y = 0;
        context->band_height = 0;
        context->scratch_buffer = NULL;
        context->draw_alpha = ER_ALPHA_OPAQUE;
    }
    font_registry_init();
    font_blob_init(0);
    image_registry_init();
    er_input_reset();
    er_force_full_repaint(); /* fresh framebuffer: first commit must fully repaint, not damage-clip */
}

void embedded_renderer_tick(uint32_t delta_ms)
{
    er_anim_tick(delta_ms);
    er_layout_anim_tick(delta_ms);
    er_input_tick(delta_ms);
    er_tick(delta_ms);
}

void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y)
{
    /* The app under the error screen gets no input. */
    if (er_error_screen_active())
        return;
    er_input_queue_touch(finger_id, phase, x, y);
}

void embedded_renderer_flush_touch(void)
{
    er_input_flush_moves();
}

bool embedded_renderer_has_pending_touch(void)
{
    return er_input_has_pending_moves();
}

void embedded_renderer_set_touch_coalescing(bool enabled)
{
    er_input_set_move_coalescing(enabled);
}

void embedded_renderer_key(uint32_t keycode, const char* utf8_char)
{
    if (er_error_screen_active())
        return;
    er_text_input_key(keycode, utf8_char);
}
