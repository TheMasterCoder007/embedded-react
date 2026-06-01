#ifndef EMBEDDED_REACT_FONT_REGISTRY_H
#define EMBEDDED_REACT_FONT_REGISTRY_H

#include "font_bitmap.h"
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum number of distinct font families that can be registered. */
#define FONT_REGISTRY_MAX 8U

/** @brief Maximum number of pixel sizes registered per font family. */
#define FONT_FAMILY_MAX_SIZES 16U

/** @brief Maximum length of a font family name, excluding the null terminator. */
#define FONT_NAME_MAX 31U

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Initializes the font registry, clearing all previously registered fonts.
 */
void font_registry_init(void);

/**
 * @brief Adds or replaces a BitmapFont in the registry under the given family name.
 *
 * If a family with this name already exists and it already holds a size entry with the
 * same pixel_size, that entry is replaced in place (no new slot is consumed). Otherwise
 * the font is appended as a new size for the family. If the name is new, a fresh family
 * slot is allocated.
 *
 * Note: replacing a size entry only swaps the registry's pointer; it does not reclaim any
 * font-pool bytes the prior BitmapFont occupied. See font_blob_register() for how the
 * blob loader reuses pool bytes on re-registration.
 *
 * @param[in] name  Null-terminated font family name (max FONT_NAME_MAX characters).
 * @param[in] font  Pointer to the BitmapFont to register.
 *
 * @return true on success, false if the registry or the family's size table is full.
 */
bool font_registry_add(const char* name, const BitmapFont* font);

/**
 * @brief Looks up the exact BitmapFont registered for a family name and pixel size.
 *
 * Unlike font_registry_get(), this performs no closest-size matching and no fallback to
 * the built-in font: it returns a hit only when an entry with precisely the requested
 * pixel_size exists for the named family.
 *
 * @param[in] name        Null-terminated font family name.
 * @param[in] pixel_size  Exact pixel size to match.
 *
 * @return Pointer to the matching BitmapFont, or NULL if none is registered.
 */
const BitmapFont* font_registry_get_exact(const char* name, uint8_t pixel_size);

/**
 * @brief Retrieves the best-matching BitmapFont for a given family name and pixel size.
 *
 * If the name is NULL or empty, or no matching family is found, it falls back to the
 * built-in Inter Regular font at the closest available size.
 *
 * @param[in] name        Null-terminated font family name, or NULL for the default.
 * @param[in] pixel_size  Desired font size in pixels (0 defaults to 16).
 *
 * @return Pointer to the closest matching BitmapFont (never NULL).
 */
const BitmapFont* font_registry_get(const char* name, uint8_t pixel_size);

#endif
