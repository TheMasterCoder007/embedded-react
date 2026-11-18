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
 * The DMA2D backend's band mode with two page-flipped framebuffers, at every rotation and in two pixel
 * formats. The peripheral is a block of plain memory and every op stays on the CPU.
 *   - each framebuffer holds the whole scene, turned, once it has been rendered into,
 *   - a change costs strips of its own damage rect, not of the full width, each within the pixel budget,
 *   - the other framebuffer gets what it missed by a copy, at its next commit,
 *   - with `no_buffer_sync` it is rendered again, in strips,
 *   - a commit that changes nothing paints nothing,
 *   - er_dma2d_backend_present() reports the painted box only when something was painted.
 */

#include "dma2d_backend.h"
#include "er_scene.h"
#include "native_renderer.h"
#include "renderer_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define LOGICAL_WIDTH 80
#define LOGICAL_HEIGHT 48
#define BAND_PIXELS (LOGICAL_WIDTH * 8)
#define STRIDE_PADDING_PIXELS 3
#define MAX_BYTES_PER_PIXEL 4
#define FRAMEBUFFER_COUNT 2
#define FRAMEBUFFER_BYTES (LOGICAL_WIDTH * (LOGICAL_WIDTH + STRIDE_PADDING_PIXELS) * MAX_BYTES_PER_PIXEL)
#define DMA2D_REGISTER_COUNT 20
#define STRIPS_MAX 64
#define UNTOUCHED_BYTE 0xEEU
#define OPACITY_OPAQUE 255U
#define ROTATION_COUNT 4
#define FORMAT_COUNT 3
#define BLACK 0xFF000000U
#define STALE_PIXEL 0xEEEEEEEEU

#define ROOT_COLOR 0xFF102030U
#define TALL_COLOR 0xFF3366FFU
#define TALL_COLOR_CHANGED 0xFFEE2222U
#define SMALL_COLOR 0xFF22AA44U
#define SMALL_COLOR_CHANGED 0xFFFFCC00U

/** @brief A tall narrow box near the top left and a small one near the bottom right. */
static const ERRect s_tall_box = {4, 4, 10, 30};
static const ERRect s_small_box = {60, 38, 16, 8};

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static const ERDisplayRotation s_rotations[ROTATION_COUNT] = {
    ER_ROTATION_0, ER_ROTATION_90, ER_ROTATION_180, ER_ROTATION_270};
static const ErDma2dFramebufferFormat s_formats[FORMAT_COUNT] = {
    ER_DMA2D_FRAMEBUFFER_ARGB8888, ER_DMA2D_FRAMEBUFFER_RGB888, ER_DMA2D_FRAMEBUFFER_RGB565};

/** @brief What shows where no box is: the root's color, or black under a root with no background. */
static uint32_t s_backdrop_color;

static uint32_t s_dma2d_registers[DMA2D_REGISTER_COUNT];
static uint32_t s_band_buffer[BAND_PIXELS];
static uint8_t s_framebuffers[FRAMEBUFFER_COUNT][FRAMEBUFFER_BYTES];

static EmbeddedRenderBackend s_recording_backend;
static const EmbeddedRenderBackend* s_dma2d_backend;
static ERRect s_strips[STRIPS_MAX];
static int s_strip_count;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static int fail(const char* message, ERDisplayRotation rotation, ErDma2dFramebufferFormat format)
{
    fprintf(stderr, "FAIL (rotation %d, format %d): %s\n", (int)rotation, (int)format, message);
    return EXIT_FAILURE;
}

/** @brief Notes the strip, then hands it to the DMA2D backend. */
static void record_band_begin(int x, int y, int width, int height, void* context)
{
    if (s_strip_count < STRIPS_MAX)
    {
        const ERRect strip = {x, y, width, height};
        s_strips[s_strip_count] = strip;
    }
    s_strip_count++;
    s_dma2d_backend->band_begin(x, y, width, height, context);
}

static ERProps props_default(void)
{
    ERProps props = {0};
    props.left = props.top = props.right = props.bottom = ER_LAYOUT_AUTO;
    props.width = props.height = ER_LAYOUT_AUTO;
    props.min_width = props.max_width = ER_LAYOUT_AUTO;
    props.min_height = props.max_height = ER_LAYOUT_AUTO;
    props.padding = props.padding_left = props.padding_top = ER_LAYOUT_AUTO;
    props.padding_right = props.padding_bottom = ER_LAYOUT_AUTO;
    props.margin = props.margin_left = props.margin_top = ER_LAYOUT_AUTO;
    props.margin_right = props.margin_bottom = ER_LAYOUT_AUTO;
    props.gap = props.row_gap = props.column_gap = ER_LAYOUT_AUTO;
    props.flex_basis = ER_LAYOUT_AUTO;
    props.opacity = OPACITY_OPAQUE;
    return props;
}

/** @brief Sets a box's props. The small box sits below the tall one in the root's column. */
static void set_box(ERNode* node, const ERRect* box, int margin_top, uint32_t color)
{
    ERProps props = props_default();
    props.width = box->width;
    props.height = box->height;
    props.margin_left = box->x;
    props.margin_top = margin_top;
    props.background_color = color;
    er_node_set_props(node, &props);
}

static bool rect_contains(const ERRect* rect, int x, int y)
{
    return x >= rect->x && x < rect->x + rect->width && y >= rect->y && y < rect->y + rect->height;
}

/** @brief Where a logical pixel lands on the panel, worked out independently of the engine. */
static void logical_to_panel(ERDisplayRotation rotation, int logical_x, int logical_y, int* panel_x, int* panel_y)
{
    switch (rotation)
    {
        case ER_ROTATION_90:
            *panel_x = LOGICAL_HEIGHT - 1 - logical_y;
            *panel_y = logical_x;
            break;
        case ER_ROTATION_180:
            *panel_x = LOGICAL_WIDTH - 1 - logical_x;
            *panel_y = LOGICAL_HEIGHT - 1 - logical_y;
            break;
        case ER_ROTATION_270:
            *panel_x = logical_y;
            *panel_y = LOGICAL_WIDTH - 1 - logical_x;
            break;
        default:
            *panel_x = logical_x;
            *panel_y = logical_y;
            break;
    }
}

/** @brief The color as the framebuffer stores it. */
static uint32_t stored_color(ErDma2dFramebufferFormat format, uint32_t argb)
{
    if (format == ER_DMA2D_FRAMEBUFFER_RGB888)
        return argb & 0x00FFFFFFU;
    if (format != ER_DMA2D_FRAMEBUFFER_RGB565)
        return argb;
    const uint32_t red = (argb >> 16) & 0xFFU;
    const uint32_t green = (argb >> 8) & 0xFFU;
    const uint32_t blue = argb & 0xFFU;
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
}

/** @brief Checks a framebuffer holds the root and both boxes in the given colors, turned. */
static bool framebuffer_matches(const uint8_t* framebuffer,
                                ERDisplayRotation rotation,
                                ErDma2dFramebufferFormat format,
                                int stride_pixels,
                                uint32_t tall_color,
                                uint32_t small_color)
{
    const int bytes_per_pixel =
        format == ER_DMA2D_FRAMEBUFFER_RGB565 ? 2 : (format == ER_DMA2D_FRAMEBUFFER_RGB888 ? 3 : 4);
    for (int logical_y = 0; logical_y < LOGICAL_HEIGHT; logical_y++)
        for (int logical_x = 0; logical_x < LOGICAL_WIDTH; logical_x++)
        {
            uint32_t color = s_backdrop_color;
            if (rect_contains(&s_tall_box, logical_x, logical_y))
                color = tall_color;
            else if (rect_contains(&s_small_box, logical_x, logical_y))
                color = small_color;
            int panel_x = 0;
            int panel_y = 0;
            logical_to_panel(rotation, logical_x, logical_y, &panel_x, &panel_y);
            uint32_t stored = 0;
            memcpy(
                &stored, framebuffer + (panel_y * stride_pixels + panel_x) * bytes_per_pixel, (size_t)bytes_per_pixel);
            if (stored != stored_color(format, color))
            {
                fprintf(stderr,
                        "logical (%d, %d): stored %08x, expected %08x\n",
                        logical_x,
                        logical_y,
                        (unsigned)stored,
                        (unsigned)stored_color(format, color));
                return false;
            }
        }
    return true;
}

/** @brief Checks every strip of the last commit is narrower than the screen and within the budget. */
static bool strips_are_rect_sized(void)
{
    for (int strip_index = 0; strip_index < s_strip_count && strip_index < STRIPS_MAX; strip_index++)
    {
        const ERRect* strip = &s_strips[strip_index];
        if (strip->width >= LOGICAL_WIDTH || strip->width * strip->height > BAND_PIXELS)
            return false;
    }
    return s_strip_count > 0 && s_strip_count <= STRIPS_MAX;
}

/** @brief Reports whether some strip of the last commit covers the box. */
static bool strips_cover(const ERRect* box)
{
    for (int y = box->y; y < box->y + box->height; y++)
    {
        bool row_covered = false;
        for (int strip_index = 0; strip_index < s_strip_count && !row_covered; strip_index++)
            row_covered = rect_contains(&s_strips[strip_index], box->x, y)
                          && rect_contains(&s_strips[strip_index], box->x + box->width - 1, y);
        if (!row_covered)
            return false;
    }
    return true;
}

/**
 * @brief Runs the two-buffer scenario at one rotation and pixel format.
 *
 * @param[in] root_color  The root's background, or 0 for a root that leaves the strips to be blanked.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_page_flip(ERDisplayRotation rotation, ErDma2dFramebufferFormat format, uint32_t root_color)
{
    s_backdrop_color = root_color ? root_color : BLACK;
    for (int pixel = 0; pixel < BAND_PIXELS; pixel++)
        s_band_buffer[pixel] = STALE_PIXEL;
    const bool swapped = rotation == ER_ROTATION_90 || rotation == ER_ROTATION_270;
    const int panel_width = swapped ? LOGICAL_HEIGHT : LOGICAL_WIDTH;
    const int panel_height = swapped ? LOGICAL_WIDTH : LOGICAL_HEIGHT;
    const int stride_pixels = panel_width + STRIDE_PADDING_PIXELS;

    memset(s_framebuffers, UNTOUCHED_BYTE, sizeof(s_framebuffers));
    er_set_display_rotation(rotation, panel_width, panel_height);

    ErDma2dBackendConfig config = {0};
    config.dma2d = s_dma2d_registers;
    config.framebuffer = s_framebuffers[0];
    config.stride_pixels = stride_pixels;
    config.format = format;
    config.band_buffer = s_band_buffer;
    config.min_dma_pixels = INT_MAX; /* no peripheral here: every op on the CPU */

    config.band_pixels = LOGICAL_WIDTH - 1;
    if (er_dma2d_backend_init(&config))
        return fail("a band buffer smaller than one row was accepted", rotation, format);
    config.band_pixels = BAND_PIXELS;
    config.render_buffer = s_framebuffers[1];
    if (er_dma2d_backend_init(&config))
        return fail("a band buffer and a render buffer together were accepted", rotation, format);
    config.render_buffer = NULL;
    if (!er_dma2d_backend_init(&config))
        return fail("the band config was refused", rotation, format);
    if (!er_backend()->buffer_sync)
        return fail("the backend does not copy by default", rotation, format);

    /* Record each strip on its way to the backend. */
    s_dma2d_backend = er_backend();
    s_recording_backend = *s_dma2d_backend;
    s_recording_backend.band_begin = record_band_begin;
    embedded_renderer_set_backend(&s_recording_backend);
    er_set_display_buffer_count(FRAMEBUFFER_COUNT);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = LOGICAL_WIDTH;
    root_props.height = LOGICAL_HEIGHT;
    root_props.background_color = root_color;
    er_node_set_props(root, &root_props);
    ERNode* tall = er_node_create(ER_NODE_VIEW);
    ERNode* small = er_node_create(ER_NODE_VIEW);
    const int small_margin_top = s_small_box.y - (s_tall_box.y + s_tall_box.height);
    set_box(tall, &s_tall_box, s_tall_box.y, TALL_COLOR);
    set_box(small, &s_small_box, small_margin_top, SMALL_COLOR);
    er_tree_append_child(root, tall);
    er_tree_append_child(root, small);
    er_tree_set_root(root);

    int back = 0;
    ERRect painted = {0, 0, 0, 0};

    /* The first buffer gets the whole scene, in full-width strips. */
    s_strip_count = 0;
    er_commit();
    if (s_strip_count != (LOGICAL_HEIGHT * LOGICAL_WIDTH + BAND_PIXELS - 1) / BAND_PIXELS)
        return fail("a full repaint did not take the fewest full-width strips", rotation, format);
    if (!er_dma2d_backend_present(&painted.x, &painted.y, &painted.width, &painted.height) || painted.x != 0
        || painted.y != 0 || painted.width != panel_width || painted.height != panel_height)
        return fail("a full repaint did not report the whole panel", rotation, format);
    if (!framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR))
        return fail("a full repaint left a wrong pixel", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();

    /* Nothing changes: the second buffer is copied whole, not rendered. */
    s_strip_count = 0;
    er_commit();
    if (s_strip_count != 0 || er_dma2d_backend_present(NULL, NULL, NULL, NULL))
        return fail("the second buffer was rendered with nothing changed", rotation, format);
    if (!framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR))
        return fail("the second buffer was not copied whole", rotation, format);

    /* The tall box changes: one strip of its own rect, in this buffer only. */
    set_box(tall, &s_tall_box, s_tall_box.y, TALL_COLOR_CHANGED);
    s_strip_count = 0;
    er_commit();
    if (!strips_are_rect_sized() || !strips_cover(&s_tall_box))
        return fail("the tall box was not banded on its own rect", rotation, format);
    if (s_strip_count != 1)
        return fail("a narrow rect within the budget took more than one strip", rotation, format);
    if (!er_dma2d_backend_present(&painted.x, &painted.y, &painted.width, &painted.height)
        || painted.width >= panel_width || painted.height >= panel_height)
        return fail("a small change did not report a small box", rotation, format);
    if (!framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR_CHANGED, SMALL_COLOR))
        return fail("the buffer rendered after the first change is wrong", rotation, format);
    if (!framebuffer_matches(s_framebuffers[back ^ 1], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR))
        return fail("the buffer on screen was written to", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();

    /* The small box changes: it is banded alone, and the tall box this buffer missed is copied over. */
    set_box(small, &s_small_box, small_margin_top, SMALL_COLOR_CHANGED);
    s_strip_count = 0;
    er_commit();
    if (!strips_are_rect_sized() || !strips_cover(&s_small_box) || strips_cover(&s_tall_box))
        return fail("the small box was not banded on its own rect", rotation, format);
    if (!er_dma2d_backend_present(&painted.x, &painted.y, &painted.width, &painted.height)
        || painted.width * painted.height > 2 * s_small_box.width * s_small_box.height)
        return fail("the copied rect was reported as painted", rotation, format);
    if (!framebuffer_matches(
            s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR_CHANGED, SMALL_COLOR_CHANGED))
        return fail("the buffer rendered after the second change is wrong", rotation, format);
    if (!framebuffer_matches(
            s_framebuffers[back ^ 1], rotation, format, stride_pixels, TALL_COLOR_CHANGED, SMALL_COLOR))
        return fail("the buffer on screen was written to by the copy", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();

    /* Nothing changes: the small box this buffer missed is copied over, and nothing is painted. */
    s_strip_count = 0;
    er_commit();
    if (s_strip_count != 0 || er_dma2d_backend_present(NULL, NULL, NULL, NULL))
        return fail("a commit with no change still painted", rotation, format);
    if (!framebuffer_matches(
            s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR_CHANGED, SMALL_COLOR_CHANGED))
        return fail("a commit with no change did not copy what the buffer missed", rotation, format);

    /* The tall box changes back: it is banded alone. */
    set_box(tall, &s_tall_box, s_tall_box.y, TALL_COLOR);
    s_strip_count = 0;
    er_commit();
    if (!strips_are_rect_sized() || !strips_cover(&s_tall_box) || strips_cover(&s_small_box))
        return fail("the tall box was not banded alone after a copy", rotation, format);
    er_dma2d_backend_present(NULL, NULL, NULL, NULL);
    if (!framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR_CHANGED))
        return fail("the buffer rendered after the copy is wrong", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();

    /* A backend with no flush hook has nowhere to send strips, so its budget is ignored. */
    s_recording_backend.band_flush = NULL;
    set_box(tall, &s_tall_box, s_tall_box.y, TALL_COLOR_CHANGED);
    s_strip_count = 0;
    er_commit();
    if (s_strip_count != 0)
        return fail("a backend without a flush hook was still given strips", rotation, format);

    er_node_destroy(root);
    er_dma2d_backend_destroy();
    return EXIT_SUCCESS;
}

/**
 * @brief Runs two buffers with `no_buffer_sync`: what a buffer missed is rendered again.
 *
 * @return EXIT_SUCCESS on pass.
 */
static int check_page_flip_rendered(ERDisplayRotation rotation, ErDma2dFramebufferFormat format)
{
    s_backdrop_color = ROOT_COLOR;
    const bool swapped = rotation == ER_ROTATION_90 || rotation == ER_ROTATION_270;
    const int panel_width = swapped ? LOGICAL_HEIGHT : LOGICAL_WIDTH;
    const int panel_height = swapped ? LOGICAL_WIDTH : LOGICAL_HEIGHT;
    const int stride_pixels = panel_width + STRIDE_PADDING_PIXELS;

    memset(s_framebuffers, UNTOUCHED_BYTE, sizeof(s_framebuffers));
    er_set_display_rotation(rotation, panel_width, panel_height);

    ErDma2dBackendConfig config = {0};
    config.dma2d = s_dma2d_registers;
    config.framebuffer = s_framebuffers[0];
    config.stride_pixels = stride_pixels;
    config.format = format;
    config.band_buffer = s_band_buffer;
    config.band_pixels = BAND_PIXELS;
    config.min_dma_pixels = INT_MAX;
    config.no_buffer_sync = true;
    if (!er_dma2d_backend_init(&config))
        return fail("the band config with no_buffer_sync was refused", rotation, format);
    if (er_backend()->buffer_sync)
        return fail("the backend copies with no_buffer_sync set", rotation, format);

    s_dma2d_backend = er_backend();
    s_recording_backend = *s_dma2d_backend;
    s_recording_backend.band_begin = record_band_begin;
    embedded_renderer_set_backend(&s_recording_backend);
    er_set_display_buffer_count(FRAMEBUFFER_COUNT);

    ERNode* root = er_node_create(ER_NODE_VIEW);
    ERProps root_props = props_default();
    root_props.width = LOGICAL_WIDTH;
    root_props.height = LOGICAL_HEIGHT;
    root_props.background_color = ROOT_COLOR;
    er_node_set_props(root, &root_props);
    ERNode* tall = er_node_create(ER_NODE_VIEW);
    ERNode* small = er_node_create(ER_NODE_VIEW);
    set_box(tall, &s_tall_box, s_tall_box.y, TALL_COLOR);
    set_box(small, &s_small_box, s_small_box.y - (s_tall_box.y + s_tall_box.height), SMALL_COLOR);
    er_tree_append_child(root, tall);
    er_tree_append_child(root, small);
    er_tree_set_root(root);

    int back = 0;
    er_commit();
    if (!er_dma2d_backend_present(NULL, NULL, NULL, NULL)
        || !framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR))
        return fail("the first buffer is wrong with no_buffer_sync", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();

    /* Nothing changes: the second buffer is rendered whole, and reported. */
    s_strip_count = 0;
    er_commit();
    if (s_strip_count == 0 || !er_dma2d_backend_present(NULL, NULL, NULL, NULL))
        return fail("the second buffer was not rendered with no_buffer_sync", rotation, format);
    if (!framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR))
        return fail("the second buffer is wrong with no_buffer_sync", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();

    /* The tall box changes in one buffer, then is rendered again in the other. */
    set_box(tall, &s_tall_box, s_tall_box.y, TALL_COLOR_CHANGED);
    er_commit();
    er_dma2d_backend_present(NULL, NULL, NULL, NULL);
    if (!framebuffer_matches(s_framebuffers[back ^ 1], rotation, format, stride_pixels, TALL_COLOR, SMALL_COLOR))
        return fail("the buffer on screen was written to with no_buffer_sync", rotation, format);
    back ^= 1;
    er_dma2d_backend_set_framebuffer(s_framebuffers[back]);
    er_display_present();
    s_strip_count = 0;
    er_commit();
    if (!strips_are_rect_sized() || !strips_cover(&s_tall_box) || strips_cover(&s_small_box))
        return fail("the missed change was not rendered on its own rect", rotation, format);
    if (!er_dma2d_backend_present(NULL, NULL, NULL, NULL))
        return fail("the missed change was rendered and not reported", rotation, format);
    if (!framebuffer_matches(s_framebuffers[back], rotation, format, stride_pixels, TALL_COLOR_CHANGED, SMALL_COLOR))
        return fail("the missed change was rendered wrong", rotation, format);

    er_node_destroy(root);
    er_dma2d_backend_destroy();
    return EXIT_SUCCESS;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

int main(void)
{
    for (int rotation_index = 0; rotation_index < ROTATION_COUNT; rotation_index++)
        for (int format_index = 0; format_index < FORMAT_COUNT; format_index++)
        {
            /* Once under a root that fills the screen, once under one with no background. */
            if (check_page_flip(s_rotations[rotation_index], s_formats[format_index], ROOT_COLOR) != EXIT_SUCCESS
                || check_page_flip(s_rotations[rotation_index], s_formats[format_index], 0U) != EXIT_SUCCESS
                || check_page_flip_rendered(s_rotations[rotation_index], s_formats[format_index]) != EXIT_SUCCESS)
                return EXIT_FAILURE;
        }
    printf("PASS: DMA2D band mode page-flips two buffers at every rotation\n");
    return EXIT_SUCCESS;
}
