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

#include "er_scene.h"
#include "native_renderer.h"
#include "sdl_backend.h"
#include <SDL2/SDL.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Configuration
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Banded rendering (desktop mirror of the no-full-framebuffer path).
 *
 * When ER_LCD_BANDED is set the backend advertises a band height to the engine, which then renders each
 * commit's damage region in horizontal strips through a small band texture — exactly as a RAM-constrained
 * LCD backend does — instead of compositing into a full persistent framebuffer. The persistent `framebuffer`
 * texture stands in for the panel's own GRAM: it retains the pixels outside the strips. This lets the
 * engine's banded driver be validated on desktop with screenshots. 0 = classic full-framebuffer path.
 */
#ifndef ER_LCD_BANDED
#define ER_LCD_BANDED 0
#endif

/** @brief Rows per band buffer when ER_LCD_BANDED is enabled. */
#ifndef ER_LCD_BANDED_ROWS
#define ER_LCD_BANDED_ROWS 40
#endif

/** @brief Full scale of an 8-bit color channel; as a color or alpha mod it leaves a texture unchanged. */
#define CHANNEL_MAX 255

/** @brief An alpha of this value is fully opaque. */
#define ALPHA_OPAQUE 255

/** @brief The dirty-rect overlay's outline color (yellow). */
#define DIRTY_OVERLAY_RED 255
#define DIRTY_OVERLAY_GREEN 220
#define DIRTY_OVERLAY_BLUE 0

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal state for the SDL2 render backend.
 */
typedef struct
{
    SDL_Renderer* renderer;            /**< SDL2 renderer owned by the caller. */
    SDL_Texture* framebuffer;          /**< Render-target texture used as a persistent framebuffer. */
    SDL_Texture* scratch;              /**< Streaming ARGB8888 texture for copy/blend ops. */
    SDL_Texture* scratch565;           /**< Streaming RGB565 texture for opaque native-format blits. */
    int scratch_width;                 /**< Width of the scratch texture in pixels. */
    int scratch_height;                /**< Height of the scratch texture in pixels. */
    SDL_BlendMode premultiplied_blend; /**< Custom blend mode for premultiplied ARGB sources. */
#if ER_LCD_BANDED
    SDL_Texture* band; /**< Small render-target strip buffer (framebuffer_width × ER_LCD_BANDED_ROWS). */
    SDL_Rect strip;    /**< Screen-space rect of the strip currently being rendered. */
#endif
} SDLCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static SDLCtx s_ctx;
static EmbeddedRenderBackend s_backend;
static bool s_show_dirty_overlay = false;
static float s_dirty_scale = 1.0f;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Fills a solid-color rectangle using the SDL2 renderer.
 *
 * Switches between SDL_BLENDMODE_NONE (fully opaque) and SDL_BLENDMODE_BLEND
 * (semi-transparent) based on the alpha channel of argb.
 *
 * @param[in] argb  Straight-alpha ARGB8888 color.
 * @param[in] x     Left edge of the rectangle.
 * @param[in] y     Top edge of the rectangle.
 * @param[in] width     Width in pixels.
 * @param[in] height     Height in pixels.
 * @param[in] ctx   Pointer to the SDLCtx.
 */
static void fill_rect_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    SDLCtx* context = ctx;
    uint8_t alpha = (uint8_t)(argb >> 24);
    uint8_t red = (uint8_t)(argb >> 16);
    uint8_t green = (uint8_t)(argb >> 8);
    uint8_t blue = (uint8_t)(argb);
    SDL_Rect rect = {x, y, width, height};

    SDL_SetRenderDrawBlendMode(context->renderer, alpha == ALPHA_OPAQUE ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(context->renderer, red, green, blue, alpha);
    SDL_RenderFillRect(context->renderer, &rect);
}

/**
 * @brief Copies a premultiplied ARGB8888 buffer to the framebuffer.
 *
 * Uploads source into the scratch texture then renders it at (x, y) using the
 * premultiplied-alpha blend mode so the engine's composite model is preserved.
 *
 * @param[in] source              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] source_stride_bytes Row stride of source in bytes.
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] width                Width of the region in pixels.
 * @param[in] height                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SDLCtx.
 */
static void copy_rect_cb(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* ctx)
{
    SDLCtx* context = ctx;
    SDL_Rect source_rect = {0, 0, width, height};
    SDL_Rect destination_rect = {x, y, width, height};

    SDL_UpdateTexture(context->scratch, &source_rect, source, source_stride_bytes);
    SDL_SetTextureColorMod(context->scratch, CHANNEL_MAX, CHANNEL_MAX, CHANNEL_MAX);
    SDL_SetTextureAlphaMod(context->scratch, ALPHA_OPAQUE);
    SDL_RenderCopy(context->renderer, context->scratch, &source_rect, &destination_rect);
}

/**
 * @brief Blends a premultiplied ARGB8888 buffer onto the framebuffer at global alpha.
 *
 * Scales the premultiplied source uniformly by alpha/255 using SDL texture color and
 * alpha modulation before rendering with the premultiplied blend mode. This preserves
 * the engine's blend formula: out.C = src.C*(a/255) + dst.C*(1 - src.A*(a/255)).
 *
 * @param[in] source              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] source_stride_bytes Row stride of source in bytes.
 * @param[in] alpha            Global opacity (0 = transparent, 255 = opaque).
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] width                Width of the region in pixels.
 * @param[in] height                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SDLCtx.
 */
static void blend_rect_cb(
    const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    SDLCtx* context = ctx;
    SDL_Rect source_rect = {0, 0, width, height};
    SDL_Rect destination_rect = {x, y, width, height};

    SDL_UpdateTexture(context->scratch, &source_rect, source, source_stride_bytes);
    /* Scaling both color and alpha channels by the same factor preserves premultiplied invariant. */
    SDL_SetTextureColorMod(context->scratch, alpha, alpha, alpha);
    SDL_SetTextureAlphaMod(context->scratch, alpha);
    SDL_RenderCopy(context->renderer, context->scratch, &source_rect, &destination_rect);
}

/**
 * @brief Copies a KNOWN-FULLY-OPAQUE source in its native format to the framebuffer (replace).
 *
 * The engine only routes buffers its registration-time opacity scan proved opaque (RGB565 is opaque
 * by construction), so the strip renders with blending disabled — a straight replace. RGB565 sources
 * upload to a dedicated RGB565 streaming texture and the GPU does the format conversion; ARGB sources
 * reuse the regular scratch texture with its premultiplied blend mode temporarily bypassed.
 *
 * @param[in] source              Source pixel buffer (fully opaque, in format's layout).
 * @param[in] source_stride_bytes Row stride of source in bytes.
 * @param[in] format              Pixel format of source (ER_IMAGE_ARGB8888 or ER_IMAGE_RGB565).
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] width                Width of the region in pixels.
 * @param[in] height                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SDLCtx.
 */
static void copy_rect_fmt_cb(
    const void* source, int source_stride_bytes, ERImageFormat format, int x, int y, int width, int height, void* ctx)
{
    SDLCtx* context = ctx;
    SDL_Rect source_rect = {0, 0, width, height};
    SDL_Rect destination_rect = {x, y, width, height};

    if (format == ER_IMAGE_RGB565)
    {
        SDL_UpdateTexture(context->scratch565, &source_rect, source, source_stride_bytes);
        SDL_RenderCopy(context->renderer, context->scratch565, &source_rect, &destination_rect);
        return;
    }
    SDL_UpdateTexture(context->scratch, &source_rect, source, source_stride_bytes);
    SDL_SetTextureColorMod(context->scratch, CHANNEL_MAX, CHANNEL_MAX, CHANNEL_MAX);
    SDL_SetTextureAlphaMod(context->scratch, ALPHA_OPAQUE);
    SDL_SetTextureBlendMode(context->scratch, SDL_BLENDMODE_NONE); /* opaque contract: replace, no blend */
    SDL_RenderCopy(context->renderer, context->scratch, &source_rect, &destination_rect);
    SDL_SetTextureBlendMode(context->scratch, context->premultiplied_blend);
}

#if ER_LCD_BANDED
/**
 * @brief Begins a strip: targets the band texture and clears its used region to opaque black.
 *
 * The engine emits the strip's fill/copy/blend ops with band-local Y (the strip top is subtracted
 * upstream), so they land in the band texture; band_flush_cb then copies the strip into `framebuffer`.
 *
 * @param[in] x    Strip left edge in screen space.
 * @param[in] y    Strip top edge in screen space.
 * @param[in] width    Strip width in pixels.
 * @param[in] height    Strip height in pixels.
 * @param[in] ctx  Pointer to the SDLCtx.
 */
static void band_begin_cb(int x, int y, int width, int height, void* ctx)
{
    SDLCtx* context = ctx;
    context->strip.x = x;
    context->strip.y = y;
    context->strip.w = width;
    context->strip.h = height;
    /* X stays absolute (band texture is full screen width); only Y is band-local, so clear [x, 0, width, height]. */
    const SDL_Rect local = {x, 0, width, height};
    SDL_SetRenderTarget(context->renderer, context->band);
    SDL_SetRenderDrawBlendMode(context->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(
        context->renderer, 0, 0, 0, ALPHA_OPAQUE); /* matches a RAM backend's zeroed (opaque black) framebuffer */
    SDL_RenderFillRect(context->renderer, &local);
}

/**
 * @brief Flushes the current strip from the band texture into the persistent framebuffer.
 *
 * Mirrors a hardware backend's per-band DMA: the band buffer is pushed to the panel (here, the `framebuffer`
 * texture that stands in for panel GRAM) at the strip's screen position.
 *
 * @param[in] ctx  Pointer to the SDLCtx.
 */
static void band_flush_cb(void* ctx)
{
    SDLCtx* context = ctx;
    const SDL_Rect source_rect = {context->strip.x, 0, context->strip.w, context->strip.h};
    SDL_SetRenderTarget(context->renderer, context->framebuffer);
    SDL_SetTextureBlendMode(context->band, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(context->renderer, context->band, &source_rect, &context->strip);
}
#endif /* ER_LCD_BANDED */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_sdl_backend_init(SDL_Renderer* renderer, int framebuffer_width, int framebuffer_height)
{
    if (!renderer || framebuffer_width <= 0 || framebuffer_height <= 0)
        return false;

    s_ctx.renderer = renderer;
    s_ctx.scratch_width = framebuffer_width;
    s_ctx.scratch_height = framebuffer_height;

    /*
     * Custom blend mode for premultiplied ARGB sources:
     *   out.C = 1 * src.C + (1 - src.A) * dst.C
     * Standard SDL_BLENDMODE_BLEND would expect un-premultiplied src and would
     * double-multiply the alpha channel.
     */
    s_ctx.premultiplied_blend = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
                                                           SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                                           SDL_BLENDOPERATION_ADD,
                                                           SDL_BLENDFACTOR_ONE,
                                                           SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                                           SDL_BLENDOPERATION_ADD);

    s_ctx.scratch = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, framebuffer_width, framebuffer_height);
    if (!s_ctx.scratch)
        return false;

    SDL_SetTextureBlendMode(s_ctx.scratch, s_ctx.premultiplied_blend);

    /* Persistent framebuffer: all engine draw calls target this texture, so the
     * last rendered frame survives across frames where er_commit() is a no-op. */
    s_ctx.framebuffer = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, framebuffer_width, framebuffer_height);
    if (!s_ctx.framebuffer)
    {
        SDL_DestroyTexture(s_ctx.scratch);
        s_ctx.scratch = NULL;
        return false;
    }

    SDL_SetTextureBlendMode(s_ctx.framebuffer, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(renderer, s_ctx.framebuffer);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, ALPHA_OPAQUE);
    SDL_RenderClear(renderer);

    s_backend.fill_rect = fill_rect_cb;
    s_backend.copy_rect = copy_rect_cb;
    s_backend.blend_rect = blend_rect_cb;
    s_backend.wait = NULL;
    s_backend.frame_ready = NULL;
    s_backend.context = &s_ctx;

    /* Opaque native-format blit path. Optional: if the RGB565 texture can't be created the hook stays
     * NULL and the engine falls back to CPU expansion through copy_rect — behaviour is unchanged. */
    s_backend.copy_rect_format = NULL;
    s_ctx.scratch565 = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, framebuffer_width, framebuffer_height);
    if (s_ctx.scratch565)
    {
        SDL_SetTextureBlendMode(s_ctx.scratch565, SDL_BLENDMODE_NONE);
        s_backend.copy_rect_format = copy_rect_fmt_cb;
    }

#if ER_LCD_BANDED
    /* Band buffer: a strip of the screen the engine recomposites and flushes one row-range at a time. */
    s_ctx.band = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, framebuffer_width, ER_LCD_BANDED_ROWS);
    if (!s_ctx.band)
    {
        SDL_DestroyTexture(s_ctx.framebuffer);
        s_ctx.framebuffer = NULL;
        SDL_DestroyTexture(s_ctx.scratch);
        s_ctx.scratch = NULL;
        return false;
    }
    SDL_SetTextureBlendMode(s_ctx.band, SDL_BLENDMODE_NONE);
    s_backend.band_height = ER_LCD_BANDED_ROWS;
    s_backend.band_begin = band_begin_cb;
    s_backend.band_flush = band_flush_cb;
#endif

    embedded_renderer_set_backend(&s_backend);

    return true;
}

void er_sdl_set_show_dirty_rect(bool enable, float scale)
{
    s_show_dirty_overlay = enable;
    s_dirty_scale = (scale > 0.0f) ? scale : 1.0f;
}

void er_sdl_present(void)
{
    if (!s_ctx.renderer)
        return;

    /* Composite the persistent framebuffer onto the screen, then flip. */
    SDL_SetRenderTarget(s_ctx.renderer, NULL);
    SDL_SetRenderDrawColor(s_ctx.renderer, 0, 0, 0, ALPHA_OPAQUE);
    SDL_RenderClear(s_ctx.renderer);
    SDL_RenderCopy(s_ctx.renderer, s_ctx.framebuffer, NULL, NULL);

    /* Optional dirty-rect diagnostic overlay: yellow outline of repainted region. */
    if (s_show_dirty_overlay)
    {
        ERRect dirty_rect;
        if (er_get_dirty_rect(&dirty_rect))
        {
            SDL_Rect overlay_rect = {
                (int)((float)dirty_rect.x / s_dirty_scale),
                (int)((float)dirty_rect.y / s_dirty_scale),
                (int)((float)dirty_rect.width / s_dirty_scale),
                (int)((float)dirty_rect.height / s_dirty_scale),
            };
            SDL_SetRenderDrawBlendMode(s_ctx.renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(
                s_ctx.renderer, DIRTY_OVERLAY_RED, DIRTY_OVERLAY_GREEN, DIRTY_OVERLAY_BLUE, ALPHA_OPAQUE);
            SDL_RenderDrawRect(s_ctx.renderer, &overlay_rect);
        }
    }

    SDL_RenderPresent(s_ctx.renderer);

    /* Restore the render target so the next er_commit() draws back into the framebuffer. */
    SDL_SetRenderTarget(s_ctx.renderer, s_ctx.framebuffer);
}

void er_sdl_backend_destroy(void)
{
#if ER_LCD_BANDED
    if (s_ctx.band)
    {
        SDL_DestroyTexture(s_ctx.band);
        s_ctx.band = NULL;
    }
#endif
    if (s_ctx.framebuffer)
    {
        SDL_DestroyTexture(s_ctx.framebuffer);
        s_ctx.framebuffer = NULL;
    }
    if (s_ctx.scratch)
    {
        SDL_DestroyTexture(s_ctx.scratch);
        s_ctx.scratch = NULL;
    }
    if (s_ctx.scratch565)
    {
        SDL_DestroyTexture(s_ctx.scratch565);
        s_ctx.scratch565 = NULL;
    }
    s_ctx.renderer = NULL;
}
