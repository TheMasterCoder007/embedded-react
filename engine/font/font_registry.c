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

#include "font_registry.h"
#include "er_scene.h"
#include "font_blob.h"
#include <stdlib.h>
#include <string.h>

/** @brief Pixel size a request of 0 asks for. */
#define FONT_DEFAULT_PIXEL_SIZE 16U

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One slot in the font registry representing a single named font family.
 */
typedef struct
{
    char name[FONT_NAME_MAX + 1];                   /**< Null-terminated family name. */
    const BitmapFont* sizes[FONT_FAMILY_MAX_SIZES]; /**< Registered sizes for this family. */
    uint8_t size_count;                             /**< Number of valid entries in sizes[]. */
    bool in_use;                                    /**< True when this slot holds an active family. */
} RegistryEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static RegistryEntry s_entries[FONT_REGISTRY_MAX];
static bool s_initialised = false;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Picks the BitmapFont from a candidate list whose pixel_size is closest to target_pixels.
 *
 * When two candidates are equidistant, the larger size is preferred.
 *
 * @param[in] candidates     Array of pointers to candidate BitmapFonts.
 * @param[in] count          Number of entries in candidates.
 * @param[in] target_pixels  Desired pixel size.
 *
 * @return Pointer to the best-matching BitmapFont.
 */
static const BitmapFont* pick_closest(const BitmapFont* const* candidates, size_t count, uint8_t target_pixels)
{
    const BitmapFont* best = candidates[0];
    int best_distance = abs((int)target_pixels - (int)best->pixel_size);

    for (size_t candidate_index = 1; candidate_index < count; candidate_index++)
    {
        const BitmapFont* candidate = candidates[candidate_index];
        const int distance = abs((int)target_pixels - (int)candidate->pixel_size);
        if (distance < best_distance || (distance == best_distance && candidate->pixel_size > best->pixel_size))
        {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void font_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_initialised = true;
}

bool font_registry_add(const char* name, const BitmapFont* font)
{
    if (!s_initialised || !name || !font)
        return false;

    int family_slot = -1;
    int free_slot = -1;
    for (int slot = 0; slot < (int)FONT_REGISTRY_MAX; slot++)
    {
        if (s_entries[slot].in_use)
        {
            if (strncmp(s_entries[slot].name, name, FONT_NAME_MAX) == 0)
            {
                family_slot = slot;
                break;
            }
        }
        else if (free_slot < 0)
        {
            free_slot = slot;
        }
    }

    if (family_slot < 0)
    {
        if (free_slot < 0)
            return false;
        family_slot = free_slot;
        RegistryEntry* entry = &s_entries[family_slot];
        strncpy(entry->name, name, FONT_NAME_MAX);
        entry->name[FONT_NAME_MAX] = '\0';
        entry->size_count = 0;
        entry->in_use = true;
    }

    RegistryEntry* entry = &s_entries[family_slot];

    /* Replace in place when a size entry with the same pixel_size already exists, so that
     * re-registering the same (name, size) is idempotent rather than creating a duplicate
     * entry that would make pick_closest() ambiguous and waste a size slot. */
    for (uint8_t slot = 0; slot < entry->size_count; slot++)
    {
        if (entry->sizes[slot] != NULL && entry->sizes[slot]->pixel_size == font->pixel_size)
        {
            entry->sizes[slot] = font;
            return true;
        }
    }

    if (entry->size_count >= FONT_FAMILY_MAX_SIZES)
        return false;

    entry->sizes[entry->size_count++] = font;
    return true;
}

const BitmapFont* font_registry_get_exact(const char* name, uint8_t pixel_size)
{
    if (!s_initialised || !name || name[0] == '\0')
        return NULL;

    for (int family_index = 0; family_index < (int)FONT_REGISTRY_MAX; family_index++)
    {
        if (s_entries[family_index].in_use && strncmp(s_entries[family_index].name, name, FONT_NAME_MAX) == 0)
        {
            for (uint8_t size_index = 0; size_index < s_entries[family_index].size_count; size_index++)
            {
                if (s_entries[family_index].sizes[size_index] != NULL
                    && s_entries[family_index].sizes[size_index]->pixel_size == pixel_size)
                    return s_entries[family_index].sizes[size_index];
            }
            return NULL;
        }
    }
    return NULL;
}

const BitmapFont* font_registry_get(const char* name, uint8_t pixel_size)
{
    if (pixel_size == 0U)
        pixel_size = FONT_DEFAULT_PIXEL_SIZE;

    if (s_initialised && name && name[0] != '\0')
    {
        for (int family_index = 0; family_index < (int)FONT_REGISTRY_MAX; family_index++)
        {
            if (s_entries[family_index].in_use && s_entries[family_index].size_count > 0U
                && strncmp(s_entries[family_index].name, name, FONT_NAME_MAX) == 0)
            {
                return pick_closest(s_entries[family_index].sizes, s_entries[family_index].size_count, pixel_size);
            }
        }
    }

    return pick_closest(g_inter_sizes, g_inter_sizes_count, pixel_size);
}

void er_font_load(const char* name, const void* buffer, size_t length)
{
    if (!name || !buffer || length == 0)
        return;
    (void)font_blob_register(name, buffer, (uint32_t)length);
}

void er_font_clear(void)
{
    font_registry_init();
    font_blob_init(0);
}

void er_font_register(const char* family, const BitmapFont* font)
{
    /* Public, zero-copy companion to er_font_load: the BitmapFont is referenced by pointer (no font
     * pool involved), so this is the build-time path the font converter's baked C uses. */
    (void)font_registry_add(family, font);
}
