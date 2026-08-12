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
 * @brief Pixel format of a registered image's caller-owned buffer.
 */
typedef enum
{
    ER_IMG_ARGB8888 = 0, /**< 32-bit premultiplied ARGB (0xAARRGGBB), 4 bytes per pixel. */
    ER_IMG_RGB565 = 1,   /**< 16-bit RGB565, 2 bytes per pixel; inherently fully opaque. */
} ERImageFormat;

/**
 * @brief One slot in the image registry representing a single named image asset.
 */
typedef struct
{
    char name[IMAGE_NAME_MAX + 1]; /**< Null-terminated asset name. */
    const void* buf;               /**< Pixel data in `format` layout (caller-owned). */
    int w;                         /**< Image width in pixels. */
    int h;                         /**< Image height in pixels. */
    uint8_t format;                /**< Pixel layout of buf (ERImageFormat). */
    bool opaque;                   /**< Every pixel is fully opaque (scanned at registration). */
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
 * If an entry with this name already exists its buffer pointer, dimensions, and format are
 * updated in place (the caller-owned pixel data is not copied). If the name is new, a
 * free slot is claimed. ARGB8888 pixels are scanned once here for full opacity so the
 * renderer can take the opaque copy path on every subsequent blit; RGB565 has no alpha
 * channel and is always opaque.
 *
 * @param[in] name    Null-terminated asset name (max IMAGE_NAME_MAX characters).
 * @param[in] buf     Pixel data in the given format; caller must keep it live. RGB565 buffers
 *                    must be 2-byte aligned (they are read as uint16_t).
 * @param[in] w       Image width in pixels.
 * @param[in] h       Image height in pixels.
 * @param[in] format  Pixel layout of buf; values outside ERImageFormat are rejected.
 *
 * @return true on success, false if the registry is full or the arguments are invalid
 *         (unknown format, misaligned RGB565 buffer, non-positive dimensions, NULL name/buf).
 */
bool image_registry_store(const char* name, const void* buf, int w, int h, ERImageFormat format);

/**
 * @brief Retrieves a registered image entry by name.
 *
 * @param[in] name  Null-terminated asset name to look up.
 *
 * @return Pointer to the ImageEntry, or NULL if not found.
 */
const ImageEntry* image_registry_get(const char* name);

/**
 * @brief Counts the registry slots currently holding an asset.
 *
 * The perf overlay's "image slots in use" counter — a screen that silently fails to show an image
 * because the registry filled up reads as IMAGE_REGISTRY_MAX here.
 *
 * @return Number of occupied slots, 0 to IMAGE_REGISTRY_MAX.
 */
unsigned image_registry_in_use(void);

#endif
