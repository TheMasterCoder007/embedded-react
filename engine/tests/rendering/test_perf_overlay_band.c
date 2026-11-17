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
 * The perf overlay on a backend that bands by pixel budget. No strip is open after a commit, so the overlay
 * draws through strips of its own:
 *   - the picture matches the one a backend with no band buffer gets,
 *   - every strip is within the budget and inside the panel, and each is flushed before the next begins,
 *   - nothing is drawn outside a strip.
 */

#include "er_scene.h"
#include "native_renderer.h"
#include "perf_overlay.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define BAND_PIXELS (SCREEN_WIDTH * 4)
#define LINE_COUNT 3
#define BACKDROP_COLOR 0xFF204060U
#define ALPHA_OPAQUE 255U
#define CHANNEL_MAX 255U

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static const char* const s_lines[LINE_COUNT] = {"FPS 60  LOAD 12%", "FRAME 4ms  MAX 9ms", "STACK 3/32K"};

static uint32_t s_screen[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint32_t s_reference[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint32_t s_band_buffer[BAND_PIXELS];

/** @brief The surface the blits land on: the screen, or the band buffer while a strip is open. */
static uint32_t* s_surface;
static int s_surface_width;
static int s_surface_height;

static bool s_banded;
static bool s_strip_open;
static ERRect s_strip;
static ERRect s_panel;
static int s_strip_count;
static bool s_strip_fault;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Notes a blit that no surface can take: outside a strip, or past the surface's edges. */
static bool blit_fits(int x, int y, int width, int height)
{
    if ((s_banded && !s_strip_open) || x < 0 || y < 0 || x + width > s_surface_width || y + height > s_surface_height)
    {
        s_strip_fault = true;
        return false;
    }
    return true;
}

static void fill_rect(uint32_t argb, int x, int y, int width, int height, void* context)
{
    (void)context;
    if (!blit_fits(x, y, width, height))
        return;
    for (int row = 0; row < height; row++)
        for (int column = 0; column < width; column++)
            s_surface[(y + row) * s_surface_width + x + column] = argb;
}

static void copy_rect(const void* source, int source_stride_bytes, int x, int y, int width, int height, void* context)
{
    (void)context;
    if (!blit_fits(x, y, width, height))
        return;
    for (int row = 0; row < height; row++)
        memcpy(&s_surface[(y + row) * s_surface_width + x],
               (const uint8_t*)source + (size_t)row * (size_t)source_stride_bytes,
               (size_t)width * sizeof(uint32_t));
}

/** @brief Blends one premultiplied channel over the destination's. */
static uint32_t blend_channel(uint32_t source, uint32_t destination, uint32_t source_alpha, uint32_t alpha)
{
    const uint32_t scaled = source * alpha / CHANNEL_MAX;
    const uint32_t kept = destination * (CHANNEL_MAX - source_alpha * alpha / CHANNEL_MAX) / CHANNEL_MAX;
    return scaled + kept > CHANNEL_MAX ? CHANNEL_MAX : scaled + kept;
}

static void blend_rect(
    const void* source, int source_stride_bytes, uint8_t alpha, int x, int y, int width, int height, void* context)
{
    (void)context;
    if (!blit_fits(x, y, width, height))
        return;
    for (int row = 0; row < height; row++)
    {
        const uint8_t* source_row = (const uint8_t*)source + (size_t)row * (size_t)source_stride_bytes;
        for (int column = 0; column < width; column++)
        {
            uint32_t pixel;
            memcpy(&pixel, source_row + (size_t)column * sizeof(uint32_t), sizeof(pixel));
            uint32_t* destination = &s_surface[(y + row) * s_surface_width + x + column];
            const uint32_t source_alpha = pixel >> 24;
            const uint32_t red =
                blend_channel((pixel >> 16) & 0xFFU, (*destination >> 16) & 0xFFU, source_alpha, alpha);
            const uint32_t green =
                blend_channel((pixel >> 8) & 0xFFU, (*destination >> 8) & 0xFFU, source_alpha, alpha);
            const uint32_t blue = blend_channel(pixel & 0xFFU, *destination & 0xFFU, source_alpha, alpha);
            *destination = 0xFF000000U | (red << 16) | (green << 8) | blue;
        }
    }
}

static void band_begin(int x, int y, int width, int height, void* context)
{
    (void)context;
    if (s_strip_open || width <= 0 || height <= 0 || width * height > BAND_PIXELS)
        s_strip_fault = true;
    const ERRect strip = {x, y, width, height};
    s_strip = strip;
    s_strip_open = true;
    s_strip_count++;
    s_surface = s_band_buffer;
    s_surface_width = width;
    s_surface_height = height;
}

static void band_flush(void* context)
{
    (void)context;
    if (!s_strip_open)
    {
        s_strip_fault = true;
        return;
    }
    for (int row = 0; row < s_strip.height; row++)
        memcpy(&s_screen[(s_strip.y + row) * SCREEN_WIDTH + s_strip.x],
               &s_band_buffer[row * s_strip.width],
               (size_t)s_strip.width * sizeof(uint32_t));
    s_strip_open = false;
    s_surface = NULL;
    s_surface_width = 0;
    s_surface_height = 0;
}

/** @brief Draws the overlay over a plain backdrop, through strips or straight to the screen. */
static void draw_overlay(bool banded)
{
    static EmbeddedRenderBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.fill_rect = fill_rect;
    backend.copy_rect = copy_rect;
    backend.blend_rect = blend_rect;
    if (banded)
    {
        backend.band_begin = band_begin;
        backend.band_flush = band_flush;
        backend.band_pixels = BAND_PIXELS;
    }
    for (int pixel = 0; pixel < SCREEN_WIDTH * SCREEN_HEIGHT; pixel++)
        s_screen[pixel] = BACKDROP_COLOR;
    s_banded = banded;
    s_strip_open = false;
    s_strip_count = 0;
    s_surface = banded ? NULL : s_screen;
    s_surface_width = banded ? 0 : SCREEN_WIDTH;
    s_surface_height = banded ? 0 : SCREEN_HEIGHT;

    embedded_renderer_set_backend(&backend);
    er_perf_overlay_draw(
        SCREEN_WIDTH, SCREEN_HEIGHT, s_lines, LINE_COUNT, &s_panel.x, &s_panel.y, &s_panel.width, &s_panel.height);
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    draw_overlay(false);
    if (s_strip_fault || s_panel.width <= 0 || s_panel.height <= 0)
        return fail("the overlay did not draw on a backend with no band buffer");
    memcpy(s_reference, s_screen, sizeof(s_reference));
    const ERRect reference_panel = s_panel;

    draw_overlay(true);
    if (s_strip_fault)
        return fail("a strip was over the budget, left open, or drawn outside of");
    if (s_strip_open)
        return fail("the last strip was not flushed");
    if (s_panel.x != reference_panel.x || s_panel.y != reference_panel.y || s_panel.width != reference_panel.width
        || s_panel.height != reference_panel.height)
        return fail("the panel moved on a banded backend");
    const int strip_rows = BAND_PIXELS / s_panel.width;
    if (s_strip_count != (s_panel.height + strip_rows - 1) / strip_rows || s_strip_count < 2)
        return fail("the panel did not take the fewest strips of its own width");
    if (memcmp(s_reference, s_screen, sizeof(s_reference)) != 0)
        return fail("the banded overlay differs from the one drawn straight to the screen");

    bool panel_drawn = false;
    for (int pixel = 0; pixel < SCREEN_WIDTH * SCREEN_HEIGHT && !panel_drawn; pixel++)
        panel_drawn = s_screen[pixel] != BACKDROP_COLOR;
    if (!panel_drawn)
        return fail("nothing was drawn");

    printf("PASS: the perf overlay draws through strips of its own on a pixel-budget backend\n");
    return EXIT_SUCCESS;
}
