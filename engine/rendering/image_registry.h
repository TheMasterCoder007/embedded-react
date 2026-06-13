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

#ifndef EMBEDDED_REACT_IMAGE_REGISTRY_H
#define EMBEDDED_REACT_IMAGE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of images that can be registered simultaneously. */
#define IMAGE_REGISTRY_MAX 32U

/** @brief Maximum length of an image asset name, excluding the null terminator. */
#define IMAGE_NAME_MAX 63U

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One slot in the image registry representing a single named image asset.
 */
typedef struct
{
    char name[IMAGE_NAME_MAX + 1]; /**< Null-terminated asset name. */
    const uint32_t* buf;           /**< Premultiplied ARGB8888 pixel data (caller-owned). */
    int w;                         /**< Image width in pixels. */
    int h;                         /**< Image height in pixels. */
    bool in_use;                   /**< True when this slot holds a valid entry. */
} ImageEntry;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initializes the image registry, clearing all entries.
 */
void image_registry_init(void);

/**
 * @brief Stores or replaces an image entry under the given name.
 *
 * If an entry with this name already exists its buffer pointer, width, and height are
 * updated in place (the caller-owned pixel data is not copied). If the name is new, a
 * free slot is claimed.
 *
 * @param[in] name      Null-terminated asset name (max IMAGE_NAME_MAX characters).
 * @param[in] argb_buf  Premultiplied ARGB8888 pixel data; caller must keep it live.
 * @param[in] w         Image width in pixels.
 * @param[in] h         Image height in pixels.
 *
 * @return true on success, false if the registry is full or the arguments are invalid.
 */
bool image_registry_store(const char* name, const void* argb_buf, int w, int h);

/**
 * @brief Retrieves a registered image entry by name.
 *
 * @param[in] name  Null-terminated asset name to look up.
 *
 * @return Pointer to the ImageEntry, or NULL if not found.
 */
const ImageEntry* image_registry_get(const char* name);

#endif
