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

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static ImageEntry s_entries[IMAGE_REGISTRY_MAX];

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void image_registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
}

bool image_registry_store(const char* name, const void* argb_buf, int w, int h)
{
    if (!name || !argb_buf || w <= 0 || h <= 0)
        return false;

    int free_slot = -1;
    for (int i = 0; i < (int)IMAGE_REGISTRY_MAX; i++)
    {
        if (s_entries[i].in_use && strncmp(s_entries[i].name, name, IMAGE_NAME_MAX) == 0)
        {
            s_entries[i].buf = (const uint32_t*)argb_buf;
            s_entries[i].w = w;
            s_entries[i].h = h;
            return true;
        }
        if (!s_entries[i].in_use && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0)
        return false;

    ImageEntry* e = &s_entries[free_slot];
    strncpy(e->name, name, IMAGE_NAME_MAX);
    e->name[IMAGE_NAME_MAX] = '\0';
    e->buf = (const uint32_t*)argb_buf;
    e->w = w;
    e->h = h;
    e->in_use = true;
    return true;
}

const ImageEntry* image_registry_get(const char* name)
{
    if (!name || name[0] == '\0')
        return NULL;
    for (int i = 0; i < (int)IMAGE_REGISTRY_MAX; i++)
    {
        if (s_entries[i].in_use && strncmp(s_entries[i].name, name, IMAGE_NAME_MAX) == 0)
            return &s_entries[i];
    }
    return NULL;
}
