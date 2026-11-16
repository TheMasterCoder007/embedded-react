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
 * Display rotation: the app draws in logical coordinates, the panel scans in its own. A logical
 * point (x, y) lands on the panel at
 *    90:  (panel_width - 1 - y, x)
 *   180:  (panel_width - 1 - x, panel_height - 1 - y)
 *   270:  (y, panel_height - 1 - x)
 * Touch points go the other way; dirty rects and er_rotate_copy() go this way.
 */

#include "er_scene.h"
#include "renderer_internal.h"

#include <stddef.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Width of the destination strips a 90/270 copy works in, so the source rows it reads stay in cache. */
#define ROTATE_STRIP_PIXELS 16

/** @brief Largest pixel size er_rotate_copy() accepts, in bytes. */
#define MAX_BYTES_PER_PIXEL 4

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ERDisplayRotation s_rotation = ER_ROTATION_0; /**< Set by er_set_display_rotation(). */
static int s_panel_width = 0;                        /**< Panel width as it scans; 0 = not set. */
static int s_panel_height = 0;                       /**< Panel height as it scans; 0 = not set. */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Reports whether the rotation swaps width and height.
 *
 * @return true at 90 and 270.
 */
static bool rotation_swaps_axes(void)
{
    return s_rotation == ER_ROTATION_90 || s_rotation == ER_ROTATION_270;
}

/**
 * @brief Copies one destination row segment whose source pixels sit a fixed byte step apart.
 *
 * @param[out] destination      First destination pixel; the segment is contiguous.
 * @param[in]  source           Source pixel for the first destination pixel.
 * @param[in]  source_step      Bytes from one source pixel to the next (may be negative).
 * @param[in]  pixel_count      Pixels to copy.
 * @param[in]  bytes_per_pixel  Pixel size in bytes, 1 to 4.
 */
static void copy_stepped_span(
    uint8_t* destination, const uint8_t* source, ptrdiff_t source_step, int pixel_count, int bytes_per_pixel)
{
    switch (bytes_per_pixel)
    {
        case 2:
            for (int pixel = 0; pixel < pixel_count; pixel++, destination += 2, source += source_step)
                memcpy(destination, source, 2);
            break;
        case 4:
            for (int pixel = 0; pixel < pixel_count; pixel++, destination += 4, source += source_step)
                memcpy(destination, source, 4);
            break;
        default:
            for (int pixel = 0; pixel < pixel_count; pixel++, destination += bytes_per_pixel, source += source_step)
                memcpy(destination, source, (size_t)bytes_per_pixel);
            break;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Internal
 ---------------------------------------------------------------------------------------------------------------------*/

void er_display_point_to_logical(int* x, int* y)
{
    const int panel_x = *x;
    const int panel_y = *y;
    switch (s_rotation)
    {
        case ER_ROTATION_90:
            *x = panel_y;
            *y = s_panel_width - 1 - panel_x;
            break;
        case ER_ROTATION_180:
            *x = s_panel_width - 1 - panel_x;
            *y = s_panel_height - 1 - panel_y;
            break;
        case ER_ROTATION_270:
            *x = s_panel_height - 1 - panel_y;
            *y = panel_x;
            break;
        default:
            break;
    }
}

void er_display_rect_to_panel(ERRect* rect)
{
    const ERRect logical = *rect;
    switch (s_rotation)
    {
        case ER_ROTATION_90:
            rect->x = s_panel_width - (logical.y + logical.height);
            rect->y = logical.x;
            rect->width = logical.height;
            rect->height = logical.width;
            break;
        case ER_ROTATION_180:
            rect->x = s_panel_width - (logical.x + logical.width);
            rect->y = s_panel_height - (logical.y + logical.height);
            break;
        case ER_ROTATION_270:
            rect->x = logical.y;
            rect->y = s_panel_height - (logical.x + logical.width);
            rect->width = logical.height;
            rect->height = logical.width;
            break;
        default:
            break;
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_set_display_rotation(ERDisplayRotation rotation, int panel_width, int panel_height)
{
    const bool known_rotation = rotation == ER_ROTATION_0 || rotation == ER_ROTATION_90 || rotation == ER_ROTATION_180
                                || rotation == ER_ROTATION_270;
    if (!known_rotation || panel_width < 1 || panel_height < 1)
        return false;
    s_rotation = rotation;
    s_panel_width = panel_width;
    s_panel_height = panel_height;
    er_force_full_repaint();
    return true;
}

ERDisplayRotation er_get_display_rotation(void)
{
    return s_rotation;
}

bool er_get_display_size(int* width, int* height)
{
    const bool swapped = rotation_swaps_axes();
    if (width)
        *width = swapped ? s_panel_height : s_panel_width;
    if (height)
        *height = swapped ? s_panel_width : s_panel_height;
    return s_panel_width > 0;
}

void er_rotate_copy(const void* source,
                    int source_stride_bytes,
                    void* destination,
                    int destination_stride_bytes,
                    int bytes_per_pixel,
                    const ERRect* panel_rect)
{
    if (!source || !destination || bytes_per_pixel < 1 || bytes_per_pixel > MAX_BYTES_PER_PIXEL)
        return;

    ERRect rect;
    if (panel_rect)
        rect = *panel_rect;
    else
    {
        rect.x = 0;
        rect.y = 0;
        rect.width = s_panel_width;
        rect.height = s_panel_height;
    }
    /* Clip to the panel once its size is known; at 0 with no size set the caller's rect is trusted. */
    if (s_panel_width > 0)
    {
        int left = rect.x < 0 ? 0 : rect.x;
        int top = rect.y < 0 ? 0 : rect.y;
        int right = rect.x + rect.width > s_panel_width ? s_panel_width : rect.x + rect.width;
        int bottom = rect.y + rect.height > s_panel_height ? s_panel_height : rect.y + rect.height;
        rect.x = left;
        rect.y = top;
        rect.width = right - left;
        rect.height = bottom - top;
    }
    if (rect.width <= 0 || rect.height <= 0)
        return;

    const uint8_t* source_bytes = (const uint8_t*)source;
    uint8_t* destination_bytes = (uint8_t*)destination;
    const ptrdiff_t source_stride = source_stride_bytes;
    const ptrdiff_t destination_stride = destination_stride_bytes;
    const ptrdiff_t pixel_bytes = bytes_per_pixel;

    if (s_rotation == ER_ROTATION_0 || s_rotation == ER_ROTATION_180)
    {
        for (int panel_y = rect.y; panel_y < rect.y + rect.height; panel_y++)
        {
            uint8_t* destination_row = destination_bytes + panel_y * destination_stride + rect.x * pixel_bytes;
            if (s_rotation == ER_ROTATION_0)
            {
                memcpy(destination_row,
                       source_bytes + panel_y * source_stride + rect.x * pixel_bytes,
                       (size_t)rect.width * (size_t)bytes_per_pixel);
                continue;
            }
            /* 180: the row comes from the mirrored row, read right to left. */
            const int logical_x = s_panel_width - 1 - rect.x;
            const int logical_y = s_panel_height - 1 - panel_y;
            copy_stepped_span(destination_row,
                              source_bytes + logical_y * source_stride + logical_x * pixel_bytes,
                              -pixel_bytes,
                              rect.width,
                              bytes_per_pixel);
        }
        return;
    }

    /* 90 and 270: a panel row is a logical column, so each destination pixel comes from another source row.
     * Working a narrow strip of panel columns at a time keeps that strip's source rows in cache while every
     * panel row of the strip is filled. */
    const ptrdiff_t source_step = s_rotation == ER_ROTATION_90 ? -source_stride : source_stride;
    for (int strip_x = rect.x; strip_x < rect.x + rect.width; strip_x += ROTATE_STRIP_PIXELS)
    {
        const int remaining = rect.x + rect.width - strip_x;
        const int strip_width = remaining < ROTATE_STRIP_PIXELS ? remaining : ROTATE_STRIP_PIXELS;
        for (int panel_y = rect.y; panel_y < rect.y + rect.height; panel_y++)
        {
            int logical_x = strip_x;
            int logical_y = panel_y;
            er_display_point_to_logical(&logical_x, &logical_y);
            copy_stepped_span(destination_bytes + panel_y * destination_stride + strip_x * pixel_bytes,
                              source_bytes + logical_y * source_stride + logical_x * pixel_bytes,
                              source_step,
                              strip_width,
                              bytes_per_pixel);
        }
    }
}
