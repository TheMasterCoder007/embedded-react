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

/* Registration failures are silent falses to the caller (er_image_load is void), and both failure modes —
 * registry full, misaligned RGB565 pixels — present identically as "the image just isn't there". */
#include "image_registry.h"
#include "er_diagnostics.h"
#include "renderer_internal.h"
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Low address bits that must be clear for an RGB565 buffer read as uint16_t. */
#define RGB565_ALIGN_MASK ((uintptr_t)1U)

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ImageEntry s_entries[ERUI_IMAGE_REGISTRY_MAX];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void image_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
}

/**
 * @brief Scans a buffer once at registration for full opacity.
 *
 * Opaque images take the copy (replace) blit path on every subsequent repaint instead of the
 * read-modify-write blend path, so the one-time scan pays for itself on the first frame. Early-out
 * on the first non-opaque pixel keeps transparent art cheap.
 */
static bool scan_opaque(const void* pixels, int width, int height, ERImageFormat format)
{
    if (format == ER_IMG_RGB565)
        return true; /* no alpha channel */
    const uint32_t* argb_pixels = (const uint32_t*)pixels;
    const size_t pixel_count = (size_t)width * (size_t)height;
    for (size_t pixel_index = 0; pixel_index < pixel_count; pixel_index++)
        if ((argb_pixels[pixel_index] >> 24) != ER_ALPHA_OPAQUE)
            return false;
    return true;
}

bool image_registry_store(const char* name, const void* pixels, int width, int height, ERImageFormat format)
{
    if (!name || !pixels || width <= 0 || height <= 0)
        return false;
    /* Validate the format before it decides the buffer's element size: an out-of-range value would make
     * scan_opaque() and the renderer read the buffer with the wrong stride (out-of-bounds reads). */
    if (format != ER_IMG_ARGB8888 && format != ER_IMG_RGB565)
        return false;
    /* RGB565 pixels are read as uint16_t; reject a misaligned buffer at registration rather than
     * fault mid-frame on strict-alignment targets. ARGB8888 is deliberately not word-checked:
     * version-1 asset packs have always handed out pixel pointers at offset 2 (mod 4) and every
     * shipping target tolerates those reads — rejecting them here would break existing packs. */
    if (format == ER_IMG_RGB565 && ((uintptr_t)pixels & RGB565_ALIGN_MASK) != 0u)
    {
        ERUI_WARN_ONCE("embedded-react image: \"%s\" RGB565 pixels at odd address %p - rejected, will not draw "
                       "(is the asset pack/container placed word-aligned?).\n",
                       name,
                       pixels);
        return false;
    }

    int free_slot = -1;
    for (int entry_index = 0; entry_index < (int)ERUI_IMAGE_REGISTRY_MAX; entry_index++)
    {
        if (s_entries[entry_index].in_use && strncmp(s_entries[entry_index].name, name, IMAGE_NAME_MAX) == 0)
        {
            s_entries[entry_index].pixels = pixels;
            s_entries[entry_index].width = width;
            s_entries[entry_index].height = height;
            s_entries[entry_index].format = (uint8_t)format;
            s_entries[entry_index].opaque = scan_opaque(pixels, width, height, format);
            return true;
        }
        if (!s_entries[entry_index].in_use && free_slot < 0)
            free_slot = entry_index;
    }

    if (free_slot < 0)
    {
        ERUI_WARN_ONCE("embedded-react image: registry full (%d slots) - \"%s\" and later images will not draw; "
                       "raise ERUI_IMAGE_REGISTRY_MAX.\n",
                       (int)ERUI_IMAGE_REGISTRY_MAX,
                       name);
        return false;
    }

    ImageEntry* entry = &s_entries[free_slot];
    strncpy(entry->name, name, IMAGE_NAME_MAX);
    entry->name[IMAGE_NAME_MAX] = '\0';
    entry->pixels = pixels;
    entry->width = width;
    entry->height = height;
    entry->format = (uint8_t)format;
    entry->opaque = scan_opaque(pixels, width, height, format);
    entry->in_use = true;
    return true;
}

unsigned image_registry_in_use(void)
{
    unsigned in_use_count = 0U;
    for (int entry_index = 0; entry_index < (int)ERUI_IMAGE_REGISTRY_MAX; entry_index++)
        if (s_entries[entry_index].in_use)
            in_use_count++;
    return in_use_count;
}

const ImageEntry* image_registry_get(const char* name)
{
    if (!name || name[0] == '\0')
        return NULL;
    for (int entry_index = 0; entry_index < (int)ERUI_IMAGE_REGISTRY_MAX; entry_index++)
    {
        if (s_entries[entry_index].in_use && strncmp(s_entries[entry_index].name, name, IMAGE_NAME_MAX) == 0)
            return &s_entries[entry_index];
    }
    return NULL;
}
