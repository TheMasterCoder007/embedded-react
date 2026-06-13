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
 * LCD backend does — instead of compositing into a full persistent framebuffer. The persistent `fb`
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

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Internal state for the SDL2 render backend.
 */
typedef struct
{
    SDL_Renderer* renderer;      /**< SDL2 renderer owned by the caller. */
    SDL_Texture* fb;             /**< Render-target texture used as a persistent framebuffer. */
    SDL_Texture* scratch;        /**< Streaming ARGB8888 texture for copy/blend ops. */
    int scratch_w;               /**< Width of the scratch texture in pixels. */
    int scratch_h;               /**< Height of the scratch texture in pixels. */
    SDL_BlendMode premult_blend; /**< Custom blend mode for premultiplied ARGB sources. */
#if ER_LCD_BANDED
    SDL_Texture* band; /**< Small render-target strip buffer (fb_w × ER_LCD_BANDED_ROWS). */
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
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] ctx   Pointer to the SDLCtx.
 */
static void fill_rect_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    SDLCtx* c = ctx;
    uint8_t a = (uint8_t)(argb >> 24);
    uint8_t r = (uint8_t)(argb >> 16);
    uint8_t g = (uint8_t)(argb >> 8);
    uint8_t b = (uint8_t)(argb);
    SDL_Rect rect = {x, y, w, h};

    SDL_SetRenderDrawBlendMode(c->renderer, a == 0xFFU ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(c->renderer, r, g, b, a);
    SDL_RenderFillRect(c->renderer, &rect);
}

/**
 * @brief Copies a premultiplied ARGB8888 buffer to the framebuffer.
 *
 * Uploads src into the scratch texture then renders it at (x, y) using the
 * premultiplied-alpha blend mode so the engine's composite model is preserved.
 *
 * @param[in] src              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] src_stride_bytes Row stride of src in bytes.
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] w                Width of the region in pixels.
 * @param[in] h                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SDLCtx.
 */
static void copy_rect_cb(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx)
{
    SDLCtx* c = ctx;
    SDL_Rect src_rect = {0, 0, w, h};
    SDL_Rect dst_rect = {x, y, w, h};

    SDL_UpdateTexture(c->scratch, &src_rect, src, src_stride_bytes);
    SDL_SetTextureColorMod(c->scratch, 255, 255, 255);
    SDL_SetTextureAlphaMod(c->scratch, 255);
    SDL_RenderCopy(c->renderer, c->scratch, &src_rect, &dst_rect);
}

/**
 * @brief Blends a premultiplied ARGB8888 buffer onto the framebuffer at global alpha.
 *
 * Scales the premultiplied source uniformly by alpha/255 using SDL texture color and
 * alpha modulation before rendering with the premultiplied blend mode. This preserves
 * the engine's blend formula: out.C = src.C*(a/255) + dst.C*(1 - src.A*(a/255)).
 *
 * @param[in] src              Source pixel buffer (premultiplied ARGB8888).
 * @param[in] src_stride_bytes Row stride of src in bytes.
 * @param[in] alpha            Global opacity (0 = transparent, 255 = opaque).
 * @param[in] x                Destination X coordinate.
 * @param[in] y                Destination Y coordinate.
 * @param[in] w                Width of the region in pixels.
 * @param[in] h                Height of the region in pixels.
 * @param[in] ctx              Pointer to the SDLCtx.
 */
static void blend_rect_cb(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx)
{
    SDLCtx* c = ctx;
    SDL_Rect src_rect = {0, 0, w, h};
    SDL_Rect dst_rect = {x, y, w, h};

    SDL_UpdateTexture(c->scratch, &src_rect, src, src_stride_bytes);
    /* Scaling both color and alpha channels by the same factor preserves premultiplied invariant. */
    SDL_SetTextureColorMod(c->scratch, alpha, alpha, alpha);
    SDL_SetTextureAlphaMod(c->scratch, alpha);
    SDL_RenderCopy(c->renderer, c->scratch, &src_rect, &dst_rect);
}

#if ER_LCD_BANDED
/**
 * @brief Begins a strip: targets the band texture and clears its used region to opaque black.
 *
 * The engine emits the strip's fill/copy/blend ops with band-local Y (the strip top is subtracted
 * upstream), so they land in the band texture; band_flush_cb then copies the strip into `fb`.
 *
 * @param[in] x    Strip left edge in screen space.
 * @param[in] y    Strip top edge in screen space.
 * @param[in] w    Strip width in pixels.
 * @param[in] h    Strip height in pixels.
 * @param[in] ctx  Pointer to the SDLCtx.
 */
static void band_begin_cb(int x, int y, int w, int h, void* ctx)
{
    SDLCtx* c = ctx;
    c->strip.x = x;
    c->strip.y = y;
    c->strip.w = w;
    c->strip.h = h;
    /* X stays absolute (band texture is full screen width); only Y is band-local, so clear [x,0,w,h]. */
    const SDL_Rect local = {x, 0, w, h};
    SDL_SetRenderTarget(c->renderer, c->band);
    SDL_SetRenderDrawBlendMode(c->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(c->renderer, 0, 0, 0, 255); /* matches a RAM backend's zeroed (opaque black) fb */
    SDL_RenderFillRect(c->renderer, &local);
}

/**
 * @brief Flushes the current strip from the band texture into the persistent framebuffer.
 *
 * Mirrors a hardware backend's per-band DMA: the band buffer is pushed to the panel (here, the `fb`
 * texture that stands in for panel GRAM) at the strip's screen position.
 *
 * @param[in] ctx  Pointer to the SDLCtx.
 */
static void band_flush_cb(void* ctx)
{
    SDLCtx* c = ctx;
    const SDL_Rect src = {c->strip.x, 0, c->strip.w, c->strip.h};
    SDL_SetRenderTarget(c->renderer, c->fb);
    SDL_SetTextureBlendMode(c->band, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(c->renderer, c->band, &src, &c->strip);
}
#endif /* ER_LCD_BANDED */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_sdl_backend_init(SDL_Renderer* renderer, int fb_w, int fb_h)
{
    if (!renderer || fb_w <= 0 || fb_h <= 0)
        return false;

    s_ctx.renderer = renderer;
    s_ctx.scratch_w = fb_w;
    s_ctx.scratch_h = fb_h;

    /*
     * Custom blend mode for premultiplied ARGB sources:
     *   out.C = 1 * src.C + (1 - src.A) * dst.C
     * Standard SDL_BLENDMODE_BLEND would expect un-premultiplied src and would
     * double-multiply the alpha channel.
     */
    s_ctx.premult_blend = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
                                                     SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                                     SDL_BLENDOPERATION_ADD,
                                                     SDL_BLENDFACTOR_ONE,
                                                     SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                                     SDL_BLENDOPERATION_ADD);

    s_ctx.scratch = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, fb_w, fb_h);
    if (!s_ctx.scratch)
        return false;

    SDL_SetTextureBlendMode(s_ctx.scratch, s_ctx.premult_blend);

    /* Persistent framebuffer: all engine draw calls target this texture, so the
     * last rendered frame survives across frames where er_commit() is a no-op. */
    s_ctx.fb = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, fb_w, fb_h);
    if (!s_ctx.fb)
    {
        SDL_DestroyTexture(s_ctx.scratch);
        s_ctx.scratch = NULL;
        return false;
    }

    SDL_SetTextureBlendMode(s_ctx.fb, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(renderer, s_ctx.fb);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    s_backend.fill_rect = fill_rect_cb;
    s_backend.copy_rect = copy_rect_cb;
    s_backend.blend_rect = blend_rect_cb;
    s_backend.wait = NULL;
    s_backend.frame_ready = NULL;
    s_backend.ctx = &s_ctx;

#if ER_LCD_BANDED
    /* Band buffer: a strip of the screen the engine recomposites and flushes one row-range at a time. */
    s_ctx.band =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, fb_w, ER_LCD_BANDED_ROWS);
    if (!s_ctx.band)
    {
        SDL_DestroyTexture(s_ctx.fb);
        s_ctx.fb = NULL;
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
    SDL_SetRenderDrawColor(s_ctx.renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_ctx.renderer);
    SDL_RenderCopy(s_ctx.renderer, s_ctx.fb, NULL, NULL);

    /* Optional dirty-rect diagnostic overlay: yellow outline of repainted region. */
    if (s_show_dirty_overlay)
    {
        ERRect dr;
        if (er_get_dirty_rect(&dr))
        {
            SDL_Rect sdl_r = {
                (int)((float)dr.x / s_dirty_scale),
                (int)((float)dr.y / s_dirty_scale),
                (int)((float)dr.w / s_dirty_scale),
                (int)((float)dr.h / s_dirty_scale),
            };
            SDL_SetRenderDrawBlendMode(s_ctx.renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(s_ctx.renderer, 255, 220, 0, 255);
            SDL_RenderDrawRect(s_ctx.renderer, &sdl_r);
        }
    }

    SDL_RenderPresent(s_ctx.renderer);

    /* Restore the render target so the next er_commit() draws back into the framebuffer. */
    SDL_SetRenderTarget(s_ctx.renderer, s_ctx.fb);
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
    if (s_ctx.fb)
    {
        SDL_DestroyTexture(s_ctx.fb);
        s_ctx.fb = NULL;
    }
    if (s_ctx.scratch)
    {
        SDL_DestroyTexture(s_ctx.scratch);
        s_ctx.scratch = NULL;
    }
    s_ctx.renderer = NULL;
}
