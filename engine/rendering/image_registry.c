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

#include "image_registry.h"
#include <string.h>

/* Registration failures are silent falses to the caller (er_image_load is void), and both failure
 * modes — registry full, misaligned RGB565 pixels — present identically as "the image just isn't
 * there". Warn once per failure mode in diagnostics builds so a sim/debug run names the problem.
 * Same knob convention as ERUI_VECTOR_DIAGNOSTICS: on unless NDEBUG, forcible either way. */
#ifndef ERUI_IMAGE_DIAGNOSTICS
#ifdef NDEBUG
#define ERUI_IMAGE_DIAGNOSTICS 0
#else
#define ERUI_IMAGE_DIAGNOSTICS 1
#endif
#endif

#if ERUI_IMAGE_DIAGNOSTICS
#include <stdio.h>
#define ERUI_IMG_WARN_ONCE(...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        static bool er_img_warned_ = false;                                                                            \
        if (!er_img_warned_)                                                                                           \
        {                                                                                                              \
            er_img_warned_ = true;                                                                                     \
            fprintf(stderr, __VA_ARGS__);                                                                              \
        }                                                                                                              \
    } while (0)
#else
#define ERUI_IMG_WARN_ONCE(...) ((void)0)
#endif

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
static bool scan_opaque(const void* buf, int w, int h, ERImageFormat format)
{
    if (format == ER_IMG_RGB565)
        return true; /* no alpha channel */
    const uint32_t* px = (const uint32_t*)buf;
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++)
        if ((px[i] >> 24) != 0xFFu)
            return false;
    return true;
}

bool image_registry_store(const char* name, const void* buf, int w, int h, ERImageFormat format)
{
    if (!name || !buf || w <= 0 || h <= 0)
        return false;
    /* Validate the format before it decides buf's element size: an out-of-range value would make
     * scan_opaque() and the renderer read the buffer with the wrong stride (out-of-bounds reads). */
    if (format != ER_IMG_ARGB8888 && format != ER_IMG_RGB565)
        return false;
    /* RGB565 pixels are read as uint16_t; reject a misaligned buffer at registration rather than
     * fault mid-frame on strict-alignment targets. ARGB8888 is deliberately not word-checked:
     * version-1 asset packs have always handed out pixel pointers at offset 2 (mod 4) and every
     * shipping target tolerates those reads — rejecting them here would break existing packs. */
    if (format == ER_IMG_RGB565 && ((uintptr_t)buf & 1u) != 0u)
    {
        ERUI_IMG_WARN_ONCE("embedded-react image: \"%s\" RGB565 pixels at odd address %p - rejected, will not draw "
                           "(is the asset pack/container placed word-aligned?).\n",
                           name,
                           buf);
        return false;
    }

    int free_slot = -1;
    for (int i = 0; i < (int)ERUI_IMAGE_REGISTRY_MAX; i++)
    {
        if (s_entries[i].in_use && strncmp(s_entries[i].name, name, IMAGE_NAME_MAX) == 0)
        {
            s_entries[i].buf = buf;
            s_entries[i].w = w;
            s_entries[i].h = h;
            s_entries[i].format = (uint8_t)format;
            s_entries[i].opaque = scan_opaque(buf, w, h, format);
            return true;
        }
        if (!s_entries[i].in_use && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0)
    {
        ERUI_IMG_WARN_ONCE("embedded-react image: registry full (%d slots) - \"%s\" and later images will not draw; "
                           "raise ERUI_IMAGE_REGISTRY_MAX.\n",
                           (int)ERUI_IMAGE_REGISTRY_MAX,
                           name);
        return false;
    }

    ImageEntry* e = &s_entries[free_slot];
    strncpy(e->name, name, IMAGE_NAME_MAX);
    e->name[IMAGE_NAME_MAX] = '\0';
    e->buf = buf;
    e->w = w;
    e->h = h;
    e->format = (uint8_t)format;
    e->opaque = scan_opaque(buf, w, h, format);
    e->in_use = true;
    return true;
}

unsigned image_registry_in_use(void)
{
    unsigned n = 0U;
    for (int i = 0; i < (int)ERUI_IMAGE_REGISTRY_MAX; i++)
        if (s_entries[i].in_use)
            n++;
    return n;
}

const ImageEntry* image_registry_get(const char* name)
{
    if (!name || name[0] == '\0')
        return NULL;
    for (int i = 0; i < (int)ERUI_IMAGE_REGISTRY_MAX; i++)
    {
        if (s_entries[i].in_use && strncmp(s_entries[i].name, name, IMAGE_NAME_MAX) == 0)
            return &s_entries[i];
    }
    return NULL;
}
