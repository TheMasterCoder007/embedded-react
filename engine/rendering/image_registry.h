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

#include "native_renderer.h" /* ERImageFormat */

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Maximum number of images that can be registered simultaneously.
 *
 * Registration past this point is refused and those images never draw, so the pool has to clear the
 * size of a real asset set rather than a demo's: an icon-heavy app runs well past a hundred images,
 * and the old fixed 32 turned that into an unexplained blank halfway down the screen. Sized here
 * for that case and shrunk per board, the same way as the other pools.
 *
 * Cost is ~80 B/slot on a 32-bit target — ~10 KB at the default — and the 64-byte name field is
 * most of it. Nothing scales with it per frame: lookups skip free slots on a bool test, so a board
 * with 6 images pays only the .bss. RAM-tight boards should still set it down (the RP2040 and
 * ESP32-2432S028R examples do); the CMake knob is `ERUI_IMAGE_REGISTRY_MAX`, and an ESP-IDF
 * consumer appends `ERUI_IMAGE_REGISTRY_MAX=n` to COMPILE_DEFINITIONS like any other ERUI_* flag.
 */
#ifndef ERUI_IMAGE_REGISTRY_MAX
#define ERUI_IMAGE_REGISTRY_MAX 128U
#endif

/** @brief Maximum length of an image asset name, excluding the null terminator. */
#define IMAGE_NAME_MAX 63U

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One slot in the image registry representing a single named image asset.
 *
 * (ERImageFormat lives in native_renderer.h — it is part of the backend blit contract.)
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
 * because the registry filled up reads as ERUI_IMAGE_REGISTRY_MAX here.
 *
 * @return Number of occupied slots, 0 to ERUI_IMAGE_REGISTRY_MAX.
 */
unsigned image_registry_in_use(void);

#endif
